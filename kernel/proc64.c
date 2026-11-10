/* proc64.c - the state a process owns, as opposed to a thread. */

#include "proc64.h"
#include "smp64.h"
#include "gdt64.h"
#include "serial64.h"
#include "pipe64.h"
#include "epoll64.h"
#include "inet64.h"
#include "ramfs64.h"
#include "wmdev64.h"
#include "kstring.h"

static proc64_t procs[PROC64_MAX];
/* Per CPU, like the scheduler's current task (sched64.c). */
static int      current_pid_cpu[SMP64_MAX_CPUS] = { -1, -1, -1, -1, -1, -1, -1, -1 };
#define current_pid (current_pid_cpu[smp64_active ? gdt64_cpu() & (SMP64_MAX_CPUS - 1) : 0])
static int      next_pid = 1;

void proc64_init(void) {
    for (int i = 0; i < PROC64_MAX; i++) procs[i].used = 0;
    for (int c = 0; c < SMP64_MAX_CPUS; c++) current_pid_cpu[c] = -1;
    next_pid = 1;
}

static int current_slot_index(void);

static proc64_t* slot_for(int pid) {
    if (pid >= 0 && pid == current_pid) {
        int c = current_slot_index();
        return c >= 0 ? &procs[c] : 0;
    }
    for (int i = 0; i < PROC64_MAX; i++)
        if (procs[i].used && procs[i].pid == pid) return &procs[i];
    return 0;
}

proc64_t* proc64_get(int pid)     { return slot_for(pid); }

/* The current process's slot, remembered per CPU. Every `fds[...]` in
 * syscall64.c is a proc64_current(), and a scan of 128 slots each time
 * made a wineserver poll over a few hundred descriptors cost 169,000
 * cycles. Checked on every use, so a slot reused since is not trusted. */
static int cur_slot_cpu[SMP64_MAX_CPUS] = { -1, -1, -1, -1, -1, -1, -1, -1 };

static int current_slot_index(void) {
    int c = smp64_active ? gdt64_cpu() & (SMP64_MAX_CPUS - 1) : 0;
    int pid = current_pid_cpu[c], i = cur_slot_cpu[c];
    if (i >= 0 && procs[i].used && procs[i].pid == pid) return i;
    for (i = 0; i < PROC64_MAX; i++)
        if (procs[i].used && procs[i].pid == pid) { cur_slot_cpu[c] = i; return i; }
    return -1;
}

proc64_t* proc64_current(void) {
    int i = current_slot_index();
    return i >= 0 ? &procs[i] : 0;
}
int       proc64_current_pid(void){ return current_pid; }

/* Which slot a pid occupies, so that per-process state kept outside
 * this file - the signal handler table, which needs signal64.h - can be
 * indexed without every such table walking this one itself. -1 for a
 * pid that is not live. */
int proc64_slot_of(int pid) {
    if (pid >= 0 && pid == current_pid) return current_slot_index();
    for (int i = 0; i < PROC64_MAX; i++)
        if (procs[i].used && procs[i].pid == pid) return i;
    return -1;
}

int proc64_current_slot(void) { return current_slot_index(); }
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
    /* The parent's CPU. Nearly every fork here is followed by an
     * execve, which is where a process is placed (smp64_pick_cpu): it
     * has a fresh address space then and nothing left on this CPU.
     * Choosing here instead put every launcher's short-lived helper on
     * one CPU and the program it started on the next, so with round
     * robin over four CPUs the programs only ever landed on two. */
    child->cpu = parent->cpu;

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
            /* A window's descriptor too: the child's copy is a second
             * name for the same window (see wmdev64_ref). */
            if (parent->fds[f].node >= 0 &&
                ramfs64_device(parent->fds[f].node) == RAMFS64_DEV_WM &&
                parent->fds[f].pos)
                wmdev64_ref((int)parent->fds[f].pos - 1);
        } else {
            pipe64_ref(parent->fds[f].rx, 1, 0);
            pipe64_ref(parent->fds[f].tx, 0, 1);
        }
        if (parent->fds[f].kind == FD64_INET) inet64_ref(parent->fds[f].sock);
        if (parent->fds[f].kind == FD64_EPOLL) epoll64_ref(parent->fds[f].sock);
    }
    child->brk_base    = parent->brk_base;
    child->brk_current = parent->brk_current;
    child->mmap_next   = parent->mmap_next;
    child->nresv       = parent->nresv;
    for (uint32_t r = 0; r < parent->nresv; r++)
        child->resv[r] = parent->resv[r];
    kstrlcpy(child->exe_path, parent->exe_path, PROC64_PATH_MAX);
    /* The name comes across too: Linux's child keeps the name its
     * parent had until something execs. Measured. */
    kstrlcpy(child->comm, parent->comm, sizeof(child->comm));
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

/* The signal that ended the process most recently reaped, so that
 * wait4 can encode a death the way Linux does without every caller of
 * proc64_reap_child growing another out-parameter. */
static int reaped_signal;

int proc64_reaped_signal(void) { return reaped_signal; }

void proc64_exit_signalled(int pid, int sig) {
    proc64_t* p = slot_for(pid);
    if (!p) return;
    p->term_sig = sig;
    proc64_exit(pid, 128 + sig);
}

int proc64_reap_child_pid(int parent, int want, int* status) {
    for (int i = 0; i < PROC64_MAX; i++) {
        if (!procs[i].used || !procs[i].exited) continue;
        if (procs[i].parent != parent) continue;
        if (want > 0 && procs[i].pid != want) continue;
        if (status) *status = procs[i].exit_status;
        reaped_signal = procs[i].term_sig;
        procs[i].used = 0;
        return procs[i].pid;
    }
    reaped_signal = 0;
    return -1;
}

/* Releases every exited process nobody is left to reap: one whose
 * parent is 0 - a bring-up layer's own program - or whose parent's slot
 * is gone. Repeats until nothing changes, because releasing a parent
 * orphans its exited children. Returns how many slots it freed.
 *
 * Their address spaces are not this file's business; by the time a
 * process is exited, the kernel has destroyed its space or will sweep
 * it. */
int proc64_release_orphans(void) {
    int n = 0, changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < PROC64_MAX; i++) {
            proc64_t* par;
            if (!procs[i].used || !procs[i].exited) continue;
            /* Never the current process, exited or not: the early
             * bring-up layers run one program after another as the same
             * process, and each one's exit marks it exited while the next
             * layer goes on using its descriptors. */
            if (procs[i].pid == current_pid) continue;
            par = procs[i].parent ? slot_for(procs[i].parent) : 0;
            if (par) continue;
            procs[i].used = 0;
            n++;
            changed = 1;
        }
    }
    return n;
}

int proc64_reap_child(int parent, int* status) {
    return proc64_reap_child_pid(parent, -1, status);
}

int proc64_has_child(int pid, int want) {
    for (int i = 0; i < PROC64_MAX; i++) {
        if (!procs[i].used || procs[i].parent != pid) continue;
        if (want > 0 && procs[i].pid != want) continue;
        return 1;
    }
    return 0;
}

int proc64_has_children(int pid) {
    return proc64_has_child(pid, -1);
}

uint64_t proc64_count(void) {
    uint64_t n = 0;
    for (int i = 0; i < PROC64_MAX; i++) if (procs[i].used) n++;
    return n;
}

int proc64_cpu_of(int pid) {
    proc64_t* p = slot_for(pid);
    return p ? p->cpu : 0;
}

void proc64_set_cpu(int pid, int cpu) {
    proc64_t* p = slot_for(pid);
    if (p) p->cpu = cpu;
}

/* Live processes placed on `cpu`, not counting `except`: the load
 * smp64_pick_cpu balances. */
int proc64_count_on(int cpu, int except) {
    int n = 0;
    for (int i = 0; i < PROC64_MAX; i++)
        if (procs[i].used && !procs[i].exited && procs[i].cpu == cpu &&
            procs[i].pid != except) n++;
    return n;
}

const char* proc64_comm_of(int pid) {
    proc64_t* p = slot_for(pid);
    return p ? p->comm : 0;
}
