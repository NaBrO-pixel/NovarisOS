/* kmain64.c - the 64-bit kernel's entry point during bring-up.
 *
 * This is deliberately not kernel.c. Porting 40,000 lines of kernel to a
 * new architecture in one step produces a mountain of compiler errors and
 * no way to tell which of them matter; the way this is done instead is to
 * bring the machine up one layer at a time and prove each layer before
 * putting the next one on top of it.
 *
 * What is proved here, in order:
 *   1. long mode is genuinely active (boot64.s)
 *   2. the GDT and TSS load, and the CPU is running on them (gdt64.c)
 *   3. an interrupt can be taken, dispatched to C, and returned from
 *      with every register intact (idt64.c, isr64.s)
 *   4. a handler can rewrite the frame it was given and change where
 *      execution resumes
 *
 * (4) is not a curiosity. It is the whole of Wine's exception dispatch,
 * and Milestone 23 of the 32-bit kernel exists because of it, so the
 * 64-bit interrupt path has to be able to do it from the start rather
 * than discovering later that it cannot. */

#include <stdint.h>
#include "io.h"
#include "serial64.h"
#include "gdt64.h"
#include "idt64.h"
#include "multiboot.h"
#include "pmm64.h"
#include "paging64.h"

static int failures = 0;

static void check(const char* what, int ok) {
    if (!ok) failures++;
    serial64_puts(ok ? "NOVARIS64: ok    " : "NOVARIS64: FAIL  ");
    serial64_puts(what);
    serial64_putc('\n');
}

static uint64_t read_cr0(void) {
    uint64_t v; __asm__ __volatile__("mov %%cr0, %0" : "=r"(v)); return v;
}
static uint64_t read_cr3(void) {
    uint64_t v; __asm__ __volatile__("mov %%cr3, %0" : "=r"(v)); return v;
}
static uint64_t read_cr4(void) {
    uint64_t v; __asm__ __volatile__("mov %%cr4, %0" : "=r"(v)); return v;
}
static uint64_t read_msr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static uint16_t read_cs(void) {
    uint16_t v; __asm__ __volatile__("mov %%cs, %0" : "=r"(v)); return v;
}
static uint16_t read_tr(void) {
    uint16_t v; __asm__ __volatile__("str %0" : "=r"(v)); return v;
}
static uint64_t read_cr2(void) {
    uint64_t v; __asm__ __volatile__("mov %%cr2, %0" : "=r"(v)); return v;
}

extern char _kernel_virtual_start[];
extern char _kernel_virtual_end[];
extern char _kernel_physical_start[];
extern char _kernel_physical_end[];

/* isr64.s - a load that is allowed to fault, and where to resume if it does. */
extern uint64_t probe_read64(const void* addr);
extern void probe_read64_recover(void);

/* --- what the handlers below record ---------------------------------- */
static volatile int      bp_hits;
static volatile uint64_t bp_rip;

static volatile int      pf_hits;
static volatile uint64_t pf_cr2;
static volatile uint64_t pf_err;
static volatile uint64_t pf_resume_rip;

static void breakpoint_handler(registers64_t* r) {
    bp_hits++;
    bp_rip = r->rip;
    /* #BP is a trap: rip already points past the int3, so returning
     * unchanged resumes at the next instruction. */
}

static void page_fault_handler(registers64_t* r) {
    pf_hits++;
    pf_cr2 = read_cr2();
    pf_err = r->err_code;
    /* #PF is a fault: rip points *at* the instruction that faulted, so
     * returning unchanged would re-run it and fault forever. Send
     * execution to the recovery label instead. */
    r->rip = pf_resume_rip;
}

void kernel_main(uint32_t magic, void* mbi) {
    uint64_t cr0, cr4, efer, rip;
    uint64_t before, after, probed;

    serial64_init();

    cr0  = read_cr0();
    cr4  = read_cr4();
    efer = read_msr(0xC0000080);
    __asm__ __volatile__("lea (%%rip), %0" : "=r"(rip));

    serial64_puts("\nNOVARIS64: ---- long mode bring-up ----\n");

    /* --- layer 1: the mode itself --------------------------------- */
    /* EFER.LMA is set by the CPU when long mode actually goes active. It
     * is the one bit here that is proof rather than assertion. */
    check("EFER.LMA  - long mode is active", (efer & (1ULL << 10)) != 0);
    check("EFER.LME  - long mode is enabled", (efer & (1ULL << 8)) != 0);
    check("CR0.PG    - paging is on",         (cr0 & (1ULL << 31)) != 0);
    check("CR4.PAE   - PAE is on",            (cr4 & (1ULL << 5)) != 0);
    check("CR4.OSFXSR- SSE is usable",        (cr4 & (1ULL << 9)) != 0);
    check("pointers are 8 bytes",             sizeof(void*) == 8);
    check("long is 8 bytes (LP64)",           sizeof(long) == 8);
    check("RIP is in the higher half",        rip >= 0xFFFFFFFF80000000ULL);
    check("multiboot magic is 0x2BADB002",    magic == 0x2BADB002u);
    check("multiboot info pointer is non-NULL", mbi != 0);

    serial64_puts("NOVARIS64: cs      = "); serial64_puthex(read_cs());
    serial64_puts("\nNOVARIS64: rip     = "); serial64_puthex(rip);
    serial64_puts("\nNOVARIS64: cr3     = "); serial64_puthex(read_cr3());
    serial64_puts("\nNOVARIS64: efer    = "); serial64_puthex(efer);
    serial64_puts("\nNOVARIS64: kernel  = ");
    serial64_puthex((uint64_t)_kernel_virtual_start);
    serial64_puts(" .. ");
    serial64_puthex((uint64_t)_kernel_virtual_end);
    serial64_puts("\nNOVARIS64: size    = ");
    serial64_putdec((uint64_t)(_kernel_virtual_end - _kernel_virtual_start));
    serial64_puts(" bytes\n");

    /* --- layer 2: descriptors -------------------------------------- */
    serial64_puts("NOVARIS64: -- descriptors --\n");
    gdt64_install();

    /* Surviving gdt64_flush at all is most of the test: it reloads every
     * segment register and changes cs through a far return, so a wrong
     * descriptor here is a fault rather than a wrong answer. */
    check("GDT loaded, cs is the kernel code selector",
          read_cs() == GDT64_KCODE_SEL);
    check("TSS loaded into the task register",
          read_tr() == GDT64_TSS_SEL);
    check("IST1 (double fault) stack is set",
          gdt64_ist_stack(IST_DOUBLE_FAULT) != 0);
    check("IST2 (NMI) stack is set",
          gdt64_ist_stack(IST_NMI) != 0);

    /* --- layer 3: an interrupt, taken and returned from ------------- */
    serial64_puts("NOVARIS64: -- interrupts --\n");
    idt64_install();
    register_interrupt_handler64(3,  breakpoint_handler);
    register_interrupt_handler64(14, page_fault_handler);

    /* Registers either side of an int3. If the stub's fifteen pushes and
     * pops are not exact mirrors, this is where it shows: a value lands
     * back in the wrong register and the comparison fails. */
    before = 0x0123456789ABCDEFULL;
    __asm__ __volatile__(
        "mov %1, %%r12\n\t"
        "int $3\n\t"
        "mov %%r12, %0"
        : "=r"(after)
        : "r"(before)
        : "r12");

    check("int3 reached a C handler", bp_hits == 1);
    check("execution resumed after int3", 1);
    check("a callee-saved register survived the interrupt", after == before);
    check("the saved rip was in the kernel", bp_rip >= 0xFFFFFFFF80000000ULL);

    /* --- layer 4: a handler that rewrites the frame ----------------- */
    /* 0xFFFF800000000000 is canonical but has no PML4 entry, so reading it
     * is a page fault at a known address. probe_read64 is the faulting
     * instruction and probe_read64_recover is where the handler sends
     * execution instead - both real symbols, for the reason isr64.s
     * explains at length. */
    pf_resume_rip = (uint64_t)&probe_read64_recover;
    probed = probe_read64((const void*)0xFFFF800000000000ULL);

    check("the probe returned the recovery value, not the memory", probed == 0);
    check("a page fault reached a C handler", pf_hits == 1);
    check("cr2 held the address that faulted",
          pf_cr2 == 0xFFFF800000000000ULL);
    check("the fault was a not-present read",
          (pf_err & 0x1) == 0);
    check("the handler rewrote rip and execution resumed there", 1);

    serial64_puts("NOVARIS64: cr2     = "); serial64_puthex(pf_cr2);
    serial64_puts("\nNOVARIS64: pf err  = "); serial64_puthex(pf_err);
    serial64_putc('\n');

    /* --- layer 5: physical frames ---------------------------------- */
    serial64_puts("NOVARIS64: -- physical memory --\n");
    {
        uint64_t kphys_start = (uint64_t)_kernel_physical_start;
        uint64_t kphys_end   = (uint64_t)_kernel_physical_end;
        uint64_t total, free_before, f1, f2, free_after, dbl;

        pmm64_init((const multiboot_info_t*)mbi, kphys_start, kphys_end);

        check("the frame allocator came up", pmm64_ready());
        total = pmm64_total_frames();
        free_before = pmm64_free_frames();
        check("RAM was found", total > 0);
        check("some of it is free", free_before > 0);
        check("not all of it is free", free_before < total);

        f1 = pmm64_alloc_frame();
        f2 = pmm64_alloc_frame();
        check("a frame was allocated", f1 != 0);
        check("a second frame was allocated", f2 != 0);
        check("the two are different frames", f1 != f2);
        check("frames are page aligned", (f1 % PMM64_FRAME_SIZE) == 0);
        /* The first megabyte and the kernel's own image are the two things
         * that must never be handed out; a bug in the reserve pass shows
         * up here rather than as corruption three milestones later. */
        check("no frame comes from the first megabyte", f1 >= 0x100000ULL);
        check("no frame overlaps the kernel image",
              f1 < kphys_start || f1 >= kphys_end);
        check("no frame overlaps the frame bitmap",
              f1 < pmm64_bitmap_phys() ||
              f1 >= pmm64_bitmap_phys() + pmm64_bitmap_bytes());

        check("allocating two frames used two frames",
              pmm64_free_frames() == free_before - 2);
        pmm64_free_frame(f1);
        pmm64_free_frame(f2);
        free_after = pmm64_free_frames();
        check("freeing them gave both back", free_after == free_before);

        /* Freeing twice is the one error this allocator is asked to
         * notice rather than obey. */
        dbl = pmm64_double_frees();
        pmm64_free_frame(f1);
        check("a double free was counted, not obeyed",
              pmm64_double_frees() == dbl + 1);
        check("and it did not change the free count",
              pmm64_free_frames() == free_after);

        serial64_puts("NOVARIS64: ram top = ");
        serial64_puthex(pmm64_highest_addr());
        serial64_puts("\nNOVARIS64: frames  = ");
        serial64_putdec(total);
        serial64_puts(" total, ");
        serial64_putdec(free_before);
        serial64_puts(" free\n");
        serial64_puts("NOVARIS64: bitmap  = ");
        serial64_puthex(pmm64_bitmap_phys());
        serial64_puts(" .. +");
        serial64_putdec(pmm64_bitmap_bytes());
        serial64_puts(" bytes\n");
    }

    /* --- layer 6: four-level paging --------------------------------- */
    serial64_puts("NOVARIS64: -- paging --\n");
    {
        /* PML4 slot 384. Not 256: that is the address layer 4 above
         * relies on being unmapped, and mapping it here would quietly
         * turn that page-fault test into a test of nothing. */
        const uint64_t TEST_VA = 0xFFFFC00000000000ULL;
        const uint64_t MAGIC   = 0x00C0FFEE5EA51DE5ULL;
        uint64_t frame, got, tables_before;
        volatile uint64_t* p;
        int rc;

        paging64_init();

        /* The self-reference, read through itself: if the recursive
         * address were wrong this load would fault rather than answer. */
        {
            uint64_t* pml4 = paging64_pml4();
            check("the PML4 can be read through its own recursive slot",
                  (pml4[510] & 1) != 0);
            check("the higher half is still mapped in it",
                  (pml4[511] & 1) != 0);
            check("the recursive slot points at the PML4 itself",
                  (pml4[510] & 0x000FFFFFFFFFF000ULL) ==
                  (read_cr3() & 0x000FFFFFFFFFF000ULL));
        }

        tables_before = paging64_tables_allocated();
        frame = pmm64_alloc_frame();
        check("a frame to map was allocated", frame != 0);

        rc = paging64_map(TEST_VA, frame, PAGE64_PRESENT | PAGE64_WRITE);
        check("a fresh 4KB page mapped", rc == PAGING64_OK);
        /* An empty PML4 slot needs a PDPT, a PD and a PT built under it. */
        check("three intermediate tables were built",
              paging64_tables_allocated() == tables_before + 3);

        p = (volatile uint64_t*)TEST_VA;
        *p = MAGIC;
        check("the mapped page is writable and reads back",
              *p == MAGIC);

        rc = paging64_translate(TEST_VA, &got);
        check("translate resolved the mapping", rc == PAGING64_OK);
        check("and resolved it to the frame that was mapped", got == frame);

        /* The strongest form of the same question: the bytes written
         * through the new virtual address must be visible at that
         * physical frame's other address. A mapping that merely does not
         * fault would pass everything above and fail this. */
        if (frame < 0x40000000ULL) {
            volatile uint64_t* through_phys =
                (volatile uint64_t*)(0xFFFFFFFF80000000ULL + frame);
            check("the write landed in that physical frame",
                  *through_phys == MAGIC);
        }

        check("an offset inside the page translates with its offset",
              paging64_translate(TEST_VA + 0x123, &got) == PAGING64_OK &&
              got == frame + 0x123);

        /* The kernel's own text is inside a 2MB page boot64.s built.
         * Refusing to map over it is the honest answer while splitting
         * huge pages is unimplemented. */
        check("mapping over a 2MB page is refused, not corrupted",
              paging64_map(0xFFFFFFFF80000000ULL, frame,
                           PAGE64_PRESENT | PAGE64_WRITE)
              == PAGING64_HUGE_IN_WAY);

        check("an unmapped address does not translate",
              paging64_translate(0xFFFFC00000200000ULL, &got)
              == PAGING64_NOT_MAPPED);

        /* And unmapping must genuinely remove it - checked by faulting on
         * it, using the same probe/recover pair as layer 4. */
        rc = paging64_unmap(TEST_VA);
        check("the page unmapped", rc == PAGING64_OK);
        check("it no longer translates",
              paging64_translate(TEST_VA, &got) == PAGING64_NOT_MAPPED);

        pf_hits = 0;
        pf_resume_rip = (uint64_t)&probe_read64_recover;
        probe_read64((const void*)TEST_VA);
        check("reading it now faults", pf_hits == 1);
        check("and it faulted at that address", pf_cr2 == TEST_VA);

        check("unmapping it twice is refused",
              paging64_unmap(TEST_VA) == PAGING64_NOT_MAPPED);

        pmm64_free_frame(frame);

        serial64_puts("NOVARIS64: tables  = ");
        serial64_putdec(paging64_tables_allocated());
        serial64_puts(" allocated\n");
    }

    /* --- verdict ---------------------------------------------------- */
    serial64_puts("NOVARIS64: failures = ");
    serial64_putdec((uint64_t)failures);
    serial64_putc('\n');
    serial64_puts("NOVARIS64: ---- bring-up complete ----\n");

    for (;;) __asm__ __volatile__("hlt");
}
