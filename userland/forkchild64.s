; forkchild64.s - the program forkexec64 becomes.
;
; Deliberately a separate binary rather than a branch inside the parent:
; execve has to replace the address space with something that was never
; part of it, and a test where the "new" program was already mapped
; would prove nothing about that.

bits 64

SYS_write      equ 1
SYS_exit_group equ 231

STDOUT equ 1

section .text
global _start

_start:
    mov rax, SYS_write
    mov rdi, STDOUT
    lea rsi, [rel msg]
    mov rdx, msg_len
    syscall

    ; The value the parent checks for.
    mov rax, SYS_exit_group
    mov rdi, 24
    syscall

section .data
msg:     db "  (the execed child is running)", 10
msg_len  equ $ - msg
