/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Windows implementations of what win-compat.h declares. */
#define TL_WIN_COMPAT_IMPL
#include "win-compat.h"
/* The header is force-included before this file's first line, so its redirections are in
 * effect here too: this file is where they end, and it must call the CRT's own. */
#undef malloc
#undef calloc
#undef realloc
#undef free
#undef posix_memalign
#undef memalign
#undef strdup
#undef strndup

#include <windows.h>
#include <bcrypt.h>
void tl_win_arc4random_buf(void *buf, size_t n) { BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG); }
uint32_t tl_win_arc4random(void) { uint32_t v; tl_win_arc4random_buf(&v, 4); return v; }
unsigned long tl_win_thread_id(void) { return GetCurrentThreadId(); }
int tl_win_setname(const char *name)
{
    wchar_t w[64];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 64);
    SetThreadDescription(GetCurrentThread(), w);
    return 0;
}

/* The header in front of every block: the CRT block it lives in, its size, and a magic. */
typedef struct { void *raw; uint32_t size_lo, magic; } tl_hdr;
#define TL_MAGIC 0x7e57a11cu
_Static_assert(sizeof(tl_hdr) == 16, "the header keeps blocks 16-byte aligned");

static void *carve(void *raw, size_t align, size_t n)
{
    if (!raw) return NULL;
    uintptr_t p = ((uintptr_t)raw + sizeof(tl_hdr) + align - 1) & ~(uintptr_t)(align - 1);
    tl_hdr *h = (tl_hdr *)p - 1;
    h->raw = raw; h->size_lo = (uint32_t)n; h->magic = TL_MAGIC;
    return (void *)p;
}

static tl_hdr *hdr_of(const void *p) { return p ? (tl_hdr *)p - 1 : NULL; }
static int ours(const void *p) { return p && hdr_of(p)->magic == TL_MAGIC; }

void *tl_malloc(size_t n) { return carve(malloc(n + sizeof(tl_hdr)), 16, n); }
void *tl_calloc(size_t n, size_t m)
{
    if (m && n > SIZE_MAX / m) return NULL;
    void *p = tl_malloc(n * m);
    if (p) memset(p, 0, n * m);
    return p;
}
size_t tl_malloc_usable_size(const void *p)
{
    if (!p) return 0;
    if (!ours(p)) return _msize((void *)p);
    tl_hdr *h = hdr_of(p);
    return _msize(h->raw) - (size_t)((const uint8_t *)p - (const uint8_t *)h->raw);
}
void tl_free(void *p)
{
    if (!p) return;
    if (!ours(p)) { free(p); return; }
    tl_hdr *h = hdr_of(p);
    void *raw = h->raw;
    h->magic = 0;
    free(raw);
}
void *tl_realloc(void *p, size_t n)
{
    if (!p) return tl_malloc(n);
    if (!n) { tl_free(p); return NULL; }
    size_t have = tl_malloc_usable_size(p);
    if (n <= have && ours(p) && (const uint8_t *)p - (const uint8_t *)hdr_of(p)->raw == sizeof(tl_hdr)) return p;
    void *q = tl_malloc(n);
    if (!q) return NULL;
    memcpy(q, p, have < n ? have : n);
    tl_free(p);
    return q;
}
int tl_posix_memalign(void **out, size_t align, size_t n)
{
    if (align < 16) align = 16;
    if (align & (align - 1)) return 22; /* EINVAL */
    void *p = carve(malloc(n + sizeof(tl_hdr) + align), align, n);
    if (!p) return 12; /* ENOMEM */
    *out = p;
    return 0;
}
void *tl_memalign(size_t align, size_t n) { void *p = NULL; return tl_posix_memalign(&p, align, n) ? NULL : p; }
char *tl_strdup(const char *s) { size_t n = strlen(s) + 1; char *d = tl_malloc(n); if (d) memcpy(d, s, n); return d; }
char *tl_strndup(const char *s, size_t n)
{
    size_t l = 0; while (l < n && s[l]) l++;
    char *d = tl_malloc(l + 1);
    if (d) { memcpy(d, s, l); d[l] = 0; }
    return d;
}
void *memmem(const void *h, size_t hl, const void *n, size_t nl)
{
    if (!nl) return (void *)h;
    if (hl < nl) return NULL;
    const uint8_t *a = h, *b = n;
    for (size_t i = 0; i + nl <= hl; i++) if (a[i] == b[0] && !memcmp(a + i, b, nl)) return (void *)(a + i);
    return NULL;
}
void tl_win_flush_icache(void *addr, size_t len) { FlushInstructionCache(GetCurrentProcess(), addr, len); }
void *tl_win_stack_top(void) { ULONG_PTR lo, hi; GetCurrentThreadStackLimits(&lo, &hi); return (void *)hi; }
size_t tl_win_stack_size(void) { ULONG_PTR lo, hi; GetCurrentThreadStackLimits(&lo, &hi); return (size_t)(hi - lo); }

/* ------------------------------------------------------------------ dlfcn */

#include <dlfcn.h>
#include <stdio.h>
#include <psapi.h>

static __thread char g_dlerr[256];
static __thread int g_dlerr_set;

void *dlopen(const char *path, int mode)
{
    (void)mode;
    if (!path) return GetModuleHandleA(NULL);
    HMODULE h = LoadLibraryA(path);
    if (!h) { snprintf(g_dlerr, sizeof(g_dlerr), "LoadLibrary(%s) failed: %lu", path, GetLastError()); g_dlerr_set = 1; }
    return h;
}

void *dlsym(void *handle, const char *name)
{
    if (handle && handle != RTLD_NEXT) {
        void *p = (void *)GetProcAddress((HMODULE)handle, name);
        if (!p) { snprintf(g_dlerr, sizeof(g_dlerr), "%s: not found", name); g_dlerr_set = 1; }
        return p;
    }
    /* RTLD_DEFAULT: this program first, then every module loaded in the process. */
    void *p = (void *)GetProcAddress(GetModuleHandleA(NULL), name);
    if (p) return p;
    HMODULE mods[512]; DWORD need = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 512; i++)
            if ((p = (void *)GetProcAddress(mods[i], name))) return p;
    }
    snprintf(g_dlerr, sizeof(g_dlerr), "%s: not found", name); g_dlerr_set = 1;
    return NULL;
}

int dlclose(void *handle) { return handle && handle != GetModuleHandleA(NULL) ? !FreeLibrary((HMODULE)handle) : 0; }
char *dlerror(void) { if (!g_dlerr_set) return NULL; g_dlerr_set = 0; return g_dlerr; }

int dladdr(const void *addr, Dl_info *info)
{
    HMODULE m = NULL;
    memset(info, 0, sizeof(*info));
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m)) return 0;
    static __thread char name[MAX_PATH];
    GetModuleFileNameA(m, name, sizeof(name));
    info->dli_fname = name; info->dli_fbase = m;
    return 1;
}

/* ------------------------------------------------------------------- mman */

#include <sys/mman.h>
#include <io.h>
#include <errno.h>

static DWORD win_prot(int prot)
{
    if (prot & PROT_EXEC) return (prot & PROT_WRITE) ? PAGE_EXECUTE_READWRITE : (prot & PROT_READ) ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (prot & PROT_WRITE) return PAGE_READWRITE;
    if (prot & PROT_READ) return PAGE_READONLY;
    return PAGE_NOACCESS;
}

/* What a mapping is, so munmap knows how to undo it. VirtualFree can only free a whole
 * reservation, and UnmapViewOfFile only a whole view; a partial unmap decommits instead. */
typedef struct tl_map { uint8_t *base; size_t len; int is_view; struct tl_map *next; } tl_map;
static tl_map *g_maps;
static SRWLOCK g_maps_lock = SRWLOCK_INIT;

static void maps_add(void *base, size_t len, int is_view)
{
    tl_map *m = malloc(sizeof(*m));
    m->base = base; m->len = len; m->is_view = is_view;
    AcquireSRWLockExclusive(&g_maps_lock); m->next = g_maps; g_maps = m; ReleaseSRWLockExclusive(&g_maps_lock);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, long long off)
{
    if (!len) { errno = EINVAL; return MAP_FAILED; }
    if (flags & MAP_ANON) {
        void *want = addr;
        if ((flags & MAP_FIXED) && addr) {
            /* Inside one of ours: recommit the range (what a fixed anonymous map over memory we own means). */
            MEMORY_BASIC_INFORMATION mi;
            if (VirtualQuery(addr, &mi, sizeof(mi)) && mi.State != MEM_FREE) {
                if (!VirtualAlloc(addr, len, MEM_COMMIT, win_prot(prot))) { errno = ENOMEM; return MAP_FAILED; }
                memset(addr, 0, len);
                return addr;
            }
        }
        void *p = VirtualAlloc(want, len, MEM_RESERVE | MEM_COMMIT, win_prot(prot));
        if (!p && want && !(flags & MAP_FIXED)) p = VirtualAlloc(NULL, len, MEM_RESERVE | MEM_COMMIT, win_prot(prot));
        if (!p) { errno = ENOMEM; return MAP_FAILED; }
        maps_add(p, len, 0);
        return p;
    }
    /* A file: a copy of it, which is what every private mapping the runtime makes needs
     * (the APK, read-only). Shared writable file mappings are not supported. */
    HANDLE fh = (HANDLE)_get_osfhandle(fd);
    if (fh == INVALID_HANDLE_VALUE) { errno = EBADF; return MAP_FAILED; }
    HANDLE sec = CreateFileMappingA(fh, NULL, (prot & PROT_WRITE) && (flags & MAP_SHARED) ? PAGE_READWRITE : PAGE_WRITECOPY, 0, 0, NULL);
    if (!sec) { errno = EACCES; return MAP_FAILED; }
    DWORD access = (prot & PROT_WRITE) ? ((flags & MAP_SHARED) ? FILE_MAP_WRITE : FILE_MAP_COPY) : FILE_MAP_READ;
    if (prot & PROT_EXEC) access |= FILE_MAP_EXECUTE;
    void *p = MapViewOfFileEx(sec, access, (DWORD)((unsigned long long)off >> 32), (DWORD)off, len, (flags & MAP_FIXED) ? addr : NULL);
    CloseHandle(sec);
    if (!p) { errno = ENOMEM; return MAP_FAILED; }
    maps_add(p, len, 1);
    return p;
}

int munmap(void *addr, size_t len)
{
    AcquireSRWLockExclusive(&g_maps_lock);
    for (tl_map **pp = &g_maps; *pp; pp = &(*pp)->next) {
        tl_map *m = *pp;
        if ((uint8_t *)addr == m->base && len >= m->len) {
            *pp = m->next;
            ReleaseSRWLockExclusive(&g_maps_lock);
            if (m->is_view) UnmapViewOfFile(m->base); else VirtualFree(m->base, 0, MEM_RELEASE);
            free(m);
            return 0;
        }
    }
    ReleaseSRWLockExclusive(&g_maps_lock);
    /* Part of a mapping, or memory the runtime made otherwise: decommit what can be. */
    VirtualFree(addr, len, MEM_DECOMMIT);
    return 0;
}

int mprotect(void *addr, size_t len, int prot)
{
    DWORD old;
    if (!VirtualProtect(addr, len, win_prot(prot), &old)) { errno = EACCES; return -1; }
    if (prot & PROT_EXEC) FlushInstructionCache(GetCurrentProcess(), addr, len);
    return 0;
}
int msync(void *addr, size_t len, int flags) { (void)flags; return FlushViewOfFile(addr, len) ? 0 : 0; }
int madvise(void *addr, size_t len, int advice) { (void)addr; (void)len; (void)advice; return 0; }
int mlock(const void *addr, size_t len) { (void)addr; (void)len; return 0; }
int munlock(const void *addr, size_t len) { (void)addr; (void)len; return 0; }

/* ------------------------------------------------------- positioned I/O */

long long tl_win_pwrite(int fd, const void *buf, size_t n, long long off)
{
    long long was = _lseeki64(fd, 0, SEEK_CUR);
    if (_lseeki64(fd, off, SEEK_SET) < 0) return -1;
    int r = _write(fd, buf, (unsigned)n);
    _lseeki64(fd, was, SEEK_SET);
    return r;
}
long long tl_win_pread(int fd, void *buf, size_t n, long long off)
{
    long long was = _lseeki64(fd, 0, SEEK_CUR);
    if (_lseeki64(fd, off, SEEK_SET) < 0) return -1;
    int r = _read(fd, buf, (unsigned)n);
    _lseeki64(fd, was, SEEK_SET);
    return r;
}

/* ------------------------------------------------- the driver's Darwin calls */

#include <sys/mount.h>
#include <uuid/uuid.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <dirent.h>

int statfs(const char *path, struct statfs *out)
{
    ULARGE_INTEGER a = {0}, t = {0}, f = {0};
    if (!GetDiskFreeSpaceExA(path, &a, &t, &f)) return -1;
    memset(out, 0, sizeof(*out));
    out->f_bsize = 4096; out->f_blocks = t.QuadPart / 4096; out->f_bfree = f.QuadPart / 4096; out->f_bavail = a.QuadPart / 4096;
    out->f_files = 1000000; out->f_ffree = 900000;
    return 0;
}
void uuid_generate_random(uuid_t out)
{
    tl_win_arc4random_buf(out, 16);
    out[6] = (unsigned char)((out[6] & 0x0F) | 0x40);
    out[8] = (unsigned char)((out[8] & 0x3F) | 0x80);
}
void uuid_unparse_lower(const uuid_t u, char *o)
{
    snprintf(o, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}
kern_return_t tl_win_task_info(task_vm_info_data_t *info)
{
    PROCESS_MEMORY_COUNTERS pmc = { sizeof(pmc) };
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 1;
    info->phys_footprint = pmc.WorkingSetSize;
    return KERN_SUCCESS;
}
/* A read that answers "not readable" rather than faulting: ReadProcessMemory on this process. */
kern_return_t vm_read_overwrite(int task, vm_address_t addr, vm_size_t n, vm_address_t out, vm_size_t *got)
{
    (void)task;
    SIZE_T r = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, (LPVOID)out, n, &r)) { *got = 0; return 1; }
    *got = r;
    return KERN_SUCCESS;
}
int sysctlbyname(const char *name, void *out, size_t *len, void *newp, size_t newlen)
{
    (void)newp; (void)newlen;
    if (!strcmp(name, "hw.memsize") && *len >= 8) {
        MEMORYSTATUSEX m = { sizeof(m) }; GlobalMemoryStatusEx(&m);
        uint64_t v = m.ullTotalPhys; memcpy(out, &v, 8); *len = 8; return 0;
    }
    if ((!strcmp(name, "hw.ncpu") || !strcmp(name, "hw.logicalcpu")) && *len >= 4) {
        SYSTEM_INFO si; GetSystemInfo(&si); int v = (int)si.dwNumberOfProcessors; memcpy(out, &v, 4); *len = 4; return 0;
    }
    return -1;
}
int link(const char *from, const char *to) { return CreateHardLinkA(to, from, NULL) ? 0 : -1; }
int alphasort(const struct dirent **a, const struct dirent **b) { return strcmp((*a)->d_name, (*b)->d_name); }
int scandir(const char *dir, struct dirent ***list, int (*filter)(const struct dirent *), int (*cmp)(const struct dirent **, const struct dirent **))
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent **v = NULL; int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (filter && !filter(e)) continue;
        if (n == cap) { cap = cap ? cap * 2 : 32; v = realloc(v, (size_t)cap * sizeof(*v)); }
        v[n] = malloc(sizeof(struct dirent)); memcpy(v[n], e, sizeof(struct dirent)); n++;
    }
    closedir(d);
    if (cmp && n > 1) qsort(v, (size_t)n, sizeof(*v), (int (*)(const void *, const void *))cmp);
    *list = v;
    return n;
}

/* getline: a line with its newline, in a buffer the caller frees (with free(), so tl_malloc'd). */
long long tl_win_getline(char **line, size_t *cap, FILE *f)
{
    size_t n = 0;
    int c;
    if (!*line || !*cap) { *cap = 128; *line = tl_malloc(*cap); if (!*line) return -1; }
    while ((c = fgetc(f)) != EOF) {
        if (n + 2 > *cap) { size_t nc = *cap * 2; char *p = tl_realloc(*line, nc); if (!p) return -1; *line = p; *cap = nc; }
        (*line)[n++] = (char)c;
        if (c == '\n') break;
    }
    if (!n) return -1;
    (*line)[n] = 0;
    return (long long)n;
}
