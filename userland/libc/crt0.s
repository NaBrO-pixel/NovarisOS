; crt0.s - the very first code that runs in a user-mode C program linked
; against Novaris's tiny libc. Mirrors a normal C runtime's _start: call
; main(), then pass its return value to exit() (which never returns).

BITS 32
global _start
extern main
extern exit

_start:
    call main
    push eax        ; main()'s return value becomes exit()'s argument
    call exit
.hang:
    jmp .hang        ; exit() should never return, but just in case
