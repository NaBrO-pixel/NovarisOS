/* sched64.c - round-robin preemption, done inside the timer interrupt. */

#include "sched64.h"
#include "clock64.h"
#include "smp64.h"
#include "proc64.h"
#include "idt64.h"
#include "gdt64.h"
#include "serial64.h"

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

    /* What rax becomes when the deadline below ends the wait instead -
     * which futex needs to tell apart from being woken (-ETIMEDOUT
     * against 0). Everything else that sleeps is re-issued on waking
     * and asks the clock itself, so for it the two are the same. */
    uint64_t      timeout_rax;

    /* FUTEX_WAIT_BITSET's mask: a FUTEX_WAKE_BITSET releases this
     * thread only if the two masks share a bit. All ones for every
     * other kind of wait, which any wake matches. */
    uint32_t      wait_bits;

    /* The tick this thread stops waiting at, or 0 for "no deadline".
     *
     * Without it a wait that has a timeout cannot sleep: it has to
     * reach ring 3 to notice its own deadline, so it yields around a
     * loop instead. That is what a poll(2) with nothing ready did -
     * 13,870,656 times in one chrome.exe run, 99% of every syscall the
     * machine made, while chrome's own processes got 0.3% and timed
     * out. */
    uint64_t      deadline;

    /* For a wait that resumes past its syscall rather than re-running it
     * - futex, whose wake is a return value - the syscall number to
     * re-run it with if a *signal* is what ends the wait. 0 for the
     * waits that are re-run anyway. See sched64_signal_restart. */
    uint64_t      signal_restart;

    int           pid;       /* the process this thread belongs to      */
    int           cpu;       /* the CPU it runs on: its process's (smp64) */
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

/* A task this CPU may run: its process is pinned here (smp64.c). With one
 * CPU every task is CPU 0's. */
static int mine(int i) {
    return tasks[i].used && tasks[i].cpu == smp64_this();
}
static int      task_total;
static int      current = -1;
/* One past the highest slot handed out since sched64_init. Every scan of
 * the table stops here rather than at SCHED64_MAX_TASKS: the table has
 * room for 1024 tasks and a busy run has a few dozen, and the scans run on
 * every timer tick (deadlines) and every wake - a pipe write, a futex
 * wake - so a thousand empty slots were walked tens of thousands of times
 * a second. */
static int task_hwm;

static uint64_t switch_count;

/* How long a task keeps the processor before the timer takes it away, in
 * ticks (milliseconds). It was one: every tick switched, so with forty
 * Chromium tasks runnable the machine changed task a thousand times a
 * second, and each change between processes reloads CR3 - which under
 * QEMU's emulation throws the whole software TLB away, to be refilled a
 * page walk at a time. A task that blocks still gives the processor up
 * at once; this only bounds how long one that does not may run. */
#ifndef SCHED64_SLICE_TICKS
#define SCHED64_SLICE_TICKS 4
#endif
static uint64_t slice_start;
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
    task_hwm = 0;
    task_total = 0;
    current = -1;
    switch_count = 0;
    stop_after = 0;
    stop_rip = 0;
}

/* Says once that the task table is full.
 *
 * The process table learned to report this and it settled a question
 * that would otherwise have been argued from the wrong end. This table
 * is the other half of the same ceiling - a process needs a task to run
 * - and running out of tasks surfaces as clone(2) returning -EAGAIN,
 * exactly as running out of processes does. Without a line here the two
 * are indistinguishable from the log. */
static void tasks_full(void) {
    static int said;
    int blocked = 0, running = 0;
    if (said) return;
    said = 1;
    for (int k = 0; k < task_hwm; k++)
        if (tasks[k].blocked) blocked++; else running++;
    serial64_puts("NOVARIS64: [sched] all ");
    serial64_putdec((uint64_t)SCHED64_MAX_TASKS);
    serial64_puts(" task slots in use - ");
    serial64_putdec((uint64_t)running);
    serial64_puts(" runnable, ");
    serial64_putdec((uint64_t)blocked);
    serial64_puts(" blocked\n");
}

/* Every task, and what it is waiting for.
 *
 * A deadlock is not visible from any one process: each one is simply
 * blocked, which is what a healthy idle thread looks like too. What
 * distinguishes them is the graph - who waits on what, and whether
 * anything is runnable at all - and that only exists in this table.
 * Printed at the moment the run gives up, which is the one instant the
 * whole system is known to be stuck.
 *
 * wait_addr is the key the task blocked on: a futex address for a lock,
 * or the encoded pipe key for a read that is waiting for bytes. Two
 * tasks naming the same key are waiting for the same thing; a key
 * nobody will ever signal is the deadlock. */
void sched64_dump_blocked(void) {
    int live = 0, blocked = 0;

    serial64_puts("\nNOVARIS64: [tasks] --- every live task at the deadline ---\n");
    for (int i = 0; i < task_hwm; i++) {
        if (!tasks[i].used) continue;
        live++;
        if (tasks[i].blocked) blocked++;
        serial64_puts("NOVARIS64: [tasks] slot ");
        serial64_putdec((uint64_t)i);
        serial64_puts(" pid ");
        serial64_putdec((uint64_t)tasks[i].pid);
        serial64_puts(tasks[i].blocked ? " BLOCKED on " : " runnable   ");
        if (tasks[i].blocked) serial64_puthex(tasks[i].wait_addr);
        serial64_puts(" rip=");
        serial64_puthex(tasks[i].regs.rip);
        serial64_putc('\n');
    }
    serial64_puts("NOVARIS64: [tasks] ");
    serial64_putdec((uint64_t)live);
    serial64_puts(" live, ");
    serial64_putdec((uint64_t)blocked);
    serial64_puts(" blocked, ");
    serial64_putdec((uint64_t)(live - blocked));
    serial64_puts(" runnable\n");
}

/* Enough of a task to read its stacks from outside: its process, what
 * it waits on, its GS base - a Wine thread's TEB - its ring-3 stack
 * pointer and its address space. 0 if the slot is empty. */
int sched64_task_peek(int i, int* pid, uint64_t* wait, uint64_t* gs,
                      uint64_t* rsp, vmspace64_t* space) {
    if (i < 0 || i >= SCHED64_MAX_TASKS || !tasks[i].used) return 0;
    *pid   = tasks[i].pid;
    *wait  = tasks[i].blocked ? tasks[i].wait_addr : 0;
    *gs    = tasks[i].gs_base;
    *rsp   = tasks[i].regs.rsp;
    *space = tasks[i].space;
    return 1;
}

/* What slot `i` is blocked on, or 0 if it is not blocked. Lets a layer
 * that knows what the wait keys mean - syscall64.c owns PIPE64_WAIT_KEY
 * - say something useful about them. */
uint64_t sched64_blocked_on(int i) {
    if (i < 0 || i >= SCHED64_MAX_TASKS) return 0;
    if (!tasks[i].used || !tasks[i].blocked) return 0;
    return tasks[i].wait_addr;
}

int sched64_blocked_pid(int i) {
    if (i < 0 || i >= SCHED64_MAX_TASKS) return -1;
    if (!tasks[i].used || !tasks[i].blocked) return -1;
    return tasks[i].pid;
}

int sched64_pid_tasks(int pid) {
    int n = 0;
    for (int i = 0; i < task_hwm; i++)
        if (tasks[i].used && tasks[i].pid == pid) n++;
    return n;
}

int sched64_space_in_use(uint64_t pml4_phys) {
    if (!pml4_phys) return 1;                  /* never free nothing */
    for (int i = 0; i < task_hwm; i++)
        if (tasks[i].used && tasks[i].space.pml4_phys == pml4_phys)
            return 1;
    return 0;
}

int sched64_add(uint64_t rip, uint64_t rsp, uint64_t arg,
                const vmspace64_t* space) {
    int i;

    for (i = 0; i < SCHED64_MAX_TASKS; i++) if (!tasks[i].used) break;
    if (i < SCHED64_MAX_TASKS && i + 1 > task_hwm) task_hwm = i + 1;
    if (i == SCHED64_MAX_TASKS) { tasks_full(); return -1; }

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
    tasks[i].cpu       = proc64_cpu_of(tasks[i].pid);
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
    if (i < SCHED64_MAX_TASKS && i + 1 > task_hwm) task_hwm = i + 1;
    if (i == SCHED64_MAX_TASKS) { tasks_full(); return -1; }

    tasks[i].pid       = pid;
    tasks[i].cpu       = proc64_cpu_of(pid);
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

/* Defined with the rest of the idle machinery further down; needed
 * here because a thread can exit while every one of its siblings is
 * asleep on the clock. */
static int any_deadline(void);
static int idle_until_runnable(void);

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
    for (next = 0; next < task_hwm; next++)
        if (mine(next) && !tasks[next].blocked) break;

    /* Nothing runnable at this instant is not the same as nothing left
     * to run: a sibling asleep on the clock becomes runnable when the
     * timer says so. Without this, a thread that exits while every
     * other thread of its process is in nanosleep ends the whole run -
     * measured in the tgkill differential as a program that stopped
     * printing the moment its second thread finished, having passed
     * every assertion up to that one.
     *
     * The guard is the one the blocking path uses: halting is safe only
     * while the timer can still interrupt and something is genuinely
     * waiting on it. Otherwise there really is nothing to come back
     * for. */
    if (next == task_hwm && (smp64_this() != 0 ||
        (!idt64_irq_is_masked(0) && any_deadline())))
        next = idle_until_runnable();

    if (next < 0 || next >= task_hwm) { current = -1; return 0; }

    /* Nothing to save: the task that was running has ended. */
    current = next;
    slice_start = clock64_ticks();
    fpu_restore(tasks[next].fpu);
    write_gs_base(tasks[next].gs_base);
    if (out_regs)     *out_regs     = tasks[next].regs;
    if (out_space)    *out_space    = tasks[next].space;
    if (out_fs_base)  *out_fs_base  = tasks[next].fs_base;
    proc64_set_current(tasks[next].pid);
    return 1;
}

/* Every task of `pid` out of the table, without choosing a successor:
 * for a process ending when there is nothing to hand the CPU to - the
 * leader of a run, whose ending ends the run. Returns how many went. */
int sched64_remove_pid(int pid) {
    int n = 0;
    for (int i = 0; i < task_hwm; i++) {
        if (!tasks[i].used || tasks[i].pid != pid) continue;
        tasks[i].used = 0;
        if (task_total > 0) task_total--;
        if (i == current) current = -1;
        n++;
    }
    return n;
}

int sched64_exit_process(int pid, registers64_t* out_regs,
                         vmspace64_t* out_space, uint64_t* out_fs_base) {
    int next;

    /* exit_group ends every thread of the process, not just the one
     * that called it. Ending only the caller would leave its siblings
     * running in an address space nobody owns - and, when a test's two
     * tasks belong to the same process, would hand the CPU to the other
     * one instead of finishing the run. */
    for (int i = 0; i < task_hwm; i++) {
        if (!tasks[i].used || tasks[i].pid != pid) continue;
        tasks[i].used = 0;
        if (task_total > 0) task_total--;
    }

    /* Runnable, for the same reason as above - and idle rather than
     * give up, for the same reason as above too: one process ending
     * while another sleeps on the clock is not the end of the run. */
    for (next = 0; next < task_hwm; next++)
        if (mine(next) && !tasks[next].blocked) break;

    if (next == task_hwm && (smp64_this() != 0 ||
        (!idt64_irq_is_masked(0) && any_deadline())))
        next = idle_until_runnable();

    if (next < 0 || next >= task_hwm) { current = -1; return 0; }

    /* Nothing to save: every thread of that process has ended. */
    current = next;
    slice_start = clock64_ticks();
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
/* True while the machine has nothing to run and is waiting for an
 * interrupt. `current` still names the thread that parked, and its
 * frame is already saved, so the timer must not try to switch away from
 * it: there is no live task frame to save over. */
/* Per CPU (Milestone 101): each one idles on its own. */
static volatile int idling_cpu[SMP64_MAX_CPUS];
#define idling (idling_cpu[smp64_this()])
volatile int sched64_external_wakers;

static int first_runnable(void) {
    for (int i = 0; i < task_hwm; i++)
        if (mine(i) && !tasks[i].blocked) return i;
    return -1;
}

/* Whether anything is waiting on the clock rather than on an event.
 *
 * This is the difference between idle and deadlock. If some thread has
 * a deadline then the timer will release it and the machine has a
 * future; if none has, and nothing is runnable, then nothing will ever
 * happen and halting would be a hang. */
static int any_deadline(void) {
    for (int i = 0; i < task_hwm; i++)
        if (tasks[i].used && tasks[i].blocked && tasks[i].deadline) return 1;
    return 0;
}

/* Release every blocked thread whose deadline has passed. The clock
 * notices the timeout, which is what lets the thread sleep through it
 * rather than spin to watch for it. A thread woken early re-checks what
 * it was waiting for and parks again, so waking one costs a re-check. */
static void expire_deadlines(void) {
    uint64_t now = clock64_ticks();

    for (int i = 0; i < task_hwm; i++) {
        if (!tasks[i].used || !tasks[i].blocked) continue;
        if (!tasks[i].deadline || now < tasks[i].deadline) continue;
        tasks[i].blocked   = 0;
        tasks[i].wait_addr = 0;
        tasks[i].deadline  = 0;
        tasks[i].regs.rax  = tasks[i].timeout_rax;
    }
}

/* Halt until an interrupt makes something runnable.
 *
 * Syscalls run with interrupts off - FMASK clears IF on entry and the
 * stub never turns it back on - so a kernel with nothing to run must
 * enable them itself or the tick that would end the wait can never
 * arrive. `sti; hlt` is one unit on x86: sti defers enabling by one
 * instruction precisely so that the halt cannot be missed. cli again on
 * the way out, because the caller is still inside a syscall and the
 * rest of it expects interrupts off. */
/* Cycles spent halted with nothing to run, for the [cycles] report. */
volatile uint64_t sched64_idle_cycles;

static inline uint64_t idle_tsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static int idle_until_runnable(void) {
    int n;
    uint64_t t0 = idle_tsc();

    idling = 1;
    for (;;) {
        /* With other CPUs, the lock goes for the halt: they may be the
         * ones to make something here runnable. */
        int depth;
        smp64_set_halted(1);
        depth = smp64_release_all();
        smp64_kernel_space();
        __asm__ __volatile__("sti; hlt; cli");
        smp64_reacquire(depth);
        smp64_set_halted(0);
        if (smp64_this() != 0 && smp64_quiescing()) {
            idling = 0;
            smp64_ap_idle();                       /* the run is over */
        }
        expire_deadlines();
        n = first_runnable();
        if (n >= 0) break;
        /* Nothing anyone could wake: the end of the run - on the
         * bootstrap CPU alone. An AP waits for whatever is pinned to it. */
        if (smp64_this() == 0 && !smp64_active &&
            !any_deadline() && !sched64_external_wakers) { n = -1; break; }
    }
    idling = 0;
    sched64_idle_cycles += idle_tsc() - t0;
    return n;
}

static int next_task(int from) {
    for (int n = 1; n <= task_hwm; n++) {
        int i = (from + n) % task_hwm;
        if (mine(i) && !tasks[i].blocked) return i;
    }
    return from;
}

int sched64_block_current(const registers64_t* regs, uint64_t addr,
                          uint64_t wake_rax,
                          registers64_t* out_regs, vmspace64_t* out_space,
                          uint64_t* out_fs_base) {
    return sched64_block_until(regs, addr, wake_rax, 0,
                               out_regs, out_space, out_fs_base);
}

int sched64_block_until(const registers64_t* regs, uint64_t addr,
                        uint64_t wake_rax, uint64_t deadline,
                        registers64_t* out_regs, vmspace64_t* out_space,
                        uint64_t* out_fs_base) {
    return sched64_block_until_ex(regs, addr, wake_rax, wake_rax, deadline,
                                  0xFFFFFFFFu, out_regs, out_space,
                                  out_fs_base);
}

static uint64_t next_signal_restart;

void sched64_signal_restart(uint64_t nr) { next_signal_restart = nr; }

/* A thread woken by a signal, rather than by what it waited for.
 *
 * Most waits resume by re-running their syscall, and the re-run sees the
 * signal and returns -EINTR, with the delivery at the end of the call.
 * A futex wait cannot work that way - a wake that leaves the word
 * unchanged has to return 0, not sleep again - so it resumes past the
 * syscall, and a signal that woke it went straight back to ring 3 with
 * nothing delivering it. The thread ran on to its next syscall, and a
 * SIGKILL'd child calling _exit() exited with its own status instead of
 * dying. Pointed back at its syscall instead, the re-run is what takes
 * the signal. */
static void woken_by_signal(int i) {
    tasks[i].blocked   = 0;
    tasks[i].wait_addr = 0;
    if (tasks[i].signal_restart) {
        tasks[i].regs.rip -= 2;                 /* the syscall instruction */
        tasks[i].regs.rax  = tasks[i].signal_restart;
        tasks[i].signal_restart = 0;
    } else {
        tasks[i].regs.rax = tasks[i].wake_rax;
    }
}

int sched64_block_until_ex(const registers64_t* regs, uint64_t addr,
                           uint64_t wake_rax, uint64_t timeout_rax,
                           uint64_t deadline, uint32_t bits,
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
    tasks[current].wait_addr   = addr;
    tasks[current].wake_rax    = wake_rax;
    tasks[current].timeout_rax = timeout_rax;
    tasks[current].wait_bits   = bits;
    tasks[current].deadline    = deadline;
    tasks[current].signal_restart = next_signal_restart;
    next_signal_restart = 0;

    next = next_task(current);
    if (next == current || tasks[next].blocked) {
        /* Nothing else can run *right now*.
         *
         * That used to mean deadlock, and it was a fair reading while
         * every wait was a yield-loop: a waiting thread stayed
         * runnable, so "nobody else can run" really did mean nobody
         * ever would. Once waits sleep, a parent waiting on a sleeping
         * child is this, and it is ordinary.
         *
         * So: halt and let the clock decide. Only when something is
         * actually waiting on the clock - otherwise nothing will ever
         * happen and halting is a hang, and the old answer is the
         * honest one. The timer being masked says the same thing: most
         * bring-up layers run with IRQ0 off on purpose, and there the
         * deadline would never arrive. */
        if (idt64_irq_is_masked(0) ||
            (!any_deadline() && !sched64_external_wakers && !smp64_active)) {
            tasks[current].blocked   = 0;
            tasks[current].wait_addr = 0;
            tasks[current].deadline  = 0;
            return 0;
        }

        next = idle_until_runnable();
        if (next < 0) {
            tasks[current].blocked   = 0;
            tasks[current].wait_addr = 0;
            tasks[current].deadline  = 0;
            return 0;
        }
    }

    tasks[current].gs_base = read_gs_base();
    fpu_save(tasks[current].fpu);
    current = next;
    slice_start = clock64_ticks();
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
    slice_start = clock64_ticks();
    fpu_restore(tasks[next].fpu);
    write_gs_base(tasks[next].gs_base);
    if (out_regs)    *out_regs    = tasks[next].regs;
    if (out_space)   *out_space   = tasks[next].space;
    if (out_fs_base) *out_fs_base = tasks[next].fs_base;
    proc64_set_current(tasks[next].pid);
    return 1;
}

/* Wake every blocked thread of one process, whatever it is waiting on.
 *
 * This is what a signal does to a wait. A thread blocked in read(2) on
 * an empty pipe, or in wait4, or in an untimed futex, never reaches the
 * boundary where a pending signal is taken - so without this, kill(2)
 * to a process that is asleep is a signal nothing will ever deliver.
 * Linux interrupts a blocking syscall for exactly this reason.
 *
 * The woken thread re-runs its syscall, and the delivery point at the
 * end of syscall64_dispatch is where the signal is taken. */
int sched64_wake_pid(int pid) {
    int woken = 0;

    for (int i = 0; i < task_hwm; i++) {
        if (!tasks[i].used || !tasks[i].blocked) continue;
        if (tasks[i].pid != pid) continue;
        woken_by_signal(i);
        woken++;
    }
    if (woken) smp64_kick_halted();
    return woken;
}

int sched64_task_pid(int tid) {
    if (tid < 0 || tid >= SCHED64_MAX_TASKS) return -1;
    if (!tasks[tid].used) return -1;
    return tasks[tid].pid;
}

int sched64_wake_tid(int tid) {
    if (tid < 0 || tid >= SCHED64_MAX_TASKS) return 0;
    if (!tasks[tid].used || !tasks[tid].blocked) return 0;
    woken_by_signal(tid);
    smp64_kick_halted();
    return 1;
}

int sched64_wake(uint64_t addr, int max) {
    return sched64_wake_bits(addr, max, 0xFFFFFFFFu);
}

int sched64_wake_bits(uint64_t addr, int max, uint32_t bits) {
    return sched64_wake_futex(addr, max, bits, -1);
}

int sched64_wake_futex(uint64_t addr, int max, uint32_t bits, int pid) {
    int woken = 0;

    for (int i = 0; i < task_hwm && woken < max; i++) {
        if (!tasks[i].used || !tasks[i].blocked) continue;
        if (tasks[i].wait_addr != addr) continue;
        /* A private futex is an address in one process, and the same
         * number in another process is a different word. Without this a
         * FUTEX_WAKE(addr, 1) could spend its one wake on a thread of
         * some other process parked at the same address - which Wine
         * makes common, since ntdll sits at the same place in every Wine
         * process - and that thread re-checks its own word, sleeps
         * again, and the waiter the wake was for never hears of it. */
        if (pid >= 0 && tasks[i].pid != pid) continue;
        if (!(tasks[i].wait_bits & bits)) continue;   /* a different bitset */
        tasks[i].blocked   = 0;
        tasks[i].wait_addr = 0;
        /* futex(2) returns 0 to a waiter that was woken. The value goes
         * into the saved frame, because that frame IS the thread. */
        tasks[i].regs.rax = tasks[i].wake_rax;
        woken++;
    }

    /* Any wake at all also releases the threads parked on the shared
     * condition key, uncounted. A poll(2) cannot name one address - it
     * waits for whichever descriptor becomes ready first - so it parks
     * on one key and re-checks its whole set when anything anywhere
     * changes. Wider than it needs to be, and still far cheaper than
     * the spin it replaces; a spurious wake costs one re-check. */
    for (int i = 0; i < task_hwm; i++) {
        if (!tasks[i].used || !tasks[i].blocked) continue;
        if (tasks[i].wait_addr != SCHED64_COND_KEY) continue;
        tasks[i].blocked   = 0;
        tasks[i].wait_addr = 0;
        tasks[i].deadline  = 0;
        tasks[i].regs.rax  = tasks[i].wake_rax;
    }
    /* A task woken here may belong to a CPU that is halted with nothing
     * to do; it would only notice at its next timer tick, a millisecond
     * on - which, on every round trip between a Wine client and its
     * server, is what made four CPUs slower than one. */
    smp64_kick_halted();
    return woken;
}

void sched64_tick(registers64_t* frame) {
    int next;

    expire_deadlines();

    /* While idling there is no running task whose frame this is - it is
     * the idle loop's - so switching would save the halt over a parked
     * thread's continuation. idle_until_runnable picks the successor
     * itself once expire_deadlines above has produced one. */
    if (idling) return;

    if (current < 0 || task_total < 2) return;
    if (clock64_ticks() - slice_start < SCHED64_SLICE_TICKS) return;

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
    slice_start = clock64_ticks();
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

/* For smp64.c, which keeps each CPU's `current` while another holds the
 * lock: the value alone, nothing else touched. */
void sched64_set_current_raw(int i) { current = i; }

/* An application processor with nothing running: start a task pinned to
 * it, if there is one. Does not return if there is. */
int sched64_run_this_cpu(void) {
    int n = first_runnable();
    if (n < 0) return 0;
    current = n;
    slice_start = clock64_ticks();
    fpu_restore(tasks[n].fpu);
    write_gs_base(tasks[n].gs_base);
    write_fs_base(tasks[n].fs_base);
    vmspace64_switch(&tasks[n].space);
    proc64_set_current(tasks[n].pid);
    sched64_resume(&tasks[n].regs);            /* releases the lock */
    return 1;
}

/* Into a task, for good. The frame is copied to this CPU's own buffer and
 * the lock given up before the assembly loads it: past that point another
 * CPU may change the task table. */
extern void sched64_resume_raw(const registers64_t* regs) __attribute__((noreturn));
void sched64_resume(const registers64_t* regs) {
    static registers64_t copy[SMP64_MAX_CPUS];
    registers64_t* mine_copy = &copy[smp64_this()];
    *mine_copy = *regs;
    smp64_leave_to_user();
    sched64_resume_raw(mine_copy);
}

/* A task interrupted on an AP at the end of a run: its frame saved as a
 * switch away would save it, so it resumes correctly if scheduling starts
 * again, and nothing current on this CPU. */
void sched64_park_current(const registers64_t* frame) {
    if (current < 0 || !tasks[current].used) return;
    tasks[current].regs    = *frame;
    tasks[current].fs_base = read_fs_base();
    tasks[current].gs_base = read_gs_base();
    fpu_save(tasks[current].fpu);
    current = -1;
}
