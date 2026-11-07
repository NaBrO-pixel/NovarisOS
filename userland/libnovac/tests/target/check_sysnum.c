/* Build-time check (host headers): every syscall number libNovaC uses must
 * equal the kernel's definition in include/syscall64.h. */
#include "syscall64.h"
#include "../../src/arch/x86_64/sysnum.h"

#define SAME(name, kname) _Static_assert(NOVAC_SYS_##name == kname, #name " differs from the kernel")
SAME(read, SYS64_READ);           SAME(write, SYS64_WRITE);
SAME(open, SYS64_OPEN);           SAME(close, SYS64_CLOSE);
SAME(stat, SYS64_STAT);           SAME(fstat, SYS64_FSTAT);
SAME(lstat, SYS64_LSTAT);         SAME(lseek, SYS64_LSEEK);
SAME(mmap, SYS64_MMAP);           SAME(munmap, SYS64_MUNMAP);
SAME(access, SYS64_ACCESS);       SAME(dup, SYS64_DUP);
SAME(dup2, SYS64_DUP2);           SAME(nanosleep, SYS64_NANOSLEEP);
SAME(getpid, SYS64_GETPID);       SAME(ftruncate, SYS64_FTRUNCATE);
SAME(getcwd, SYS64_GETCWD);       SAME(chdir, SYS64_CHDIR);
SAME(rename, SYS64_RENAME);       SAME(mkdir, SYS64_MKDIR);
SAME(rmdir, SYS64_RMDIR);         SAME(unlink, SYS64_UNLINK);
SAME(gettimeofday, SYS64_GETTIMEOFDAY);
SAME(clock_gettime, SYS64_CLOCK_GETTIME);
SAME(exit_group, SYS64_EXIT_GROUP);
SAME(pipe2, SYS64_PIPE2);         SAME(fork, SYS64_FORK);
SAME(wait4, SYS64_WAIT4);         SAME(getppid, SYS64_GETPPID);
SAME(getdents64, SYS64_GETDENTS64);
SAME(rt_sigaction, SYS64_RT_SIGACTION); SAME(rt_sigprocmask, SYS64_RT_SIGPROCMASK);
SAME(rt_sigreturn, SYS64_RT_SIGRETURN); SAME(execve, SYS64_EXECVE);
SAME(kill, SYS64_KILL);           SAME(gettid, SYS64_GETTID);
SAME(tgkill, SYS64_TGKILL);
int novac_sysnum_checked;
