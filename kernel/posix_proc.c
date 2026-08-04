/* posix_proc.c - the process table. See include/posix_proc.h for why it
 * exists and why the key is an address space.
 *
 * This file owns nothing but the table and its lifecycle; every field in
 * it belongs to one of posix.c, posix_signal.c or posix_thread.c, which
 * reach it through posix_current(). Keeping the allocation here rather
 * than in posix.c is what stops the signal layer having to include the
 * descriptor layer to find its own state. */

#include "posix_proc.h"
#include "paging.h"
#include "scheduler.h"
#include "kstring.h"
#include "socket.h"

static posix_proc_t procs[POSIX_MAX_PROCS];

/* 0x100 rather than 1, which is the value getpid() reported for the whole
 * of Milestones 18-28 - there was one process and it was a constant. The
 * first real pid keeping that value means nothing that already worked
 * changes its output. */
static int next_pid = 0x100;

/* Where the arenas start. Duplicated from posix.c deliberately: those
 * constants describe the layout of an address space, and this file has to
 * write them into a new process's structure before posix.c ever sees it. */
#define MMAP_ARENA_START 0x42000000u
#define BRK_ARENA_START  0x48000000u

static void proc_init(posix_proc_t* p, uint32_t pd, int ppid) {
    kmemset(p, 0, sizeof(*p));
    p->in_use = 1;
    p->page_directory = pd;
    p->pid = next_pid++;
    p->ppid = ppid;
    p->mmap_next = MMAP_ARENA_START;
    p->brk_current = BRK_ARENA_START;
    p->brk_mapped = BRK_ARENA_START;
    kstrlcpy(p->exe_path, "/program", sizeof(p->exe_path));
    kstrlcpy(p->cwd, "/", sizeof(p->cwd));

    for (int i = 0; i < NSIG; i++) {
        p->actions[i].handler = SIG_DFL;
        p->info_of[i].code = SI_USER;
    }
}

posix_proc_t* posix_current(void) {
    /* The scheduler's answer rather than CR3's, because the two disagree
     * in exactly one place that matters: reap_all() walks every address
     * space with no task running, and a stray descriptor lookup during
     * that walk must not find some other process's table. */
    uint32_t pd = scheduler_current_address_space();
    if (!pd) pd = paging_current_address_space();

    for (int i = 0; i < POSIX_MAX_PROCS; i++) {
        if (procs[i].in_use && !procs[i].exited &&
            procs[i].page_directory == pd) {
            return &procs[i];
        }
    }

    /* No process owns this address space. That is not an error: the
     * shell's `multitask` spawns tasks straight into the scheduler
     * without going through the POSIX layer's program-start path, and
     * they are entitled to a descriptor table of their own rather than to
     * somebody else's. Registering on first sight is cheaper than a
     * second entry point nothing else would use.
     *
     * A full table falls back to slot 0, because every caller of this
     * function is inside a syscall and none of them can usefully handle
     * "there is no process". */
    for (int i = 0; i < POSIX_MAX_PROCS; i++) {
        if (!procs[i].in_use) {
            proc_init(&procs[i], pd, 0);
            return &procs[i];
        }
    }
    if (!procs[0].in_use) proc_init(&procs[0], pd, 0);
    return &procs[0];
}

posix_proc_t* posix_proc_create(int ppid) {
    uint32_t pd = paging_current_address_space();

    /* An address space can only belong to one process. Re-registering the
     * one that is already current is what a second `run` does, and it must
     * replace rather than accumulate. */
    for (int i = 0; i < POSIX_MAX_PROCS; i++) {
        if (procs[i].in_use && !procs[i].exited &&
            procs[i].page_directory == pd) {
            proc_init(&procs[i], pd, ppid);
            procs[i].owns_memory = 1;
            return &procs[i];
        }
    }
    for (int i = 0; i < POSIX_MAX_PROCS; i++) {
        if (!procs[i].in_use) {
            proc_init(&procs[i], pd, ppid);
            /* Registered deliberately, by a program that is starting -
             * so the address space is this process's and its pages come
             * back when it exits. The auto-registration in
             * posix_current() leaves this 0 on purpose. */
            procs[i].owns_memory = 1;
            return &procs[i];
        }
    }
    return 0;
}

posix_proc_t* posix_proc_clone(const posix_proc_t* src, uint32_t pd) {
    for (int i = 0; i < POSIX_MAX_PROCS; i++) {
        if (procs[i].in_use) continue;
        posix_proc_t* p = &procs[i];
        /* kmemcpy rather than `*p = *src`, which is the same thing until
         * the struct is big enough that gcc stops inlining the copy and
         * emits a call to memcpy - a libc this kernel does not have. The
         * descriptor table crossing that threshold is what found it. */
        kmemcpy(p, src, sizeof(*p));
        p->page_directory = pd;
        p->pid = next_pid++;
        p->ppid = src->pid;
        p->exited = 0;
        p->exit_status = 0;
        p->poll_key = 0;
        p->poll_deadline = 0;
        p->unimplemented = 0;
        /* Every descriptor the parent had is the child's too, and the
         * *object* behind each is now referred to twice - which is what
         * makes closing one end in the child leave the parent's working.
         * Both kinds are counted since Milestone 30: a file's node can
         * outlive its name, so a descriptor to an unlinked file is the
         * only thing keeping it. */
        for (int f = 0; f < POSIX_MAX_FDS; f++) {
            if (p->fds[f].kind == FD_SOCKET) socket_ref(p->fds[f].sock);
            if (p->fds[f].kind == FD_FILE) vfs_node_ref(p->fds[f].node);
        }
        /* And the same for the nodes the parent's *mappings* hold, which
         * the copy above duplicated the pointers to. The child's address
         * space is a copy of the parent's, so those mappings are real in
         * it and the references have to be too.
         *
         * This was a leak until Milestone 36 and then briefly a
         * use-after-free. Nothing ever gave a pin back - posix_unpin_all()
         * asked posix_current(), which by then answered with a fresh
         * process and an empty table - so an unreferenced duplicate cost
         * nothing. Making the unpin work is what made the duplicate
         * matter: a forked child exiting dropped references it had never
         * taken, and freed a node the parent still had mapped. */
        for (int i2 = 0; i2 < POSIX_MAX_PINNED; i2++) {
            if (p->pinned[i2]) vfs_node_ref(p->pinned[i2]);
        }
        return p;
    }
    return 0;
}

/* The process a *task* belongs to. A thread's id is a task id, not a
 * process id, so a signal naming one - which is what tgkill does, and
 * what wineserver's kill() does when it suspends a client thread - has
 * to be routed through the scheduler to find whose thread it is. */
posix_proc_t* posix_proc_by_task(int tid) {
    uint32_t pd = scheduler_address_space_of(tid);
    if (!pd) return 0;
    for (int i = 0; i < POSIX_MAX_PROCS; i++) {
        if (procs[i].in_use && !procs[i].exited &&
            procs[i].page_directory == pd) {
            return &procs[i];
        }
    }
    return 0;
}

posix_proc_t* posix_proc_by_pid(int pid) {
    for (int i = 0; i < POSIX_MAX_PROCS; i++) {
        if (procs[i].in_use && procs[i].pid == pid) return &procs[i];
    }
    return 0;
}

void posix_proc_exit(posix_proc_t* p, int status) {
    if (!p || !p->in_use) return;
    p->exited = 1;
    p->exit_status = status;
    /* The address space is about to be emptied and eventually destroyed,
     * and a later one could be allocated the same page directory frame.
     * Clearing the key means a zombie can never be mistaken for the
     * process that reuses its space. */
    p->page_directory = 0;

    /* Children outlive their parent. Nothing here reparents them to an
     * init process, because there is none - they simply have a pid for a
     * parent that no longer exists, and nobody will collect them. Their
     * slots come back when the program run ends. */
}

void posix_proc_reap(posix_proc_t* p) {
    if (!p) return;
    p->in_use = 0;
}

void posix_proc_reset_all(void) {
    for (int i = 0; i < POSIX_MAX_PROCS; i++) procs[i].in_use = 0;
    next_pid = 0x100;
}

int posix_proc_count(void) {
    int n = 0;
    for (int i = 0; i < POSIX_MAX_PROCS; i++) if (procs[i].in_use) n++;
    return n;
}

posix_proc_t* posix_proc_at(int index) {
    if (index < 0 || index >= POSIX_MAX_PROCS) return 0;
    return procs[index].in_use ? &procs[index] : 0;
}
