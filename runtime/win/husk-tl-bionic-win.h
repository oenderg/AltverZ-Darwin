/* SPDX-License-Identifier: GPL-2.0-or-later */
/* What the Windows bionic files share. */
#ifndef HUSK_TL_BIONIC_WIN_H
#define HUSK_TL_BIONIC_WIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "husk-tl-bionic.h"

/* Linux errno values, as the guest sees them. */
enum { G_EPERM = 1, G_ENOENT = 2, G_EINTR = 4, G_EIO = 5, G_EBADF = 9, G_ECHILD = 10, G_EAGAIN = 11, G_ENOMEM = 12,
       G_EACCES = 13, G_EFAULT = 14, G_EBUSY = 16, G_EEXIST = 17, G_ENOTDIR = 20, G_EISDIR = 21, G_EINVAL = 22,
       G_EMFILE = 24, G_ENOSPC = 28, G_ESPIPE = 29, G_EROFS = 30, G_ERANGE = 34, G_EDEADLK = 35,
       G_ENAMETOOLONG = 36, G_ENOSYS = 38, G_ENOTEMPTY = 39, G_ETIMEDOUT = 110, G_ENOTSUP = 95, G_EAFNOSUPPORT = 97 };

void tl_win_log_open(const char *path);
void tl_win_mkdirs(const char *path);
const char *tl_data_dir(void);
const char *tl_path_resolve(const char *path, char *buf, size_t n);
void tl_set_data_dir(const char *dir);
void tl_win_set_errno_from_crt(void);
void tl_win_set_errno_from_win32(unsigned long err);
long long tl_linux_syscall(long long a0, long long a1, long long a2, long long a3, long long a4, long long a5, long long nr);

/* Guest time types: Android's 64-bit long. */
typedef struct { int64_t tv_sec, tv_nsec; } g_timespec;
typedef struct { int64_t tv_sec, tv_usec; } g_timeval;

#endif
