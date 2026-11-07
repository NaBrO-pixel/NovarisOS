#ifndef NOVAC_UNISTD_H
#define NOVAC_UNISTD_H
#include <stddef.h>
#include <sys/types.h>
#include <novac/defs.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

extern char **environ;

NOVAC_BEGIN_DECLS
ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int     close(int fd);
off_t   lseek(int fd, off_t off, int whence);
int     unlink(const char *path);
int     rmdir(const char *path);
int     access(const char *path, int mode);
int     chdir(const char *path);
char   *getcwd(char *buf, size_t size);
int     dup(int fd);
int     dup2(int from, int to);
int     ftruncate(int fd, off_t length);
pid_t   getpid(void);
pid_t   getppid(void);
int     pipe(int fds[2]);
int     pipe2(int fds[2], int flags);
/* Buffered stdio data is duplicated into the child: fflush(NULL) first, and
 * leave the child with _exit(), not exit(). */
pid_t   fork(void);
unsigned int sleep(unsigned int seconds);
_Noreturn void _exit(int status);
int     execve(const char *path, char *const argv[], char *const envp[]);
int     execv(const char *path, char *const argv[]);
int     execvp(const char *file, char *const argv[]);
NOVAC_END_DECLS
#endif
