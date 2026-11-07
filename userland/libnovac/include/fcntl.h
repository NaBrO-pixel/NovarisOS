#ifndef NOVAC_FCNTL_H
#define NOVAC_FCNTL_H
#include <sys/types.h>
#include <novac/defs.h>

/* Linux x86-64 values. */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_ACCMODE   3
#define O_CREAT     0100
#define O_EXCL      0200
#define O_TRUNC     01000
#define O_APPEND    02000
#define O_NONBLOCK  04000
#define O_DIRECTORY 0200000
#define O_CLOEXEC   02000000

NOVAC_BEGIN_DECLS
/* open(path, flags) or open(path, flags, mode); mode is read only with O_CREAT. */
int open(const char *path, int flags, ...);
NOVAC_END_DECLS
#endif
