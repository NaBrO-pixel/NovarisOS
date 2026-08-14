#ifndef SYSCALL64_H
#define SYSCALL64_H

#include <stdint.h>

/* Ring 3, and the syscall instruction that gets back out of it.
 *
 * `int 0x80` is not how a 64-bit kernel is entered. SYSCALL/SYSRET are a
 * pair of instructions with no descriptor lookup and no stack switch at
 * all - which is the part that has to be handled by hand: SYSCALL leaves
 * the user's rsp loaded, so the entry stub is running on a ring-3 stack
 * until it moves off it.
 *
 * The selectors come out of three MSRs rather than the IDT, and they are
 * computed from one base by fixed offsets, which is why gdt64.h orders
 * the user pair data-before-code:
 *
 *   SYSCALL:  CS = STAR[47:32],      SS = STAR[47:32] + 8
 *   SYSRET :  CS = STAR[63:48] + 16, SS = STAR[63:48] + 8   (both RPL 3)
 *
 * With STAR[47:32] = 0x08 that gives kernel 0x08/0x10, and with
 * STAR[63:48] = 0x10 it gives user 0x23/0x1B. */

/* Linux's x86-64 numbers, not Novaris's own and not the i386 ones.
 *
 * This is the point where Milestone 44's item 4 starts being paid for.
 * The 32-bit kernel implements Linux's *i386* ABI - `write` is 4 there
 * and 1 here, `exit` is 1 there and 60 here, the arguments arrive in
 * different registers, and the structures they point at are laid out
 * differently. Every one of those has to be re-earned, and using the
 * real numbers from the start is how the rest of it gets earned against
 * something real rather than against a private convention. */
#define SYS64_WRITE       1
#define SYS64_EXIT        60
#define SYS64_EXIT_GROUP  231

/* One Novaris-private number, well above anything Linux uses, kept for
 * the bring-up test that has no way to print. */
#define SYS64_ECHO  0x1000   /* returns its argument + 0x1111 */

/* Sets EFER.SCE, STAR, LSTAR and FMASK. Call after gdt64_install(). */
void syscall64_init(void);

/* Enters ring 3 at user_rip with user_rsp and `arg` in rdi, and returns
 * here when the program makes a SYS64_EXIT call - whichever program that
 * turns out to be, once a scheduler is rotating between several.
 * Implemented in syscall64.s. */
extern void enter_user_mode64(uint64_t user_rip, uint64_t user_rsp,
                              uint64_t arg);

/* The ring-3 test program, as bytes to be copied into a user page. It is
 * position independent - immediates and `syscall`, no absolute
 * addressing - so it runs wherever it is mapped. */
extern uint8_t user_test_code[];
extern uint8_t user_test_code_end[];

/* The scheduler's test program: increments the counter whose address it
 * is given, forever. It is stopped by resuming it at task_count_exit
 * rather than by anything it does itself. */
extern uint8_t task_count_code[];
extern uint8_t task_count_exit[];
extern uint8_t task_count_code_end[];

/* What the dispatcher saw. */
uint64_t syscall64_count(void);
uint64_t syscall64_last_arg(void);
uint64_t syscall64_exit_code(void);
uint64_t syscall64_bytes_written(void);

#endif
