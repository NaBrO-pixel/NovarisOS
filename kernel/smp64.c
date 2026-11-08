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

    lapic_enable();
    online[cpu] = 1;

    for (;;) __asm__ __volatile__("cli; hlt");
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
