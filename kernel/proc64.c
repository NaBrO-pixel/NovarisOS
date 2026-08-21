/* proc64.c - the state a process owns, as opposed to a thread. */

#include "proc64.h"
#include "pipe64.h"
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
void      proc64_set_current(int pid) { current_pid = pid; }

int proc64_create(void) {
    int i;

    for (i = 0; i < PROC64_MAX; i++) if (!procs[i].used) break;
    if (i == PROC64_MAX) return -1;

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
        if (parent->fds[f].used && parent->fds[f].kind == FD64_PIPE) {
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
