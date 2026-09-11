/* proc64.c - the state a process owns, as opposed to a thread. */

#include "proc64.h"
#include "serial64.h"
#include "pipe64.h"
#include "ramfs64.h"
#include "kstring.h"

static proc64_t procs[PROC64_MAX];
static int      current_pid = -1;
static int      next_pid = 1;

void proc64_init(void) {
    for (int i = 0; i < PROC64_MAX; i++) procs[i].used = 0;
    current_pid = -1;
    next_pid = 1;
}

static proc64_t* slot_for(int pid) {
    for (int i = 0; i < PROC64_MAX; i++)
        if (procs[i].used && procs[i].pid == pid) return &procs[i];
    return 0;
}

proc64_t* proc64_get(int pid)     { return slot_for(pid); }
proc64_t* proc64_current(void)    { return slot_for(current_pid); }
int       proc64_current_pid(void){ return current_pid; }

/* Which slot a pid occupies, so that per-process state kept outside
 * this file - the signal handler table, which needs signal64.h - can be
 * indexed without every such table walking this one itself. -1 for a
 * pid that is not live. */
int proc64_slot_of(int pid) {
    for (int i = 0; i < PROC64_MAX; i++)
        if (procs[i].used && procs[i].pid == pid) return i;
    return -1;
}

int proc64_current_slot(void) { return proc64_slot_of(current_pid); }
void      proc64_set_current(int pid) { current_pid = pid; }

int proc64_create(void) {
    int i;

    for (i = 0; i < PROC64_MAX; i++) if (!procs[i].used) break;
    if (i == PROC64_MAX) {
        /* Full. Said once, with the one number that decides what to do
         * about it.
         *
         * A slot is held by a process that is running and by one that
         * has exited and not been reaped - Unix keeps the second so its
         * parent can still ask how it died. Those two want opposite
         * fixes: live processes mean the table is too small, zombies
         * mean nothing is reaping them. Measured against the host, a
         * prefix run peaks at 22 concurrent processes against this
         * table's 32, which says the answer is the second - but saying
         * it here means not having to infer it. */
        static int said;
        if (!said) {
            int live = 0, dead = 0;
            said = 1;
            for (int k = 0; k < PROC64_MAX; k++)
                if (procs[k].exited) dead++; else live++;
            serial64_puts("NOVARIS64: [proc] all ");
            serial64_putdec(PROC64_MAX);
            serial64_puts(" slots in use - ");
            serial64_putdec((uint64_t)live);
            serial64_puts(" running, ");
            serial64_putdec((uint64_t)dead);
            serial64_puts(" exited and unreaped; clone now returns"
                          " -EAGAIN\n");
        }
        return -1;
    }

    kmemset(&procs[i], 0, sizeof(procs[i]));
    procs[i].used   = 1;
    procs[i].pid    = next_pid++;
    procs[i].parent = 0;
    /* A process always has a working directory. The root is the only
     * one that is certain to exist this early. */
    kstrlcpy(procs[i].cwd, "/", PROC64_PATH_MAX);
    return procs[i].pid;
}

int proc64_fork_from(int pid) {
    proc64_t* parent = slot_for(pid);
    int child_pid;
    proc64_t* child;

    if (!parent) return -1;

    child_pid = proc64_create();
    if (child_pid < 0) return -1;

    child = slot_for(child_pid);

    /* Everything the child inherits: open files at the same offsets,
     * the same heap layout, the same idea of what it is. The address
     * space is the caller's job, because copying it can fail after this
     * point and undoing a half-built process is worse than checking. */
    /* Open files at the same offsets - and, for a pipe, a reference of
     * its own. A fork that copied the descriptor without counting it
     * would leave the pipe believing one reader had gone the first time
     * either process closed, so the survivor's next read would report
     * end of file with the writer still there. */
    for (int f = 0; f < PROC64_FD_MAX; f++) {
        child->fds[f] = parent->fds[f];
        if (!parent->fds[f].used) continue;
        if (parent->fds[f].kind == FD64_FILE) {
            ramfs64_ref_node(parent->fds[f].node);
        } else {
            pipe64_ref(parent->fds[f].rx, 1, 0);
            pipe64_ref(parent->fds[f].tx, 0, 1);
        }
    }
    child->brk_base    = parent->brk_base;
    child->brk_current = parent->brk_current;
    child->mmap_next   = parent->mmap_next;
    kstrlcpy(child->exe_path, parent->exe_path, PROC64_PATH_MAX);
    kstrlcpy(child->cwd, parent->cwd, PROC64_PATH_MAX);
    child->parent = parent->pid;

    return child_pid;
}

void proc64_exit(int pid, int status) {
    proc64_t* p = slot_for(pid);
    if (!p) return;
    p->exited      = 1;
    p->exit_status = status;
    /* The slot stays used: a parent that has not asked yet still needs
     * somewhere to read the status from. It is freed by whoever reaps
     * it - which is why a process nobody waits for is a zombie on every
     * system that has ever had fork. */
}

int proc64_reap_child(int parent, int* status) {
    for (int i = 0; i < PROC64_MAX; i++) {
        if (!procs[i].used || !procs[i].exited) continue;
        if (procs[i].parent != parent) continue;
        if (status) *status = procs[i].exit_status;
        procs[i].used = 0;
        return procs[i].pid;
    }
    return -1;
}

int proc64_has_children(int pid) {
    for (int i = 0; i < PROC64_MAX; i++)
        if (procs[i].used && procs[i].parent == pid) return 1;
    return 0;
}

uint64_t proc64_count(void) {
    uint64_t n = 0;
    for (int i = 0; i < PROC64_MAX; i++) if (procs[i].used) n++;
    return n;
}
