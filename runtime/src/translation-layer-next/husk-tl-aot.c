/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * husk-tl-aot.c -- load guest libraries that were converted ahead of time.
 *
 * The translation layer maps an ELF and, when the device allows it, makes memory
 * executable so the code it maps can be patched. Neither half works on an iPadOS that
 * enforces TXM: nothing may become executable unless a debugger says so, and writing to
 * mapped code is what library signing forbids anyway.
 *
 * So the same libraries arrive already converted: scripts/aot-convert.py turns each .so
 * into a signed arm64 Mach-O dylib whose every load-time rewrite is baked in -- the
 * thread-block references, the PC-relative pairs into data, the relocation expressions the
 * linker resolved, the stubs for the reserved register. Nothing is generated at run time.
 * A converted library is then an ordinary dylib: dyld validates its signature, applies its
 * rebases and binds, and runs its constructors, and the process never writes code.
 *
 * What this file adds is the one thing dyld cannot do: the driver asks the loader for
 * symbols by name ("JNI_OnLoad", "Java_com_ea_nimble_..."), and those names live inside the
 * guest image as bytes rather than in a symbol table, because the linker could not export
 * what it could not see. Each dylib therefore ships with the converter's .facts file, and
 * a name in it resolves to code_base + (vaddr - the guest's own base).
 */
#include <mach-o/loader.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "husk-tl-aot.h"
#include "husk-tl-bionic.h"
#include "husk-tl-ld.h"

/* One of the translation layer's own implementations, by the guest's name for it. */
static void *host_impl(const char *name)
{
    /* Every table, and the same list the translating route binds an import to
     * (k_tables in husk-tl-bionic.c). A name left out of this list is a name
     * the shim cannot forward: the shim asks here for the implementation, gets
     * nothing, and answers with its "absent" value -- which for dirname is a
     * null pointer, and the guest dereferences it. */
    static const tl_bionic_entry *tabs[] = { tl_tab_core, tl_tab_str, tl_tab_io,
                                             tl_tab_io2, tl_tab_str2, tl_tab_net,
                                             tl_tab_pthread, tl_tab_ndk, tl_tab_egl,
                                             tl_tab_cxx, tl_tab_opensles, tl_tab_sys,
                                             tl_tab_ldbl, tl_tab_jvm };
    for (size_t t = 0; t < sizeof tabs / sizeof *tabs; t++)
        for (const tl_bionic_entry *e = tabs[t]; e && e->name; e++)
            if (!strcmp(e->name, name)) return e->addr;
    return NULL;
}

#define AOT_MAX_LIBS 16
#define AOT_HASH 4096                  /* power of two; a name map, open addressed */

typedef struct {
    uint64_t hash;                    /* 0 = empty; the FNV of the name, never 0 */
    uint32_t name_off;                /* into the name pool */
    uint64_t addr;                    /* where it lives once loaded */
} aot_sym;

typedef struct {
    bool used;
    char so_name[96];                 /* "libPVZ2.so", as the loader knows it */
    char path[512];
    void *handle;
    uint8_t *code, *data, *tcb, *vx18;
    uint64_t code_size, data_size, guest_code_base;
    size_t names_cap;
    uint64_t *ctors;                  /* the guest's constructors, in order */
    size_t nctors, ctors_cap;
    bool ctor_done;
    char *names;                      /* every symbol name, NUL separated */
    size_t names_len;
    aot_sym *tab;                     /* the name map */
    size_t nslots, nsyms;             /* and how big it is: it grows */
    struct tl_lib *stub;              /* what the loader hands out for this library */
} aot_lib;

static aot_lib g_libs[AOT_MAX_LIBS];
static int g_nlibs;
static bool g_tried;

static bool sym_grow(aot_lib *L, size_t names_cap);

static uint64_t fnv1a(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
    return h ? h : 1;
}

/* read() on a regular file is allowed to come up short, and libPVZ2's facts file is
 * 17 MB: one read() of it would stop early and the symbol map would come out truncated.
 * So read until the file says it has nothing more. */
static char *read_file(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return NULL; }
    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) { close(fd); return NULL; }
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got == 0) { free(buf); return NULL; }
    buf[got] = 0;
    if (len) *len = got;
    return buf;
}

/* The facts file the converter writes next to each dylib: link-time addresses, the
 * rebases and patches (for tests and for the record), the symbol map, and the stub table. */
static void parse_facts(aot_lib *L, const char *path)
{
    char *text = read_file(path, NULL);
    if (!text) { tl_log_line("aot: %s: no facts file", L->so_name); return; }

    size_t nslots = 1024, names_cap = 256 * 1024;
    L->names = malloc(names_cap);
    L->names_cap = names_cap;
    L->names_len = 0;
    L->tab = calloc(nslots, sizeof(aot_sym));
    L->nslots = nslots;
    L->nsyms = 0;
    if (!L->names || !L->tab) { free(text); return; }

    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        char key[32], arg1[512], arg2[512];
        unsigned long long v0 = 0, v1 = 0;
        if (sscanf(line, "%31s", key) != 1) continue;
        if (!strcmp(key, "sym") && sscanf(line, "sym %llx %511s", &v0, arg1) == 2) {
            size_t len = strlen(arg1) + 1;
            if (L->names_len + len > names_cap) break;
            memcpy(L->names + L->names_len, arg1, len);
            uint64_t h = fnv1a(arg1);
            size_t i = (size_t)(h & (nslots - 1));
            while (L->tab[i].hash) i = (i + 1) & (nslots - 1);
            L->tab[i].hash = h;
            L->tab[i].name_off = (uint32_t)L->names_len;
            L->tab[i].addr = v0;
            L->names_len += len;
            L->nsyms++;
            /* Keep the table half empty: a full one turns the probe above into a spin. */
            if (L->nsyms * 2 >= nslots && !sym_grow(L, names_cap)) break;
            nslots = L->nslots;
            names_cap = (size_t)L->names_cap;
        } else if (!strcmp(key, "init") && sscanf(line, "init %llx", &v0) == 1) {
            /* The guest's constructor list. dyld did not run these: a converted
             * library is dlopen()ed long before the driver has its JNI environment,
             * and these call back into Java. The runtime calls them from tl_ld_init,
             * where the ELF's loader would have run them. */
            if (L->nctors == L->ctors_cap) {
                size_t cap = L->ctors_cap ? L->ctors_cap * 2 : 64;
                uint64_t *grown = realloc(L->ctors, cap * sizeof *grown);
                if (!grown) break;
                L->ctors = grown;
                L->ctors_cap = cap;
            }
            L->ctors[L->nctors++] = v0;
        } else if (!strcmp(key, "guest_code_base") &&
                   sscanf(line, "guest_code_base %llx", &v0) == 1) {
            L->guest_code_base = v0;
        } else if (!strcmp(key, "code_size") && sscanf(line, "code_size %llx", &v0) == 1) {
            L->code_size = v0;
        } else if (!strcmp(key, "data_size") && sscanf(line, "data_size %llx", &v0) == 1) {
            L->data_size = v0;
        } else if (!strcmp(key, "hook")) {
            /* hook <site vaddr> <name>: a call site the mod patches at load time. The
             * branch is already in the dylib; the target goes in this slot. */
            if (sscanf(line, "hook %llx %511s", &v0, arg1) == 2) {
                /* recorded for tl_aot_hook_slots(); the slot address is the one after it */
                continue;
            }
        }
        (void)arg2;
        (void)v1;
    }
    free(text);
    /* the code label is the image's base: every guest address is code_base + (v - base) */
    if (L->code)
        for (size_t i = 0; i < L->nslots; i++)
            if (L->tab[i].hash)
                L->tab[i].addr = (uint64_t)(L->code + (L->tab[i].addr - L->guest_code_base));
    tl_log_line("aot: %s: %zu symbols, %zu constructors, code %p, data %p, tcb %p",
                L->so_name, L->nsyms, L->nctors,
                (void *)L->code, (void *)L->data, (void *)L->tcb);
}

/* Double the table and reinsert. The names live in one pool, so only the slots move. */
static bool sym_grow(aot_lib *L, size_t names_cap)
{
    size_t nslots = L->nslots * 2;
    aot_sym *tab = calloc(nslots, sizeof *tab);
    char *names = realloc(L->names, names_cap * 2);
    if (!tab || !names) { free(tab); return false; }
    L->names = names;
    L->names_cap = names_cap * 2;
    for (size_t i = 0; i < L->nslots; i++) {
        if (!L->tab[i].hash) continue;
        size_t j = (size_t)(L->tab[i].hash & (nslots - 1));
        while (tab[j].hash) j = (j + 1) & (nslots - 1);
        tab[j] = L->tab[i];
    }
    free(L->tab);
    L->tab = tab;
    L->nslots = nslots;
    return true;
}

static void *sym_lookup(const aot_lib *L, const char *name, uint64_t *out)
{
    if (!L->tab) return NULL;
    uint64_t h = fnv1a(name);
    size_t i = (size_t)(h & (L->nslots - 1));
    for (size_t n = 0; n < L->nslots; n++) {
        if (!L->tab[i].hash) return NULL;
        if (L->tab[i].hash == h && !strcmp(L->names + L->tab[i].name_off, name)) {
            *out = L->tab[i].addr;
            return (void *)L->tab[i].addr;
        }
        i = (i + 1) & (L->nslots - 1);
    }
    return NULL;
}

const char *tl_aot_dir(void)
{
    const char *d = getenv("TL_AOT_DIR");
    return (d && *d) ? d : NULL;
}

/* Has this library been through the loop already? A second tl_aot_init() must not load it
 * twice: dlopen would hand back the same image, but the tables would be rebuilt and the
 * first one's addresses would be orphaned. */
static void scan_for_stub_pointers(aot_lib *L, const char *path);

static bool g_loaded_name(const char *base)
{
    char so[96];
    snprintf(so, sizeof so, "%s.so", base);
    for (int i = 0; i < g_nlibs; i++)
        if (!strcmp(g_libs[i].so_name, so)) return true;
    return false;
}

bool tl_aot_ready(void) { return g_tried && g_nlibs > 0; }

int tl_aot_init(const char *dir)
{
    g_tried = true;
    if (!dir || !*dir) return 0;
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        tl_log_line("aot: %s is not a directory; the guest libraries will be translated", dir);
        return 0;
    }

    /* The guest's libraries, in the order the loader will ask for them: the game library
     * first (it is what the driver starts from), then its framework layer, then the C++
     * runtime and the JNI helper it imports from. */
    static const char *candidates[] = { "libPVZ2", "libRFLib", "libc++_shared",
                                        "libNimble", "libcrashlytics-common" };
    for (size_t c = 0; c < sizeof(candidates) / sizeof(*candidates); c++) {
        if (g_nlibs >= AOT_MAX_LIBS) break;
        if (g_loaded_name(candidates[c])) continue;      /* a second call must not reload */
        char path[512], facts[544];
        snprintf(path, sizeof path, "%s/%s.dylib", dir, candidates[c]);
        struct stat s2;
        if (stat(path, &s2) != 0) continue;
        aot_lib *L = &g_libs[g_nlibs];
        snprintf(L->so_name, sizeof L->so_name, "%s.so", candidates[c]);
        snprintf(L->path, sizeof L->path, "%s", path);
        snprintf(facts, sizeof facts, "%s/%s.dylib.facts", dir, candidates[c]);
        g_nlibs++;
        L->handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!L->handle) {
            tl_log_line("aot: %s did not load: %s", path, dlerror());
            L->used = false;
            continue;
        }
        /* dlsym takes the C name; the linker exported these with a leading underscore */
        L->code = dlsym(L->handle, "guest_code");
        L->data = dlsym(L->handle, "guest_data");
        L->tcb = dlsym(L->handle, "guest_tcb");
        L->vx18 = dlsym(L->handle, "guest_vx18");
        if (!L->code || !L->data || !L->tcb) {
            tl_log_line("aot: %s is missing its image labels; refusing it", path);
            dlclose(L->handle);
            L->handle = NULL;
            L->used = false;
            continue;
        }
        /* The guest's thread block, laid out exactly as husk-tl-ld.c's ensure_tcb()
         * lays it out. These are not decoration: the self pointer at word 0 is what
         * code that walks the block expects to find, and the two 1000s are the stack
         * guard and stack size the compilers read. The cookie at +0x28 is a value that
         * never changes, which is what the matching stack-protector check needs. */
        memset(L->tcb, 0, 0x1000);
        uint64_t *t = (uint64_t *)L->tcb;
        t[0] = (uint64_t)(uintptr_t)L->tcb;
        t[1] = 1000; t[2] = 1000;
        t[5] = 0xdeadbeefcafebabeull;
        L->used = true;
        parse_facts(L, facts);
        scan_for_stub_pointers(L, path);
    }
    /* The thread that loaded them is about to run their code. */
    tl_aot_enter();
    tl_log_line("aot: %d converted librar%s from %s (%s)", g_nlibs, g_nlibs == 1 ? "y" : "ies",
                dir, g_nlibs ? "no translation, no executable memory" : "none");
    return g_nlibs;
}

static aot_lib *find(const char *so_name)
{
    if (!g_tried) tl_aot_init(tl_aot_dir());
    for (int i = 0; i < g_nlibs; i++)
        if (!strcmp(g_libs[i].so_name, so_name)) return &g_libs[i];
    return NULL;
}

bool tl_aot_has(const char *so_name) { return find(so_name) != NULL; }

void *tl_aot_sym(const char *so_name, const char *sym)
{
    aot_lib *L = find(so_name);
    uint64_t addr;
    if (!L || !sym) return NULL;
    return sym_lookup(L, sym, &addr);
}

void *tl_aot_base(const char *so_name)
{
    aot_lib *L = find(so_name);
    return L ? (void *)L->code : NULL;
}

/* The host address of a guest address in a converted library's code. */
void *tl_aot_guest_code_ptr(const char *so_name, uint64_t guest_vaddr)
{
    aot_lib *L = find(so_name);
    if (!L || !L->code) return NULL;
    if (guest_vaddr < L->guest_code_base) return NULL;
    uint64_t off = guest_vaddr - L->guest_code_base;
    if (off >= L->code_size) return NULL;
    return (void *)(L->code + off);
}

/* The same, for whichever converted library holds the address. */
void *tl_aot_guest_code_ptr_any(uint64_t guest_vaddr)
{
    aot_lib *best = NULL;
    for (int i = 0; i < g_nlibs; i++) {
        aot_lib *L = &g_libs[i];
        if (!L->used || !L->code) continue;
        if (guest_vaddr < L->guest_code_base) continue;
        if (guest_vaddr - L->guest_code_base >= L->code_size) continue;
        if (!best || L->code_size > best->code_size) best = L;
    }
    if (!best) return NULL;
    return (void *)(best->code + (guest_vaddr - best->guest_code_base));
}

/* The host address of a guest address in a converted library's data.
 *
 * The converter lays the data segment out at exactly code + (data_base -
 * code_base), so the two segments move together and the base can be recovered
 * from the two runtime addresses -- there is no need for it in the facts file.
 *
 * This is what makes the guest's own globals reachable without the kernel's
 * help (mach_vm_read is not there on iOS): the guest's data segment is mapped
 * in this process, at a known place. */
void *tl_aot_guest_data_ptr(const char *so_name, uint64_t guest_vaddr)
{
    aot_lib *L = find(so_name);
    if (!L || !L->code || !L->data) return NULL;
    if (guest_vaddr < L->guest_code_base) return NULL;
    uint8_t *p = L->code + (guest_vaddr - L->guest_code_base);
    /* The data segment is inside the image, and the thread block is after it. */
    if (p < L->data) return NULL;
    if (L->tcb && p >= L->tcb) return NULL;
    return p;
}

/* The layout, for the record: where the two segments are and how the guest's
 * own addresses map onto them. */
void tl_aot_layout(const char *so_name, uint64_t *code, uint64_t *data,
                   uint64_t *data_base, uint64_t *code_size, uint64_t *data_size)
{
    aot_lib *L = find(so_name);
    uint64_t base = 0;
    if (L && L->data && L->code) base = L->guest_code_base + (uint64_t)(L->data - L->code);
    if (code) *code = L ? (uint64_t)(uintptr_t)L->code : 0;
    if (data) *data = L ? (uint64_t)(uintptr_t)L->data : 0;
    if (data_base) *data_base = base;
    if (code_size) *code_size = L ? L->code_size : 0;
    if (data_size) *data_size = L ? L->data_size : 0;
}

/* The same, for whichever converted library holds the address. */
void *tl_aot_guest_data_ptr_any(uint64_t guest_vaddr)
{
    aot_lib *best = NULL;
    for (int i = 0; i < g_nlibs; i++) {
        aot_lib *L = &g_libs[i];
        if (!L->used || !L->code || !L->data) continue;
        if (guest_vaddr < L->guest_code_base) continue;
        uint8_t *p = L->code + (guest_vaddr - L->guest_code_base);
        if (p < L->data) continue;
        if (L->tcb && p >= L->tcb) continue;
        if (!best || L->data_size > best->data_size) best = L;
    }
    if (!best) return NULL;
    return (void *)(best->code + (guest_vaddr - best->guest_code_base));
}

/* Does anything in the loaded library point into its own stub pool?
 *
 * The stub pool is a segment of generated code the converter adds, and the guest must
 * never learn where it is: it holds no data, and a pointer into it is a pointer into
 * read-only code. The symptom is a fault with a fault address inside the pool, and the
 * address moves when the pool moves, which is what identifies it.
 *
 * So the library is searched, once, right after it is loaded: every word of every
 * segment, compared against the pool's range. A word in there is a leak -- a relocation
 * that resolved to a stub, or a value the converter wrote that should not be there.
 */
static void scan_for_stub_pointers(aot_lib *L, const char *path)
{
    Dl_info di;
    if (!dladdr(L->code, &di) || !di.dli_fbase) { tl_log_line("aot: scan: no dladdr"); return; }
    const struct mach_header *mh = (const void *)di.dli_fbase;
    if (mh->magic != MH_MAGIC_64) { tl_log_line("aot: scan: magic %x", mh->magic); return; }
    const struct segment_command_64 *segs[64];
    int nseg = 0;
    const struct load_command *lc = (const void *)((const char *)mh + sizeof(struct mach_header_64));
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (lc->cmd == LC_SEGMENT_64 && nseg < 64)
            segs[nseg++] = (const void *)lc;
        lc = (const void *)((const char *)lc + lc->cmdsize);
    }
    uint64_t lo = 0, hi = 0;
    for (int i = 0; i < nseg; i++) {
        if (!segs[i]->nsects) continue;
        const struct section_64 *sc = (const void *)(segs[i] + 1);
        for (uint32_t k = 0; k < segs[i]->nsects; k++, sc++)
            if (strcmp(sc->sectname, "__guest_stubs") == 0) {
                /* The section's address is an offset from its segment, except for the
                 * first section in a segment, where the linker leaves it absolute. */
                uint64_t a = sc->addr;
                if (a < segs[i]->vmaddr || a >= segs[i]->vmaddr + segs[i]->vmsize)
                    a = segs[i]->vmaddr + a;
                lo = a; hi = lo + sc->size;
            }
    }
    if (hi <= lo) { tl_log_line("aot: %s: scan: no pool found (%d segments)", path, nseg); return; }
    /* The load commands in a loaded image still hold link-time addresses; the image is
     * mapped at its base plus those, and this one has its text at link address 0. */
    lo += (uint64_t)di.dli_fbase;
    hi += (uint64_t)di.dli_fbase;
    tl_log_line("aot: %s: the stub pool is at %#lx-%#lx", path, lo, hi);
    int shown = 0;
    for (int i = 0; i < nseg && shown < 16; i++) {
        const struct segment_command_64 *s = segs[i];
        if (s->vmaddr == lo) continue;              /* the pool itself */
        if (s->vmsize == 0 || s->vmaddr == 0) continue;   /* __PAGEZERO and friends */
        if (s->vmsize > (1UL << 28)) continue;     /* nothing sane is this big */
        /* __LINKEDIT holds the symbol tables and the signature, never a pointer -- and a
         * signer that is not Apple's (zsign, inside ksign and other sideloaders) can leave
         * it declared larger than the file it rewrote, so reading it to its vmsize touched
         * pages past the end of the file: SIGBUS on the first launch. */
        if (!strncmp(s->segname, "__LINKEDIT", 16)) continue;
        const uint64_t *p = (const void *)(uintptr_t)(s->vmaddr + (uint64_t)di.dli_fbase);
        /* A read-only segment is the file's own pages, so only what the file holds is read;
         * a writable one is backed by memory past its file part (zero fill), all of it. */
        size_t n = ((s->initprot & VM_PROT_WRITE) ? s->vmsize : s->filesize) / 8;
        for (size_t k = 0; k < n && shown < 16; k++) {
            uint64_t v = p[k];
            if (v >= lo && v < hi) {
                tl_log_line("aot: %s: %s+%#lx holds %#lx, which points into the pool",
                            path, s->segname, k * 8, v);
                shown++;
            }
        }
    }
    if (!shown)
        tl_log_line("aot: %s: nothing in this library points into its stub pool", path);
}

/* A thread that is about to run guest code needs the register that stands for the guest's
 * thread pointer, and under conversion nothing sets it: the rewriting replaced every
 * `mrs x, tpidr_el0` with a branch to a stub that asks the shim where this thread's block
 * is, and every instruction that *uses* x18 with a branch to a stub that asks the shim
 * where this thread's value is. On a thread that has never run guest code that value is
 * zero, so the first guest instruction that stores through the thread pointer writes to
 * address 0 - 16 and faults there, which looks like a wild pointer rather than a missing
 * line here.
 *
 * The two have to be the same page: a guest that takes the thread pointer from one and
 * the register from the other is reading one thread's block through another's. So both
 * come from husk_guest_tcb_addr(), which hands out one per thread.
 *
 * Each library still gets its own *slot* for the register value, because each library's
 * TLS lives in its own word there; the block itself is the thread's, not a library's. */
void tl_aot_enter(void)
{
    /* The reserved register's value is not seeded here. It is a general register for this
     * game -- the engine writes it in 7,832 places and reads it in 9,470 -- and the thread's
     * block is reached through `mrs tpidr_el0`, which is a different thing entirely. The
     * shim keeps the value in one word per thread, and that word starts at zero, as the
     * translating route's slot does. Seeding it with the block's address gave the first
     * read of the register in a thread a heap pointer where the game expects zero, and the
     * game indexes off it.
     *
     * The thread's block is still there: the stub the converter wrote for `mrs tpidr_el0`
     * asks the shim for it, per thread, when the guest asks for it. */
    for (int i = 0; i < g_nlibs; i++) {
        if (!g_libs[i].used) continue;
        if (g_libs[i].vx18) *(uint64_t *)g_libs[i].vx18 = 0;
    }
}

void *tl_aot_tcb(const char *so_name){
    aot_lib *L = find(so_name);
    if (!L) return NULL;
    static void *(*tcb_addr)(void);
    if (!tcb_addr && g_nlibs)
        tcb_addr = (void *(*)(void))dlsym(g_libs[0].handle, "husk_guest_tcb_addr");
    return (tcb_addr && tcb_addr()) ? tcb_addr() : (void *)L->tcb;
}

/* The stub the loader hands out: enough for the bookkeeping that treats a library as
 * initialised, and nothing else, because dyld has already done the real work. */
struct tl_lib *tl_aot_stub(const char *so_name)
{
    aot_lib *L = find(so_name);
    if (!L) return NULL;
    if (!L->stub)
        L->stub = tl_ld_aot_stub(L->so_name, L->code, L->data, L->guest_code_base,
                                 (size_t)(L->code_size + L->data_size), L);
    return L->stub;
}

/* The guest's file names are Android's; the files are the host's, and the translation
 * layer is what knows the mapping. A converted library binds `fopen` to libc rather than
 * to that layer, so the layer hands its implementations to the shim once, here. */
static void publish_file_hooks(void)
{
    static bool done;
    if (done || g_nlibs == 0) return;
    done = true;
    void (*set)(void *, void *, void *, void *, void *, void *, void *) =
        dlsym(g_libs[0].handle, "husk_shim_set_file_hooks");
    if (!set) { tl_log_line("aot: the shim has no file hooks; the guest will use the host's paths"); return; }
    set(host_impl("fopen"), host_impl("open"), host_impl("stat"),
        host_impl("access"), host_impl("mkdir"), host_impl("unlink"),
        host_impl("opendir"));
    /* The shim owns the names Darwin also has, and asks here for the implementation rather
     * than keeping a second copy of Android's layouts. It is the same table the ELF loader
     * binds an import to when it maps the guest. */
    void (*set_lookup)(void *) = dlsym(g_libs[0].handle, "husk_shim_set_host_lookup");
    if (set_lookup) set_lookup((void *)host_impl);

    /* OpenGL ES is reached the way the guest expects it to be: by name, through the same
     * eglGetProcAddress the EGL layer already answers. On iOS that is ANGLE's, on the Mac
     * the GLES a Steam app ships, and either way it is the real implementation rather than
     * a stub that reports itself and returns. */
    void (*set_gles)(void *) = dlsym(g_libs[0].handle, "husk_shim_set_gles_resolver");
    if (set_gles) set_gles((void *)tl_egl_resolve);
    else tl_log_line("aot: the shim cannot reach OpenGL ES; the guest's gl* will not draw");

    /* An OpenSLES interface id is a constant the guest passes by address, and the value is
     * the interface this translation layer built. The shim owns the storage -- it is the
     * symbol the guest's bind resolves to -- and the host supplies the value. */
    void (*set_data)(const char *, const void *) =
        dlsym(g_libs[0].handle, "husk_shim_set_data");
    if (set_data) {
        static const char *const ids[] = { "SL_IID_ENGINE", "SL_IID_PLAY",
                                           "SL_IID_BUFFERQUEUE", "SL_IID_ANDROIDCONFIGURATION" };
        for (size_t i = 0; i < sizeof ids / sizeof *ids; i++) {
            void *src = host_impl(ids[i]);
            if (src) set_data(ids[i], src);
        }
    }

    /* The same for a thread the guest starts: it has never been through the loader, so the
     * register that stands for its thread pointer is whatever it was. */
    void (*set_entry)(void (*)(void)) =
        dlsym(g_libs[0].handle, "husk_shim_set_thread_entry");
    if (set_entry) set_entry(tl_aot_enter);
    tl_log_line("aot: the guest's file calls go through the translation layer's own");
}

bool tl_aot_run_ctors(const char *so_name)
{
    aot_lib *L = find(so_name);
    if (!L) return false;
    if (L->ctor_done) return true;
    L->ctor_done = true;
    publish_file_hooks();
    bool trace = getenv("TL_AOT_TRACE_CTORS") != NULL;
    /* The same call the guest's own loader makes: one argument, the process's argv and
     * its environment. A constructor that reads them has to find what it expects. */
    char **argv, **envp;
    tl_ld_ctor_args(&argv, &envp);
    typedef void (*ctor_fn)(int, char **, char **);
    for (size_t i = 0; i < L->nctors; i++) {
        /* An empty slot is an empty slot: the ELF's loader skips it, so we do too. One
         * of libPVZ2's 1,046 entries is null. */
        if (L->ctors[i] == 0 || L->ctors[i] == ~0ull) continue;
        ctor_fn fn = (ctor_fn)(L->code + (L->ctors[i] - L->guest_code_base));
        if (trace)
            tl_log_line("aot: %s: constructor %zu/%zu at guest %#llx", so_name, i + 1,
                        L->nctors, (unsigned long long)L->ctors[i]);
        fn(1, argv, envp);
    }
    tl_log_line("aot: %s: ran %zu constructor(s)", so_name, L->nctors);
    return true;
}

bool tl_aot_is_aot(const struct tl_lib *L) { return tl_ld_aot_cookie(L) != NULL; }

/* The loader holds our record; the name is the one the loader asked for. */
const char *tl_aot_name(const struct tl_lib *L)
{
    const aot_lib *conv = tl_ld_aot_cookie(L);
    return conv ? conv->so_name : NULL;
}