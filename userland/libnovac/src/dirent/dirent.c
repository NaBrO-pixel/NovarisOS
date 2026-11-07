/* opendir/readdir/closedir over getdents64. */
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <errno.h>
#include "../internal/syscall.h"

_Static_assert(offsetof(struct dirent, d_name) == 19, "dirent must match the Linux getdents64 layout");

struct novac_dir {
    int fd;
    size_t pos, len;
    long eof;
    char buf[1024] __attribute__((aligned(8)));
};

DIR *opendir(const char *path)
{
    DIR *d;
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return NULL;
    d = calloc(1, sizeof *d);
    if (!d) { int e = errno; close(fd); errno = e; return NULL; }
    d->fd = fd;
    return d;
}

struct dirent *readdir(DIR *d)
{
    struct dirent *e;
    if (d->pos >= d->len) {
        long n;
        if (d->eof) return NULL;
        n = novac_sys3(NOVAC_SYS_getdents64, d->fd, d->buf, sizeof d->buf);
        if (n < 0) return NULL;                 /* errno set by the wrapper */
        if (n == 0) { d->eof = 1; return NULL; }
        d->pos = 0;
        d->len = (size_t)n;
    }
    e = (struct dirent *)(d->buf + d->pos);
    if (e->d_reclen == 0 || d->pos + e->d_reclen > d->len) { errno = EIO; return NULL; }
    d->pos += e->d_reclen;
    return e;
}

int closedir(DIR *d)
{
    int rc;
    if (!d) { errno = EINVAL; return -1; }
    rc = close(d->fd);
    free(d);
    return rc;
}
