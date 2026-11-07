/* POSIX-style file and process calls: thin, checked wrappers over the Linux
 * x86-64 system calls NovarisOS implements. Errors: -1 with errno set. */
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/wait.h>
#include "../internal/sys.h"
#include "../internal/syscall.h"

_Static_assert(sizeof(struct stat) == 144, "struct stat must match the Linux x86-64 layout");

ssize_t read(int fd, void *buf, size_t n)
{
    if (n > (size_t)LONG_MAX) { errno = EINVAL; return -1; }
    return novac_sys3(NOVAC_SYS_read, fd, buf, n);
}

ssize_t write(int fd, const void *buf, size_t n)
{
    if (n > (size_t)LONG_MAX) { errno = EINVAL; return -1; }
    return novac_sys3(NOVAC_SYS_write, fd, buf, n);
}

int open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    return (int)novac_sys3(NOVAC_SYS_open, path, flags, mode);
}

int     close(int fd)                    { return (int)novac_sys1(NOVAC_SYS_close, fd); }
off_t   lseek(int fd, off_t off, int w)  { return novac_sys3(NOVAC_SYS_lseek, fd, off, w); }
int     unlink(const char *p)            { return (int)novac_sys1(NOVAC_SYS_unlink, p); }
int     rmdir(const char *p)             { return (int)novac_sys1(NOVAC_SYS_rmdir, p); }
int     access(const char *p, int m)     { return (int)novac_sys2(NOVAC_SYS_access, p, m); }
int     chdir(const char *p)             { return (int)novac_sys1(NOVAC_SYS_chdir, p); }
int     dup(int fd)                      { return (int)novac_sys1(NOVAC_SYS_dup, fd); }
int     dup2(int a, int b)               { return (int)novac_sys2(NOVAC_SYS_dup2, a, b); }
int     ftruncate(int fd, off_t len)     { return (int)novac_sys2(NOVAC_SYS_ftruncate, fd, len); }
pid_t   getpid(void)                     { return (pid_t)novac_syscall6(NOVAC_SYS_getpid, 0, 0, 0, 0, 0, 0); }
pid_t   getppid(void)                    { return (pid_t)novac_syscall6(NOVAC_SYS_getppid, 0, 0, 0, 0, 0, 0); }
int     pipe2(int fds[2], int flags)     { return (int)novac_sys2(NOVAC_SYS_pipe2, fds, flags); }
int     pipe(int fds[2])                 { return pipe2(fds, 0); }
pid_t   fork(void)                       { return (pid_t)novac_ret(novac_syscall6(NOVAC_SYS_fork, 0, 0, 0, 0, 0, 0)); }
int     stat(const char *p, struct stat *s)  { return (int)novac_sys2(NOVAC_SYS_stat, p, s); }
int     lstat(const char *p, struct stat *s) { return (int)novac_sys2(NOVAC_SYS_lstat, p, s); }
int     fstat(int fd, struct stat *s)    { return (int)novac_sys2(NOVAC_SYS_fstat, fd, s); }
int     mkdir(const char *p, mode_t m)   { return (int)novac_sys2(NOVAC_SYS_mkdir, p, m); }
int     rename(const char *a, const char *b) { return (int)novac_sys2(NOVAC_SYS_rename, a, b); }

char *getcwd(char *buf, size_t size)
{
    long r;
    if (!buf || size == 0) { errno = EINVAL; return NULL; }
    r = novac_sys2(NOVAC_SYS_getcwd, buf, size);   /* returns length incl. NUL */
    return r < 0 ? NULL : buf;
}

unsigned int sleep(unsigned int seconds)
{
    struct timespec req = { (time_t)seconds, 0 }, rem = { 0, 0 };
    if (nanosleep(&req, &rem) == 0) return 0;
    return errno == EINTR ? (unsigned int)rem.tv_sec : seconds;
}

_Noreturn void _exit(int status) { novac_sys_exit(status); }

char *getenv(const char *name)
{
    size_t n;
    char **e;
    if (!environ || !name || !*name) return NULL;
    for (n = 0; name[n]; n++)
        if (name[n] == '=') return NULL;
    for (e = environ; *e; e++) {
        size_t i = 0;
        while (i < n && (*e)[i] == name[i]) i++;
        if (i == n && (*e)[n] == '=') return *e + n + 1;
    }
    return NULL;
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    return (pid_t)novac_ret(novac_syscall6(NOVAC_SYS_wait4, pid, (long)status, options, 0, 0, 0));
}

pid_t wait(int *status) { return waitpid(-1, status, 0); }
