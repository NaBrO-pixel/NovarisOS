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
#include <stdlib.h>
#include <string.h>

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

/* --- running another program ------------------------------------------- */

int execve(const char *path, char *const argv[], char *const envp[])
{
    return (int)novac_sys3(NOVAC_SYS_execve, path, argv, envp);
}

int execv(const char *path, char *const argv[])
{
    return execve(path, argv, environ);
}

/* A name with a '/' is a path; any other is looked for in each directory
 * of PATH in turn, as the shell would (default "/bin:/usr/bin"). ENOENT
 * and ENOTDIR move on to the next directory; any other error is
 * remembered - EACCES above all - and is what the call fails with if
 * nothing runs. */
int execvp(const char *file, char *const argv[])
{
    const char *path, *p;
    char buf[PATH_MAX];
    int saw_eacces = 0, last = ENOENT;

    if (!file || !*file) { errno = ENOENT; return -1; }
    if (strchr(file, '/')) return execv(file, argv);

    path = getenv("PATH");
    if (!path) path = "/bin:/usr/bin";
    for (p = path; ; ) {
        const char *end = strchr(p, ':');
        size_t dlen = end ? (size_t)(end - p) : strlen(p);
        size_t flen = strlen(file);
        if (dlen + 1 + flen + 1 <= sizeof buf) {
            size_t n = 0;
            if (dlen == 0) buf[n++] = '.';      /* an empty entry is the cwd */
            else { memcpy(buf, p, dlen); n = dlen; }
            buf[n++] = '/';
            memcpy(buf + n, file, flen + 1);
            execve(buf, argv, environ);
            if (errno == EACCES) saw_eacces = 1;
            else if (errno != ENOENT && errno != ENOTDIR) last = errno;
        }
        if (!end) break;
        p = end + 1;
    }
    errno = saw_eacces ? EACCES : last;
    return -1;
}

/* --- the environment ----------------------------------------------------- *
 *
 * environ starts as the array the kernel left on the stack. The first
 * change copies it into memory of our own, which then grows as needed. A
 * string replaced or removed is not freed - it may be one of the
 * kernel's, or one a caller still holds - which is the usual trade. */
static char **owned_env;
static size_t owned_cap;

static int env_name_ok(const char *name)
{
    if (!name || !*name) return 0;
    for (; *name; name++) if (*name == '=') return 0;
    return 1;
}

static long env_find(const char *name, size_t n)
{
    long i;
    if (!environ) return -1;
    for (i = 0; environ[i]; i++)
        if (strncmp(environ[i], name, n) == 0 && environ[i][n] == '=') return i;
    return -1;
}

static size_t env_count(void)
{
    size_t c = 0;
    if (environ) while (environ[c]) c++;
    return c;
}

static int env_own(size_t need)
{
    size_t count = env_count(), cap, i;
    char **fresh;
    if (environ == owned_env && need + 1 <= owned_cap) return 0;
    cap = owned_cap ? owned_cap : 16;
    while (cap < need + 1) cap *= 2;
    fresh = malloc(cap * sizeof *fresh);
    if (!fresh) { errno = ENOMEM; return -1; }
    for (i = 0; i < count; i++) fresh[i] = environ[i];
    fresh[count] = NULL;
    if (environ == owned_env) free(owned_env);
    owned_env = environ = fresh;
    owned_cap = cap;
    return 0;
}

int setenv(const char *name, const char *value, int overwrite)
{
    size_t n, v;
    long at;
    char *entry;

    if (!env_name_ok(name)) { errno = EINVAL; return -1; }
    if (!value) value = "";
    n = strlen(name);
    at = env_find(name, n);
    if (at >= 0 && !overwrite) return 0;
    v = strlen(value);
    entry = malloc(n + 1 + v + 1);
    if (!entry) { errno = ENOMEM; return -1; }
    memcpy(entry, name, n);
    entry[n] = '=';
    memcpy(entry + n + 1, value, v + 1);
    if (env_own(env_count() + (at < 0 ? 1 : 0)) < 0) { free(entry); return -1; }
    if (at >= 0) {
        environ[at] = entry;
    } else {
        size_t c = env_count();
        environ[c] = entry;
        environ[c + 1] = NULL;
    }
    return 0;
}

int unsetenv(const char *name)
{
    size_t n;
    long at;
    if (!env_name_ok(name)) { errno = EINVAL; return -1; }
    n = strlen(name);
    while ((at = env_find(name, n)) >= 0) {
        if (env_own(env_count()) < 0) return -1;
        for (; environ[at]; at++) environ[at] = environ[at + 1];
    }
    return 0;
}
