/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Strings, wide characters, locales, time, numbers and printf for the guest on
 * Windows, plus setjmp, memory, math and the network stand-ins.
 *
 * The guest's wchar_t is 32 bits (UTF-32) and Windows' is 16, so every wide-character
 * function here is written over uint32_t; none goes to Windows' own. Its `long` is 64
 * bits, so everything returning or taking one is a wrapper over the long long version.
 * A guest long double is an IEEE quad returned in q0; Windows has none, so strtold and
 * friends parse a double and widen it.
 */
#include "husk-tl-bionic-win.h"

#include <windows.h>
#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "husk-tl-va.h"

int *tl_guest_errno_ptr(void);
#define GERR(e) (*tl_guest_errno_ptr() = (e))
typedef uint32_t gwchar;

/* ----------------------------------------------------------------- memory */

static void *b_malloc(size_t n) { return tl_malloc(n); }
static void *b_calloc(size_t n, size_t m) { return tl_calloc(n, m); }
static void *b_realloc(void *p, size_t n) { return tl_realloc(p, n); }
static void b_free(void *p) { tl_free(p); }
static int b_posix_memalign(void **o, size_t a, size_t n) { return tl_posix_memalign(o, a, n); }
static void *b_memalign(size_t a, size_t n) { return tl_memalign(a, n); }
static char *b_strdup(const char *s) { return tl_strdup(s); }

/* ---------------------------------------------------------------- strings */

static void *b_memrchr(const void *s, int c, size_t n) { const uint8_t *p = s; while (n--) if (p[n] == (uint8_t)c) return (void *)(p + n); return NULL; }
static size_t b_strlcpy(char *d, const char *s, size_t n) { size_t l = strlen(s); if (n) { size_t k = l < n - 1 ? l : n - 1; memcpy(d, s, k); d[k] = 0; } return l; }
static char *b_strsep(char **sp, const char *delim)
{
    char *s = *sp;
    if (!s) return NULL;
    char *e = s + strcspn(s, delim);
    if (*e) { *e = 0; *sp = e + 1; } else *sp = NULL;
    return s;
}
static char *b_strtok_r(char *s, const char *d, char **save) { return strtok_s(s, d, save); }
static int b_strcoll_l(const char *a, const char *b, void *l) { (void)l; return strcmp(a, b); }
static size_t b_strxfrm_l(char *d, const char *s, size_t n, void *l) { (void)l; size_t len = strlen(s); if (n) { size_t k = len < n - 1 ? len : n - 1; memcpy(d, s, k); d[k] = 0; } return len; }
static void *b___memmove_chk(void *d, const void *s, size_t n, size_t dn) { (void)dn; return memmove(d, s, n); }
static size_t b___strlen_chk(const char *s, size_t n) { (void)n; return strlen(s); }

/* Linux's messages for Linux's numbers */
static const char *g_errstr[] = {
    "Success", "Operation not permitted", "No such file or directory", "No such process", "Interrupted system call",
    "I/O error", "No such device or address", "Argument list too long", "Exec format error", "Bad file descriptor",
    "No child processes", "Try again", "Out of memory", "Permission denied", "Bad address", "Block device required",
    "Device or resource busy", "File exists", "Cross-device link", "No such device", "Not a directory", "Is a directory",
    "Invalid argument", "File table overflow", "Too many open files", "Not a typewriter", "Text file busy", "File too large",
    "No space left on device", "Illegal seek", "Read-only file system", "Too many links", "Broken pipe",
    "Math argument out of domain of func", "Math result not representable", "Resource deadlock would occur",
    "File name too long", "No record locks available", "Function not implemented", "Directory not empty" };
static char *b_strerror(int e)
{
    static __thread char buf[64];
    if (e >= 0 && e < (int)(sizeof(g_errstr) / sizeof(g_errstr[0]))) return (char *)g_errstr[e];
    snprintf(buf, sizeof(buf), "Unknown error %d", e);
    return buf;
}
static int b_strerror_r(int e, char *buf, size_t n) { snprintf(buf, n, "%s", b_strerror(e)); return 0; }

/* ----------------------------------------------------------------- ctype */

static int b_isupper_l(int c, void *l) { (void)l; return isupper(c); }
static int b_islower_l(int c, void *l) { (void)l; return islower(c); }
static int b_isdigit_l(int c, void *l) { (void)l; return isdigit(c); }
static int b_isxdigit_l(int c, void *l) { (void)l; return isxdigit(c); }
static int b_tolower_l(int c, void *l) { (void)l; return tolower(c); }
static int b_toupper_l(int c, void *l) { (void)l; return toupper(c); }
static size_t b___ctype_get_mb_cur_max(void) { return 1; }

/* ---------------------------------------------------- wide characters */

static WORD wtype(gwchar c)
{
    if (c > 0xFFFF) return 0;
    wchar_t w = (wchar_t)c; WORD t = 0;
    GetStringTypeW(CT_CTYPE1, &w, 1, &t);
    return t;
}
static int b_iswalpha(gwchar c) { return c < 128 ? isalpha((int)c) != 0 : (wtype(c) & C1_ALPHA) != 0; }
static int b_iswalnum(gwchar c) { return c < 128 ? isalnum((int)c) != 0 : (wtype(c) & (C1_ALPHA | C1_DIGIT)) != 0; }
static int b_iswdigit(gwchar c) { return c >= '0' && c <= '9'; }
static int b_iswxdigit(gwchar c) { return c < 128 && isxdigit((int)c); }
static int b_iswspace(gwchar c) { return c < 128 ? isspace((int)c) != 0 : (wtype(c) & C1_SPACE) != 0; }
static int b_iswblank(gwchar c) { return c == ' ' || c == '\t' || (c >= 128 && (wtype(c) & C1_BLANK)); }
static int b_iswcntrl(gwchar c) { return c < 32 || c == 127 || (c >= 128 && (wtype(c) & C1_CNTRL)); }
static int b_iswupper(gwchar c) { return c < 128 ? isupper((int)c) != 0 : (wtype(c) & C1_UPPER) != 0; }
static int b_iswlower(gwchar c) { return c < 128 ? islower((int)c) != 0 : (wtype(c) & C1_LOWER) != 0; }
static int b_iswpunct(gwchar c) { return c < 128 ? ispunct((int)c) != 0 : (wtype(c) & C1_PUNCT) != 0; }
static int b_iswprint(gwchar c) { return c < 128 ? isprint((int)c) != 0 : c <= 0x10FFFF && !(wtype(c) & C1_CNTRL); }
#define WL(name) static int name##_l(gwchar c, void *l) { (void)l; return name(c); }
WL(b_iswalpha) WL(b_iswblank) WL(b_iswcntrl) WL(b_iswdigit) WL(b_iswlower) WL(b_iswprint) WL(b_iswpunct) WL(b_iswspace) WL(b_iswupper) WL(b_iswxdigit)
static gwchar b_towlower(gwchar c) { if (c < 128) return (gwchar)tolower((int)c); if (c > 0xFFFF) return c; wchar_t w = (wchar_t)c; CharLowerBuffW(&w, 1); return w; }
static gwchar b_towupper(gwchar c) { if (c < 128) return (gwchar)toupper((int)c); if (c > 0xFFFF) return c; wchar_t w = (wchar_t)c; CharUpperBuffW(&w, 1); return w; }
static gwchar b_towlower_l(gwchar c, void *l) { (void)l; return b_towlower(c); }
static gwchar b_towupper_l(gwchar c, void *l) { (void)l; return b_towupper(c); }

static size_t b_wcslen(const gwchar *s) { size_t n = 0; while (s[n]) n++; return n; }
static gwchar *b_wcschr(const gwchar *s, gwchar c) { for (;; s++) { if (*s == c) return (gwchar *)s; if (!*s) return NULL; } }
static int b_wcscmp(const gwchar *a, const gwchar *b) { while (*a && *a == *b) { a++; b++; } return *a < *b ? -1 : *a > *b; }
static int b_wcsncmp(const gwchar *a, const gwchar *b, size_t n) { for (; n; n--, a++, b++) { if (*a != *b) return *a < *b ? -1 : 1; if (!*a) return 0; } return 0; }
static gwchar *b_wcscpy(gwchar *d, const gwchar *s) { gwchar *r = d; while ((*d++ = *s++)); return r; }
static gwchar *b_wcsncpy(gwchar *d, const gwchar *s, size_t n) { size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
static size_t b_wcsspn(const gwchar *s, const gwchar *acc) { size_t i = 0; for (; s[i]; i++) if (!b_wcschr(acc, s[i])) break; return i; }
static int b_wcscoll_l(const gwchar *a, const gwchar *b, void *l) { (void)l; return b_wcscmp(a, b); }
static size_t b_wcsxfrm_l(gwchar *d, const gwchar *s, size_t n, void *l) { (void)l; size_t len = b_wcslen(s); if (n) { size_t k = len < n - 1 ? len : n - 1; memcpy(d, s, k * 4); d[k] = 0; } return len; }
static gwchar *b_wmemchr(const gwchar *s, gwchar c, size_t n) { for (size_t i = 0; i < n; i++) if (s[i] == c) return (gwchar *)(s + i); return NULL; }
static int b_wmemcmp(const gwchar *a, const gwchar *b, size_t n) { for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1; return 0; }
static gwchar *b_wmemcpy(gwchar *d, const gwchar *s, size_t n) { return memcpy(d, s, n * 4); }
static gwchar *b_wmemmove(gwchar *d, const gwchar *s, size_t n) { return memmove(d, s, n * 4); }
static gwchar *b_wmemset(gwchar *d, gwchar c, size_t n) { for (size_t i = 0; i < n; i++) d[i] = c; return d; }

/* UTF-8, which is what bionic's multibyte functions speak. The conversion state is not kept: a
 * sequence split across calls is reported incomplete, which the guest (libc++'s codecvt) handles. */
static size_t utf8_dec(const unsigned char *s, size_t n, gwchar *out)
{
    if (!n) return (size_t)-2;
    unsigned char c = s[0];
    if (c < 0x80) { *out = c; return 1; }
    size_t len = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!len) return (size_t)-1;
    if (n < len) return (size_t)-2;
    gwchar v = c & (0x7F >> len);
    for (size_t i = 1; i < len; i++) { if ((s[i] & 0xC0) != 0x80) return (size_t)-1; v = (v << 6) | (s[i] & 0x3F); }
    *out = v;
    return len;
}
static size_t utf8_enc(gwchar c, char *o)
{
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 0x3F)); o[2] = (char)(0x80 | (c & 0x3F)); return 3; }
    if (c < 0x110000) { o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 0x3F)); o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F)); return 4; }
    return (size_t)-1;
}
static size_t b_mbrtowc(gwchar *pwc, const char *s, size_t n, void *ps)
{
    (void)ps;
    if (!s) return 0;
    gwchar w = 0;
    size_t r = utf8_dec((const unsigned char *)s, n, &w);
    if (r == (size_t)-1) { GERR(84); return r; }
    if (r == (size_t)-2) return r;
    if (pwc) *pwc = w;
    return w ? r : 0;
}
static size_t b_mbrlen(const char *s, size_t n, void *ps) { return b_mbrtowc(NULL, s, n, ps); }
static int b_mbtowc(gwchar *pwc, const char *s, size_t n) { if (!s) return 0; size_t r = b_mbrtowc(pwc, s, n, NULL); return r > 4 ? -1 : (int)r; }
static size_t b_wcrtomb(char *s, gwchar c, void *ps)
{
    (void)ps;
    char tmp[4];
    if (!s) return 1;
    size_t r = utf8_enc(c, tmp);
    if (r == (size_t)-1) { GERR(84); return r; }
    memcpy(s, tmp, r);
    return r;
}
static gwchar b_btowc(int c) { return c >= 0 && c < 128 ? (gwchar)c : 0xFFFFFFFFu; }
static int b_wctob(gwchar c) { return c < 128 ? (int)c : -1; }
static size_t b_mbsnrtowcs(gwchar *dst, const char **src, size_t nms, size_t len, void *ps)
{
    (void)ps;
    const unsigned char *s = (const unsigned char *)*src;
    size_t i = 0, out = 0;
    while ((!dst || out < len) && i < nms) {
        gwchar w;
        size_t r = utf8_dec(s + i, nms - i, &w);
        if (r == (size_t)-1) { GERR(84); *src = (const char *)(s + i); return (size_t)-1; }
        if (r == (size_t)-2) break;
        if (dst) dst[out] = w;
        if (!w) { if (dst) *src = NULL; return out; }
        out++; i += r;
    }
    if (dst) *src = (const char *)(s + i);
    return out;
}
static size_t b_mbsrtowcs(gwchar *dst, const char **src, size_t len, void *ps) { return b_mbsnrtowcs(dst, src, (size_t)-1 / 2, len, ps); }
static size_t b_wcsnrtombs(char *dst, const gwchar **src, size_t nwc, size_t len, void *ps)
{
    (void)ps;
    const gwchar *s = *src;
    size_t out = 0;
    for (size_t i = 0; i < nwc; i++) {
        char tmp[4];
        size_t r = utf8_enc(s[i], tmp);
        if (r == (size_t)-1) { GERR(84); *src = s + i; return (size_t)-1; }
        if (dst && out + r > len) { *src = s + i; return out; }
        if (dst) memcpy(dst + out, tmp, r);
        if (!s[i]) { if (dst) *src = NULL; return out; }
        out += r;
    }
    if (dst) *src = s + nwc;
    return out;
}
static size_t b_wcstombs(char *dst, const gwchar *src, size_t n)
{
    const gwchar *s = src;
    size_t r = b_wcsnrtombs(dst, &s, (size_t)-1 / 2, n, NULL);
    if (dst && s == NULL && r < n) dst[r] = 0;
    return r;
}

/* Wide numbers: narrowed (ASCII digits and signs), parsed, the end mapped back. */
static size_t narrow(const gwchar *w, char *out, size_t n)
{
    size_t i = 0;
    for (; w[i] && i + 1 < n; i++) out[i] = w[i] < 128 ? (char)w[i] : '?';
    out[i] = 0;
    return i;
}
#define WIDE_NUM(name, T, call) \
    static T name(const gwchar *s, gwchar **end, int base) { char b[256]; narrow(s, b, sizeof(b)); char *e; T v = (T)call(b, &e, base); if (end) *end = (gwchar *)(s + (e - b)); return v; }
WIDE_NUM(b_wcstol, int64_t, strtoll)
WIDE_NUM(b_wcstoll, int64_t, strtoll)
WIDE_NUM(b_wcstoul, uint64_t, strtoull)
WIDE_NUM(b_wcstoull, uint64_t, strtoull)
static double b_wcstod(const gwchar *s, gwchar **end) { char b[256]; narrow(s, b, sizeof(b)); char *e; double v = strtod(b, &e); if (end) *end = (gwchar *)(s + (e - b)); return v; }
static float b_wcstof(const gwchar *s, gwchar **end) { char b[256]; narrow(s, b, sizeof(b)); char *e; float v = strtof(b, &e); if (end) *end = (gwchar *)(s + (e - b)); return v; }

/* --------------------------------------------------------- long double */

/* A guest long double is IEEE binary128, returned in q0: a 128-bit vector return is in v0 here too. */
typedef uint64_t v2u64 __attribute__((vector_size(16)));
static v2u64 quad_from_double(double d)
{
    uint64_t b; memcpy(&b, &d, 8);
    uint64_t sign = b >> 63, exp = (b >> 52) & 0x7FF, man = b & 0xFFFFFFFFFFFFFull;
    uint64_t hi, lo;
    if (exp == 0 && man == 0) { hi = sign << 63; lo = 0; }
    else if (exp == 0x7FF) { hi = (sign << 63) | (0x7FFFull << 48) | (man >> 4); lo = man << 60; }
    else {
        int64_t e = (int64_t)exp - 1023;
        if (exp == 0) { while (!(man & (1ull << 52))) { man <<= 1; e--; } man &= 0xFFFFFFFFFFFFFull; e += 1; }
        hi = (sign << 63) | ((uint64_t)(e + 16383) << 48) | (man >> 4);
        lo = man << 60;
    }
    v2u64 r = { lo, hi };
    return r;
}
static v2u64 b_strtold(const char *s, char **end) { return quad_from_double(strtod(s, end)); }
static v2u64 b_strtold_l(const char *s, char **end, void *l) { (void)l; return quad_from_double(strtod(s, end)); }
static v2u64 b_wcstold(const gwchar *s, gwchar **end) { return quad_from_double(b_wcstod(s, end)); }

/* ---------------------------------------------------------------- numbers */

static int64_t b_atol(const char *s) { return strtoll(s, NULL, 10); }
static int64_t b_strtol(const char *s, char **e, int base) { errno = 0; long long v = strtoll(s, e, base); if (errno == ERANGE) GERR(G_ERANGE); return v; }
static uint64_t b_strtoul(const char *s, char **e, int base) { errno = 0; unsigned long long v = strtoull(s, e, base); if (errno == ERANGE) GERR(G_ERANGE); return v; }
static int64_t b_strtoll_l(const char *s, char **e, int base, void *l) { (void)l; return strtoll(s, e, base); }
static uint64_t b_strtoull_l(const char *s, char **e, int base, void *l) { (void)l; return strtoull(s, e, base); }

/* ---------------------------------------------------------------- locales */

typedef struct {
    char *decimal_point, *thousands_sep, *grouping, *int_curr_symbol, *currency_symbol, *mon_decimal_point,
         *mon_thousands_sep, *mon_grouping, *positive_sign, *negative_sign;
    char frac[14];
} g_lconv;
static g_lconv g_lc = { ".", "", "", "", "", "", "", "", "", "", { 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127 } };
static void *b_localeconv(void) { return &g_lc; }
static int g_locale_obj;
static void *b_newlocale(int mask, const char *name, void *base) { (void)mask; (void)name; (void)base; return &g_locale_obj; }
static void b_freelocale(void *l) { (void)l; }
static __thread void *t_locale;
static void *b_uselocale(void *l) { void *o = t_locale ? t_locale : (void *)-1L; if (l) t_locale = l; return o; }
static char *b_setlocale(int cat, const char *name) { (void)cat; (void)name; return "C"; }

/* -------------------------------------------------------------------- time */

typedef struct { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; int pad; int64_t tm_gmtoff; const char *tm_zone; } g_tm;
_Static_assert(sizeof(g_tm) == 56, "bionic arm64 struct tm");

static int64_t g_timezone;   /* the data symbol "timezone": seconds west of UTC */
static int64_t local_offset(void) { long tz = 0; _get_timezone(&tz); return -(int64_t)tz; }
static void to_g(const struct tm *t, g_tm *g, int64_t gmtoff, const char *zone)
{
    g->tm_sec = t->tm_sec; g->tm_min = t->tm_min; g->tm_hour = t->tm_hour; g->tm_mday = t->tm_mday; g->tm_mon = t->tm_mon;
    g->tm_year = t->tm_year; g->tm_wday = t->tm_wday; g->tm_yday = t->tm_yday; g->tm_isdst = t->tm_isdst;
    g->tm_gmtoff = gmtoff; g->tm_zone = zone;
}
static void from_g(const g_tm *g, struct tm *t)
{
    memset(t, 0, sizeof(*t));
    t->tm_sec = g->tm_sec; t->tm_min = g->tm_min; t->tm_hour = g->tm_hour; t->tm_mday = g->tm_mday; t->tm_mon = g->tm_mon;
    t->tm_year = g->tm_year; t->tm_wday = g->tm_wday; t->tm_yday = g->tm_yday; t->tm_isdst = g->tm_isdst;
}
static g_tm *b_gmtime_r(const int64_t *t, g_tm *g) { struct tm w; __time64_t v = *t; if (_gmtime64_s(&w, &v)) return NULL; to_g(&w, g, 0, "UTC"); return g; }
static g_tm *b_localtime_r(const int64_t *t, g_tm *g)
{
    struct tm w; __time64_t v = *t;
    if (_localtime64_s(&w, &v)) return NULL;
    to_g(&w, g, local_offset() + (w.tm_isdst > 0 ? 3600 : 0), "LOC");
    return g;
}
static g_tm *b_gmtime(const int64_t *t) { static __thread g_tm g; return b_gmtime_r(t, &g); }
static g_tm *b_localtime(const int64_t *t) { static __thread g_tm g; return b_localtime_r(t, &g); }
static int64_t b_mktime(g_tm *g)
{
    struct tm w; from_g(g, &w);
    __time64_t v = _mktime64(&w);
    if (v != -1) to_g(&w, g, local_offset() + (w.tm_isdst > 0 ? 3600 : 0), "LOC");
    return v;
}
static int64_t b_timegm(g_tm *g) { struct tm w; from_g(g, &w); __time64_t v = _mkgmtime64(&w); if (v != -1) to_g(&w, g, 0, "UTC"); return v; }
static size_t b_strftime(char *s, size_t n, const char *fmt, const g_tm *g) { struct tm w; from_g(g, &w); return strftime(s, n, fmt, &w); }
static size_t b_strftime_l(char *s, size_t n, const char *fmt, const g_tm *g, void *l) { (void)l; return b_strftime(s, n, fmt, g); }
static char *b_ctime(const int64_t *t) { static __thread char buf[64]; __time64_t v = *t; return _ctime64_s(buf, sizeof(buf), &v) ? NULL : buf; }
static int64_t b_time(int64_t *t) { int64_t v = _time64(NULL); if (t) *t = v; return v; }

static int64_t qpc_ns(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}
static int64_t realtime_ns(void)
{
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (int64_t)(t - 116444736000000000ull) * 100;
}
static int b_clock_gettime(int clk, g_timespec *ts)
{
    int64_t ns;
    switch (clk) {
    case 0: case 5: ns = realtime_ns(); break;                                   /* REALTIME, REALTIME_COARSE */
    case 2: case 3: {                                                            /* PROCESS/THREAD_CPUTIME */
        FILETIME c, e, k, u;
        if (clk == 2) GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u); else GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
        ns = (int64_t)((((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime) + (((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime)) * 100;
        break;
    }
    default: ns = qpc_ns(); break;                                               /* MONOTONIC, RAW, COARSE, BOOTTIME */
    }
    ts->tv_sec = ns / 1000000000; ts->tv_nsec = ns % 1000000000;
    return 0;
}
static int b_gettimeofday(g_timeval *tv, void *tz) { (void)tz; int64_t ns = realtime_ns(); if (tv) { tv->tv_sec = ns / 1000000000; tv->tv_usec = (ns / 1000) % 1000000; } return 0; }
static int64_t b_clock(void) { g_timespec t; b_clock_gettime(2, &t); return t.tv_sec * 1000000 + t.tv_nsec / 1000; }   /* bionic: CLOCKS_PER_SEC 1000000 */

/* Sleeps shorter than Windows' timer tick: a high-resolution waitable timer, else Sleep. */
static void sleep_ns(int64_t ns)
{
    if (ns <= 0) { SwitchToThread(); return; }
    static __thread HANDLE timer;
    if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer) {
        LARGE_INTEGER due; due.QuadPart = -(ns / 100 ? ns / 100 : 1);
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) { WaitForSingleObject(timer, INFINITE); return; }
    }
    Sleep((DWORD)((ns + 999999) / 1000000));
}
static int b_nanosleep(const g_timespec *req, g_timespec *rem) { sleep_ns(req->tv_sec * 1000000000 + req->tv_nsec); if (rem) rem->tv_sec = rem->tv_nsec = 0; return 0; }
static int b_usleep(uint32_t us) { sleep_ns((int64_t)us * 1000); return 0; }

/* ----------------------------------------------------------------- printf */

int tl_va_snprintf_impl(tl_va_frame *f);
int tl_va_snprintf_impl(tl_va_frame *f) { tl_va_list ap; tl_va_start(f, 3, 0, &ap); return tl_format((char *)f->gp[0], (size_t)f->gp[1], (const char *)f->gp[2], &ap); }
int tl_va_sprintf_impl(tl_va_frame *f);
int tl_va_sprintf_impl(tl_va_frame *f) { tl_va_list ap; tl_va_start(f, 2, 0, &ap); return tl_format((char *)f->gp[0], (size_t)1 << 30, (const char *)f->gp[1], &ap); }
int tl_va_sscanf_impl(tl_va_frame *f);
int tl_va_sscanf_impl(tl_va_frame *f) { tl_va_list ap; tl_va_start(f, 2, 0, &ap); return tl_scan_string((const char *)f->gp[0], (const char *)f->gp[1], &ap); }
int tl_va_swprintf_impl(tl_va_frame *f);
int tl_va_swprintf_impl(tl_va_frame *f)
{
    tl_va_list ap; tl_va_start(f, 3, 0, &ap);
    size_t n = (size_t)f->gp[1];
    int r = tl_format_wide((tl_gwchar *)f->gp[0], n, (const tl_gwchar *)f->gp[2], &ap);
    return (size_t)r >= n ? -1 : r;
}
int tl_va_swscanf_impl(tl_va_frame *f);
int tl_va_swscanf_impl(tl_va_frame *f)
{
    tl_va_list ap; tl_va_start(f, 2, 0, &ap);
    char s[1024], fm[256];
    narrow((const gwchar *)f->gp[0], s, sizeof(s)); narrow((const gwchar *)f->gp[1], fm, sizeof(fm));
    return tl_scan_string(s, fm, &ap);
}
TL_VA_STUB(tl_va_snprintf, tl_va_snprintf_impl);
TL_VA_STUB(tl_va_sprintf, tl_va_sprintf_impl);
TL_VA_STUB(tl_va_sscanf, tl_va_sscanf_impl);
TL_VA_STUB(tl_va_swprintf, tl_va_swprintf_impl);
TL_VA_STUB(tl_va_swscanf, tl_va_swscanf_impl);
extern void tl_va_snprintf(void), tl_va_sprintf(void), tl_va_sscanf(void), tl_va_swprintf(void), tl_va_swscanf(void);

static int b_vsnprintf(char *buf, size_t n, const char *fmt, tl_va_list *ap) { tl_va_list c = *ap; return tl_format(buf, n, fmt, &c); }
static int b_vsprintf(char *buf, const char *fmt, tl_va_list *ap) { tl_va_list c = *ap; return tl_format(buf, (size_t)1 << 30, fmt, &c); }
static int b___vsnprintf_chk(char *buf, size_t n, int flags, size_t slen, const char *fmt, tl_va_list *ap) { (void)flags; (void)slen; return b_vsnprintf(buf, n, fmt, ap); }
static int b_vasprintf(char **out, const char *fmt, tl_va_list *ap)
{
    tl_va_list a = *ap, b = *ap;
    int n = tl_format(NULL, 0, fmt, &a);
    *out = tl_malloc((size_t)n + 1);
    if (!*out) return -1;
    tl_format(*out, (size_t)n + 1, fmt, &b);
    return n;
}
static int b_vsscanf(const char *s, const char *fmt, tl_va_list *ap) { tl_va_list c = *ap; return tl_scan_string(s, fmt, &c); }
static int b_vswprintf(tl_gwchar *out, size_t n, const tl_gwchar *fmt, tl_va_list *ap)
{
    tl_va_list c = *ap;
    int r = tl_format_wide(out, n, fmt, &c);
    return (size_t)r >= n ? -1 : r;
}

/* ----------------------------------------------------------------- setjmp */

/* bionic's jmp_buf is 32 longs; only the callee-saved state goes in it. Windows' own setjmp
 * unwinds frames with its exception tables, which the guest's code has none of, so it is not used. */
__asm__(
    ".text\n.p2align 2\n.globl tl_win_setjmp\ntl_win_setjmp:\n"
    "  mov x16, sp\n"
    "  stp x19, x20, [x0, #0]\n  stp x21, x22, [x0, #16]\n  stp x23, x24, [x0, #32]\n  stp x25, x26, [x0, #48]\n"
    "  stp x27, x28, [x0, #64]\n  stp x29, x30, [x0, #80]\n  str x16, [x0, #96]\n"
    "  stp d8, d9, [x0, #104]\n  stp d10, d11, [x0, #120]\n  stp d12, d13, [x0, #136]\n  stp d14, d15, [x0, #152]\n"
    "  mov x0, #0\n  ret\n"
    ".globl tl_win_longjmp\ntl_win_longjmp:\n"
    "  ldp x19, x20, [x0, #0]\n  ldp x21, x22, [x0, #16]\n  ldp x23, x24, [x0, #32]\n  ldp x25, x26, [x0, #48]\n"
    "  ldp x27, x28, [x0, #64]\n  ldp x29, x30, [x0, #80]\n  ldr x16, [x0, #96]\n"
    "  ldp d8, d9, [x0, #104]\n  ldp d10, d11, [x0, #120]\n  ldp d12, d13, [x0, #136]\n  ldp d14, d15, [x0, #152]\n"
    "  mov sp, x16\n"
    "  cmp w1, #0\n  csinc w0, w1, wzr, ne\n  br x30\n");
extern void tl_win_setjmp(void), tl_win_longjmp(void);

/* --------------------------------------------------------------------- math */

static int64_t b_lroundf(float f) { return llroundf(f); }
static void b_sincosf(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }

/* ---------------------------------------------------------------- network */

/* The game reaches the network through Java (the driver answers that). A socket of its own fails. */
static int b_socket(int d, int t, int p) { (void)d; (void)t; (void)p; GERR(G_EAFNOSUPPORT); return -1; }
static int b_sock_fail(void) { GERR(G_EBADF); return -1; }
static int b_getaddrinfo(const char *n, const char *s, const void *h, void **res) { (void)n; (void)s; (void)h; if (res) *res = NULL; return -4; /* EAI_FAIL */ }
static void b_freeaddrinfo(void *a) { (void)a; }
static const char *b_gai_strerror(int e) { (void)e; return "Name resolution is not available"; }
static void *b_gethostbyname(const char *n) { (void)n; return NULL; }
static unsigned b_if_nametoindex(const char *n) { (void)n; return 0; }
static int b_inet_pton(int af, const char *s, void *dst)
{
    if (af != 2) return 0;
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return 0;
    uint8_t *o = dst; o[0] = (uint8_t)a; o[1] = (uint8_t)b; o[2] = (uint8_t)c; o[3] = (uint8_t)d;
    return 1;
}
static const char *b_inet_ntop(int af, const void *src, char *dst, uint32_t n)
{
    if (af != 2) return NULL;
    const uint8_t *i = src;
    snprintf(dst, n, "%u.%u.%u.%u", i[0], i[1], i[2], i[3]);
    return dst;
}
static int b___FD_ISSET_chk(int fd, const void *set, size_t n) { (void)n; return (((const uint64_t *)set)[fd / 64] >> (fd % 64)) & 1; }
static void b___FD_SET_chk(int fd, void *set, size_t n) { (void)n; ((uint64_t *)set)[fd / 64] |= 1ull << (fd % 64); }

/* ------------------------------------------------------------------ tables */

const tl_bionic_entry tl_tab_win_str[] = {
    TL_WRAP("malloc", b_malloc), TL_WRAP("calloc", b_calloc), TL_WRAP("realloc", b_realloc), TL_WRAP("free", b_free),
    TL_WRAP("posix_memalign", b_posix_memalign), TL_WRAP("memalign", b_memalign), TL_WRAP("strdup", b_strdup),
    TL_WRAP("memchr", memchr), TL_WRAP("memcmp", memcmp), TL_WRAP("memcpy", memcpy), TL_WRAP("memmove", memmove),
    TL_WRAP("memset", memset), TL_WRAP("memrchr", b_memrchr), TL_WRAP("strcasecmp", _stricmp), TL_WRAP("strncasecmp", _strnicmp),
    TL_WRAP("strcat", strcat), TL_WRAP("strchr", strchr), TL_WRAP("strcmp", strcmp), TL_WRAP("strcpy", strcpy),
    TL_WRAP("strcspn", strcspn), TL_WRAP("strlcpy", b_strlcpy), TL_WRAP("strlen", strlen), TL_WRAP("strncat", strncat),
    TL_WRAP("strncmp", strncmp), TL_WRAP("strncpy", strncpy), TL_WRAP("strpbrk", strpbrk), TL_WRAP("strrchr", strrchr),
    TL_WRAP("strsep", b_strsep), TL_WRAP("strspn", strspn), TL_WRAP("strstr", strstr), TL_WRAP("strtok", strtok),
    TL_WRAP("strtok_r", b_strtok_r), TL_WRAP("strcoll_l", b_strcoll_l), TL_WRAP("strxfrm_l", b_strxfrm_l),
    TL_WRAP("strerror", b_strerror), TL_WRAP("strerror_r", b_strerror_r),
    TL_WRAP("__memmove_chk", b___memmove_chk), TL_WRAP("__strlen_chk", b___strlen_chk),
    TL_WRAP("isalnum", isalnum), TL_WRAP("isalpha", isalpha), TL_WRAP("isupper", isupper), TL_WRAP("islower", islower),
    TL_WRAP("isdigit", isdigit), TL_WRAP("ispunct", ispunct), TL_WRAP("isgraph", isgraph), TL_WRAP("isblank", isblank), TL_WRAP("iscntrl", iscntrl), TL_WRAP("isprint", isprint),
    TL_WRAP("isspace", isspace), TL_WRAP("isxdigit", isxdigit), TL_WRAP("tolower", tolower), TL_WRAP("toupper", toupper),
    TL_WRAP("isupper_l", b_isupper_l), TL_WRAP("islower_l", b_islower_l), TL_WRAP("isdigit_l", b_isdigit_l),
    TL_WRAP("isxdigit_l", b_isxdigit_l), TL_WRAP("tolower_l", b_tolower_l), TL_WRAP("toupper_l", b_toupper_l),
    TL_WRAP("__ctype_get_mb_cur_max", b___ctype_get_mb_cur_max),
    TL_WRAP("iswalnum", b_iswalnum), TL_WRAP("iswalpha", b_iswalpha), TL_WRAP("iswspace", b_iswspace),
    TL_WRAP("iswalpha_l", b_iswalpha_l), TL_WRAP("iswblank_l", b_iswblank_l), TL_WRAP("iswcntrl_l", b_iswcntrl_l),
    TL_WRAP("iswdigit_l", b_iswdigit_l), TL_WRAP("iswlower_l", b_iswlower_l), TL_WRAP("iswprint_l", b_iswprint_l),
    TL_WRAP("iswpunct_l", b_iswpunct_l), TL_WRAP("iswspace_l", b_iswspace_l), TL_WRAP("iswupper_l", b_iswupper_l),
    TL_WRAP("iswxdigit_l", b_iswxdigit_l), TL_WRAP("towlower", b_towlower), TL_WRAP("towupper", b_towupper),
    TL_WRAP("towlower_l", b_towlower_l), TL_WRAP("towupper_l", b_towupper_l),
    TL_WRAP("wcslen", b_wcslen), TL_WRAP("wcschr", b_wcschr), TL_WRAP("wcscmp", b_wcscmp), TL_WRAP("wcsncmp", b_wcsncmp),
    TL_WRAP("wcscpy", b_wcscpy), TL_WRAP("wcsncpy", b_wcsncpy), TL_WRAP("wcsspn", b_wcsspn), TL_WRAP("wcscoll_l", b_wcscoll_l),
    TL_WRAP("wcsxfrm_l", b_wcsxfrm_l), TL_WRAP("wmemchr", b_wmemchr), TL_WRAP("wmemcmp", b_wmemcmp), TL_WRAP("wmemcpy", b_wmemcpy),
    TL_WRAP("wmemmove", b_wmemmove), TL_WRAP("wmemset", b_wmemset),
    TL_WRAP("mbrtowc", b_mbrtowc), TL_WRAP("mbrlen", b_mbrlen), TL_WRAP("mbtowc", b_mbtowc), TL_WRAP("wcrtomb", b_wcrtomb),
    TL_WRAP("btowc", b_btowc), TL_WRAP("wctob", b_wctob), TL_WRAP("mbsnrtowcs", b_mbsnrtowcs), TL_WRAP("mbsrtowcs", b_mbsrtowcs),
    TL_WRAP("wcsnrtombs", b_wcsnrtombs), TL_WRAP("wcstombs", b_wcstombs),
    TL_WRAP("wcstol", b_wcstol), TL_WRAP("wcstoll", b_wcstoll), TL_WRAP("wcstoul", b_wcstoul), TL_WRAP("wcstoull", b_wcstoull),
    TL_WRAP("wcstod", b_wcstod), TL_WRAP("wcstof", b_wcstof), TL_WRAP("wcstold", b_wcstold),
    TL_WRAP("strtold", b_strtold), TL_WRAP("strtold_l", b_strtold_l),
    TL_WRAP("atof", atof), TL_WRAP("atoi", atoi), TL_WRAP("atol", b_atol), TL_WRAP("strtod", strtod), TL_WRAP("strtof", strtof),
    TL_WRAP("strtol", b_strtol), TL_WRAP("strtoll", strtoll), TL_WRAP("strtoul", b_strtoul), TL_WRAP("strtoull", strtoull),
    TL_WRAP("strtoll_l", b_strtoll_l), TL_WRAP("strtoull_l", b_strtoull_l),
    TL_WRAP("localeconv", b_localeconv), TL_WRAP("newlocale", b_newlocale), TL_WRAP("freelocale", b_freelocale),
    TL_WRAP("uselocale", b_uselocale), TL_WRAP("setlocale", b_setlocale),
    TL_DATA("timezone", &g_timezone),
    TL_WRAP("gmtime_r", b_gmtime_r), TL_WRAP("localtime_r", b_localtime_r), TL_WRAP("gmtime", b_gmtime), TL_WRAP("localtime", b_localtime),
    TL_WRAP("mktime", b_mktime), TL_WRAP("timegm", b_timegm), TL_WRAP("strftime", b_strftime), TL_WRAP("strftime_l", b_strftime_l),
    TL_WRAP("ctime", b_ctime), TL_WRAP("time", b_time), TL_WRAP("clock_gettime", b_clock_gettime), TL_WRAP("gettimeofday", b_gettimeofday),
    TL_WRAP("clock", b_clock), TL_WRAP("nanosleep", b_nanosleep), TL_WRAP("usleep", b_usleep),
    TL_WRAP("snprintf", tl_va_snprintf), TL_WRAP("sprintf", tl_va_sprintf), TL_WRAP("sscanf", tl_va_sscanf),
    TL_WRAP("swprintf", tl_va_swprintf), TL_WRAP("swscanf", tl_va_swscanf), TL_WRAP("vsnprintf", b_vsnprintf),
    TL_WRAP("vsprintf", b_vsprintf), TL_WRAP("__vsnprintf_chk", b___vsnprintf_chk), TL_WRAP("vasprintf", b_vasprintf),
    TL_WRAP("vsscanf", b_vsscanf), TL_WRAP("vswprintf", b_vswprintf),
    TL_WRAP("setjmp", tl_win_setjmp), TL_WRAP("sigsetjmp", tl_win_setjmp), TL_WRAP("_setjmp", tl_win_setjmp),
    TL_WRAP("longjmp", tl_win_longjmp), TL_WRAP("siglongjmp", tl_win_longjmp), TL_WRAP("_longjmp", tl_win_longjmp),
    TL_WRAP("qsort", qsort),
    TL_WRAP("socket", b_socket), TL_WRAP("socketpair", b_sock_fail), TL_WRAP("accept", b_sock_fail), TL_WRAP("bind", b_sock_fail),
    TL_WRAP("connect", b_sock_fail), TL_WRAP("listen", b_sock_fail), TL_WRAP("recv", b_sock_fail), TL_WRAP("send", b_sock_fail),
    TL_WRAP("sendto", b_sock_fail), TL_WRAP("shutdown", b_sock_fail), TL_WRAP("getpeername", b_sock_fail),
    TL_WRAP("getsockname", b_sock_fail), TL_WRAP("getsockopt", b_sock_fail), TL_WRAP("setsockopt", b_sock_fail),
    TL_WRAP("getnameinfo", b_sock_fail), TL_WRAP("getaddrinfo", b_getaddrinfo), TL_WRAP("freeaddrinfo", b_freeaddrinfo),
    TL_WRAP("gai_strerror", b_gai_strerror), TL_WRAP("gethostbyname", b_gethostbyname), TL_WRAP("if_nametoindex", b_if_nametoindex),
    TL_WRAP("inet_pton", b_inet_pton), TL_WRAP("inet_ntop", b_inet_ntop),
    TL_WRAP("__FD_ISSET_chk", b___FD_ISSET_chk), TL_WRAP("__FD_SET_chk", b___FD_SET_chk),
    TL_END
};

const tl_bionic_entry tl_tab_win_math[] = {
    TL_WRAP("acos", acos), TL_WRAP("acosf", acosf), TL_WRAP("asin", asin), TL_WRAP("asinf", asinf), TL_WRAP("atan2", atan2),
    TL_WRAP("atan2f", atan2f), TL_WRAP("atanf", atanf), TL_WRAP("cos", cos), TL_WRAP("cosf", cosf), TL_WRAP("exp", exp),
    TL_WRAP("fmod", fmod), TL_WRAP("fmodf", fmodf), TL_WRAP("log", log), TL_WRAP("log10", log10), TL_WRAP("log10f", log10f),
    TL_WRAP("logf", logf), TL_WRAP("lroundf", b_lroundf), TL_WRAP("modf", modf), TL_WRAP("modff", modff), TL_WRAP("pow", pow),
    TL_WRAP("powf", powf), TL_WRAP("sin", sin), TL_WRAP("sincosf", b_sincosf), TL_WRAP("sinf", sinf), TL_WRAP("sqrt", sqrt),
    TL_WRAP("sqrtf", sqrtf), TL_WRAP("tanf", tanf),
    TL_END
};
