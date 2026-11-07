#ifndef NOVAC_SYS_WAIT_H
#define NOVAC_SYS_WAIT_H
#include <sys/types.h>
#include <novac/defs.h>

#define WNOHANG 1

/* Linux wait-status encoding. */
#define WIFEXITED(s)   (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) ((((s) & 0x7f) + 1) >> 1 > 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s)    ((s) & 0x7f)

NOVAC_BEGIN_DECLS
pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);
NOVAC_END_DECLS
#endif
