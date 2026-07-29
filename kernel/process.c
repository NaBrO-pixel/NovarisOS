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

/* Non-zero between entering ring 3 and coming back out of it. Only the
 * fault path reads it, and only to decide whether a CPU exception belongs
 * to a user program or to the kernel. */
static int user_active = 0;

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
    process_run_user_mode(entry, stack_top);
    user_active = 0;
}

void process_run_flat_binary(const uint8_t* image, uint32_t size) {
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0) pages = 1;

    for (uint32_t i = 0; i < pages; i++) {
        uint32_t phys = pmm_alloc_frame();
        paging_map_page(USER_LOAD_VADDR + i * PAGE_SIZE, phys,
                         PAGE_PRESENT | PAGE_RW | PAGE_USER);
    }

    uint8_t* dest = (uint8_t*)USER_LOAD_VADDR;
    for (uint32_t i = 0; i < size; i++) dest[i] = image[i];

    /* One page of user stack, mapped just below USER_STACK_TOP so the
     * initial ESP we hand to ring 3 is a valid "one past the mapped
     * region" descending-stack pointer. */
    uint32_t stack_phys = pmm_alloc_frame();
    paging_map_page(USER_STACK_TOP - PAGE_SIZE, stack_phys,
                     PAGE_PRESENT | PAGE_RW | PAGE_USER);

    enter_user_mode(USER_LOAD_VADDR, USER_STACK_TOP);
}

void process_run_demo_user_program(void) {
    process_run_flat_binary(user_hello_bin, user_hello_bin_len);
}

int process_run_elf(const uint8_t* image, uint32_t size) {
    uint32_t entry;
    if (!elf_load(image, size, &entry)) return 0;

    uint32_t stack_phys = pmm_alloc_frame();
    paging_map_page(USER_STACK_TOP - PAGE_SIZE, stack_phys,
                     PAGE_PRESENT | PAGE_RW | PAGE_USER);

    enter_user_mode(entry, USER_STACK_TOP);
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
