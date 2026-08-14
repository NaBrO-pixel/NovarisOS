; hello64.s - a real, static, x86-64 Linux executable.
;
; Nothing about this file knows what Novaris is. It is assembled and
; linked by the host toolchain into an ordinary ELF64 executable, it uses
; Linux's x86-64 syscall numbers and register convention, and it will run
; unmodified on Linux - which is the entire point of it. If it prints on
; Novaris too, then what Novaris implements is Linux's ABI rather than a
; private convention that resembles it.
;
;   rax = syscall number
;   rdi, rsi, rdx, r10, r8, r9 = arguments   (rcx and r11 are destroyed)
;
; Deliberately assembled with no libc: glibc's startup would need a great
; deal more of the ABI than exists yet - and getting that far is the point
; of the milestones after this one, not this one.

bits 64

SYS_write equ 1
SYS_exit  equ 60

STDOUT    equ 1

section .text
global _start

_start:
    mov rax, SYS_write
    mov rdi, STDOUT
    lea rsi, [rel msg]
    mov rdx, msg_len
    syscall

    ; A distinctive status, so the kernel can tell "the program exited"
    ; from "something else ended the run".
    mov rax, SYS_exit
    mov rdi, 42
    syscall

    ; exit does not return. If it somehow does, do not run into whatever
    ; happens to follow.
.hang:
    jmp .hang

; In .text rather than .rodata so the whole program is one PT_LOAD, which
; keeps what this is testing to the loader rather than to ld's layout.
msg:     db "hello from a real x86-64 ELF, loaded by Novaris", 10
msg_len  equ $ - msg
