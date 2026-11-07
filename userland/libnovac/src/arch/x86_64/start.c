/* Process entry for x86-64. The kernel enters _start with rsp pointing at
 * argc, followed by argv[], a NULL, envp[], a NULL and the auxiliary vector
 * (kernel/uspace64.c). The trampoline hands that pointer to C. */
#include <stdlib.h>
#include <stddef.h>

extern int main(int argc, char **argv, char **envp);

char **environ;

__attribute__((used, noreturn))
static void novac_start_c(long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    environ = argv + argc + 1;
    exit(main(argc, argv, environ));
}

__asm__(
    ".text\n"
    ".globl _start\n"
    ".type _start,@function\n"
    "_start:\n"
    "    xorl %ebp, %ebp\n"
    "    movq %rsp, %rdi\n"
    "    andq $-16, %rsp\n"
    "    call novac_start_c\n"
    "    hlt\n"
);
