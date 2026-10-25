/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Reflourished's Import/Export menu.
 *
 * The mod's libRFLib calls five static methods on FilePickerHelper.  Android
 * posts each one to the UI thread, opens the system document picker and copies
 * the chosen file under getExternalFilesDir(null) (our data folder's
 * sdcard/Android/data/<package>/files).  Here the JNI methods only queue a
 * request: a worker asks the host for a path, then does the copy, zip or unzip
 * there, so the game's thread keeps drawing.
 *
 * The paths are the ones libRFLib passes, not names invented here:
 *   Import Save    -> No_Backup/pp.dat.imported
 *   Export Save    <- No_Backup/pp.dat
 *   Import Quests  -> No_Backup (a zip of the quest folder)
 *   Export Quests  <- No_Backup/activequests (zipped)
 *   Import Level   -> No_Backup/CDN.1.4.2/levels/egypt1.json
 * The save loader in libRFLib prefers pp.dat.imported to pp.dat and pp.dat.bak,
 * so the imported profile is the one the next load opens.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-pvz2.h"
#include "husk-tl-internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

void tl_log_line(const char *fmt, ...);
const char *tl_data_dir(void);

typedef struct pick_job {
    int kind;
    char internal[1024];
    char suggested[256];
    char prepared[1200];
    char temp_dir[1200];
    struct pick_job *next;
} pick_job;

static char g_files_dir[900];
static tl_pvz2_file_picker_hooks g_hooks;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static pick_job *g_head, *g_tail;
static pthread_once_t g_worker_once = PTHREAD_ONCE_INIT;
static bool g_waiting;
static bool g_answered;
static char g_answer[1400];

/* --------------------------------------------------------------- utilities */

static void mkdirs(const char *path)
{
    char tmp[1200];
    snprintf(tmp, sizeof(tmp), "%s", path ? path : "");
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

static void dirname_of(const char *path, char *out, size_t n)
{
    snprintf(out, n, "%s", path ? path : "");
    char *slash = strrchr(out, '/');
    if (!slash) { snprintf(out, n, "."); return; }
    if (slash == out) slash[1] = 0;
    else *slash = 0;
}

static const char *base_of(const char *path)
{
    const char *slash = strrchr(path ? path : "", '/');
    return slash ? slash + 1 : (path ? path : "");
}

/* No component may be "..", and a backslash is not a separator on either
 * Android or Darwin, so a name that carries one is not one of the game's. */
static bool path_is_safe(const char *rel)
{
    if (!rel) return false;
    if (strchr(rel, '\\')) return false;
    const char *p = rel;
    while (*p) {
        while (*p == '/') p++;
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);
        if (len == 1 && start[0] == '.') return false;
        if (len == 2 && start[0] == '.' && start[1] == '.') return false;
    }
    return true;
}

/* A path inside the Android external-files directory.  libRFLib passes
 * relative names; an empty name means the directory itself. */
static bool under_files(const char *rel, char *out, size_t n)
{
    if (!path_is_safe(rel)) return false;
    while (rel && *rel == '/') rel++;
    if (!rel || !*rel) return snprintf(out, n, "%s", g_files_dir) < (int)n;
    return snprintf(out, n, "%s/%s", g_files_dir, rel) < (int)n;
}

static bool same_file(const char *a, const char *b)
{
    struct stat sa, sb;
    return stat(a, &sa) == 0 && stat(b, &sb) == 0 &&
           sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static bool copy_file(const char *src, const char *dst, char *err, size_t errlen)
{
    if (same_file(src, dst)) return true;
    struct stat st;
    if (stat(src, &st) != 0) {
        snprintf(err, errlen, "cannot read %s: %s", src, strerror(errno));
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        snprintf(err, errlen, "%s is not a file", src);
        return false;
    }
    char parent[1200], tmp[1400];
    dirname_of(dst, parent, sizeof(parent));
    mkdirs(parent);
    snprintf(tmp, sizeof(tmp), "%s.part", dst);
    int in = open(src, O_RDONLY);
    if (in < 0) {
        snprintf(err, errlen, "cannot open %s: %s", src, strerror(errno));
        return false;
    }
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        snprintf(err, errlen, "cannot write %s: %s", tmp, strerror(errno));
        close(in);
        return false;
    }
    uint8_t buf[64 * 1024];
    bool ok = true;
    for (;;) {
        ssize_t got = read(in, buf, sizeof(buf));
        if (got < 0) {
            if (errno == EINTR) continue;
            snprintf(err, errlen, "cannot read %s: %s", src, strerror(errno));
            ok = false;
            break;
        }
        if (got == 0) break;
        ssize_t done = 0;
        while (done < got) {
            ssize_t put = write(out, buf + done, (size_t)(got - done));
            if (put < 0) {
                if (errno == EINTR) continue;
                snprintf(err, errlen, "cannot write %s: %s", tmp, strerror(errno));
                ok = false;
                break;
            }
            done += put;
        }
        if (!ok) break;
    }
    close(in);
    if (close(out) != 0 && ok) {
        snprintf(err, errlen, "cannot finish %s: %s", tmp, strerror(errno));
        ok = false;
    }
    if (ok && rename(tmp, dst) != 0) {
        snprintf(err, errlen, "cannot replace %s: %s", dst, strerror(errno));
        ok = false;
    }
    if (!ok) unlink(tmp);
    return ok;
}

static void remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) { unlink(path); return; }
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char child[1400];
            if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name) < (int)sizeof(child))
                remove_tree(child);
        }
        closedir(d);
    }
    rmdir(path);
}

/* -------------------------------------------------------------- zip writer */

typedef struct {
    char name[1024];
    uint16_t method;
    uint32_t crc;
    uint64_t csize, usize;
    uint64_t offset;
    uint32_t ext_attr;
} zip_entry_out;

typedef struct {
    FILE *f;
    zip_entry_out *v;
    size_t n, cap;
} zip_writer;

static void put16(FILE *f, uint16_t v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); }
static void put32(FILE *f, uint32_t v)
{
    for (int i = 0; i < 4; i++) fputc((v >> (8 * i)) & 0xff, f);
}

static void dos_time(uint16_t *out_time, uint16_t *out_date)
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    int year = tmv.tm_year + 1900;
    if (year < 1980) year = 1980;
    *out_time = (uint16_t)((tmv.tm_hour << 11) | (tmv.tm_min << 5) | (tmv.tm_sec / 2));
    *out_date = (uint16_t)(((year - 1980) << 9) | ((tmv.tm_mon + 1) << 5) | tmv.tm_mday);
}

static bool writer_add(zip_writer *z, const char *name, const char *file, bool directory,
                       char *err, size_t errlen)
{
    if (strlen(name) >= sizeof(z->v[0].name)) {
        snprintf(err, errlen, "zip entry name is too long");
        return false;
    }
    if (z->n >= 0xffff) {
        snprintf(err, errlen, "too many files to zip");
        return false;
    }
    uint8_t *data = NULL, *packed = NULL;
    size_t len = 0, packed_len = 0;
    uint32_t crc = 0;
    uint16_t method = 0;
    if (!directory) {
        struct stat st;
        if (stat(file, &st) != 0 || !S_ISREG(st.st_mode)) {
            snprintf(err, errlen, "cannot read %s", file);
            return false;
        }
        if (st.st_size > (off_t)(1u << 30)) {
            snprintf(err, errlen, "%s is too large to zip", file);
            return false;
        }
        len = (size_t)st.st_size;
        data = malloc(len ? len : 1);
        if (!data) {
            snprintf(err, errlen, "out of memory reading %s", file);
            return false;
        }
        FILE *in = fopen(file, "rb");
        if (!in || fread(data, 1, len, in) != len) {
            if (in) fclose(in);
            free(data);
            snprintf(err, errlen, "cannot read %s", file);
            return false;
        }
        fclose(in);
        crc = (uint32_t)crc32(0, data, (uInt)len);

        uLong bound = compressBound((uLong)len);
        packed = malloc(bound ? bound : 1);
        if (!packed) {
            free(data);
            snprintf(err, errlen, "out of memory compressing %s", file);
            return false;
        }
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                         Z_DEFAULT_STRATEGY) != Z_OK) {
            free(packed); free(data);
            snprintf(err, errlen, "deflate failed to start");
            return false;
        }
        zs.next_in = data;
        zs.avail_in = (uInt)len;
        zs.next_out = packed;
        zs.avail_out = (uInt)bound;
        int rc = deflate(&zs, Z_FINISH);
        packed_len = zs.total_out;
        deflateEnd(&zs);
        if (rc != Z_STREAM_END) {
            free(packed); free(data);
            snprintf(err, errlen, "cannot compress %s", file);
            return false;
        }
        if (packed_len < len) {
            method = 8;
        } else {
            /* Storing is smaller than (or equal to) deflating this file. */
            free(packed);
            packed = NULL;
            packed_len = len;
        }
    }

    uint16_t time16, date16;
    dos_time(&time16, &date16);
    long offset = ftell(z->f);
    if (offset < 0) {
        free(packed); free(data);
        snprintf(err, errlen, "cannot tell the zip's position");
        return false;
    }
    uint32_t csize = (uint32_t)(method == 8 ? packed_len : len);
    uint32_t usize = (uint32_t)len;
    put32(z->f, 0x04034b50u);
    put16(z->f, 20);
    put16(z->f, 0x0800);                 /* UTF-8 names */
    put16(z->f, method);
    put16(z->f, time16);
    put16(z->f, date16);
    put32(z->f, crc);
    put32(z->f, csize);
    put32(z->f, usize);
    put16(z->f, (uint16_t)strlen(name));
    put16(z->f, 0);
    fwrite(name, 1, strlen(name), z->f);
    if (!directory && csize) {
        if (method == 8) fwrite(packed, 1, csize, z->f);
        else fwrite(data, 1, csize, z->f);
    }
    free(packed); free(data);

    if (ferror(z->f)) {
        snprintf(err, errlen, "cannot write the zip");
        return false;
    }
    if (z->n == z->cap) {
        size_t cap = z->cap ? z->cap * 2 : 32;
        zip_entry_out *v = realloc(z->v, cap * sizeof(*v));
        if (!v) {
            snprintf(err, errlen, "out of memory listing zip entries");
            return false;
        }
        z->v = v;
        z->cap = cap;
    }
    zip_entry_out *e = &z->v[z->n++];
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->method = method;
    e->crc = crc;
    e->csize = csize;
    e->usize = usize;
    e->offset = (uint64_t)offset;
    e->ext_attr = directory ? 0040755u << 16 : 0100644u << 16;
    return true;
}

static bool writer_walk(zip_writer *z, const char *dir, const char *prefix,
                        char *err, size_t errlen)
{
    struct dirent **list = NULL;
    int n = scandir(dir, &list, NULL, alphasort);
    if (n < 0) {
        snprintf(err, errlen, "cannot list %s: %s", dir, strerror(errno));
        return false;
    }
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        const char *name = list[i]->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        char path[1400], entry[1200];
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        snprintf(entry, sizeof(entry), "%s%s", prefix, name);
        struct stat st;
        if (lstat(path, &st) != 0) {
            ok = false;
            snprintf(err, errlen, "cannot read %s: %s", path, strerror(errno));
        } else if (S_ISDIR(st.st_mode)) {
            char dir_entry[1200];
            snprintf(dir_entry, sizeof(dir_entry), "%s/", entry);
            if ((ok = writer_add(z, dir_entry, NULL, true, err, errlen)))
                ok = writer_walk(z, path, dir_entry, err, errlen);
        } else if (S_ISREG(st.st_mode)) {
            ok = writer_add(z, entry, path, false, err, errlen);
        }
    }
    for (int i = 0; i < n; i++) free(list[i]);
    free(list);
    return ok;
}

static bool zip_folder(const char *folder, const char *dst, char *err, size_t errlen)
{
    struct stat st;
    if (stat(folder, &st) != 0 || !S_ISDIR(st.st_mode)) {
        snprintf(err, errlen, "%s is not a folder", folder);
        return false;
    }
    char root[256];
    snprintf(root, sizeof(root), "%s", base_of(folder));
    char root_entry[300];
    snprintf(root_entry, sizeof(root_entry), "%s/", root);

    zip_writer z;
    memset(&z, 0, sizeof(z));
    z.f = fopen(dst, "wb");
    if (!z.f) {
        snprintf(err, errlen, "cannot create %s: %s", dst, strerror(errno));
        return false;
    }
    bool ok = writer_add(&z, root_entry, NULL, true, err, errlen);
    if (ok) ok = writer_walk(&z, folder, root_entry, err, errlen);

    uint16_t time16, date16;
    dos_time(&time16, &date16);
    long cd_offset = ok ? ftell(z.f) : -1;
    if (ok && cd_offset < 0) {
        snprintf(err, errlen, "cannot tell the zip's position");
        ok = false;
    }
    for (size_t i = 0; ok && i < z.n; i++) {
        zip_entry_out *e = &z.v[i];
        put32(z.f, 0x02014b50u);
        put16(z.f, 0x031e);              /* made by Unix, version 3.0 */
        put16(z.f, 20);
        put16(z.f, 0x0800);
        put16(z.f, e->method);
        put16(z.f, time16);
        put16(z.f, date16);
        put32(z.f, e->crc);
        put32(z.f, (uint32_t)e->csize);
        put32(z.f, (uint32_t)e->usize);
        put16(z.f, (uint16_t)strlen(e->name));
        put16(z.f, 0);
        put16(z.f, 0);
        put16(z.f, 0);
        put16(z.f, 0);
        put32(z.f, e->ext_attr);
        put32(z.f, (uint32_t)e->offset);
        fwrite(e->name, 1, strlen(e->name), z.f);
    }
    if (ok) {
        long cd_end = ftell(z.f);
        if (cd_end < 0) {
            snprintf(err, errlen, "cannot tell the zip's position");
            ok = false;
        } else {
            put32(z.f, 0x06054b50u);
            put16(z.f, 0);
            put16(z.f, 0);
            put16(z.f, (uint16_t)z.n);
            put16(z.f, (uint16_t)z.n);
            put32(z.f, (uint32_t)(cd_end - cd_offset));
            put32(z.f, (uint32_t)cd_offset);
            put16(z.f, 0);
        }
    }
    if (fclose(z.f) != 0 && ok) {
        snprintf(err, errlen, "cannot finish %s", dst);
        ok = false;
    }
    free(z.v);
    if (!ok) unlink(dst);
    return ok;
}

/* ------------------------------------------------------------ zip import */

static bool import_zip(const char *zip_path, const char *dest_rel, char *err, size_t errlen)
{
    char target[1200];
    if (!under_files(dest_rel, target, sizeof(target))) {
        snprintf(err, errlen, "bad import folder %s", dest_rel ? dest_rel : "");
        return false;
    }
    mkdirs(target);

    tl_zip z;
    if (!tl_zip_open(&z, zip_path, err, errlen)) return false;

    char **roots = NULL;
    size_t nroots = 0;
    size_t files = 0;
    bool ok = true;
    for (size_t i = 0; i < z.count && ok; i++) {
        const tl_zip_entry *e = &z.entries[i];
        bool is_dir = e->name[0] && e->name[strlen(e->name) - 1] == '/';
        char name[1024];
        snprintf(name, sizeof(name), "%s", e->name);
        char *p = name;
        while (*p == '/') p++;
        size_t len = strlen(p);
        while (len && p[len - 1] == '/') p[--len] = 0;
        if (!*p) continue;
        if (!path_is_safe(p)) {
            tl_log_line("pvz2: file picker: skipping unsafe zip entry %s", e->name);
            continue;
        }

        char root[256];
        const char *slash = strchr(p, '/');
        size_t root_len = slash ? (size_t)(slash - p) : strlen(p);
        if (root_len >= sizeof(root)) {
            tl_log_line("pvz2: file picker: skipping zip entry with a long name");
            continue;
        }
        memcpy(root, p, root_len);
        root[root_len] = 0;
        bool first = true;
        for (size_t k = 0; k < nroots; k++) if (!strcmp(roots[k], root)) { first = false; break; }
        if (first) {
            char **grown = realloc(roots, (nroots + 1) * sizeof(*roots));
            if (!grown) { snprintf(err, errlen, "out of memory reading the zip"); ok = false; break; }
            roots = grown;
            roots[nroots] = strdup(root);
            if (!roots[nroots]) {
                snprintf(err, errlen, "out of memory reading the zip");
                ok = false;
                break;
            }
            nroots++;
            char doomed[1400];
            if (snprintf(doomed, sizeof(doomed), "%s/%s", target, root) < (int)sizeof(doomed))
                remove_tree(doomed);
        }

        char out[1400];
        if (snprintf(out, sizeof(out), "%s/%s", target, p) >= (int)sizeof(out)) {
            tl_log_line("pvz2: file picker: skipping zip entry with a long path");
            continue;
        }
        if (is_dir) {
            mkdirs(out);
            continue;
        }
        const uint8_t *data = NULL;
        size_t data_len = 0;
        bool owned = false;
        if (!tl_zip_data(&z, e, (size_t)1 << 30, &data, &data_len, &owned, err, errlen)) {
            ok = false;
            break;
        }
        char parent[1200];
        dirname_of(out, parent, sizeof(parent));
        mkdirs(parent);
        int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            snprintf(err, errlen, "cannot write %s: %s", out, strerror(errno));
            if (owned) free((void *)data);
            ok = false;
            break;
        }
        size_t done = 0;
        while (done < data_len) {
            ssize_t put = write(fd, data + done, data_len - done);
            if (put < 0) {
                if (errno == EINTR) continue;
                snprintf(err, errlen, "cannot write %s: %s", out, strerror(errno));
                ok = false;
                break;
            }
            done += (size_t)put;
        }
        close(fd);
        if (owned) free((void *)data);
        if (ok) files++;
    }
    tl_zip_close(&z);
    for (size_t i = 0; i < nroots; i++) free(roots[i]);
    free(roots);
    if (ok) tl_log_line("pvz2: file picker: unzipped %s into %s (%zu files)", zip_path, target, files);
    return ok;
}

/* -------------------------------------------------------------- names/paths */

static void timestamp(char *out, size_t n)
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(out, n, "%Y%m%d_%H%M%S", &tmv);
}

static void suggest_file(const char *internal, char *out, size_t n)
{
    const char *base = base_of(internal);
    const char *dot = strrchr(base, '.');
    char stamp[32];
    timestamp(stamp, sizeof(stamp));
    if (dot && dot != base)
        snprintf(out, n, "%.*s_%s.%s", (int)(dot - base), base, stamp, dot + 1);
    else
        snprintf(out, n, "%s_%s", base, stamp);
}

static void suggest_folder(const char *internal, char *out, size_t n)
{
    char base[256];
    snprintf(base, sizeof(base), "%s", base_of(internal));
    size_t len = strlen(base);
    while (len && base[len - 1] == '/') base[--len] = 0;
    if (!base[0]) snprintf(base, sizeof(base), "export");
    char stamp[32];
    timestamp(stamp, sizeof(stamp));
    snprintf(out, n, "%s_%s.zip", base, stamp);
}

static bool prepare_export(pick_job *job, char *err, size_t errlen)
{
    const char *root = tl_data_dir();
    if (!root || !*root) {
        snprintf(err, errlen, "the data folder is not known");
        return false;
    }
    char cache[1200];
    snprintf(cache, sizeof(cache), "%s/cache", root);
    mkdirs(cache);
    snprintf(job->temp_dir, sizeof(job->temp_dir), "%s/file-picker.XXXXXX", cache);
    if (!mkdtemp(job->temp_dir)) {
        snprintf(err, errlen, "cannot make a temporary folder: %s", strerror(errno));
        return false;
    }
    snprintf(job->prepared, sizeof(job->prepared), "%s/%s", job->temp_dir, job->suggested);
    char source[1200];
    if (!under_files(job->internal, source, sizeof(source))) {
        snprintf(err, errlen, "bad export path %s", job->internal);
        return false;
    }
    if (job->kind == TL_PVZ2_PICK_EXPORT_FILE)
        return copy_file(source, job->prepared, err, errlen);
    return zip_folder(source, job->prepared, err, errlen);
}

static void finish_export(pick_job *job, const char *dest)
{
    if (dest && strcmp(dest, job->prepared) != 0) {
        char err[300] = "";
        if (!copy_file(job->prepared, dest, err, sizeof(err))) {
            tl_log_line("pvz2: file picker: export to %s failed: %s", dest, err);
            goto done;
        }
    }
    if (dest) {
        struct stat st;
        off_t size = stat(dest, &st) == 0 ? st.st_size : 0;
        tl_log_line("pvz2: file picker: exported %s -> %s (%lld bytes)",
                    job->internal, dest, (long long)size);
    } else {
        tl_log_line("pvz2: file picker: export cancelled");
    }
done:
    if (job->prepared[0]) unlink(job->prepared);
    if (job->temp_dir[0]) rmdir(job->temp_dir);
}

/* ------------------------------------------------------------------ worker */

static void run_job(pick_job *job)
{
    char err[400] = "";
    const char *pick = getenv("TL_PVZ2_PICK");
    const char *pick_out = getenv("TL_PVZ2_PICK_OUT");
    bool import = job->kind == TL_PVZ2_PICK_IMPORT_FILE ||
                  job->kind == TL_PVZ2_PICK_IMPORT_TO_DIR ||
                  job->kind == TL_PVZ2_PICK_IMPORT_ZIP;
    const char *answer = NULL;

    if (!import && !prepare_export(job, err, sizeof(err))) {
        tl_log_line("pvz2: file picker: cannot prepare the export: %s", err);
        finish_export(job, NULL);
        return;
    }

    if (import && pick && *pick) {
        answer = pick;
        tl_log_line("pvz2: file picker: test chooses %s", answer);
    } else if (!import && pick_out && *pick_out) {
        char dest[1400];
        mkdirs(pick_out);
        snprintf(dest, sizeof(dest), "%s/%s", pick_out, job->suggested);
        tl_log_line("pvz2: file picker: test writes the export to %s", dest);
        finish_export(job, dest);
        return;
    } else {
        tl_pvz2_file_picker_hooks hooks;
        pthread_mutex_lock(&g_lock);
        hooks = g_hooks;
        g_waiting = true;
        g_answered = false;
        g_answer[0] = 0;
        pthread_mutex_unlock(&g_lock);

        if (!hooks.open) {
            pthread_mutex_lock(&g_lock);
            g_waiting = false;
            pthread_mutex_unlock(&g_lock);
            tl_log_line("pvz2: file picker: no host handler (the picker was not installed)");
            finish_export(job, NULL);
            return;
        }
        hooks.open(job->kind, job->internal, job->suggested, job->prepared[0] ? job->prepared : NULL);

        pthread_mutex_lock(&g_lock);
        while (!g_answered) pthread_cond_wait(&g_cond, &g_lock);
        answer = g_answer[0] ? g_answer : NULL;
        g_waiting = false;
        pthread_mutex_unlock(&g_lock);
    }

    if (!answer || !*answer) {
        if (!import) finish_export(job, NULL);
        else tl_log_line("pvz2: file picker: cancelled");
        return;
    }

    if (job->kind == TL_PVZ2_PICK_IMPORT_FILE) {
        char dest[1200];
        if (!under_files(job->internal, dest, sizeof(dest))) {
            tl_log_line("pvz2: file picker: bad import path %s", job->internal);
        } else if (!copy_file(answer, dest, err, sizeof(err))) {
            tl_log_line("pvz2: file picker: import failed: %s", err);
        } else {
            struct stat st;
            off_t size = stat(dest, &st) == 0 ? st.st_size : 0;
            tl_log_line("pvz2: file picker: imported %s -> %s (%lld bytes)",
                        answer, dest, (long long)size);
        }
    } else if (job->kind == TL_PVZ2_PICK_IMPORT_TO_DIR) {
        char dir[1200], dest[1400];
        if (!under_files(job->internal, dir, sizeof(dir))) {
            tl_log_line("pvz2: file picker: bad import folder %s", job->internal);
        } else {
            mkdirs(dir);
            snprintf(dest, sizeof(dest), "%s/%s", dir, base_of(answer));
            if (!copy_file(answer, dest, err, sizeof(err)))
                tl_log_line("pvz2: file picker: import failed: %s", err);
            else
                tl_log_line("pvz2: file picker: imported %s -> %s", answer, dest);
        }
    } else if (job->kind == TL_PVZ2_PICK_IMPORT_ZIP) {
        if (!import_zip(answer, job->internal, err, sizeof(err)))
            tl_log_line("pvz2: file picker: import failed: %s", err);
    } else {
        finish_export(job, answer);
    }
}

static void *picker_worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (!g_head) pthread_cond_wait(&g_cond, &g_lock);
        pick_job *job = g_head;
        g_head = job->next;
        if (!g_head) g_tail = NULL;
        pthread_mutex_unlock(&g_lock);
        run_job(job);
        free(job);
    }
    return NULL;
}

static void start_worker(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, picker_worker, NULL) == 0) {
        pthread_detach(t);
    } else {
        tl_log_line("pvz2: file picker: cannot start its worker");
    }
}

static void request(int kind, const char *internal)
{
    if (!g_files_dir[0]) {
        tl_log_line("pvz2: file picker: the data folder is not configured yet");
        return;
    }
    pick_job *job = calloc(1, sizeof(*job));
    if (!job) return;
    job->kind = kind;
    snprintf(job->internal, sizeof(job->internal), "%s", internal ? internal : "");
    if (kind == TL_PVZ2_PICK_EXPORT_FILE)
        suggest_file(job->internal, job->suggested, sizeof(job->suggested));
    else if (kind == TL_PVZ2_PICK_EXPORT_FOLDER)
        suggest_folder(job->internal, job->suggested, sizeof(job->suggested));
    else
        snprintf(job->suggested, sizeof(job->suggested), "%s", base_of(job->internal));
    pthread_once(&g_worker_once, start_worker);
    pthread_mutex_lock(&g_lock);
    if (g_tail) g_tail->next = job; else g_head = job;
    g_tail = job;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

/* ------------------------------------------------------------- public calls */

void tl_pvz2_file_picker_configure(const char *files_dir)
{
    snprintf(g_files_dir, sizeof(g_files_dir), "%s", files_dir ? files_dir : "");
}

void tl_pvz2_set_file_picker_hooks(const tl_pvz2_file_picker_hooks *hooks)
{
    pthread_mutex_lock(&g_lock);
    g_hooks = hooks ? *hooks : (tl_pvz2_file_picker_hooks){0};
    pthread_mutex_unlock(&g_lock);
}

void tl_pvz2_file_picker_answer(const char *path)
{
    pthread_mutex_lock(&g_lock);
    if (!g_waiting) {
        pthread_mutex_unlock(&g_lock);
        tl_log_line("pvz2: file picker: an answer arrived with no request waiting");
        return;
    }
    snprintf(g_answer, sizeof(g_answer), "%s", path ? path : "");
    g_answered = true;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

void tl_pvz2_file_picker_import_file(const char *name) { request(TL_PVZ2_PICK_IMPORT_FILE, name); }
void tl_pvz2_file_picker_import_file_to_dir(const char *dir) { request(TL_PVZ2_PICK_IMPORT_TO_DIR, dir); }
void tl_pvz2_file_picker_export_file(const char *source) { request(TL_PVZ2_PICK_EXPORT_FILE, source); }
void tl_pvz2_file_picker_export_folder(const char *folder) { request(TL_PVZ2_PICK_EXPORT_FOLDER, folder); }
void tl_pvz2_file_picker_import_zip(const char *dest_parent) { request(TL_PVZ2_PICK_IMPORT_ZIP, dest_parent); }
