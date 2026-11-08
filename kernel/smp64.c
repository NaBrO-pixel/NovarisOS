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
 * And it carries the kernel's idea of who is running. The scheduler's
 * `current` and proc64's current pid are single globals; each CPU keeps its
 * own pair here, loaded when it takes the lock and stored when it lets go,
 * so the code that reads them never knows there is more than one CPU.
 *
 * None of it does anything until smp64_start_scheduling: with one CPU
 * running tasks there is nobody to exclude. */

volatile int smp64_active;
int smp64_cpu;                          /* the lock holder's CPU */

static volatile int bkl;
static volatile int bkl_owner = -1;
static int bkl_depth;

static struct {
    int sched_current;
    int proc_pid;
} ctx[SMP64_MAX_CPUS];

extern int  sched64_current(void);
extern void sched64_set_current_raw(int i);
extern int  proc64_current_pid(void);
extern void proc64_set_current(int pid);

static int this_cpu(void) {
    if (!lapic) return 0;
    uint32_t id = lapic_read(LAPIC_ID) >> 24;
    return id < SMP64_MAX_CPUS ? (int)id : 0;
}

void smp64_enter(void) {
    int cpu;
    if (!smp64_active) return;
    cpu = this_cpu();
    if (bkl_owner == cpu) { bkl_depth++; return; }
    while (__atomic_exchange_n(&bkl, 1, __ATOMIC_ACQUIRE))
        while (bkl) __asm__ __volatile__("pause");
    bkl_owner = cpu;
    bkl_depth = 1;
    smp64_cpu = cpu;
    sched64_set_current_raw(ctx[cpu].sched_current);
    proc64_set_current(ctx[cpu].proc_pid);
}

static void release(void) {
    int cpu = smp64_cpu;
    ctx[cpu].sched_current = sched64_current();
    ctx[cpu].proc_pid = proc64_current_pid();
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
    if (bkl_owner != this_cpu()) smp64_enter();
    bkl_depth = 1;
}

int smp64_this(void) { return smp64_active ? smp64_cpu : 0; }

/* --- spreading processes ---------------------------------------------- */

static int spread_next;
static volatile int spreading;

/* The CPU a new process runs on: every CPU in turn while spreading, the
 * bootstrap one otherwise. Threads stay with their process (proc64.c), so
 * a process's page tables are only ever loaded on one CPU and nothing has
 * to tell the others to flush their TLBs. */
int smp64_pick_cpu(void) {
    if (!spreading || ncpus < 2) return 0;
    spread_next = (spread_next + 1) % ncpus;
    return spread_next;
}

/* --- the application processors' scheduling ---------------------------- */

static uint32_t lapic_ticks_per_ms;
static uint8_t ap_int_stacks[SMP64_MAX_CPUS][16384] __attribute__((aligned(16)));
static volatile int quiesce;
static volatile int parked[SMP64_MAX_CPUS];

extern void gdt64_install_cpu(int cpu);
extern void gdt64_set_kernel_stack_cpu(int cpu, uint64_t rsp0);
extern void idt64_load(void);
extern void syscall64_init_cpu(int cpu);
extern int  sched64_run_this_cpu(void);      /* resumes a task, or returns 0 */
extern void sched64_tick(registers64_t* frame);
extern void sched64_park_current(const registers64_t* frame);

static void lapic_timer_start(void) {
    lapic_write(LAPIC_TMR_DIV, 0x3);                     /* divide by 16 */
    lapic_write(LAPIC_LVT_TMR, (1u << 17) | SMP64_TIMER_VECTOR);   /* periodic */
    lapic_write(LAPIC_TMR_INIT, lapic_ticks_per_ms ? lapic_ticks_per_ms : 100000);
}

static void kernel_space(void) {
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(kernel_cr3) : "memory");
}

/* An AP with nothing to do: no lock, the kernel's own page tables (the
 * process it last ran may be gone, and its tables with it), interrupts on
 * for the timer. Picks up any task pinned to it. */
void smp64_ap_idle(void) __attribute__((noreturn));
void smp64_ap_idle(void) {
    for (;;) {
        smp64_enter();
        /* Only once scheduling is on: before that the lock is not live
         * and "this CPU's tasks" would read as the bootstrap CPU's. */
        if (smp64_active && !quiesce) {
            parked[smp64_cpu] = 0;
            sched64_run_this_cpu();                      /* returns only if none */
        }
        parked[smp64_cpu] = 1;
        smp64_leave_to_user();
        kernel_space();
        __asm__ __volatile__("sti; hlt; cli");
    }
}

/* The APs' timer: a scheduler tick for whatever this CPU is running. The
 * clock itself is the bootstrap processor's PIT. */
void smp64_timer_tick(void* frame) {
    smp64_lapic_eoi();
    /* Before scheduling starts an AP has nothing to schedule - and its
     * idea of `current` would be the bootstrap CPU's. */
    if (!smp64_active) return;
    /* The run is over: whatever this CPU was running is dropped (the task
     * table is about to be reset) and it parks. The interrupt's frame is
     * abandoned on this CPU's own stack. */
    if (quiesce) {
        sched64_park_current((registers64_t*)frame);
        smp64_ap_idle();
    }
    sched64_tick((registers64_t*)frame);
}

int smp64_quiescing(void) { return quiesce; }

static void ap_timer_handler(registers64_t* r) { smp64_timer_tick(r); }

/* Starts scheduling on the APs: from here on new processes are spread
 * over every CPU (smp64_pick_cpu) and the lock is live. */
void smp64_start_scheduling(void) {
    if (ncpus < 2) return;
    quiesce = 0;
    /* This CPU is inside the kernel - kernel_main - and so holds the lock
     * from the moment it exists. */
    bkl = 1;
    bkl_owner = 0;
    bkl_depth = 1;
    smp64_cpu = 0;
    /* Every AP starts with nothing running. */
    for (int c = 1; c < SMP64_MAX_CPUS; c++) {
        ctx[c].sched_current = -1;
        ctx[c].proc_pid = -1;
    }
    smp64_active = 1;
    spreading = 1;
    serial64_puts("NOVARIS64: [smp] scheduling on ");
    serial64_putdec((uint64_t)ncpus);
    serial64_puts(" CPUs\n");
}

/* The end of a run: no more spreading, and every AP's task dropped and the
 * AP parked, before kernel_main resets the task table. The caller holds the
 * lock; it is let go while the APs park. */
void smp64_quiesce(void) {
    int depth, all;
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
    lapic_write(LAPIC_TMR_INIT, 0xFFFFFFFFu);
    wait_ms(20);
    lapic_ticks_per_ms = (0xFFFFFFFFu - lapic_read(LAPIC_TMR_CUR)) / 20;
    lapic_write(LAPIC_TMR_INIT, 0);

    register_interrupt_handler64(SMP64_TIMER_VECTOR, ap_timer_handler);

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
