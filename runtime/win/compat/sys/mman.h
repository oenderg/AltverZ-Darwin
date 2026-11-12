/* Windows: mmap and friends over VirtualAlloc and file mappings (tl-win-compat.c). */
#pragma once
#include <stddef.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANON 0x20
#define MAP_ANONYMOUS MAP_ANON
#define MAP_NORESERVE 0x4000
#define MAP_JIT 0
#define MAP_FAILED ((void *)-1)
#define MS_ASYNC 1
#define MS_SYNC 4
#define MS_INVALIDATE 2
#define MADV_NORMAL 0
#define MADV_RANDOM 1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4
#define MADV_FREE 8
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long long off);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);
int msync(void *addr, size_t len, int flags);
int madvise(void *addr, size_t len, int advice);
int mlock(const void *addr, size_t len);
int munlock(const void *addr, size_t len);
#ifdef __cplusplus
}
#endif
