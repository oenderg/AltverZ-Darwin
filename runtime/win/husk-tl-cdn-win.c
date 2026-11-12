/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The game's live content on Windows (husk-tl-cdn.m on Darwin).
 *
 * A fetch goes to the project's own CDN over WinHTTP, with the same rules as
 * the Mac client: https to reflourished.ducnguyen.org only, no redirects, 30 s
 * without data and 60 s in total, the server's Date required, and the body must
 * be a complete gzip archive before it replaces the last usable local copy.
 * When the network fails the saved copy answers, dated now, which is what the
 * Mac client does.
 */
#include "pvz2-profile.h"
#include "husk-tl-cdn.h"

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

void tl_log_line(const char *fmt, ...);

#define CDN_LIMIT (64u * 1024u * 1024u)

static void slashes(char *p) { for (; *p; p++) if (*p == '\\') *p = '/'; }

bool tl_cdn_allowed_url(const char *url)
{
    if (!url || strncmp(url, "https://", 8)) return 0;
    const char *host = url + 8;
    if (strchr(host, '@')) return 0;                 /* no user:password */
    size_t n = strcspn(host, ":/?#");
    /* The profile's hosts, "|" apart. */
    bool known = false;
    for (const char *h = PVZ2_PROFILE_CDN_HOSTS; *h && !known;) {
        size_t len = strcspn(h, "|");
        if (len == n && !_strnicmp(host, h, n)) known = true;
        h += len + (h[len] == '|');
    }
    if (!known) return 0;
    const char *rest = host + n;
    if (*rest == ':') {
        char *end = NULL;
        long port = strtol(rest + 1, &end, 10);
        if (port != 443 || (end && *end && *end != '/')) return 0;
    }
    return 1;
}

typedef struct { char key[64]; char value[256]; } hdr;
typedef struct { int n; hdr h[8]; } headers;

static bool read_file(const char *path, unsigned char **out, size_t *len)
{
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END); long long n = _ftelli64(f); _fseeki64(f, 0, SEEK_SET);
    if (n < 0 || (unsigned long long)n > CDN_LIMIT) { fclose(f); return false; }
    unsigned char *b = malloc(n > 0 ? (size_t)n : 1);
    size_t got = b ? fread(b, 1, (size_t)n, f) : 0;
    fclose(f);
    if (!b || got != (size_t)n) { free(b); return false; }
    *out = b; *len = (size_t)n;
    return true;
}

/* Reject partial gzip streams before replacing the last usable offline archive. */
static bool valid_archive(const unsigned char *b, size_t n)
{
    if (n < 2 || n > CDN_LIMIT || b[0] != 0x1f || b[1] != 0x8b) return 0;
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, MAX_WBITS + 16) != Z_OK) return 0;
    z.next_in = (Bytef *)b; z.avail_in = (uInt)n;
    unsigned char out[32768];
    int result;
    do {
        z.next_out = out; z.avail_out = sizeof(out);
        result = inflate(&z, Z_NO_FLUSH);
    } while (result == Z_OK && z.total_out <= 128u * 1024u * 1024u);
    bool valid = result == Z_STREAM_END && !z.avail_in && z.total_out >= 1024;
    inflateEnd(&z);
    return valid;
}

static bool valid_http_date(const char *s)
{
    char wd[8] = "", mon[8] = "";
    int d = 0, y = 0, h = 0, mi = 0, se = 0;
    if (!s || sscanf(s, "%7s %d %7s %d %d:%d:%d GMT", wd, &d, mon, &y, &h, &mi, &se) != 7) return 0;
    /* The weekday is "Wed," with the comma: accept the three letters. */
    if (strlen(wd) < 3) return 0;
    static const char *const months[] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
    int m = -1;
    for (int i = 0; i < 12; i++) if (!_strnicmp(mon, months[i], 3)) { m = i; break; }
    if (m < 0 || d < 1 || d > 31 || y < 2020 || y > 2200 || h > 23 || mi > 59 || se > 60) return 0;
    return 1;
}

static void header_add(headers *h, const char *key, const char *value)
{
    if (!h || h->n >= 8 || !key || !value) return;
    snprintf(h->h[h->n].key, sizeof(h->h[h->n].key), "%s", key);
    snprintf(h->h[h->n].value, sizeof(h->h[h->n].value), "%s", value);
    h->n++;
}

static tl_cdn_response *make_response(unsigned char *body, size_t len, int status, headers *h)
{
    tl_cdn_response *r = calloc(1, sizeof(*r));
    if (!r) { free(body); free(h); return NULL; }
    r->refs = 1; r->status = status; r->body = body; r->length = len; r->headers = h;
    return r;
}

/* The local copy, dated now: the offline answer and the network's fallback. */
static tl_cdn_response *local_answer(const char *cache, const char *why)
{
    unsigned char *body = NULL; size_t len = 0;
    if (!read_file(cache, &body, &len) || !valid_archive(body, len)) {
        free(body);
        tl_log_line("pvz2: CDN fallback (%s) -> no usable local copy", why ? why : "no network");
        return NULL;
    }
    headers *h = calloc(1, sizeof(*h));
    time_t now = time(NULL); struct tm g; gmtime_s(&g, &now);
    char date[64]; strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &g);
    char length[32]; snprintf(length, sizeof(length), "%zu", len);
    header_add(h, "date", date);
    header_add(h, "content-length", length);
    header_add(h, "content-type", "application/x-tar");
    tl_log_line("pvz2: CDN fallback (%s) -> local copy %s (%zu bytes), Date %s", why ? why : "no network", cache, len, date);
    return make_response(body, len, 200, h);
}

/* WinHTTP, on the worker thread: the request is synchronous there, which is
 * what the Mac's delegate queue does asynchronously. */
static tl_cdn_response *http_fetch(const char *url, const char *cache, char *why, size_t whyn)
{
    tl_cdn_response *result = NULL;
    HINTERNET session = NULL, connect = NULL, request = NULL;
    wchar_t wurl[2048];
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048)) { snprintf(why, whyn, "bad URL"); return NULL; }
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc)); uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = (DWORD)-1; uc.dwUrlPathLength = (DWORD)-1; uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) { snprintf(why, whyn, "the URL does not parse"); return NULL; }
    if (uc.nScheme != INTERNET_SCHEME_HTTPS || (uc.nPort != 443 && uc.nPort != 0)) {
        snprintf(why, whyn, "not https on 443"); return NULL;
    }
    wchar_t host[256], path[2048];
    if (uc.dwHostNameLength >= 256) { snprintf(why, whyn, "host too long"); return NULL; }
    wcsncpy(host, uc.lpszHostName, uc.dwHostNameLength); host[uc.dwHostNameLength] = 0;
    int pn = _snwprintf(path, 2048, L"%.*s%.*s", (int)uc.dwUrlPathLength, uc.lpszUrlPath,
                        (int)uc.dwExtraInfoLength, uc.lpszExtraInfo ? uc.lpszExtraInfo : L"");
    if (pn <= 0 || pn >= 2048) { snprintf(why, whyn, "path too long"); return NULL; }

    session = WinHttpOpen(L"" PVZ2_PROFILE_APP_NAME L"/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { snprintf(why, whyn, "WinHttpOpen: %lu", GetLastError()); goto done; }
    WinHttpSetTimeouts(session, 30000, 30000, 30000, 60000);
    connect = WinHttpConnect(session, host, 443, 0);
    if (!connect) { snprintf(why, whyn, "connect: %lu", GetLastError()); goto done; }
    request = WinHttpOpenRequest(connect, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) { snprintf(why, whyn, "request: %lu", GetLastError()); goto done; }
    /* The Mac client checks every redirect against the allowlist. Following
     * one here would leave the allowlist, so a redirect is a failure. */
    DWORD off = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof(off));
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0)) {
        snprintf(why, whyn, "send: %lu", GetLastError()); goto done;
    }
    if (!WinHttpReceiveResponse(request, NULL)) {
        snprintf(why, whyn, "receive: %lu", GetLastError()); goto done;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300) { snprintf(why, whyn, "HTTP %lu", status); goto done; }

    /* The Date has to be the server's own, in the Mac client's format. Ask
     * for the raw headers and parse them, which is what the Mac's dictionary
     * of headers gives it. */
    char date[128] = "", ctype[128] = "";
    size = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                        NULL, &size, WINHTTP_NO_HEADER_INDEX);
    if (size) {
        wchar_t *raw = malloc(size + 2);
        if (raw && WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                                       raw, &size, WINHTTP_NO_HEADER_INDEX)) {
            int un = WideCharToMultiByte(CP_UTF8, 0, raw, -1, NULL, 0, NULL, NULL);
            char *u = un > 0 ? malloc((size_t)un) : NULL;
            if (u && WideCharToMultiByte(CP_UTF8, 0, raw, -1, u, un, NULL, NULL)) {
                for (char *line = u; line && *line; ) {
                    char *nl = strstr(line, "\r\n");
                    if (nl) *nl = 0;
                    char *colon = strchr(line, ':');
                    if (colon) {
                        *colon = 0;
                        char *v = colon + 1; while (*v == ' ' || *v == '\t') v++;
                        if (!_stricmp(line, "date")) snprintf(date, sizeof(date), "%s", v);
                        else if (!_stricmp(line, "content-type")) snprintf(ctype, sizeof(ctype), "%s", v);
                    }
                    if (!nl) break;
                    line = nl + 2;
                }
            }
            free(u);
        }
        free(raw);
    }
    if (!valid_http_date(date)) {
        if (getenv("TL_CDN_TRACE")) tl_log_line("pvz2: CDN Date header was \"%s\", Content-Type \"%s\"", date, ctype);
        snprintf(why, whyn, "missing or invalid server Date"); goto done;
    }

    size_t cap = 1 << 20, len = 0;
    unsigned char *body = malloc(cap);
    if (!body) { snprintf(why, whyn, "out of memory"); goto done; }
    for (;;) {
        if (len == cap) {
            if (cap >= CDN_LIMIT) { snprintf(why, whyn, "the archive is over 64 MB"); free(body); goto done; }
            cap = cap * 2 > CDN_LIMIT ? CDN_LIMIT : cap * 2;
            unsigned char *bigger = realloc(body, cap);
            if (!bigger) { snprintf(why, whyn, "out of memory"); free(body); goto done; }
            body = bigger;
        }
        DWORD got = 0;
        if (!WinHttpReadData(request, body + len, (DWORD)(cap - len), &got)) {
            snprintf(why, whyn, "read: %lu", GetLastError()); free(body); goto done;
        }
        if (!got) break;
        len += got;
    }
    if (!valid_archive(body, len)) { snprintf(why, whyn, "invalid gzip archive"); free(body); goto done; }

    /* Replace the cache atomically: a sibling temporary, then a rename. */
    if (cache) {
        char tmp[1200];
        snprintf(tmp, sizeof(tmp), "%s.part", cache);
        char dir[1200]; snprintf(dir, sizeof(dir), "%s", cache); slashes(dir);
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = 0; char t[1200]; snprintf(t, sizeof(t), "%s", dir);
            for (char *p = t + 1; *p; p++) if (*p == '/') { *p = 0; CreateDirectoryA(t, NULL); *p = '/'; }
            CreateDirectoryA(t, NULL); }
        FILE *f = fopen(tmp, "wb");
        if (f) { fwrite(body, 1, len, f); fclose(f); }
        if (!f || !MoveFileExA(tmp, cache, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileA(tmp);
            tl_log_line("pvz2: CDN cache write failed for %s", cache);
        } else {
            tl_log_line("pvz2: CDN cache updated local copy %s", cache);
        }
    }
    headers *h = calloc(1, sizeof(*h));
    char length[32]; snprintf(length, sizeof(length), "%zu", len);
    header_add(h, "date", date);
    header_add(h, "content-length", length);
    header_add(h, "content-type", ctype[0] ? ctype : "application/x-tar");
    tl_log_line("pvz2: CDN live %s -> %lu, %zu bytes, Date %s", url, (unsigned long)status, len, date);
    result = make_response(body, len, (int)status, h);
done:
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return result;
}

typedef struct { char *url, *cache; uint64_t token; tl_cdn_completion done; } job;
static DWORD WINAPI run(void *p)
{
    job *j = p;
    tl_cdn_response *r = NULL;
    char why[160] = "";
    static const char *const suffix = PVZ2_PROFILE_CDN_PATH;
    size_t url_len = strlen(j->url), suffix_len = strlen(suffix);
    bool wanted = url_len >= suffix_len && !strcmp(j->url + url_len - suffix_len, suffix);
    if (tl_cdn_allowed_url(j->url) && wanted)
        r = http_fetch(j->url, j->cache, why, sizeof(why));
    else
        snprintf(why, sizeof(why), "URL not allowed");
    if (!r) r = local_answer(j->cache, why);
    j->done(j->token, r);
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
