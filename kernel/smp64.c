/* smp64.c - the other processors.
 *
 * NovarisOS ran on one CPU. Under QEMU's software emulation about 80% of a
 * Chromium run is the programs' own code (Milestone 100), and QEMU can
 * emulate several CPUs on several host threads at once - but only for a
 * guest that has more than one. This file finds and starts them.
 *
 * Starting one is the standard dance: the local APIC on the bootstrap
 * processor sends the target an INIT, then two STARTUPs naming a page
 * below 1MB, and the target begins there in 16-bit real mode. That page
 * is boot/ap_trampoline.s, which takes it to long mode under a small page
 * table of its own and calls smp64_ap_entry here on a stack of its own.
 *
 * The APIC IDs QEMU gives its CPUs are 0, 1, 2 ... in order, so the CPUs
 * are found by starting ID 1, 2, ... until one does not answer.
 */

#include <stdint.h>
#include "smp64.h"
#include "paging64.h"
#include "serial64.h"
#include "clock64.h"
#include "kstring.h"
#include "idt64.h"
#include "gdt64.h"
#include "proc64.h"

#define KERNEL_VMA_ 0xFFFFFFFF80000000ULL

/* The local APIC, mapped in the MMIO window above the framebuffer. */
#define LAPIC_VIRT  (MMIO64_BASE + 0x100000000ULL)
#define LAPIC_ID       0x020
#define LAPIC_EOI      0x0B0
#define LAPIC_SVR      0x0F0
#define LAPIC_ICR_LO   0x300
#define LAPIC_ICR_HI   0x310
#define LAPIC_LVT_TMR  0x320
#define LAPIC_TMR_INIT 0x380
#define LAPIC_TMR_CUR  0x390
#define LAPIC_TMR_DIV  0x3E0

#define TRAMPOLINE_PHYS 0x8000ULL

extern const uint8_t ap_trampoline_bin[];
extern const uint64_t ap_trampoline_len;

static volatile uint32_t* lapic;
static int ncpus = 1;
static volatile int online[SMP64_MAX_CPUS];
static uint64_t kernel_cr3;

/* The trampoline's page tables: an identity map of the first 2MB (where
 * the trampoline runs while paging comes on) beside the kernel's own upper
 * half. Static, so in the kernel image and below 4GB - the trampoline
 * loads CR3 with a 32-bit mov. */
static uint64_t tr_pml4[512] __attribute__((aligned(4096)));
static uint64_t tr_pdpt[512] __attribute__((aligned(4096)));
static uint64_t tr_pd[512]   __attribute__((aligned(4096)));

/* Each AP's first stack: where it runs until it has a task. */
static uint8_t ap_stacks[SMP64_MAX_CPUS][16384] __attribute__((aligned(16)));

static void ap_timer_handler(registers64_t* r);
static void send_kick(int c);

static uint64_t image_phys(const void* p) { return (uint64_t)p - KERNEL_VMA_; }

static uint32_t lapic_read(uint32_t reg) { return lapic[reg / 4]; }
static void lapic_write(uint32_t reg, uint32_t v) {
    lapic[reg / 4] = v;
    (void)lapic[LAPIC_ID / 4];               /* posted write: read it back */
}

static uint64_t read_msr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t read_cr3_(void) {
    uint64_t v;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
    return v;
}

/* Waits `ms` milliseconds on the PIT-driven clock, which the caller has
 * running. */
static void wait_ms(uint64_t ms) {
    uint64_t until = clock64_ticks() + ms;
    while (clock64_ticks() < until) __asm__ __volatile__("hlt");
}

int smp64_cpus(void) { return ncpus; }

uint32_t smp64_lapic_id(void) {
    return lapic ? lapic_read(LAPIC_ID) >> 24 : 0;
}

void smp64_lapic_eoi(void) {
    if (lapic) lapic_write(LAPIC_EOI, 0);
}

static void lapic_enable(void) {
    /* Software-enable, spurious vector 0xFF. */
    lapic_write(LAPIC_SVR, 0x100 | 0xFF);
}

/* --- the big kernel lock ------------------------------------------------- *
 *
 * One lock for the whole kernel, held by whichever CPU is executing kernel
 * code: taken on every way in (a system call, an interrupt, an exception),
 * dropped on every way back out to ring 3 and around every halt. The
 * kernel was written for one processor, and this keeps it so - at most one
 * CPU is ever inside it - while the programs themselves, which are most of
 * the work, run on all of them at once.
 *
 * It is recursive: an interrupt arriving while this CPU already holds it
 * (kernel_main runs with interrupts on; the idle loop does) only counts.
 *
 * Who is running is not the lock's business: the scheduler's current task
 * and proc64's current pid are kept per CPU, indexed by gdt64_cpu, so they
 * read the same with or without the lock.
 *
 * None of it does anything until smp64_start_scheduling: with one CPU
 * running tasks there is nobody to exclude. */

volatile int smp64_active;

/* What the lock costs, per CPU: how often it was taken, how often it had
 * to be waited for, and the cycles spent waiting (smp64_report). */
static uint64_t smp64_spin_cycles[SMP64_MAX_CPUS], smp64_contended[SMP64_MAX_CPUS],
                smp64_acquired[SMP64_MAX_CPUS], smp64_kicks;
/* And where the rest of each CPU's time goes: holding the lock, and
 * halted with nothing to run. What is neither is ring 3. */
static uint64_t smp64_held_cycles[SMP64_MAX_CPUS], held_since[SMP64_MAX_CPUS];
uint64_t smp64_idle_cycles[SMP64_MAX_CPUS];
static uint64_t smp64_t_start, smp64_busy_wakes;

static inline uint64_t tsc_now(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
int smp64_cpu;                          /* the lock holder's CPU */

/* Milestone 105: threads of one process on several CPUs. */
static volatile int flush_req[SMP64_MAX_CPUS];
static volatile int dead_pending[SMP64_MAX_CPUS];
static volatile int bsp_abort;
static void flush_service(int cpu);
static void bsp_abort_now(void) __attribute__((noreturn));
static void flush_and_free(int cpu);
static int  ndefer[SMP64_MAX_CPUS], mm_dirty[SMP64_MAX_CPUS];
static proc64_t* mm_held[SMP64_MAX_CPUS];
void smp64_mm_unlock(proc64_t* p);

static volatile int bkl;
static volatile int bkl_owner = -1;
static int bkl_depth;


static int this_cpu(void) {
    int c = gdt64_cpu();
    return c >= 0 && c < SMP64_MAX_CPUS ? c : 0;
}

void smp64_enter(void) {
    int cpu;
    if (!smp64_active) return;
    cpu = this_cpu();
    if (bkl_owner == cpu) { bkl_depth++; return; }
    if (__atomic_exchange_n(&bkl, 1, __ATOMIC_ACQUIRE)) {
        uint64_t t0 = tsc_now();
        while (__atomic_exchange_n(&bkl, 1, __ATOMIC_ACQUIRE))
            while (bkl) {
                /* Waiting for the lock is where a CPU is when the holder
                 * needs it to flush its TLB, or to leave a process that
                 * is being torn down (smp64_shootdown) - so it answers
                 * here, or the two wait for each other. */
                if (flush_req[cpu]) flush_service(cpu);
                if (cpu == 0 && bsp_abort) bsp_abort_now();
                __asm__ __volatile__("pause");
            }
        smp64_spin_cycles[cpu] += tsc_now() - t0;
        smp64_contended[cpu]++;
    }
    smp64_acquired[cpu]++;
    held_since[cpu] = tsc_now();
    bkl_owner = cpu;
    bkl_depth = 1;
    smp64_cpu = cpu;
}

static void release(void) {
    int cpu = smp64_cpu;
    /* A spread process's memory changed under the lock: the other CPUs
     * running it flush before the lock goes, and only then are the
     * frames it gave up handed back. */
    /* The memory map's lock first: a CPU waiting for it answers flushes
     * as it waits, but the flush below should not have to wait on it. */
    if (mm_held[cpu]) {
        proc64_t* p = mm_held[cpu];
        mm_held[cpu] = 0;
        while (p->mm_owner == cpu) smp64_mm_unlock(p);
    }
    if (ndefer[cpu] || mm_dirty[cpu]) flush_and_free(cpu);
    smp64_held_cycles[cpu] += tsc_now() - held_since[cpu];
    bkl_depth = 0;
    bkl_owner = -1;
    __atomic_store_n(&bkl, 0, __ATOMIC_RELEASE);
}

void smp64_exit(void) {
    if (!smp64_active || bkl_owner != this_cpu()) return;
    if (--bkl_depth > 0) return;
    release();
}

/* Out to ring 3 for good: whatever nesting the kernel was in is abandoned
 * (sched64_resume, enter_user_mode64). */
void smp64_leave_to_user(void) {
    if (!smp64_active || bkl_owner != this_cpu()) return;
    release();
}

/* Around a halt: give the lock up entirely, then take it back at the same
 * depth. */
int smp64_release_all(void) {
    int depth;
    if (!smp64_active || bkl_owner != this_cpu()) return 0;
    depth = bkl_depth;
    release();
    return depth;
}

void smp64_reacquire(int depth) {
    if (!depth) return;
    smp64_enter();
    bkl_depth = depth;
}

/* kernel_main's return from a run: whatever path brought the CPU back, it
 * holds the lock once now. */
void smp64_kernel_holds(void) {
    if (!smp64_active) return;
    dead_pending[this_cpu()] = 0;
    if (bkl_owner != this_cpu()) smp64_enter();
    bkl_depth = 1;
}

/* This CPU. 0 until scheduling spreads - every caller before then is the
 * bootstrap CPU - which saves the task-register read in the scheduler's
 * scans on one CPU. */
int smp64_this(void) { return smp64_active ? this_cpu() : 0; }

int smp64_held_here(void) { return smp64_active && bkl_owner == this_cpu(); }

/* --- spreading processes ---------------------------------------------- */

static int spread_next;
static volatile int spreading;

/* The CPU for a process that has just exec'd: the one with fewest live
 * processes, `self` not counted, ties going round in turn. Its threads
 * stay with it (proc64.c), so a process's page tables are only ever
 * loaded on one CPU and nothing has to tell the others to flush their
 * TLBs. Returns -1 while not spreading: the process stays where it is. */
#ifndef SMP64_SERVER_CPU
#define SMP64_SERVER_CPU 0
#endif
int smp64_pick_cpu_for(int self) {
    int best = -1, best_n = 0;
    if (!spreading || ncpus < 2) return -1;
    /* The wineserver stays with the run's leader, on the bootstrap CPU.
     * Every Wine process talks to it in strict request and reply, the
     * browser most of all; on one CPU each reply is a switch, on two it
     * is a wake-up IPI each way, and QEMU is slow at waking a halted CPU.
     * Nothing is lost in parallel: the two take turns anyway. */
    {
        const char* comm = proc64_comm_of(self);
        if (comm && kstrcmp(comm, "wineserver") == 0) return SMP64_SERVER_CPU;
    }
    spread_next = (spread_next + 1) % ncpus;
    for (int k = 0; k < ncpus; k++) {
        int c = (spread_next + k) % ncpus;
        /* The bootstrap CPU has the leader, the wineserver, the clock
         * and the network already: it counts double. */
        int n = proc64_count_on(c, self) * (c == 0 ? 2 : 1);
        if (SMP64_SERVER_CPU && c == SMP64_SERVER_CPU) n += 1000;   /* the server's own */
        if (best < 0 || n < best_n) { best = c; best_n = n; }
    }
    return best;
}

int smp64_pick_cpu(void) {
    int c = smp64_pick_cpu_for(-1);
    return c < 0 ? 0 : c;
}

/* --- the application processors' scheduling ---------------------------- */

static uint32_t lapic_ticks_per_ms;
static uint8_t ap_int_stacks[SMP64_MAX_CPUS][16384] __attribute__((aligned(16)));
static volatile int quiesce;
static volatile int halted[SMP64_MAX_CPUS];

/* Wakes every other CPU that is halted with nothing to run, so a task
 * just made runnable there starts now rather than at that CPU's next
 * tick. A fixed IPI on SMP64_KICK_VECTOR, whose handler only
 * acknowledges it: the idle loop looks again once hlt returns. */
void smp64_kick_halted(void) {
    int me;
    if (!smp64_active) return;
    me = this_cpu();
    for (int c = 0; c < ncpus; c++) {
        if (c == me || !halted[c]) continue;
        send_kick(c);
    }
}

void smp64_set_halted(int on) {
    if (smp64_active) halted[this_cpu()] = on;
}

/* A task woken for a CPU that is running something else: that CPU is
 * sent the same IPI, and switches to it at once if it was in ring 3
 * (sched64_preempt). One IPI outstanding per CPU at most. */
static volatile int preempt_pending[SMP64_MAX_CPUS];
uint64_t smp64_timer_irqs[SMP64_MAX_CPUS];   /* every AP timer interrupt */
extern void sched64_preempt(registers64_t* frame);

static void kick_handler(registers64_t* r) {
    int me = this_cpu();
    smp64_lapic_eoi();
    if (preempt_pending[me]) {
        preempt_pending[me] = 0;
        sched64_preempt(r);
    }
}

static void send_kick(int c) {
    lapic_write(LAPIC_ICR_HI, (uint32_t)c << 24);
    lapic_write(LAPIC_ICR_LO, 0x4000 | SMP64_KICK_VECTOR);   /* fixed, assert */
    smp64_kicks++;
}
static volatile int parked[SMP64_MAX_CPUS];

extern void gdt64_install_cpu(int cpu);
extern void gdt64_set_kernel_stack_cpu(int cpu, uint64_t rsp0);
extern void idt64_load(void);
extern void syscall64_init_cpu(int cpu);
extern int  sched64_run_this_cpu(void);      /* resumes a task, or returns 0 */
extern void sched64_tick(registers64_t* frame);
extern void sched64_park_current(const registers64_t* frame);

/* The bootstrap CPU's own LAPIC timer, for sampling only: its scheduling
 * tick is the PIT, whose interrupts QEMU raises from its main thread -
 * which waits for QEMU's device lock, and loses ticks when that is busy.
 * Each CPU's LAPIC timer is raised by that CPU's own thread, so every
 * CPU is sampled the same way. */
static void sample_handler(registers64_t* r);

static void lapic_sampler_start(void) {
    lapic_write(LAPIC_TMR_DIV, 0x3);
    lapic_write(LAPIC_LVT_TMR, (1u << 17) | SMP64_SAMPLE_VECTOR);
    lapic_write(LAPIC_TMR_INIT, 4 * (lapic_ticks_per_ms ? lapic_ticks_per_ms : 100000));
}

static void lapic_timer_start(void) {
    lapic_write(LAPIC_TMR_DIV, 0x3);                     /* divide by 16 */
    lapic_write(LAPIC_LVT_TMR, (1u << 17) | SMP64_TIMER_VECTOR);   /* periodic */
    /* Every 4ms: the scheduler's timeslice (sched64.c), so a tick
     * between slices would do nothing but cost an APIC access. */
    lapic_write(LAPIC_TMR_INIT, 4 * (lapic_ticks_per_ms ? lapic_ticks_per_ms : 100000));
}

static void kernel_space(void) {
    smp64_note_cr3(kernel_cr3);
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(kernel_cr3) : "memory");
}

/* An AP with nothing to do: no lock, the kernel's own page tables (the
 * process it last ran may be gone, and its tables with it), interrupts on
 * for the timer. Picks up any task pinned to it. */
/* A halt's length, less what the interrupts that ended it spent spinning
 * for the lock and holding it: those are counted as such already. */
void smp64_note_idle(int me, uint64_t dt, uint64_t sp0, uint64_t hd0) {
    uint64_t other = (smp64_spin_cycles[me] - sp0) + (smp64_held_cycles[me] - hd0);
    smp64_idle_cycles[me] += dt > other ? dt - other : 0;
}

/* An interrupt that found this CPU halted (idt64.c): acknowledged and
 * nothing else - the halt loop looks for work itself. A preemption asked
 * for meanwhile is moot: there is nothing running to preempt. */
static void sample_handler(registers64_t* r) {
    smp64_lapic_eoi();
    smp64_note_tick(0, r, 4);
}

/* The sampler's interrupt, before any lock (idt64.c). */
void smp64_sample_irq(void* frame) { sample_handler((registers64_t*)frame); }

void smp64_idle_irq(int vector) {
    int me = this_cpu();
    if (vector == SMP64_TIMER_VECTOR) smp64_timer_irqs[me]++;
    preempt_pending[me] = 0;
    smp64_lapic_eoi();
}

int smp64_idle_here(void) {
    return smp64_active && halted[this_cpu()];
}

extern int sched64_maybe_work(int cpu);

void smp64_ap_idle(void) __attribute__((noreturn));
void smp64_ap_idle(void) {
    for (;;) {
        smp64_enter();
        /* Nothing is running here to have been ended. */
        dead_pending[this_cpu()] = 0;
        /* Only once scheduling is on: before that the lock is not live
         * and "this CPU's tasks" would read as the bootstrap CPU's. */
        if (smp64_active && !quiesce) {
            parked[this_cpu()] = 0;
            sched64_run_this_cpu();                      /* returns only if none */
        }
        parked[this_cpu()] = 1;
        halted[this_cpu()] = 1;
        {
            int me = this_cpu();
            smp64_leave_to_user();
            uint64_t t0, sp0, hd0;
            kernel_space();
            t0 = tsc_now(); sp0 = smp64_spin_cycles[me]; hd0 = smp64_held_cycles[me];
            /* Halted until something here looks runnable - read without
             * the lock, so the busy CPUs are not held up while this one
             * has nothing to do. A wrong guess either way costs one
             * look under the lock, or the next tick. */
            do {
                __asm__ __volatile__("sti; hlt; cli");
            } while (smp64_active && !quiesce && !sched64_maybe_work(me));
            smp64_note_idle(me, tsc_now() - t0, sp0, hd0);
            halted[me] = 0;
        }
    }
}

/* The APs' timer: a scheduler tick for whatever this CPU is running. The
 * clock itself is the bootstrap processor's PIT. */
void smp64_timer_tick(void* frame) {
    smp64_timer_irqs[this_cpu()]++;
    smp64_lapic_eoi();
    /* Before scheduling starts an AP has nothing to schedule - and its
     * idea of `current` would be the bootstrap CPU's. */
    if (!smp64_active) return;
    /* The run is over: whatever this CPU was running is dropped (the task
     * table is about to be reset) and it parks. The interrupt's frame is
     * abandoned on this CPU's own stack. */
    if (quiesce) {
        /* Only a tick that interrupted a program parks: one that
         * interrupted the idle loop just returns to it. Parking from there
         * too called smp64_ap_idle a level deeper every millisecond, until
         * the stack ran out. */
        if ((((registers64_t*)frame)->cs & 3) != 3) return;
        sched64_park_current((registers64_t*)frame);
        smp64_ap_idle();
    }
    smp64_note_tick(this_cpu(), (registers64_t*)frame, 4);
    sched64_tick((registers64_t*)frame);
}

int smp64_quiescing(void) { return quiesce; }

/* Ring-3 time, sampled by the timer: each CPU's, and the processes that
 * had it (the busiest few are reported). `ms` is the tick's length. */
static uint64_t user_ms[SMP64_MAX_CPUS], kernel_ms[SMP64_MAX_CPUS];
static uint64_t clock_at_start;
static volatile int accounting;
static struct { int pid, cpu; uint64_t ms; char comm[16]; } by_proc[64];
/* And by thread, for knowing whether a busy process's work is spread
 * over its threads or sits in one of them. */
static struct { int tid, pid; uint64_t ms; } by_task[128];
extern int sched64_current(void);
extern int proc64_current_pid(void);
void smp64_note_tick(int cpu, const void* frame_, int ms) {
    const registers64_t* frame = (const registers64_t*)frame_;
    int pid, i, free_i = -1;
    if (!accounting) return;
    if ((frame->cs & 3) != 3) { kernel_ms[cpu] += (uint64_t)ms; return; }
    user_ms[cpu] += (uint64_t)ms;
    pid = proc64_current_pid();
    {
        int tid = sched64_current(), j, fj = -1;
        for (j = 0; j < 128; j++) {
            if (by_task[j].ms && by_task[j].tid == tid && by_task[j].pid == pid) break;
            if (!by_task[j].ms && fj < 0) fj = j;
        }
        if (j == 128 && fj >= 0) { j = fj; by_task[j].tid = tid; by_task[j].pid = pid; }
        if (j < 128) by_task[j].ms += (uint64_t)ms;
    }
    for (i = 0; i < 64; i++) {
        if (by_proc[i].ms && by_proc[i].pid == pid && by_proc[i].cpu == cpu) break;
        if (!by_proc[i].ms && free_i < 0) free_i = i;
    }
    if (i == 64) {
        const char* c = proc64_comm_of(pid);
        if (free_i < 0) return;
        i = free_i;
        by_proc[i].pid = pid; by_proc[i].cpu = cpu;
        kstrlcpy(by_proc[i].comm, c ? c : "?", sizeof(by_proc[i].comm));
    }
    by_proc[i].ms += (uint64_t)ms;
}

static void ap_timer_handler(registers64_t* r) { smp64_timer_tick(r); }

/* Starts scheduling on the APs: from here on new processes are spread
 * over every CPU (smp64_pick_cpu) and the lock is live. */
void smp64_start_scheduling(void) {
    /* The ring-3 sampling (smp64_note_tick) runs on one CPU too, for
     * the comparison. */
    for (int c = 0; c < SMP64_MAX_CPUS; c++) user_ms[c] = kernel_ms[c] = 0;
    for (int i = 0; i < 64; i++) by_proc[i].ms = 0;
    for (int i = 0; i < 128; i++) by_task[i].ms = 0;
    accounting = 1;
    if (lapic && this_cpu() == 0) lapic_sampler_start();
    for (int c = 0; c < SMP64_MAX_CPUS; c++) smp64_timer_irqs[c] = 0;
    clock_at_start = clock64_ticks();
    if (ncpus < 2) return;
    quiesce = 0;
    /* This CPU is inside the kernel - kernel_main - and so holds the lock
     * from the moment it exists. */
    bkl = 1;
    bkl_owner = 0;
    bkl_depth = 1;
    smp64_cpu = 0;
    smp64_active = 1;
    spreading = 1;
    smp64_t_start = tsc_now();
    for (int c = 0; c < SMP64_MAX_CPUS; c++) {
        smp64_spin_cycles[c] = smp64_contended[c] = smp64_acquired[c] = 0;
        smp64_held_cycles[c] = smp64_idle_cycles[c] = 0;
    }
    smp64_kicks = smp64_busy_wakes = 0;
    for (int c = 0; c < SMP64_MAX_CPUS; c++) user_ms[c] = 0;
    for (int i = 0; i < 64; i++) by_proc[i].ms = 0;
    held_since[0] = smp64_t_start;
    serial64_puts("NOVARIS64: [smp] scheduling on ");
    serial64_putdec((uint64_t)ncpus);
    serial64_puts(" CPUs\n");
}

/* The end of a run: no more spreading, and every AP's task dropped and the
 * AP parked, before kernel_main resets the task table. The caller holds the
 * lock; it is let go while the APs park. */
void smp64_quiesce(void) {
    int depth, all;
    accounting = 0;
    if (!smp64_active) return;
    spreading = 0;
    quiesce = 1;
    depth = smp64_release_all();
    for (int spin = 0; spin < 2000; spin++) {
        all = 1;
        for (int c = 1; c < ncpus; c++) if (!parked[c]) all = 0;
        if (all) break;
        __asm__ __volatile__("sti; hlt; cli");
    }
    smp64_reacquire(depth);
}

/* --- an application processor's first C ---------------------------------- */

void smp64_ap_entry(uint64_t cpu) {
    uint64_t cr0, cr4;

    /* What boot64.s and kernel_main set on the bootstrap processor: SSE on
     * (CR0.MP, not EM; CR4.OSFXSR, OSXMMEXCPT), and write protection
     * honoured in ring 0 (CR0.WP), which copy-on-write depends on. */
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    cr0 = (cr0 & ~(1ULL << 2)) | (1ULL << 1) | (1ULL << 16);
    __asm__ __volatile__("mov %0, %%cr0" :: "r"(cr0) : "memory");
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9) | (1ULL << 10);
    __asm__ __volatile__("mov %0, %%cr4" :: "r"(cr4) : "memory");

    /* Off the trampoline's tables and onto the kernel's. */
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(kernel_cr3) : "memory");
    smp64_note_cr3(kernel_cr3);

    /* This CPU's own descriptor tables and interrupt stack, the shared
     * IDT, and its SYSCALL entry. */
    gdt64_install_cpu((int)cpu);
    gdt64_set_kernel_stack_cpu((int)cpu,
        (uint64_t)&ap_int_stacks[cpu][sizeof(ap_int_stacks[cpu])]);
    idt64_load();
    syscall64_init_cpu((int)cpu);

    lapic_enable();
    lapic_timer_start();
    parked[cpu] = 1;
    online[cpu] = 1;

    smp64_ap_idle();
}

/* --- starting them ------------------------------------------------------- */

static void send_ipi(uint32_t apic_id, uint32_t low) {
    lapic_write(LAPIC_ICR_HI, apic_id << 24);
    lapic_write(LAPIC_ICR_LO, low);
    for (int spin = 0; spin < 1000000 && (lapic_read(LAPIC_ICR_LO) & (1u << 12)); spin++)
        __asm__ __volatile__("pause");
}

int smp64_init(void) {
    uint64_t apic_base = read_msr(0x1B) & 0xFFFFF000ULL;
    uint64_t* boot_pml4;
    uint8_t* tramp;
    uint64_t* params;

    if (paging64_map_mmio(LAPIC_VIRT, apic_base, 4096) != 0) {
        serial64_puts("NOVARIS64: [smp] cannot map the local APIC\n");
        return 1;
    }
    lapic = (volatile uint32_t*)LAPIC_VIRT;
    lapic_enable();

    kernel_cr3 = read_cr3_() & 0x000FFFFFFFFFF000ULL;
    boot_pml4 = (uint64_t*)phys64_to_virt(kernel_cr3);

    kmemset(tr_pml4, 0, sizeof(tr_pml4));
    kmemset(tr_pdpt, 0, sizeof(tr_pdpt));
    kmemset(tr_pd, 0, sizeof(tr_pd));
    tr_pd[0]   = 0 | 0x83;                                   /* 2MB, P|W|PS */
    tr_pdpt[0] = image_phys(tr_pd) | 0x3;
    tr_pml4[0] = image_phys(tr_pdpt) | 0x3;
    for (int i = 256; i < 512; i++) tr_pml4[i] = boot_pml4[i];

    /* The LAPIC timer's rate, against the PIT: count down for 20ms. */
    lapic_write(LAPIC_TMR_DIV, 0x3);
    lapic_write(LAPIC_LVT_TMR, 1u << 16);                /* masked, one-shot */
    {
        /* And the TSC's against the same window, for the clock
         * (clock64_use_tsc). Waited for from the edge of a tick, so the
         * window is whole ticks; the machine is idle, so none is lost. */
        uint64_t t0, t1;
        wait_ms(1);
        t0 = tsc_now();
        lapic_write(LAPIC_TMR_INIT, 0xFFFFFFFFu);
        wait_ms(50);
        lapic_ticks_per_ms = (0xFFFFFFFFu - lapic_read(LAPIC_TMR_CUR)) / 50;
        t1 = tsc_now();
        lapic_write(LAPIC_TMR_INIT, 0);
        clock64_use_tsc((t1 - t0) / 50);
        serial64_puts("NOVARIS64: [clock] the TSC runs at ");
        serial64_putdec((t1 - t0) / 50);
        serial64_puts(" a millisecond; the clock follows it from here\n");
    }

    register_interrupt_handler64(SMP64_TIMER_VECTOR, ap_timer_handler);
    register_interrupt_handler64(SMP64_KICK_VECTOR, kick_handler);
    register_interrupt_handler64(SMP64_SAMPLE_VECTOR, sample_handler);

    tramp = (uint8_t*)phys64_to_virt(TRAMPOLINE_PHYS);
    kmemcpy(tramp, ap_trampoline_bin, ap_trampoline_len);
    params = (uint64_t*)(tramp + 4096 - 32);

    for (int cpu = 1; cpu < SMP64_MAX_CPUS; cpu++) {
        uint64_t until;
        params[0] = image_phys(tr_pml4);
        params[1] = (uint64_t)&ap_stacks[cpu][sizeof(ap_stacks[cpu])];
        params[2] = (uint64_t)&smp64_ap_entry;
        params[3] = (uint64_t)cpu;

        send_ipi((uint32_t)cpu, 0x4500);                     /* INIT */
        wait_ms(10);
        send_ipi((uint32_t)cpu, 0x4600 | (TRAMPOLINE_PHYS >> 12));   /* STARTUP */
        wait_ms(1);
        if (!online[cpu])
            send_ipi((uint32_t)cpu, 0x4600 | (TRAMPOLINE_PHYS >> 12));
        until = clock64_ticks() + 200;
        while (!online[cpu] && clock64_ticks() < until) __asm__ __volatile__("hlt");
        if (!online[cpu]) break;
        ncpus = cpu + 1;
    }

    serial64_puts("NOVARIS64: [smp] ");
    serial64_putdec((uint64_t)ncpus);
    serial64_puts(ncpus == 1 ? " CPU\n" : " CPUs online\n");
    return ncpus;
}

/* The kernel's own page tables, for a CPU that has nothing loaded it can
 * trust: the process it last ran may have been reaped by another CPU. */
void smp64_kernel_space(void) {
    if (smp64_active) kernel_space();
}

#ifndef SMP64_WAKE_PREEMPT
#define SMP64_WAKE_PREEMPT 1
#endif
void smp64_note_wake(int cpu) {
    if (!smp64_active || cpu < 0 || cpu >= ncpus || cpu == this_cpu()) return;
    /* Halted with nothing to do: woken, so it starts the task now rather
     * than at its next tick. Only this CPU - waking every halted one on
     * every wake-up was 3 million IPIs a run, each a trip through QEMU's
     * device lock on both sides. */
    if (halted[cpu]) { send_kick(cpu); return; }
    smp64_busy_wakes++;
    if (SMP64_WAKE_PREEMPT && !preempt_pending[cpu]) {
        preempt_pending[cpu] = 1;
        send_kick(cpu);
    }
}

/* The processes with the most ring-3 time, and where they ran. */
#ifdef NOVARIS64_IO_COUNT
uint32_t io64_counts[65536];
#endif

static void report_tasks(void) {
    serial64_puts("NOVARIS64: [smp] busiest threads (tid/pid:ms):");
    for (int k = 0; k < 16; k++) {
        int best = -1;
        for (int i = 0; i < 128; i++)
            if (by_task[i].ms && (best < 0 || by_task[i].ms > by_task[best].ms)) best = i;
        if (best < 0) break;
        serial64_putc(' ');
        serial64_putdec((uint64_t)by_task[best].tid);
        serial64_putc('/');
        serial64_putdec((uint64_t)by_task[best].pid);
        serial64_putc(':');
        serial64_putdec(by_task[best].ms);
        by_task[best].ms = 0;
    }
    serial64_putc('\n');
}

static void report_procs(void) {
    report_tasks();
#ifdef NOVARIS64_IO_COUNT
    serial64_puts("NOVARIS64: [io] busiest ports:");
    for (int k = 0; k < 10; k++) {
        int best = -1;
        for (int p = 0; p < 65536; p++)
            if (io64_counts[p] && (best < 0 || io64_counts[p] > io64_counts[best])) best = p;
        if (best < 0) break;
        serial64_puts(" ");
        serial64_puthex((uint64_t)best);
        serial64_puts("x");
        serial64_putdec(io64_counts[best]);
        io64_counts[best] = 0;
    }
    serial64_putc('\n');
#endif
    for (int k = 0; k < 16; k++) {
        int best = -1;
        for (int i = 0; i < 64; i++)
            if (by_proc[i].ms && (best < 0 || by_proc[i].ms > by_proc[best].ms)) best = i;
        if (best < 0) break;
        serial64_puts("NOVARIS64: [smp]   pid ");
        serial64_putdec((uint64_t)by_proc[best].pid);
        serial64_puts(" ");
        serial64_puts(by_proc[best].comm);
        serial64_puts(" on cpu ");
        serial64_putdec((uint64_t)by_proc[best].cpu);
        serial64_puts(": ");
        serial64_putdec(by_proc[best].ms);
        serial64_puts("ms\n");
        by_proc[best].ms = 0;
    }
}

void smp64_report(void) {
    uint64_t el;
    if (ncpus < 2) { report_procs(); return; }
    el = tsc_now() - smp64_t_start;
    for (int c = 0; c < ncpus; c++) {
        uint64_t k = smp64_held_cycles[c], sp = smp64_spin_cycles[c], id = smp64_idle_cycles[c];
        uint64_t u = el > k + sp + id ? el - k - sp - id : 0;
        serial64_puts("NOVARIS64: [smp] cpu ");
        serial64_putdec((uint64_t)c);
        serial64_puts(" time: kernel ");
        serial64_putdec(el ? k * 100 / el : 0);
        serial64_puts("%, spinning ");
        serial64_putdec(el ? sp * 100 / el : 0);
        serial64_puts("%, idle ");
        serial64_putdec(el ? id * 100 / el : 0);
        serial64_puts("%, user ");
        serial64_putdec(el ? u * 100 / el : 0);
        serial64_puts("% of ");
        serial64_putdec(el / 1000000u);
        serial64_puts("M; sampled: ring 3 ");
        serial64_putdec(user_ms[c]);
        serial64_puts("ms, kernel with interrupts on ");
        serial64_putdec(kernel_ms[c]);
        serial64_puts("ms\n");
    }
    report_procs();
    serial64_puts("NOVARIS64: [smp] AP timer interrupts:");
    for (int c = 1; c < ncpus; c++) { serial64_putc(' '); serial64_putdec(smp64_timer_irqs[c]); }
    serial64_puts(" over ");
    serial64_putdec(clock64_ticks() - clock_at_start);
    serial64_puts("ms of the clock; lapic ");
    serial64_putdec(lapic_ticks_per_ms);
    serial64_puts(" counts/ms\n");
    {
        extern uint64_t syscall64_lockfree_hits(void), kmain64_pf_fast_hits(void);
        serial64_puts("NOVARIS64: [smp] without the lock: ");
        serial64_putdec(syscall64_lockfree_hits());
        serial64_puts(" syscalls, ");
        serial64_putdec(kmain64_pf_fast_hits());
        serial64_puts(" page faults\n");
    }
    serial64_puts("NOVARIS64: [smp] wakes of a task on a busy CPU: ");
    serial64_putdec(smp64_busy_wakes);
    serial64_putc('\n');
    for (int c = 0; c < ncpus; c++) {
        serial64_puts("NOVARIS64: [smp] cpu ");
        serial64_putdec((uint64_t)c);
        serial64_puts(": lock taken ");
        serial64_putdec(smp64_acquired[c]);
        serial64_puts(", waited for ");
        serial64_putdec(smp64_contended[c]);
        serial64_puts(" times, ");
        serial64_putdec(smp64_spin_cycles[c] / 1000000u);
        serial64_puts("M cycles spinning\n");
    }
    serial64_puts("NOVARIS64: [smp] wake-up IPIs sent: ");
    serial64_putdec(smp64_kicks);
    serial64_putc('\n');
}

uint64_t smp64_spin_snapshot(int c) { return smp64_spin_cycles[c]; }
uint64_t smp64_held_snapshot(int c) { return smp64_held_cycles[c]; }

/* --- threads of one process on several CPUs (Milestone 105) ---------------
 *
 * With -DSMP64_SPREAD_THREADS a new thread may start on another CPU than
 * its process's. Its process is then "spread" (proc64.c), and three
 * things that one CPU per process made unnecessary have to be done:
 *
 * - Each CPU says which page tables it has loaded (smp64_note_cr3), so a
 *   change to a spread process's memory can be flushed from the TLB of
 *   every other CPU running it before the kernel lock is let go - and the
 *   frames it gave up are kept back until then (smp64_defer_free), so no
 *   other CPU can be writing through a stale mapping into a frame that
 *   has already been handed to someone else.
 * - Ending a process (exit_group, a fatal signal) must get its threads off
 *   the other CPUs before the address space goes: they are marked dead,
 *   then each CPU running one is sent the same flush, finds its thread
 *   dead, loads the kernel's page tables and goes on to something else
 *   (sched64_flush_service, sched64_replace_dead).
 * - The run's leader may now end on a CPU other than the bootstrap one,
 *   whose stack kernel_main is on; that CPU asks the bootstrap CPU to
 *   finish the run (smp64_run_ended_elsewhere).
 *
 * The flush is an IPI on SMP64_FLUSH_VECTOR, answered without the kernel
 * lock; a CPU waiting for the lock answers in its wait loop instead. Only
 * the lock's holder sends one, so two can never wait on each other. */

#ifndef SMP64_SPREAD_THREADS
#define SMP64_SPREAD_THREADS 0
#endif
#define DEFER_MAX 1024

volatile uint64_t smp64_loaded_cr3[SMP64_MAX_CPUS];
volatile int spin64_held[SMP64_MAX_CPUS];
static uint64_t defer[SMP64_MAX_CPUS][DEFER_MAX], defer_pml4[SMP64_MAX_CPUS],
                dirty_pml4[SMP64_MAX_CPUS];
static uint64_t shootdowns, shootdown_ipis, frees_deferred, threads_spread;

extern int  sched64_flush_service(int cpu);
extern int  sched64_tasks_on(int cpu);
extern void pmm64_free_frame_now(uint64_t phys);
extern void enter_user_mode64_abort(void) __attribute__((noreturn));

int smp64_spread_threads(void) { return SMP64_SPREAD_THREADS && smp64_active && spreading; }

void smp64_note_cr3(uint64_t pml4) {
    smp64_loaded_cr3[this_cpu()] = pml4 & 0x000FFFFFFFFFF000ULL;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* Every other CPU with `pml4` loaded flushes its TLB - or, if what it was
 * running there has been ended, leaves it - and this waits until each
 * has. The caller holds the kernel lock and no other. */
void smp64_shootdown(uint64_t pml4) {
    int me = this_cpu(), sent = 0;
    pml4 &= 0x000FFFFFFFFFF000ULL;
    if (!smp64_active || !pml4 || pml4 == kernel_cr3) return;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (int c = 0; c < ncpus; c++) {
        if (c == me || smp64_loaded_cr3[c] != pml4) continue;
        flush_req[c] = 1;
        lapic_write(LAPIC_ICR_HI, (uint32_t)c << 24);
        lapic_write(LAPIC_ICR_LO, 0x4000 | SMP64_FLUSH_VECTOR);
        sent++;
    }
    for (int c = 0; c < ncpus; c++)
        while (flush_req[c]) __asm__ __volatile__("pause");
    shootdowns++;
    shootdown_ipis += (uint64_t)sent;
}

/* A flush asked of this CPU: answered after the TLB is clean, or after
 * the thread it was running is left if that thread has been ended. */
static void flush_service(int cpu) {
    if (sched64_flush_service(cpu)) dead_pending[cpu] = 1;
    flush_req[cpu] = 0;
}

/* The IPI. Returns 1 if the interrupted thread had been ended, so the
 * caller must put another in its place before returning to ring 3. */
int smp64_flush_irq(void) {
    int me = this_cpu();
    smp64_lapic_eoi();
    if (flush_req[me]) flush_service(me);
    if (dead_pending[me]) { dead_pending[me] = 0; return 1; }
    return 0;
}

int smp64_take_dead_pending(void) {
    int me = this_cpu();
    if (!dead_pending[me]) return 0;
    dead_pending[me] = 0;
    return 1;
}

void smp64_kernel_space_now(void) { kernel_space(); }

/* A frame a spread process gave up under the lock, kept until its other
 * CPUs have flushed. Returns 0 when it may be freed at once. */
int smp64_defer_free(uint64_t phys) {
    int me;
    uint64_t pml4;
    if (!SMP64_SPREAD_THREADS || !smp64_active) return 0;
    me = this_cpu();
    if (bkl_owner != me || !proc64_current_spread()) return 0;
    /* Under one of the small locks: a kernel buffer (a pipe's ring, a
     * file's pages) going back, never a user mapping of this process -
     * and a full list here could not be flushed safely. */
    if (spin64_held[me]) return 0;
    pml4 = smp64_loaded_cr3[me];
    if (ndefer[me] && (defer_pml4[me] != pml4 || ndefer[me] == DEFER_MAX))
        flush_and_free(me);
    defer_pml4[me] = pml4;
    defer[me][ndefer[me]++] = phys;
    frees_deferred++;
    return 1;
}

/* The loaded process's mappings have been changed in a way another CPU's
 * TLB may disagree with - a page made read-only, shared or replaced. */
void smp64_mm_dirty(void) {
    int me;
    if (!SMP64_SPREAD_THREADS || !smp64_active) return;
    me = this_cpu();
    if (!proc64_current_spread()) return;
    if (mm_dirty[me] && dirty_pml4[me] != smp64_loaded_cr3[me]) flush_and_free(me);
    mm_dirty[me] = 1;
    dirty_pml4[me] = smp64_loaded_cr3[me];
}

static void flush_and_free(int cpu) {
    uint64_t a = ndefer[cpu] ? defer_pml4[cpu] : 0;
    uint64_t b = mm_dirty[cpu] ? dirty_pml4[cpu] : 0;
    int n = ndefer[cpu];
    if (a) smp64_shootdown(a);
    if (b && b != a) smp64_shootdown(b);
    if ((a && smp64_loaded_cr3[cpu] == a) || (b && smp64_loaded_cr3[cpu] == b)) {
        uint64_t v;
        __asm__ __volatile__("mov %%cr3, %0; mov %0, %%cr3" : "=r"(v) :: "memory");
    }
    ndefer[cpu] = 0;
    mm_dirty[cpu] = 0;
    for (int i = 0; i < n; i++) pmm64_free_frame_now(defer[cpu][i]);
}

/* The CPU for a new thread of process `pid`, whose CPU is `home`: the one
 * with fewest tasks, the home one on a tie. */
int smp64_pick_thread_cpu(int pid, int home) {
    int best = home, best_n;
    if (!smp64_spread_threads() || ncpus < 2) return home;
#ifdef SMP64_SPREAD_LEADER_ONLY
    /* Only the run's leader - the browser - whose threads are the work
     * worth spreading; the rest keep the lock-free paths of a process on
     * one CPU. */
    {
        extern int syscall64_leader_pid(void);
        if (pid != syscall64_leader_pid()) return home;
    }
#else
    (void)pid;
#endif
    best_n = sched64_tasks_on(home);
    for (int c = 0; c < ncpus; c++) {
        int n = sched64_tasks_on(c);
        if (n < best_n) { best = c; best_n = n; }
    }
    if (best != home) threads_spread++;
    return best;
}

/* The run's leader ended on this CPU, which is not the one kernel_main is
 * waiting on: the bootstrap CPU is told, and this one goes idle. */
void smp64_run_ended_elsewhere(void) __attribute__((noreturn));
void smp64_run_ended_elsewhere(void) {
    bsp_abort = 1;
    send_kick(0);
    smp64_ap_idle();
}

static void bsp_abort_now(void) {
    bsp_abort = 0;
    enter_user_mode64_abort();
}

/* The bootstrap CPU, wherever it notices that the run's leader ended on
 * another: back to kernel_main, abandoning whatever it was in. */
void smp64_bsp_abort_check(void) {
    if (bsp_abort && this_cpu() == 0) bsp_abort_now();
}

void smp64_spread_report(void) {
    if (!SMP64_SPREAD_THREADS || ncpus < 2) return;
    serial64_puts("NOVARIS64: [smp] threads placed off their process's CPU ");
    serial64_putdec(threads_spread);
    serial64_puts(", TLB shootdowns ");
    serial64_putdec(shootdowns);
    serial64_puts(" (");
    serial64_putdec(shootdown_ipis);
    serial64_puts(" IPIs), frees held for them ");
    serial64_putdec(frees_deferred);
    serial64_putc('\n');
}

/* --- a spread process's memory-map lock (Milestone 106) -------------------
 *
 * Recursive per CPU. A CPU spinning for it answers TLB flushes, since the
 * holder may be ending the process and waiting for every CPU to leave it.
 * Interrupts are off while it is held or waited for. */
int smp64_mm_lock(proc64_t* p) {
    int me = this_cpu();
    if (p->mm_owner == me) { p->mm_depth++; return 1; }
    while (__atomic_exchange_n(&p->mm_lock, 1, __ATOMIC_ACQUIRE))
        while (p->mm_lock) {
            if (flush_req[me]) flush_service(me);
            __asm__ __volatile__("pause");
        }
    p->mm_owner = me;
    p->mm_depth = 1;
    return 1;
}

void smp64_mm_unlock(proc64_t* p) {
    if (p->mm_owner != this_cpu()) return;
    if (--p->mm_depth > 0) return;
    p->mm_owner = -1;
    __atomic_store_n(&p->mm_lock, 0, __ATOMIC_RELEASE);
}

/* The current process's, if it is spread, held until this CPU lets the
 * kernel lock go: for every locked syscall and page fault of a spread
 * process, so its lock-free page faults never meet a change half made. */
void smp64_mm_hold_current(void) {
    int me;
    proc64_t* p;
    if (!SMP64_SPREAD_THREADS || !smp64_active) return;
    me = this_cpu();
    if (mm_held[me]) return;
    p = proc64_current();
    if (!p || !p->spread) return;
    smp64_mm_lock(p);
    mm_held[me] = p;
}

int smp64_dead_pending_here(void) { return dead_pending[this_cpu()]; }
