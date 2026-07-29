#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>

/* Implemented in process_asm.s. Drops into ring 3 at `entry` with
 * `user_stack_top` as the initial ESP, and blocks (from the caller's
 * point of view) until the user program calls sys_exit. */
void process_run_user_mode(uint32_t entry, uint32_t user_stack_top);

/* Implemented in process_asm.s. Called only from the sys_exit syscall
 * handler; restores the kernel stack saved by process_run_user_mode and
 * returns control to whoever called it. Never returns to its own caller. */
void process_exit_to_kernel(void) __attribute__((noreturn));

/* Maps a flat (non-relocatable) binary image at USER_LOAD_VADDR, gives it
 * a one-page user stack, and runs it via process_run_user_mode(). Used
 * both for the boot-time embedded demo and for the shell's `run`
 * command loading a program from the initrd. Blocks until it exits. */
void process_run_flat_binary(const uint8_t* image, uint32_t size);

/* Runs the boot-time embedded "hello world" demo (see
 * kernel/user_hello_blob.c), via process_run_flat_binary(). */
void process_run_demo_user_program(void);

/* Loads a real ELF32 executable (PT_LOAD segments mapped at their own
 * ELF-specified addresses - see elf.c) and runs it the same way. Returns
 * 0 without running anything if `image` isn't a loadable ELF. */
int process_run_elf(const uint8_t* image, uint32_t size);

/* Loads a PE32 (.exe) image and runs it under the Win32 emulation layer.
 * A thin wrapper over win32_run_pe() - see include/win32.h, which is
 * where the interesting parts live. Returns a pe_load_result_t (PE_OK
 * once the program has run and exited), with the program's own exit code
 * in *exit_code. */
int process_run_pe(const uint8_t* image, uint32_t size, const char* name,
                   uint32_t* exit_code);

/* Selects the segment loaded into fs on the way into ring 3. Defaults to
 * 0x23 (the flat user data selector); the Win32 layer sets 0x33 so a
 * program's fs addresses its TEB (see gdt_set_teb). Applies to the next
 * process_run_user_mode() call. */
void process_set_user_fs(uint16_t selector);

/* Set while a ring-3 program started by process_run_user_mode() is
 * running. The CPU-exception path checks it: a fault in ring 3 should
 * kill the program, not panic the kernel. */
int process_user_active(void);

/* Registers that fault handler with the IDT. Call once at boot, after
 * idt_install(). */
void process_install_fault_handler(void);

#endif
