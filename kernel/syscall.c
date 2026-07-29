#include "syscall.h"
#include "idt.h"
#include "console.h"
#include "process.h"
#include "paging.h"
#include "pmm.h"
#include "scheduler.h"

/* Syscall calling convention: eax = syscall number, ebx/ecx/edx = args,
 * return value goes back in eax. Deliberately tiny for now - just enough
 * to prove a user program can ask the kernel to do something and get
 * control back cleanly. */
#define SYS_EXIT  0
#define SYS_WRITE 1
#define SYS_MMAP  2

/* Anonymous-mapping arena for SYS_MMAP: a simple bump allocator, never
 * freed. Sits well above the fixed program-load (0x40000000) and stack
 * (0x40100000) ranges so it can't collide with them. This is real
 * groundwork for Milestone 8 ("mmap-equivalent" in ROADMAP.md) but is
 * deliberately minimal - no munmap, no reuse, no per-process address
 * spaces (there's only ever one "process" running at a time - see
 * ROADMAP.md's Milestone 5 notes). */
#define MMAP_ARENA_START 0x42000000u
#define MMAP_ARENA_END   0x48000000u
static uint32_t mmap_next = MMAP_ARENA_START;

static uint32_t sys_mmap(uint32_t size) {
    uint32_t pages = (size + 4095u) / 4096u;
    if (pages == 0) pages = 1;
    uint32_t bytes = pages * 4096u;

    if (mmap_next + bytes > MMAP_ARENA_END) return 0; /* arena exhausted */

    uint32_t base = mmap_next;
    for (uint32_t va = base; va < base + bytes; va += 4096) {
        uint32_t phys = pmm_alloc_frame();
        if (!phys) return 0;
        paging_map_page(va, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER);
    }
    mmap_next += bytes;
    return base;
}

static void syscall_handler(registers_t* regs) {
    switch (regs->eax) {
        case SYS_WRITE: {
            /* ebx is a pointer into the shared address space (no
             * per-process page directories yet - see ROADMAP.md), so the
             * kernel can dereference it directly as long as it's mapped. */
            const char* str = (const char*)regs->ebx;
            terminal_writestring_color(str, VGA_COLOR_LIGHT_MAGENTA);
            regs->eax = 0;
            break;
        }

        case SYS_MMAP:
            regs->eax = sys_mmap(regs->ebx);
            break;

        case SYS_EXIT:
            if (scheduler_is_active()) {
                /* Milestone 9: this process is one of several under
                 * preemptive multitasking (see kernel/scheduler.c) -
                 * terminate it into the scheduler's ready queue instead
                 * of the older single-process blocking path below.
                 * Usually returns normally (the actual stack switch to
                 * whichever process runs next happens in isr.s's shared
                 * epilogue, after this call and the rest of the syscall
                 * dispatch chain unwind); doesn't return at all if this
                 * was the last process in the batch. */
                scheduler_exit_current();
            } else {
                terminal_writestring_color("[kernel] ", VGA_COLOR_LIGHT_GREEN);
                terminal_writestring("User process called sys_exit, returning to kernel.\n");
                process_exit_to_kernel(); /* does not return */
            }
            break;

        default:
            terminal_writestring_color("[kernel] ", VGA_COLOR_LIGHT_RED);
            terminal_writestring("Unknown syscall number\n");
            regs->eax = (uint32_t)-1;
            break;
    }
}

void syscall_install(void) {
    register_interrupt_handler(0x80, syscall_handler);
}
