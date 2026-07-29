#include "gdt.h"

/* One 8-byte descriptor per segment, per the x86 GDT entry format. */
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

/* The GDTR register the CPU actually loads: size of the table (minus 1)
 * plus the linear address of the table. */
struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* 7 entries: null, kernel code, kernel data, user code, user data, TSS,
 * and the Win32 TEB segment. The user-mode ones were set up back in
 * Milestone 2; the TSS entry came with Milestone 5; entry 6 is
 * Milestone 10's, and is the only one whose base changes at runtime -
 * see gdt_set_teb(). */
#define GDT_ENTRIES 7
static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr   gp;

/* Minimal 32-bit TSS. We don't use hardware task-switching (software
 * context switches are simpler and what every modern OS does) - the only
 * fields that matter to us are ss0/esp0: whenever the CPU takes an
 * interrupt or int 0x80 while running in ring 3, it automatically loads
 * SS:ESP from here before pushing anything, which is how we get a safe
 * kernel stack under user-mode code without it being able to tamper with
 * kernel memory via its own (untrusted) stack pointer. */
struct tss_entry {
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1, ss1, esp2, ss2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs, ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

static struct tss_entry tss;

/* Dedicated kernel stack used only while ring-0 handles an interrupt or
 * syscall that arrived from ring 3. Sized generously since interrupt
 * handlers, the syscall dispatcher, and whatever they call all share it. */
#define KERNEL_STACK_SIZE 8192
static uint8_t kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));

/* Implemented in gdt_flush.s: loads the GDTR via LGDT and reloads every
 * segment register so the CPU actually starts using the new table. */
extern void gdt_flush(uint32_t);

static void gdt_set_gate(int32_t num, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t gran) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_middle  = (base >> 16) & 0xFF;
    gdt[num].base_high    = (base >> 24) & 0xFF;

    gdt[num].limit_low    = (limit & 0xFFFF);
    gdt[num].granularity  = (limit >> 16) & 0x0F;

    gdt[num].granularity |= gran & 0xF0;
    gdt[num].access       = access;
}

void gdt_install(void) {
    gp.limit = (sizeof(struct gdt_entry) * GDT_ENTRIES) - 1;
    gp.base  = (uint32_t)&gdt;

    /* Null descriptor - required by the architecture, entry 0 must be
     * all zeros. */
    gdt_set_gate(0, 0, 0, 0, 0);

    /* Kernel code segment: base 0, limit 4GB, ring 0, executable/readable.
     * access 0x9A = present, ring 0, code, executable, readable.
     * gran   0xCF = 4KB granularity, 32-bit, limit high nibble = 0xF. */
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);

    /* Kernel data segment: base 0, limit 4GB, ring 0, writable.
     * access 0x92 = present, ring 0, data, writable. */
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    /* User code segment: same as kernel code but ring 3 (access |= 0x60).
     * Not used until Milestone 5, set up now so we never revisit this file. */
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);

    /* User data segment: same as kernel data but ring 3. */
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    /* TSS descriptor: base/limit point at our tss struct. access 0x89 =
     * present, ring 0, 32-bit TSS (available, not busy). Granularity 0x00
     * since the TSS is addressed byte-for-byte, not in 4KB pages. */
    for (int i = 0; i < (int)sizeof(tss); i++) ((uint8_t*)&tss)[i] = 0;
    tss.ss0 = 0x10;  /* kernel data selector */
    tss.esp0 = (uint32_t)kernel_stack + KERNEL_STACK_SIZE;
    /* Bit 8 (0x100) of iomap_base >= sizeof(tss) means "no I/O permission
     * bitmap" - ring 3 code gets no direct port I/O access, as intended. */
    tss.iomap_base = sizeof(tss);
    gdt_set_gate(5, (uint32_t)&tss, sizeof(tss) - 1, 0x89, 0x00);

    /* TEB segment (selector 0x33 = entry 6, RPL 3). Base and limit are
     * filled in by gdt_set_teb() when a Win32 program starts; until then
     * it's a present-but-empty ring-3 data descriptor, so a stray fs
     * access faults rather than silently addressing something real. */
    gdt_set_gate(6, 0, 0, 0xF2, 0x40);

    gdt_flush((uint32_t)&gp);

    /* Selector 0x28 = GDT entry 5 (5*8), ring 0 (RPL bits already 0). */
    __asm__ __volatile__("ltr %%ax" : : "a"((uint16_t)0x28));
}

void gdt_set_kernel_stack(uint32_t esp0) {
    tss.esp0 = esp0;
}

void gdt_set_teb(uint32_t base, uint32_t limit) {
    /* Byte granularity (gran 0x40 = 32-bit operand size, G clear) so the
     * limit is exact: the TEB is one page, and a program that walks off
     * the end of it should fault rather than reach the next allocation.
     * The GDT is loaded once at boot and the CPU re-reads the descriptor
     * on every segment load, so rewriting the entry in place is enough -
     * no second LGDT needed. */
    gdt_set_gate(6, base, limit, 0xF2, 0x40);
}
