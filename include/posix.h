#ifndef POSIX_H
#define POSIX_H

#include <stdint.h>
#include "idt.h"

/* posix.h - the Linux/i386 system call ABI.
 *
 * Milestone 18, and the first instalment of item 3 on the Path A list
 * (ROADMAP.md): the POSIX-shaped syscall surface Wine's own build links
 * against. Wine is not a Windows program - it is a *Unix* program that
 * happens to implement Windows. Cross-compiling it means giving it a
 * kernel it recognises underneath, and that means these numbers, these
 * argument registers and these errno values, not a bespoke set.
 *
 * So this replaces the three ad-hoc calls Milestone 5 defined
 * (SYS_EXIT=0, SYS_WRITE=1, SYS_MMAP=2) with the real thing. The two
 * numberings cannot coexist - Linux's exit is 1, which was Novaris's
 * write - so the handful of demo programs that used the old numbers were
 * migrated rather than bridged.
 *
 * The ABI, which is Linux's and not ours to choose:
 *
 *   eax  = syscall number
 *   ebx, ecx, edx, esi, edi, ebp = arguments 1..6
 *   eax  = return value; errors come back as -errno, in range [-4095, -1]
 *
 * What this buys, concretely: a program compiled with an ordinary
 * `gcc -m32 -static -nostdlib` and hand-written `int $0x80` stubs - a
 * program written for Linux, with nothing in it that knows Novaris
 * exists - runs unmodified here. userland/posix_test.c is exactly that,
 * and the same binary is run on the Linux build host and on Novaris and
 * the two transcripts compared. */

/* --- syscall numbers (Linux i386) --------------------------------------
 * Only the ones implemented or deliberately stubbed are named here; the
 * dispatcher reports anything else by number. */
#define SYS_exit             1
#define SYS_read             3
#define SYS_write            4
#define SYS_open             5
#define SYS_close            6
#define SYS_waitpid          7
#define SYS_unlink          10
#define SYS_time            13
#define SYS_lseek           19
#define SYS_getpid          20
#define SYS_access          33
#define SYS_kill            37
#define SYS_brk             45
#define SYS_ioctl           54
#define SYS_munmap          91
#define SYS_uname          122
#define SYS_mprotect       125
#define SYS_writev         146
#define SYS_nanosleep      162
#define SYS_rt_sigaction   174
#define SYS_rt_sigprocmask 175
#define SYS_getcwd         183
#define SYS_mmap2          192
#define SYS_stat64         195
#define SYS_fstat64        197
#define SYS_getuid32       199
#define SYS_getgid32       200
#define SYS_geteuid32      201
#define SYS_getegid32      202
#define SYS_gettid         224
#define SYS_exit_group     252
#define SYS_set_thread_area 243
#define SYS_clock_gettime  265

/* --- errno values, negated on return ------------------------------------ */
#define EPERM    1
#define ENOENT   2
#define EBADF    9
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EEXIST  17
#define EINVAL  22
#define EMFILE  24
#define ENOSPC  28
#define ESPIPE  29
#define EROFS   30
#define ENOSYS  38

/* --- mmap / mprotect flags, as Linux defines them ----------------------- */
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

/* --- open flags --------------------------------------------------------- */
#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_RDWR     0x0002
#define O_CREAT    0x0040
#define O_TRUNC    0x0200
#define O_APPEND   0x0400

/* --- lseek whence ------------------------------------------------------- */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Handles one `int 0x80`. Called from kernel/syscall.c, which owns the
 * IDT registration; split out so the ABI and its table live in one file
 * rather than growing inside the interrupt plumbing. */
void posix_syscall(registers_t* regs);

/* Per-process state: the file descriptor table and the mmap/brk
 * bookkeeping. Called when a program starts and when it exits, because
 * descriptors and mappings belong to the process, not to the kernel. */
void posix_process_begin(void);
void posix_process_end(void);

/* How many syscalls this program made that Novaris does not implement,
 * for the same reason the Win32 layer counts missing APIs: a program that
 * quietly got -ENOSYS somewhere is worth telling the user about. */
uint32_t posix_unimplemented_count(void);

#endif
