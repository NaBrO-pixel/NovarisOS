/* gdt64.c - the long-mode GDT and TSS.
 *
 * In long mode segmentation is mostly gone: the base and limit of a
 * code/data descriptor are ignored, and only P, DPL, S, the executable bit
 * and L still mean anything. So this table is much less interesting than
 * the 32-bit one - but it cannot be skipped, because the CPU still needs a
 * descriptor with L set to be executing 64-bit code at all, and the ring-3
 * entries below are what a user process will eventually be entered with.
 *
 * The TSS, by contrast, got *more* important. On 32-bit it existed to hold
 * one ring-0 stack pointer for the ring-3 transition. Here it also holds
 * the IST - a set of known-good stacks the CPU switches to unconditionally
 * for the vectors that cannot trust the current one. A double fault taken
 * on an already-broken stack is a triple fault and an instant reboot with
 * no diagnostic at all, which is precisely the failure that is hardest to
 * debug, so IST1 is set up here and wired to vector 8 in idt64.c. */

#include <stdint.h>
#include "gdt64.h"

#define GDT_ENTRIES 7   /* null, kcode, kdata, udata, ucode, tss (2 slots) */

struct gdt64_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  limit_high_flags;
    uint8_t  base_high;
} __attribute__((packed));

/* A system descriptor (the TSS is the only one here) is twice as wide in
 * long mode, because the base became 64-bit. It occupies two consecutive
 * GDT slots, which is why GDT_ENTRIES counts it as two. */
struct gdt64_tss_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  limit_high_flags;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed));

struct gdt64_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp[3];        /* rsp0-2: the stack per privilege level */
    uint64_t reserved1;
    uint64_t ist[7];        /* ist1-7, indexed from 1 by the IDT gate */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

static struct gdt64_entry gdt[GDT_ENTRIES];
static struct gdt64_ptr   gdt_ptr;
static struct tss64       tss;

/* The stacks the CPU switches to for the vectors that cannot use the one
 * they interrupted. 16KB each, and in .bss rather than allocated, because
 * they have to exist before the heap does. */
static uint8_t df_stack[16384] __attribute__((aligned(16)));
static uint8_t nmi_stack[16384] __attribute__((aligned(16)));

extern void gdt64_flush(struct gdt64_ptr* ptr);
extern void tss64_flush(uint16_t selector);

static void set_entry(int i, uint8_t access, uint8_t flags) {
    /* Base and limit are ignored for code and data in long mode. They are
     * written as zero rather than as the 32-bit kernel's 0..4GB because
     * pretending they mean something is how somebody later concludes the
     * kernel is limited to 4GB by its own GDT. */
    gdt[i].limit_low        = 0;
    gdt[i].base_low         = 0;
    gdt[i].base_mid         = 0;
    gdt[i].access           = access;
    gdt[i].limit_high_flags = flags;
    gdt[i].base_high        = 0;
}

static void set_tss_entry(int i, uint64_t base, uint32_t limit) {
    struct gdt64_tss_entry* e = (struct gdt64_tss_entry*)&gdt[i];

    e->limit_low        = (uint16_t)(limit & 0xFFFF);
    e->base_low         = (uint16_t)(base & 0xFFFF);
    e->base_mid         = (uint8_t)((base >> 16) & 0xFF);
    e->access           = 0x89;     /* P=1, DPL=0, type=9: available 64-bit TSS */
    e->limit_high_flags = (uint8_t)((limit >> 16) & 0x0F);
    e->base_high        = (uint8_t)((base >> 24) & 0xFF);
    e->base_upper       = (uint32_t)(base >> 32);
    e->reserved         = 0;
}

void gdt64_set_kernel_stack(uint64_t rsp0) {
    tss.rsp[0] = rsp0;
}

uint64_t gdt64_ist_stack(int index) {
    return (index >= 1 && index <= 7) ? tss.ist[index - 1] : 0;
}

void gdt64_install(void) {
    unsigned i;
    uint8_t* p;

    for (p = (uint8_t*)&tss, i = 0; i < sizeof(tss); i++) p[i] = 0;

    /* A stack grows down, so the CPU wants the *top*. Rounding to 16 keeps
     * the ABI's alignment guarantee on entry to a C handler. */
    tss.ist[0] = ((uint64_t)df_stack  + sizeof(df_stack))  & ~0xFULL;
    tss.ist[1] = ((uint64_t)nmi_stack + sizeof(nmi_stack)) & ~0xFULL;

    /* No I/O permission bitmap. Setting the base past the end of the
     * segment is how the manual says to say "there isn't one"; leaving it
     * zero would make the CPU read the first bytes of the TSS as a bitmap
     * and let ring 3 talk to whichever ports those bits happened to allow. */
    tss.iomap_base = sizeof(tss);

    set_entry(0, 0, 0);                 /* null */
    set_entry(1, 0x9A, 0xA0);           /* kernel code: P,S,exec,read   L=1 */
    set_entry(2, 0x92, 0x00);           /* kernel data: P,S,write            */

    /* User data before user code, and both adjacent to the kernel pair.
     * That ordering is not cosmetic: sysret derives cs and ss from a
     * single MSR by adding fixed offsets, so it only works if the
     * selectors sit where it expects them. */
    set_entry(3, 0xF2, 0x00);           /* user data: DPL=3                  */
    set_entry(4, 0xFA, 0xA0);           /* user code: DPL=3, L=1             */

    set_tss_entry(5, (uint64_t)&tss, sizeof(tss) - 1);

    gdt_ptr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdt_ptr.base  = (uint64_t)&gdt;

    gdt64_flush(&gdt_ptr);
    tss64_flush(GDT64_TSS_SEL);
}
