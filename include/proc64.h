#ifndef PROC64_H
#define PROC64_H

#include <stdint.h>
#include "vmspace64.h"

/* Processes.
 *
 * Everything before Milestone 64 had exactly one of these and did not
 * say so: the open files lived in a static array in syscall64.c, the
 * heap and mmap bookkeeping in another in uspace64.c, and the address
 * space was whatever the kernel had most recently switched to. That is
 * fine while there is one program and wrong the moment there are two -
 * a forked child that shares its parent's file descriptor table is not
 * a child, it is the same process with two register sets.
 *
 * So this owns the per-process state, and the syscall layer reaches it
 * through proc64_current(). The scheduler records which process each
 * task belongs to and switches both together.
 */

/* Four was enough while the only thing that forked was a test. Building
 * a Wine prefix runs wineserver, wineboot, services.exe and explorer.exe
 * at once, and the fifth process to start is the one that fails. */
/* Every process in the system.
 *
 * It was 32, and unlike the pipe table it was not obviously below the
 * work: a host prefix run peaks at 22 concurrent processes. Measured on
 * the guest, though, the table fills - and the report says what fills
 * it, which is the part that decided this. All 32 slots are *running*
 * processes and none is an unreaped zombie, so this is a table that is
 * too small rather than one that is leaking. That was worth checking:
 * a zombie leak looks identical from outside and raising the number
 * would have hidden it.
 *
 * Bounded by SCHED64_MAX_TASKS at 128, because a process needs at least
 * one task to run and a process table larger than the task table has
 * slots nothing can ever occupy. 128 slots of proc64_t - which is 12KB
 * apiece, nearly all of it the 256-entry descriptor table - is 1.5MB of
 * bss.
 *
 * What running out looks like: clone(2) returns -EAGAIN, which is what
 * Linux returns too, and Wine reports it as NtCreateUserProcess failing
 * to start explorer.exe. */
#define PROC64_MAX     128

/* 32 was the number a program that opens a few files needs, and the
 * wineserver is not that program. It holds a descriptor per client
 * thread - the socket, the reply pipe and the wait pipe, three each -
 * on top of every file the loader has open, and it holds them all at
 * once.
 *
 * Measured rather than guessed, the same way ramfs64's ceilings were:
 * `wineboot -u` was run to completion on Linux under strace and the
 * concurrent descriptors counted per process. The wineserver peaks at
 * 135; nothing else passes 6. At 32 the guest did not survive that -
 * openat returned -EMFILE and the process died with 0xc000011f,
 * STATUS_TOO_MANY_OPENED_FILES, which is Wine faithfully reporting the
 * limit it was given.
 *
 * 256 is the next power of two above the measurement, and it costs
 * 40 bytes a descriptor - 10KB per process, 320KB across all 32.
 *
 * It is also poll's bound: the loop below rejects nfds above
 * FD_MAX * 2, and a server polling 135 descriptors was being told
 * -EINVAL by a kernel that had sized the check for a smaller machine. */
#define PROC64_FD_MAX  256

/* 128 was under the 170 characters a real prefix's deepest path needs -
 * measured, see ramfs64.h. */
#define PROC64_PATH_MAX 1024

/* What a descriptor refers to. Until Milestone 74 there was only one
 * answer and it did not need saying. */
#define FD64_FILE   0
#define FD64_PIPE   1
#define FD64_SOCKET 2

typedef struct {
    int      kind;      /* FD64_FILE or FD64_PIPE */
    int      node;      /* FD64_FILE: the ramfs node */
    uint64_t pos;

    /* FD64_PIPE: which pipe this descriptor may read from and which it
     * may write to, -1 for "not this way". A pipe(2) read end has only
     * rx, its write end only tx, and a socketpair endpoint has both -
     * pointing at the two pipes the other endpoint has crossed over.
     * That is the whole of what makes a socketpair bidirectional. */
    int      rx, tx;

    /* FD64_SOCKET: which socket in sock64.c, or -1. A socket keeps this
     * after it is connected, because shutdown, getsockname and
     * setsockopt are still asked about it - but once connected it also
     * has rx and tx, and from read(2)'s point of view it is a pipe like
     * any other. */
    int      sock;

    /* O_NONBLOCK and O_CLOEXEC, as fcntl(2) sets and reads them. Kept
     * per descriptor rather than per pipe because they are: two
     * descriptors on the same pipe can disagree about blocking, and
     * Wine's do. */
    int      nonblock;
    int      cloexec;

    int      used;
} proc64_fd_t;

typedef struct {
    int         used;
    int         pid;
    int         parent;        /* pid of the process that forked it   */
    int         exited;
    int         exit_status;

    /* The pid of a parent suspended in vfork waiting for this process,
     * or 0. glibc's posix_spawn is clone(CLONE_VM|CLONE_VFORK) and Wine
     * reaches the wineserver through it, so "the parent does not run
     * until the child has become something else" is not an optimisation
     * here - it is the difference between starting a server and freeing
     * the stack the child is standing on. Cleared by whichever of execve
     * or exit happens first. */
    int         vfork_parent;

    vmspace64_t space;

    /* The heap and the mmap bump pointer: per process, because a fork
     * gives the child its own copy of both. */
    uint64_t    brk_base, brk_current, mmap_next;

    proc64_fd_t fds[PROC64_FD_MAX];
    char        exe_path[PROC64_PATH_MAX];

    /* The working directory, as text rather than as a node index.
     *
     * A node index would go stale: the directory a process sits in can
     * be removed underneath it, and Linux lets that happen - the process
     * keeps its cwd and every relative path from it fails. Text also
     * means getcwd is a copy rather than a reconstruction, which is what
     * a program comparing its own cwd against a path it built expects.
     *
     * Always absolute, always without a trailing slash except for the
     * root itself. fork inherits it and execve keeps it. */
    char        cwd[PROC64_PATH_MAX];

    /* The alternate signal stack, per process because that is what it
     * is. A global one was written first and was wrong the moment more
     * than one process registered one: this prefix run has wineboot,
     * services.exe and rundll32 all calling sigaltstack, and services
     * alone registers three - one per thread. Delivering process A's
     * signal on process B's alternate stack is a write to an address
     * that is not mapped in A.
     *
     * Per thread is what Linux actually does and what Wine assumes; per
     * process is what this kernel can express today, and it is right for
     * every single-threaded case and wrong in the same direction as
     * before for the rest. Said plainly rather than left to be found. */
    uint64_t    sas_sp, sas_size;
} proc64_t;

void      proc64_init(void);

/* Allocates an empty process. Returns its pid, or -1. */
int       proc64_create(void);

/* Copies everything except the address space, which fork does
 * separately - the copy is expensive and the caller may want to fail
 * before paying for it. */
int       proc64_fork_from(int pid);

proc64_t* proc64_get(int pid);
proc64_t* proc64_current(void);
void      proc64_set_current(int pid);
int       proc64_current_pid(void);

/* Marks a process finished and records what it exited with, so a parent
 * that asks later has something to be told. */
void      proc64_exit(int pid, int status);

/* The first exited child of `parent`, or -1. Reaps it. */
int       proc64_reap_child(int parent, int* status);

/* Does this process have any children at all, exited or not? wait4 has
 * to tell "nothing to reap yet" from "there was never anything". */
int       proc64_has_children(int pid);

/* The address a parent blocks on while waiting. Not a real address -
 * user pointers stop well below this - so it cannot collide with a
 * futex a program is using. */
#define PROC64_WAIT_KEY(pid) (0x1000000000000000ULL + (uint64_t)(pid))

/* And the address a parent blocks on while its vfork child runs. A
 * different key from the one above because the two waits end on
 * different events: wait4 ends when a child exits, vfork ends when the
 * child execve's - which is usually the moment the child starts being
 * interesting rather than the moment it stops. */
#define PROC64_VFORK_KEY(pid) (0x2000000000000000ULL + (uint64_t)(pid))

uint64_t  proc64_count(void);

#endif
