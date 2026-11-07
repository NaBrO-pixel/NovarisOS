/* Typed wrappers over the raw system call, shared by the file, time and
 * process modules. Translates the kernel's -errno returns into the C
 * convention (-1 and errno) in exactly one place. */
#ifndef NOVAC_INTERNAL_SYSCALL_H
#define NOVAC_INTERNAL_SYSCALL_H
#include <errno.h>
#include "../arch/x86_64/syscall.h"
#include "../arch/x86_64/sysnum.h"

#define NOVAC_MAX_ERRNO 4095L

static inline long novac_ret(long r)
{
    if (r < 0 && r >= -NOVAC_MAX_ERRNO) { errno = (int)-r; return -1; }
    return r;
}

#define novac_sys1(n, a)       novac_ret(novac_syscall6((n), (long)(a), 0, 0, 0, 0, 0))
#define novac_sys2(n, a, b)    novac_ret(novac_syscall6((n), (long)(a), (long)(b), 0, 0, 0, 0))
#define novac_sys3(n, a, b, c) novac_ret(novac_syscall6((n), (long)(a), (long)(b), (long)(c), 0, 0, 0))

#endif
