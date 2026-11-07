/* Linux x86-64 system-call numbers used by libNovaC. Copied from the
 * kernel's include/syscall64.h; tests/target/check_sysnum.c compares every
 * one against that header at build time so they cannot drift. */
#ifndef NOVAC_ARCH_X86_64_SYSNUM_H
#define NOVAC_ARCH_X86_64_SYSNUM_H

#define NOVAC_SYS_read          0
#define NOVAC_SYS_write         1
#define NOVAC_SYS_open          2
#define NOVAC_SYS_close         3
#define NOVAC_SYS_stat          4
#define NOVAC_SYS_fstat         5
#define NOVAC_SYS_lstat         6
#define NOVAC_SYS_lseek         8
#define NOVAC_SYS_mmap          9
#define NOVAC_SYS_munmap        11
#define NOVAC_SYS_access        21
#define NOVAC_SYS_pipe2         293
#define NOVAC_SYS_fork          57
#define NOVAC_SYS_wait4         61
#define NOVAC_SYS_getppid       110
#define NOVAC_SYS_getdents64    217
#define NOVAC_SYS_dup           32
#define NOVAC_SYS_dup2          33
#define NOVAC_SYS_nanosleep     35
#define NOVAC_SYS_getpid        39
#define NOVAC_SYS_ftruncate     77
#define NOVAC_SYS_getcwd        79
#define NOVAC_SYS_chdir         80
#define NOVAC_SYS_rename        82
#define NOVAC_SYS_mkdir         83
#define NOVAC_SYS_rmdir         84
#define NOVAC_SYS_unlink        87
#define NOVAC_SYS_gettimeofday  96
#define NOVAC_SYS_clock_gettime 228
#define NOVAC_SYS_exit_group    231

#endif
