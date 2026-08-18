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

#define PROC64_MAX     4
#define PROC64_FD_MAX  32
#define PROC64_PATH_MAX 128

typedef struct {
    int      node;
    uint64_t pos;
    int      used;
} proc64_fd_t;

typedef struct {
    int         used;
    int         pid;
    int         parent;        /* pid of the process that forked it   */
    int         exited;
    int         exit_status;

    vmspace64_t space;

    /* The heap and the mmap bump pointer: per process, because a fork
     * gives the child its own copy of both. */
    uint64_t    brk_base, brk_current, mmap_next;

    proc64_fd_t fds[PROC64_FD_MAX];
    char        exe_path[PROC64_PATH_MAX];
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

uint64_t  proc64_count(void);

#endif
