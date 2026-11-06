/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The game's live content on Windows (husk-tl-cdn.m on Darwin).
 *
 * For now the offline half only: a fetch answers with the local copy of the archive
 * (the bundled one, staged into the data folder) and the current date, which is what
 * the Darwin client does when the network fails. The download itself (WinHTTP) is
 * still to come.
 */
#include "husk-tl-cdn.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void tl_log_line(const char *fmt, ...);

bool tl_cdn_allowed_url(const char *url)
{
    return url && (!strncmp(url, "https://", 8));
}

typedef struct { char key[64]; char value[128]; } hdr;
typedef struct { int n; hdr h[8]; } headers;

static bool read_file(const char *path, unsigned char **out, size_t *len)
{
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END); long long n = _ftelli64(f); _fseeki64(f, 0, SEEK_SET);
    unsigned char *b = malloc(n > 0 ? (size_t)n : 1);
    size_t got = b ? fread(b, 1, (size_t)n, f) : 0;
    fclose(f);
    if (!b || got != (size_t)n) { free(b); return false; }
    *out = b; *len = (size_t)n;
    return true;
}
static bool valid_archive(const unsigned char *b, size_t n) { return n >= 2 && b[0] == 0x1f && b[1] == 0x8b; }

typedef struct { char *url, *cache; uint64_t token; tl_cdn_completion done; } job;
static DWORD WINAPI run(void *p)
{
    job *j = p;
    unsigned char *body = NULL; size_t len = 0;
    if (!read_file(j->cache, &body, &len) || !valid_archive(body, len)) {
        free(body);
        tl_log_line("pvz2: CDN fallback (no network client on Windows yet) -> no usable local copy");
        j->done(j->token, NULL);
    } else {
        tl_cdn_response *r = calloc(1, sizeof(*r));
        headers *h = calloc(1, sizeof(*h));
        time_t now = time(NULL); struct tm g; gmtime_s(&g, &now);
        snprintf(h->h[0].key, 64, "date"); strftime(h->h[0].value, 128, "%a, %d %b %Y %H:%M:%S GMT", &g);
        snprintf(h->h[1].key, 64, "content-length"); snprintf(h->h[1].value, 128, "%zu", len);
        snprintf(h->h[2].key, 64, "content-type"); snprintf(h->h[2].value, 128, "application/x-tar");
        h->n = 3;
        r->refs = 1; r->status = 200; r->body = body; r->length = len; r->headers = h;
        tl_log_line("pvz2: CDN fallback (no network client on Windows yet) -> local copy %s (%zu bytes), Date %s", j->cache, len, h->h[0].value);
        j->done(j->token, r);
    }
    free(j->url); free(j->cache); free(j);
    return 0;
}

void tl_cdn_fetch(const char *url, const char *method, const void *body, size_t length,
                  const char *cache_path, uint64_t token, tl_cdn_completion completion)
{
    (void)method; (void)body; (void)length;
    job *j = calloc(1, sizeof(*j));
    j->url = _strdup(url ? url : ""); j->cache = cache_path ? _strdup(cache_path) : NULL;
    j->token = token; j->done = completion;
    CloseHandle(CreateThread(NULL, 0, run, j, 0, NULL));
}

char *tl_cdn_header(const tl_cdn_response *r, const char *key)
{
    const headers *h = r ? r->headers : NULL;
    for (int i = 0; h && i < h->n; i++) if (!_stricmp(h->h[i].key, key)) return _strdup(h->h[i].value);
    return _strdup("");
}
void tl_cdn_response_retain(tl_cdn_response *r) { if (r) InterlockedIncrement((volatile LONG *)&r->refs); }
void tl_cdn_response_free(tl_cdn_response *r)
{
    if (!r || InterlockedDecrement((volatile LONG *)&r->refs)) return;
    free(r->headers); free(r->body); free(r);
}

void tl_cdn_stage(const char *bundled_path, const char *cache_path)
{
    if (GetFileAttributesA(cache_path) != INVALID_FILE_ATTRIBUTES) { tl_log_line("pvz2: CDN cache kept local copy %s", cache_path); return; }
    if (CopyFileA(bundled_path, cache_path, TRUE)) tl_log_line("pvz2: CDN cache staged bundled copy %s", cache_path);
    else tl_log_line("pvz2: CDN cache: no bundled copy at %s", bundled_path);
}
