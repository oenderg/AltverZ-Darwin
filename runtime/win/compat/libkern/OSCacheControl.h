/* Windows: the Darwin call that makes written code visible to instruction fetch. */
#pragma once
#include <stddef.h>
void tl_win_flush_icache(void *addr, size_t len);
#define sys_icache_invalidate(a, n) tl_win_flush_icache((void *)(a), (n))
#define sys_dcache_flush(a, n) ((void)(a), (void)(n))
