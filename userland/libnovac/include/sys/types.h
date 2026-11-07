/* Types match the Linux x86-64 (LP64) ABI that NovarisOS implements. */
#ifndef NOVAC_SYS_TYPES_H
#define NOVAC_SYS_TYPES_H
#include <stddef.h>

#if !defined(__x86_64__) || __SIZEOF_LONG__ != 8
# error "libNovaC file/time types currently support x86-64 (LP64) only"
#endif

typedef long           ssize_t;
typedef long           off_t;
typedef int            pid_t;
typedef unsigned int   mode_t;
typedef unsigned int   uid_t;
typedef unsigned int   gid_t;
typedef unsigned long  dev_t;
typedef unsigned long  ino_t;
typedef unsigned long  nlink_t;
typedef long           blksize_t;
typedef long           blkcnt_t;
typedef long           time_t;
#endif
