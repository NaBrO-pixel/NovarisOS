#ifndef POSIX_PROC_H
#define POSIX_PROC_H

#include <stdint.h>
#include "posix.h"
#include "vfs.h"

/* posix_proc.h - what a *process* is, as opposed to a task.
 *
 * Milestone 29. Until now every piece of POSIX state in this kernel was a
 * file-scope global: one descriptor table in kernel/posix.c, one mmap
 * arena, one break, one signal disposition table in kernel/posix_signal.c.
 * That was honest while there was only ever one program running, and it is
 * exactly what Milestone 28 said had to change before fork could exist -
 * two concurrent processes sharing one descriptor table would corrupt each
 * other, and a fork that quietly shared its parent's would pass a simple
 * test and be wrong.
 *
 * So: one of these per process, and the *key is the address space*. That
 * choice does the whole job of separating processes from threads without a
 * single explicit check. Threads share a page directory (Milestone 16), so
 * they find the same structure here and share its descriptors, its break
 * and its signal handlers - which is what POSIX says threads do. A forked
 * child gets its own directory, so it finds its own structure - which is
 * what POSIX says processes do. Nothing in either path has to know which
 * it is.
 *
 * The one thing that is *not* here is the open file itself. Descriptors
 * are per process; the objects behind them - a vfs node, a socket - are
 * shared, which is why fork gives the child a copy of the table and a
 * reference to each object rather than a copy of the object. */

#define POSIX_MAX_PROCS 8
/* 128 since Milestone 30. Thirty-two was a shell's worth and wineserver
 * is not a shell: it holds its listening socket, a connection, a request
 * pipe, a reply pipe and a wait pipe per client, and an open descriptor
 * for every file any client has mapped. It ran out while opening
 * ntdll.dll and reported STATUS_TOO_MANY_OPENED_FILES, which the client
 * turned into "invalid image format" - a true statement about the wrong
 * thing. */
#define POSIX_MAX_FDS   128
#define POSIX_PATH_MAX  128

typedef enum {
    FD_FREE = 0,
    FD_CONSOLE,
    FD_FILE,
    FD_SOCKET,      /* also a pipe end - see kernel/socket.c */
} fd_kind_t;

typedef struct {
    fd_kind_t   kind;
    vfs_node_t* node;
    uint32_t    offset;
    int         writable;
    int         append;      /* O_APPEND: every write goes to the end */
    uint32_t    dir_index;   /* getdents64's position in a directory */
    struct socket* sock;     /* FD_SOCKET only */
    int         cloexec;     /* closed by execve rather than inherited */
} fd_entry_t;

/* What to put in siginfo_t when each signal is finally delivered.
 * `pending` is a bitmask rather than a queue, so this is one record per
 * signal number rather than one per raise. */
typedef struct {
    int32_t  code;
    uint32_t addr;   /* si_addr, for the fault signals */
    int32_t  pid;    /* si_pid, for kill/tgkill */
} siginfo_rec_t;

typedef struct posix_proc {
    int      in_use;
    /* The key. 0 for a process whose address space has been destroyed but
     * whose exit status has not been collected yet - a zombie is still a
     * process, and its parent is entitled to ask about it. */
    uint32_t page_directory;

    int      pid;
    int      ppid;
    int      exited;
    int      exit_status;    /* the low byte of exit(), or a signal number */

    /* 1 when the pages of this address space are this process's to hand
     * back when it exits. True for a program started through
     * posix_process_begin() and for anything it forks; false for a task
     * the POSIX layer merely met - the shell's `threadtest` maps an image
     * itself and reads a counter out of it *after* its threads have
     * finished, so a thread that released "its" address space would be
     * freeing the shell's memory. That is a real crash and this flag is
     * what prevents it. */
    int      owns_memory;

    /* The pid of a parent suspended in vfork waiting for this process to
     * execve or exit, or 0. glibc's posix_spawn is
     * clone(CLONE_VM|CLONE_VFORK) and Wine reaches wineserver through it,
     * so "the parent does not run until the child has become something
     * else" is not an optimisation here - it is the difference between
     * starting a server and unmapping the stack the child is standing on. */
    int      vfork_parent;

    fd_entry_t fds[POSIX_MAX_FDS];

    uint32_t mmap_next;
    uint32_t brk_current;
    uint32_t brk_mapped;

    /* The path readlink("/proc/self/exe") answers with, and the working
     * directory every relative path resolves against. Both are per
     * process, and both are things Wine reads to work out where it is. */
    char     exe_path[POSIX_PATH_MAX];
    char     cwd[POSIX_PATH_MAX];

    /* --- signals (kernel/posix_signal.c) --------------------------------
     * Dispositions are per process and survive fork; they are reset to
     * the default by execve, because the handler addresses belonged to an
     * image that no longer exists. The mask survives both. */
    k_sigaction_t actions[NSIG];
    siginfo_rec_t info_of[NSIG];
    uint32_t blocked;
    uint32_t pending;
    int      any_handler;
    int      in_delivery;
    uint32_t last_trapno, last_err, last_cr2;

    /* The alternate signal stack, if the process registered one. Wine
     * does: a handler that runs on the faulting thread's own stack cannot
     * do anything useful about a stack overflow, which is one of the
     * faults Wine turns into a Windows exception. A handler installed
     * with SA_ONSTACK runs here instead. */
    uint32_t altstack_sp;
    uint32_t altstack_size;
    int      altstack_on;    /* 1 while a handler is running on it */

    /* poll()'s deadline, remembered across the retries that implement its
     * waiting - otherwise a relative timeout restarts every time the
     * syscall re-executes and never expires. `poll_key` is the user
     * address of the caller's own array, so two different polls cannot
     * inherit each other's clock. */
    uint32_t poll_key;
    uint32_t poll_deadline;

    uint32_t unimplemented;
} posix_proc_t;

/* The process the current address space belongs to. Never null while a
 * program is running; falls back to slot 0 outside one so that kernel
 * paths which reach the POSIX layer before any process exists (the shell
 * opening a file, say) still have somewhere to look. */
posix_proc_t* posix_current(void);

/* Allocates and initialises the process structure for the address space
 * that is current, with `ppid` as its parent. Called when a program
 * starts and by fork for the child. Returns null if the table is full. */
posix_proc_t* posix_proc_create(int ppid);

/* Copies `src` into a new structure for the address space `pd`, which is
 * fork: the same descriptors, the same arenas, the same handlers, a new
 * pid and `src` as the parent. */
posix_proc_t* posix_proc_clone(const posix_proc_t* src, uint32_t pd);

/* Finds a process by pid, including exited ones whose status nobody has
 * collected. Null if there is no such process. */
posix_proc_t* posix_proc_by_pid(int pid);

/* Marks `p` exited with `status`, unhooks it from its address space, and
 * hands any children it had to nobody (they become parentless rather than
 * orphans of a stale pid). The structure stays in the table until a
 * wait4 collects it. */
void posix_proc_exit(posix_proc_t* p, int status);

/* Releases a collected zombie's slot. */
void posix_proc_reap(posix_proc_t* p);

/* Empties the whole table. Called when a program run ends, so the next
 * one starts from nothing rather than inheriting a table full of
 * uncollected children. */
void posix_proc_reset_all(void);

/* How many processes are live, for the shell's `psinfo`. */
int posix_proc_count(void);

/* Iteration, for the shell. Returns null past the end. */
posix_proc_t* posix_proc_at(int index);

#endif
