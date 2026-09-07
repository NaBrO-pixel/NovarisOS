/* sched64.c - round-robin preemption, done inside the timer interrupt. */

#include "sched64.h"
#include "gdt64.h"

#define UCODE_SEL_RPL3 0x23
#define UDATA_SEL_RPL3 0x1B
#define RFLAGS_IF      0x202    /* IF, plus the bit that is always set */

typedef struct {
    registers64_t regs;
    vmspace64_t   space;
    uint64_t      fs_base;   /* the thread pointer, per thread          */
    uint64_t      gs_base;   /* and the TEB, which Wine keeps at GS      */
    uint64_t      wait_addr; /* the futex it is blocked on, 0 if awake  */
    /* What rax becomes when this thread is woken. futex wants 0 - the
     * value FUTEX_WAIT returns - but a syscall that blocks and then
     * *restarts* needs its own number back there instead, or the
     * re-executed `syscall` invokes whatever call 0 happens to be. */
    uint64_t      wake_rax;
    int           pid;       /* the process this thread belongs to      */
    int           blocked;
    int           used;

    /* The x87/SSE register file, which nothing else saves.
     *
     * The kernel is built -mno-sse and never touches these registers,
     * so they belong entirely to the task - and a switch that restores
     * every general register and none of these hands the incoming
     * thread the outgoing one's xmm0-15, MXCSR and x87 stack.
     *
     * While SCHED64_MAX_TASKS was 4 that almost never happened. Raising
     * it to 128 let a Wine prefix run threads concurrently for the first
     * time, and the run died on a general protection fault in ring 3, in
     * PE code, with no error code - which is what an unaligned movaps or
     * an MXCSR with reserved bits set looks like from outside.
     *
     * 512 bytes and 16-byte aligned because that is what FXSAVE writes
     * and what FXRSTOR refuses to read otherwise. Aligning the member
     * aligns the struct, which keeps every element of the array aligned
     * too. */
    uint8_t       fpu[512] __attribute__((aligned(16)));
} task64_t;

static inline void fpu_save(uint8_t* area) {
    __asm__ __volatile__("fxsave64 (%0)" :: "r"(area) : "memory");
}
static inline void fpu_restore(const uint8_t* area) {
    __asm__ __volatile__("fxrstor64 (%0)" :: "r"(area) : "memory");
}

/* What a thread that has never run starts with. Taken from the hardware
 * rather than written by hand: FXRSTOR raises #GP if MXCSR has reserved
 * bits set, so a zeroed area is not a safe starting state and a
 * hand-built one is a guess about this CPU. */
static uint8_t fpu_initial[512] __attribute__((aligned(16)));

#define IA32_FS_BASE 0xC0000100u
#define IA32_GS_BASE 0xC0000101u

static inline uint64_t read_msr_base(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void write_msr_base(uint32_t msr, uint64_t v) {
    __asm__ __volatile__("wrmsr"
                         :: "c"(msr),
                            "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static inline uint64_t read_fs_base(void)        { return read_msr_base(IA32_FS_BASE); }
static inline void     write_fs_base(uint64_t v) { write_msr_base(IA32_FS_BASE, v); }

/* GS is per-thread too, and nothing was saving it.
 *
 * This kernel deliberately does not use swapgs - syscall64.s says so and
 * says why - so IA32_GS_BASE is the *user's* GS base, with no kernel
 * copy behind it. arch_prctl(ARCH_SET_GS) writes it, and Wine uses it:
 * the Windows TEB lives at GS on x86-64, and __wine_syscall_dispatcher
 * begins
 *
 *     mov %gs:0x378,%rcx        // this thread's struct syscall_frame
 *
 * A switch that restores FS and leaves GS alone hands the incoming
 * thread the *outgoing* thread's TEB, so that read produces another
 * thread's syscall frame. The dispatcher then restores its registers
 * through it and dies on
 *
 *     movaps 0x1c0(%rcx),%xmm6
 *
 * a general protection fault in ring 3 with no error code, 0x1c0 being
 * inside the frame's xsave area at 0xc0.
 *
 * Invisible while SCHED64_MAX_TASKS was 4, because threads of different
 * processes barely overlapped. */
static inline uint64_t read_gs_base(void)        { return read_msr_base(IA32_GS_BASE); }
static inline void     write_gs_base(uint64_t v) { write_msr_base(IA32_GS_BASE, v); }

static task64_t tasks[SCHED64_MAX_TASKS];
static int      task_total;
static int      current = -1;
static uint64_t switch_count;
static uint64_t stop_after;
static uint64_t stop_rip;

void sched64_init(void) {
    /* A clean x87 state, then the default MXCSR over the top of it:
     * fninit does not touch MXCSR, and whatever the boot path left there
     * is not what a new thread should inherit. 0x1F80 is the reset
     * value - all exceptions masked, round to nearest. */
    __asm__ __volatile__("fninit");
    fpu_save(fpu_initial);
    *(uint32_t*)(fpu_initial + 24) = 0x1F80;

    for (int i = 0; i < SCHED64_MAX_TASKS; i++) tasks[i].used = 0;
    task_total = 0;
    current = -1;
    switch_count = 0;
    stop_after = 0;
    stop_rip = 0;
}

int sched64_add(uint64_t rip, uint64_t rsp, uint64_t arg,
                const vmspace64_t* space) {
    int i;

    for (i = 0; i < SCHED64_MAX_TASKS; i++) if (!tasks[i].used) break;
    if (i == SCHED64_MAX_TASKS) return -1;

    for (uint64_t* p = (uint64_t*)&tasks[i].regs;
         p < (uint64_t*)(&tasks[i].regs + 1); p++) *p = 0;

    /* A thread that has never run still needs a valid register file to
     * be restored from the first time it is switched to. */
    tasks[i].gs_base = read_gs_base();
    for (int b = 0; b < 512; b++) tasks[i].fpu[b] = fpu_initial[b];

    /* The frame iretq will consume the first time this task is resumed.
     * cs and ss are the ring-3 selectors; without IF in rflags the task
     * would take the CPU and never give it back, since the only thing
     * that preempts it is the timer. */
    tasks[i].regs.rip    = rip;
    tasks[i].regs.rsp    = rsp;
    tasks[i].regs.cs     = UCODE_SEL_RPL3;
    tasks[i].regs.ss     = UDATA_SEL_RPL3;
    tasks[i].regs.rflags = RFLAGS_IF;
    tasks[i].regs.rdi    = arg;

    tasks[i].space     = *space;
    tasks[i].fs_base   = 0;  /* a task that has never run has no TLS yet */
    tasks[i].blocked   = 0;
    tasks[i].wait_addr = 0;
    /* Belongs to whoever is running. Leaving this zero would make the
     * next timer tick set the current process to a pid nothing owns,
     * and every syscall after it would find no process at all - which
     * is a null dereference in the kernel, layers later. */
    tasks[i].pid       = proc64_current_pid();
    tasks[i].used      = 1;
    task_total++;
    return i;
}

int sched64_add_frame(const registers64_t* regs, const vmspace64_t* space,
                      uint64_t fs_base) {
    /* A thread of the process that is running. clone(2) takes this
     * path; fork(2) takes the one below, because its new thread belongs
     * to a different process. */
    return sched64_add_frame_for(regs, space, fs_base, proc64_current_pid());
}

int sched64_add_frame_for(const registers64_t* regs, const vmspace64_t* space,
                          uint64_t fs_base, int pid) {
    int i;

    for (i = 0; i < SCHED64_MAX_TASKS; i++) if (!tasks[i].used) break;
    if (i == SCHED64_MAX_TASKS) return -1;

    tasks[i].pid       = pid;
    tasks[i].regs      = *regs;
    tasks[i].space     = *space;
    tasks[i].fs_base   = fs_base;
    tasks[i].blocked   = 0;
    tasks[i].wait_addr = 0;
    tasks[i].used      = 1;
    /* This is the path clone and fork take, so it is the one that
     * matters most: a thread resumed with a register file nobody wrote
     * is a thread FXRSTOR may refuse to load at all. */
    tasks[i].gs_base = read_gs_base();
    for (int b = 0; b < 512; b++) tasks[i].fpu[b] = fpu_initial[b];
    task_total++;
    return i;
}

void sched64_set_current_space(const vmspace64_t* space) {
    /* execve replaces the address space under a running task. The task
     * carries its own copy, and the scheduler reloads that copy on every
     * switch - so without this the next timer tick puts the *old* space
     * back while rip points into the new program. Both programs being
     * linked at the same address makes that land in real code and keep
     * running, which is the worst possible version of the bug. */
    if (current < 0 || !tasks[current].used) return;
    tasks[current].space = *space;
}

const vmspace64_t* sched64_current_space(void) {
    if (current < 0 || !tasks[current].used) return 0;
    return &tasks[current].space;
}

void sched64_set_current(int index) {
    current = index;
    /* The first task is entered directly rather than switched to, so the
     * scheduler never got to record what it was running with. */
    if (index >= 0 && index < SCHED64_MAX_TASKS)
        tasks[index].fs_base = read_fs_base();
}
int  sched64_current(void)          { return current; }
uint64_t sched64_switches(void)     { return switch_count; }

void sched64_stop_after(uint64_t switches, uint64_t exit_rip) {
    stop_after = switches;
    stop_rip   = exit_rip;
}

/* syscall64.s reaches into registers64_t by hand-written byte offsets,
 * because it runs before there is a stack frame to speak of. If the
 * struct ever changes shape, that assembly reads the wrong fields and
 * jumps somewhere arbitrary - so the offsets are asserted here, where
 * the compiler can see both. */
_Static_assert(__builtin_offsetof(registers64_t, rax)    == 0,   "rax");
_Static_assert(__builtin_offsetof(registers64_t, rdi)    == 40,  "rdi");
_Static_assert(__builtin_offsetof(registers64_t, rbp)    == 48,  "rbp");
_Static_assert(__builtin_offsetof(registers64_t, r15)    == 112, "r15");
_Static_assert(__builtin_offsetof(registers64_t, rip)    == 136, "rip");
_Static_assert(__builtin_offsetof(registers64_t, cs)     == 144, "cs");
_Static_assert(__builtin_offsetof(registers64_t, rflags) == 152, "rflags");
_Static_assert(__builtin_offsetof(registers64_t, rsp)    == 160, "rsp");
_Static_assert(__builtin_offsetof(registers64_t, ss)     == 168, "ss");

int sched64_exit_current(registers64_t* out_regs, vmspace64_t* out_space,
                         uint64_t* out_fs_base) {
    int next;

    if (current < 0 || !tasks[current].used) return 0;

    tasks[current].used = 0;
    if (task_total > 0) task_total--;

    /* A *runnable* one. This used to take the first task marked used,
     * blocked or not, and hand its saved frame straight to iretq - which
     * resumes a task that is parked on a futex, a pipe or a child, with
     * none of the things its wait was going to give it. `wake_rax` is
     * never applied, so a restarted syscall comes back with whatever was
     * in rax when it blocked, and the wake-up it was actually waiting
     * for arrives later to a task that is already running.
     *
     * Nothing had exercised it: exit(2) ends a thread, and every program
     * here until now was single-threaded or ended with exit_group. It is
     * fixed on sight rather than left for the run that finds it. */
    for (next = 0; next < SCHED64_MAX_TASKS; next++)
        if (tasks[next].used && !tasks[next].blocked) break;
    if (next == SCHED64_MAX_TASKS) { current = -1; return 0; }

    /* Nothing to save: the task that was running has ended. */
    current = next;
    fpu_restore(tasks[next].fpu);
    write_gs_base(tasks[next].gs_base);
    if (out_regs)     *out_regs     = tasks[next].regs;
    if (out_space)    *out_space    = tasks[next].space;
    if (out_fs_base)  *out_fs_base  = tasks[next].fs_base;
    proc64_set_current(tasks[next].pid);
    return 1;
}

int sched64_exit_process(int pid, registers64_t* out_regs,
                         vmspace64_t* out_space, uint64_t* out_fs_base) {
    int next;

    /* exit_group ends every thread of the process, not just the one
     * that called it. Ending only the caller would leave its siblings
     * running in an address space nobody owns - and, when a test's two
     * tasks belong to the same process, would hand the CPU to the other
     * one instead of finishing the run. */
    for (int i = 0; i < SCHED64_MAX_TASKS; i++) {
        if (!tasks[i].used || tasks[i].pid != pid) continue;
        tasks[i].used = 0;
        if (task_total > 0) task_total--;
    }

    /* Runnable, for the same reason as above. */
    for (next = 0; next < SCHED64_MAX_TASKS; next++)
        if (tasks[next].used && !tasks[next].blocked) break;
    if (next == SCHED64_MAX_TASKS) { current = -1; return 0; }

    /* Nothing to save: every thread of that process has ended. */
    current = next;
    fpu_restore(tasks[next].fpu);
    write_gs_base(tasks[next].gs_base);
    if (out_regs)    *out_regs    = tasks[next].regs;
    if (out_space)   *out_space   = tasks[next].space;
    if (out_fs_base) *out_fs_base = tasks[next].fs_base;
    proc64_set_current(tasks[next].pid);
    return 1;
}

/* A blocked task is not a candidate: it is waiting on a futex and has
 * nothing to run until somebody wakes it. */
static int next_task(int from) {
    for (int n = 1; n <= SCHED64_MAX_TASKS; n++) {
        int i = (from + n) % SCHED64_MAX_TASKS;
        if (tasks[i].used && !tasks[i].blocked) return i;
    }
    return from;
}

int sched64_block_current(const registers64_t* regs, uint64_t addr,
                          uint64_t wake_rax,
                          registers64_t* out_regs, vmspace64_t* out_space,
                          uint64_t* out_fs_base) {
    int next;

    if (current < 0 || !tasks[current].used) return 0;

    /* The frame the caller built is this thread's entire continuation:
     * when something wakes it, execution resumes from exactly here.
     *
     * The thread pointer is part of that continuation and was not being
     * saved. The timer's switch-out saves it (see sched64_tick), so a
     * thread that was preempted at least once after setting up its TLS
     * carried a correct value by luck; one that blocked before that
     * woke with whatever its slot was created with - 0 for the first
     * task of a process. What that looks like from ring 3 is fs:0x10
     * reading as zero, and the crash is wherever glibc first
     * dereferences the thread descriptor - inside the *next* syscall
     * wrapper, with nothing pointing back at the wait that caused it. */
    tasks[current].fs_base   = read_fs_base();
    tasks[current].regs      = *regs;
    tasks[current].blocked   = 1;
    tasks[current].wait_addr = addr;
    tasks[current].wake_rax  = wake_rax;

    next = next_task(current);
    if (next == current || tasks[next].blocked) {
        /* Nothing else can run. Really this is a deadlock, and Linux
         * would simply block forever; unblocking the caller and letting
         * it see -EDEADLK is more useful than a machine that stops. */
        tasks[current].blocked   = 0;
        tasks[current].wait_addr = 0;
        return 0;
    }

    tasks[current].gs_base = read_gs_base();
    fpu_save(tasks[current].fpu);
    current = next;
    fpu_restore(tasks[next].fpu);
    write_gs_base(tasks[next].gs_base);
    if (out_regs)    *out_regs    = tasks[next].regs;
    if (out_space)   *out_space   = tasks[next].space;
    if (out_fs_base) *out_fs_base = tasks[next].fs_base;
    proc64_set_current(tasks[next].pid);
    return 1;
}

/* Give somebody else a turn, and stay runnable.
 *
 * This is not block_current with the flag left clear: a blocked task is
 * skipped by next_task and needs waking, and what a poll or a sleep
 * wants is the opposite - to be picked again on the next pass without
 * anybody having to remember it.
 *
 * It exists because syscalls run with interrupts *off* here. FMASK
 * clears IF on entry, deliberately: this kernel has one kernel stack, so
 * a timer that preempted a task in the middle of a syscall would hand
 * that stack to somebody else and the first syscall would resume onto
 * whatever the second left behind. The consequence is that a syscall
 * cannot wait for time to pass - the tick that would end the wait cannot
 * arrive while it is waiting - so anything that waits has to leave, and
 * this is how.
 *
 * Returns 0 when there is nobody else to run, which the caller must
 * handle by returning to ring 3 rather than looping: the timer only
 * advances while a task is out there. */
int sched64_yield_current(const registers64_t* regs, registers64_t* out_regs,
                          vmspace64_t* out_space, uint64_t* out_fs_base) {
    int next;

    if (current < 0 || !tasks[current].used) return 0;

    tasks[current].fs_base = read_fs_base();
    tasks[current].regs    = *regs;

    next = next_task(current);
    if (next == current) return 0;

    tasks[current].gs_base = read_gs_base();
    fpu_save(tasks[current].fpu);
    current = next;
    fpu_restore(tasks[next].fpu);
    write_gs_base(tasks[next].gs_base);
    if (out_regs)    *out_regs    = tasks[next].regs;
    if (out_space)   *out_space   = tasks[next].space;
    if (out_fs_base) *out_fs_base = tasks[next].fs_base;
    proc64_set_current(tasks[next].pid);
    return 1;
}

int sched64_wake(uint64_t addr, int max) {
    int woken = 0;

    for (int i = 0; i < SCHED64_MAX_TASKS && woken < max; i++) {
        if (!tasks[i].used || !tasks[i].blocked) continue;
        if (tasks[i].wait_addr != addr) continue;
        tasks[i].blocked   = 0;
        tasks[i].wait_addr = 0;
        /* futex(2) returns 0 to a waiter that was woken. The value goes
         * into the saved frame, because that frame IS the thread. */
        tasks[i].regs.rax = tasks[i].wake_rax;
        woken++;
    }
    return woken;
}

void sched64_tick(registers64_t* frame) {
    int next;

    if (current < 0 || task_total < 2) return;

    next = next_task(current);
    if (next == current) return;

    /* Everything the outgoing task is, as the CPU and the stub left it -
     * plus the thread pointer, which lives in an MSR rather than in the
     * frame, so nothing else would have saved it. */
    tasks[current].regs    = *frame;
    tasks[current].fs_base = read_fs_base();
    tasks[current].gs_base = read_gs_base();
    fpu_save(tasks[current].fpu);

    current = next;
    fpu_restore(tasks[current].fpu);
    write_gs_base(tasks[current].gs_base);

    /* And the incoming task, written over the same frame - iretq reloads
     * from exactly this memory when the stub returns. */
    *frame = tasks[current].regs;
    vmspace64_switch(&tasks[current].space);
    write_fs_base(tasks[current].fs_base);
    /* The process changes with the thread. Miss this and the incoming
     * thread runs with the outgoing one's open files and heap. */
    proc64_set_current(tasks[current].pid);
    switch_count++;

    /* Ending the run by redirecting a resume rather than by counting
     * iterations inside the task: every other register is restored
     * untouched, so the task arrives at the exit stub with the pointer
     * it was using still in place. */
    if (stop_after && switch_count >= stop_after && stop_rip)
        frame->rip = stop_rip;
}
