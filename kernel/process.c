#include "process.h"
#include "paging.h"
#include "pmm.h"
#include "user_hello.h"
#include "elf.h"
#include "pe.h"
#include "win32.h"
#include "idt.h"
#include "console.h"
#include "kstring.h"
#include "posix.h"
#include "scheduler.h"

/* Referenced from process_asm.s: the kernel-side stack pointer/frame
 * pointer to restore when the user program exits, so control returns to
 * process_run_demo_user_program()'s caller as if it were an ordinary
 * blocking function call. */
uint32_t kernel_resume_esp = 0;
uint32_t kernel_resume_ebp = 0;

/* Also referenced from process_asm.s - see process_set_user_fs(). */
uint32_t user_fs_selector = 0x23;

#define USER_LOAD_VADDR 0x40000000u /* must match userland/user.ld */
#define USER_STACK_TOP      0x40100000u /* 1MB above the program; grows down */
#define PAGE_SIZE 4096u
/* 64KB of stack for an ELF program - see process_run_elf(). */
#define ELF_STACK_SIZE (16u * PAGE_SIZE)

/* Non-zero between entering ring 3 and coming back out of it. Only the
 * fault path reads it, and only to decide whether a CPU exception belongs
 * to a user program or to the kernel. */
static int user_active = 0;

/* --- the running program's address space -------------------------------
 *
 * Milestone 15. Milestone 14 built page directories that a process could
 * run in; this is where one actually does.
 *
 * The whole thing rests on a single property of Milestone 14's design,
 * and it is worth being explicit about it because it is what keeps this
 * change small: the kernel half is *identical* in every address space. So
 * once CR3 holds the process's directory, the kernel keeps working
 * unchanged - its code, its stack, kmalloc, the console, the initrd are
 * all still exactly where they were - while the user half is now the
 * process's own. Which means an `int 0x81` handler that reads a program's
 * arguments by casting a ring-3 pointer, as the entire Win32 layer does,
 * *still reads the right memory*, because the kernel never leaves the
 * process's address space while servicing it.
 *
 * That is why win32.c, pe.c and win32_callback.c did not have to be
 * rewritten to walk page tables. The rule they now depend on is narrower
 * than "one flat address space" but just as simple: never switch CR3
 * while holding a pointer into a process.
 */
static uint32_t current_as = 0;   /* the running process's directory */
static uint32_t previous_as = 0;  /* what to go back to when it ends */

uint32_t process_current_address_space(void) {
    return current_as;
}

int process_enter_address_space(void) {
    uint32_t as = paging_create_address_space();
    /* Out of memory, or paging_finalize_kernel_space() never ran. Running
     * in the kernel's own directory is what every milestone before this
     * one did, so falling back to it is a real degradation but not a
     * broken one - and saying so beats failing to start the program. */
    if (!as) return 0;

    previous_as = paging_current_address_space();
    current_as = as;
    paging_switch_address_space(as);
    return 1;
}

void process_leave_address_space(void) {
    if (!current_as) return;
    /* Order matters: callers unmap the program's pages first, while still
     * standing in its address space, so their frames go back to the PMM
     * through the ordinary paths. Destroying the directory afterwards
     * reclaims anything they missed rather than being the only thing that
     * reclaims anything. */
    paging_switch_address_space(previous_as);
    paging_destroy_address_space(current_as);
    current_as = 0;
    previous_as = 0;
}

void process_set_user_fs(uint16_t selector) {
    user_fs_selector = selector;
}

int process_user_active(void) {
    return user_active;
}

/* Every path into ring 3 goes through here, so `user_active` can't get
 * out of step with reality. */
static void enter_user_mode(uint32_t entry, uint32_t stack_top) {
    user_active = 1;
    /* File descriptors and the mmap/brk arenas belong to the process, so
     * they are set up per run rather than once at boot - otherwise the
     * second program to run would inherit the first one's open files. */
    posix_process_begin();
    process_run_user_mode(entry, stack_top);
    posix_process_end();
    user_active = 0;
}

int process_map_image(uint32_t load_vaddr, const uint8_t* image,
                      uint32_t size) {
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0) pages = 1;

    for (uint32_t i = 0; i < pages; i++) {
        uint32_t phys = pmm_alloc_frame();
        if (!phys) return 0;
        paging_map_page(load_vaddr + i * PAGE_SIZE, phys,
                         PAGE_PRESENT | PAGE_RW | PAGE_USER);
    }

    uint8_t* dest = (uint8_t*)load_vaddr;
    for (uint32_t i = 0; i < size; i++) dest[i] = image[i];
    return 1;
}

void process_run_flat_binary(const uint8_t* image, uint32_t size) {
    int owns_as = process_enter_address_space();

    process_map_image(USER_LOAD_VADDR, image, size);

    /* One page of user stack, mapped just below USER_STACK_TOP so the
     * initial ESP we hand to ring 3 is a valid "one past the mapped
     * region" descending-stack pointer. */
    uint32_t stack_phys = pmm_alloc_frame();
    paging_map_page(USER_STACK_TOP - PAGE_SIZE, stack_phys,
                     PAGE_PRESENT | PAGE_RW | PAGE_USER);

    enter_user_mode(USER_LOAD_VADDR, USER_STACK_TOP);

    /* Everything this program mapped lived in its own directory, so
     * destroying it is the unmap - there is no page-by-page teardown to
     * do here the way there was before Milestone 15. */
    if (owns_as) process_leave_address_space();
}

void process_run_demo_user_program(void) {
    process_run_flat_binary(user_hello_bin, user_hello_bin_len);
}

int process_run_elf(const uint8_t* image, uint32_t size) {
    /* Validated before the address space is created, so a file that isn't
     * an ELF doesn't cost a page directory. `image` itself is a kernel
     * heap buffer, which is mapped identically either side of the switch. */
    if (!elf_is_valid(image, size)) return 0;

    int owns_as = process_enter_address_space();

    uint32_t entry;
    if (!elf_load(image, size, &entry)) {
        if (owns_as) process_leave_address_space();
        return 0;
    }

    /* An ELF program gets a larger stack than the single page a flat
     * binary makes do with: since Milestone 20 it can clone() threads,
     * and each of those needs somewhere to put a stack of its own out of
     * its mmap arena, but the main thread's own frames also get deeper
     * once there is a real program on top. */
    for (uint32_t va = USER_STACK_TOP - ELF_STACK_SIZE; va < USER_STACK_TOP;
         va += PAGE_SIZE) {
        uint32_t phys = pmm_alloc_frame();
        if (!phys) break;
        paging_map_page(va, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER);
    }

    /* Run as a scheduler task rather than through the older blocking
     * path. That is what makes clone() possible at all: a second thread
     * is simply a second task in this same address space, and
     * scheduler_run_single() blocks its caller exactly the way
     * process_run_user_mode() did. */
    user_active = 1;
    posix_process_begin();
    scheduler_run_single("main", entry, USER_STACK_TOP);
    posix_process_end();
    user_active = 0;

    if (owns_as) process_leave_address_space();
    return 1;
}

int process_run_pe(const uint8_t* image, uint32_t size, const char* name,
                   uint32_t* exit_code) {
    /* All the interesting work - loading, imports, the TEB, the Win32 API
     * itself - lives in kernel/win32.c. This wrapper exists so the shell
     * keeps calling process_run_*() uniformly for every binary format. */
    user_active = 1;
    pe_load_result_t result = win32_run_pe(image, size, name, name, 0, exit_code);
    user_active = 0;
    return (int)result;
}

/* --- faults in ring 3 --------------------------------------------------- */

/* Registered with the IDT for the CPU exception vectors. A fault in a
 * user program used to reach idt.c's panic screen and halt the machine,
 * which is the wrong outcome for "the program someone typed `run` on has
 * a bug in it" - and much more so now that the programs in question can
 * be arbitrary .exe files. This kills the program and returns to the
 * shell instead, deferring to the panic path only for faults that really
 * did happen in the kernel. */
static int handle_user_fault(registers_t* regs) {
    if (!user_active) return 0;
    if ((regs->cs & 3) != 3) return 0; /* the kernel itself faulted */

    /* A Win32 program gets the more detailed, Windows-shaped report. */
    if (win32_process_active()) {
        return win32_handle_user_fault(regs); /* does not return */
    }

    /* A POSIX program that installed a handler for the signal this fault
     * maps to gets it delivered instead of being killed - and, crucially,
     * gets to *return* from the handler, at which point the faulting
     * instruction is retried. That is the pattern Wine is built on: fault
     * on purpose, fix the mapping in the handler, carry on. */
    if (posix_handle_fault_signal(regs, regs->int_no)) return 1;

    char buf[16];
    terminal_writestring("\n");
    terminal_writestring_color("[kernel] ", VGA_COLOR_LIGHT_RED);
    terminal_writestring("User program faulted (exception ");
    ku32_to_dec(regs->int_no, buf);
    terminal_writestring(buf);
    terminal_writestring(") at eip=0x");
    ku32_to_hex(regs->eip, buf, 0, 8);
    terminal_writestring(buf);
    terminal_writestring("\n         Terminating it and returning to the shell.\n");

    user_active = 0;
    process_exit_to_kernel(); /* does not return */
    return 1;
}

void process_install_fault_handler(void) {
    idt_set_user_fault_hook(handle_user_fault);
}
