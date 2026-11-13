/* Windows: statfs for the data folder's drive (tl-win-compat.c). */
#pragma once
#include <stdint.h>
struct statfs { uint64_t f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree; };
int statfs(const char *path, struct statfs *out);
