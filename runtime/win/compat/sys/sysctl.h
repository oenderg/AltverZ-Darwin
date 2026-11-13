/* Windows: sysctlbyname for the names the runtime asks (hw.memsize, hw.ncpu). */
#pragma once
#include <stddef.h>
int sysctlbyname(const char *name, void *out, size_t *len, void *newp, size_t newlen);
