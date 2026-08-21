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
#define PROC64_MAX     32
#define PROC64_FD_MAX  32

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
