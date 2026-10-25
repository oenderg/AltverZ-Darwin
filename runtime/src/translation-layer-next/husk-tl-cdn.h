/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HUSK_TL_CDN_H
#define HUSK_TL_CDN_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct tl_cdn_response {
    unsigned refs;
    int status;
    unsigned char *body;
    size_t length;
    void *headers;
} tl_cdn_response;

/* Completion owns the response, and must queue it before calling any guest code. */
typedef void (*tl_cdn_completion)(uint64_t token, tl_cdn_response *response);
bool tl_cdn_allowed_url(const char *url);
void tl_cdn_fetch(const char *url, const char *method, const void *body, size_t length,
                  const char *cache_path, uint64_t token, tl_cdn_completion completion);
char *tl_cdn_header(const tl_cdn_response *response, const char *key);
void tl_cdn_response_retain(tl_cdn_response *response);
void tl_cdn_response_free(tl_cdn_response *response);
void tl_cdn_stage(const char *bundled_path, const char *cache_path);
#endif
