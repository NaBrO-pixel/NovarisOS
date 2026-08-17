; forkexec64.s - a real Linux program that forks, execs, and waits.
;
; The shape every shell has and Wine depends on: a process makes a copy
; of itself, the copy becomes a different program, and the original
; waits to be told how it went.
;
; The child execs /forkchild64, which exits with 24. The parent checks
; it got exactly that back, which is stronger than checking the calls
; returned success - a fork that quietly produced a thread instead of a
; process would satisfy every return value and fail here, because the
; "child" would have execed over its parent.

bits 64

SYS_write      equ 1
SYS_fork       equ 57
SYS_execve     equ 59
SYS_exit_group equ 231
SYS_wait4      equ 61

STDOUT equ 1

section .text
global _start

_start:
    mov rax, SYS_fork
    syscall
    test rax, rax
    js .fail_fork
    jz .child
    mov r12, rax                    ; the child's pid

    ; wait4(pid, &status, 0, NULL)
    mov rax, SYS_wait4
    mov rdi, r12
    lea rsi, [rel status]
    xor rdx, rdx
    xor r10, r10
    syscall
    cmp rax, r12
    jne .fail_wait

    ; The status is encoded: exit code in the second byte.
    mov eax, [rel status]
    shr eax, 8
    and eax, 0xff
    cmp eax, 24
    jne .fail_status

    mov rax, SYS_write
    mov rdi, STDOUT
    lea rsi, [rel msg]
    mov rdx, msg_len
    syscall

    mov rax, SYS_exit_group
    mov rdi, 71
    syscall

.child:
    ; Become a different program entirely. execve does not return.
    mov rax, SYS_execve
    lea rdi, [rel childpath]
    lea rsi, [rel child_argv]
    lea rdx, [rel child_envp]
    syscall
    ; Only reached if execve failed.
    mov rax, SYS_exit_group
    mov rdi, 99
    syscall

.fail_fork:   mov rdi, 96
              jmp .die
.fail_wait:   mov rdi, 97
              jmp .die
.fail_status: mov rdi, 98
.die:
    mov rax, SYS_exit_group
    syscall

section .data
childpath:  db "/tmp/forkchild64", 0
child_arg0: db "/tmp/forkchild64", 0
child_argv: dq child_arg0, 0
child_envp: dq 0

msg:     db "forked, execed a different program, and waited for it", 10
msg_len  equ $ - msg

section .bss
status:  resd 1
