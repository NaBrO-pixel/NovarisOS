; signal64.s - a real Linux program that catches SIGSEGV and carries on.
;
; This is Wine's exception dispatch in miniature. Wine installs a SIGSEGV
; handler, and when a Windows program faults it reads the saved registers
; out of the ucontext, builds an EXCEPTION_RECORD, and very often writes
; RIP back so execution resumes somewhere else entirely. A handler that
; cannot rewrite RIP is no use to it.
;
; So this does exactly that: dereference NULL, catch the fault, move RIP
; past it, and keep going. It runs unmodified on Linux, which is the only
; way to know the frame layout is right rather than merely self-consistent.
;
;   ucontext offsets: uc_mcontext at +40, and within it rip at +128.
;   So the saved RIP is at rdx + 168 when the handler is entered.

bits 64

SYS_write         equ 1
SYS_rt_sigaction  equ 13
SYS_rt_sigreturn  equ 15
SYS_exit_group    equ 231

STDOUT   equ 1
SIGSEGV  equ 11

SA_SIGINFO  equ 0x00000004
SA_RESTORER equ 0x04000000

UC_MCONTEXT_RIP equ 40 + 128        ; offset of saved RIP within ucontext

section .text
global _start

_start:
    ; rt_sigaction(SIGSEGV, &act, NULL, 8)
    ;
    ; The fourth argument is sizeof(sigset_t) and the kernel checks it.
    ; SA_RESTORER is mandatory on x86-64: the kernel supplies no return
    ; trampoline, so the handler would otherwise return into nothing.
    mov rax, SYS_rt_sigaction
    mov rdi, SIGSEGV
    lea rsi, [rel act]
    xor rdx, rdx
    mov r10, 8
    syscall
    test rax, rax
    js .fail

    ; The fault. Deliberate, and to an address no mapping will ever
    ; cover, so nothing can quietly make it succeed.
    xor rax, rax
    mov rax, [rax]                  ; <- SIGSEGV here

    ; The handler sends us here by rewriting RIP.
.resumed:
    mov eax, [rel caught]
    cmp eax, 1
    jne .fail

    mov rax, SYS_write
    mov rdi, STDOUT
    lea rsi, [rel msg]
    mov rdx, msg_len
    syscall

    mov rax, SYS_exit_group
    mov rdi, 41
    syscall

.fail:
    mov rax, SYS_exit_group
    mov rdi, 84
    syscall

; --- the handler: void handler(int signo, siginfo_t *info, ucontext_t *uc)
;
; Records that it ran, then points the saved RIP at .resumed. Returning
; normally lands on pretcode, which is the restorer below.
handler:
    mov dword [rel caught], 1
    lea rax, [rel _start.resumed]
    mov [rdx + UC_MCONTEXT_RIP], rax
    ret

; --- the restorer, which the kernel jumps to when the handler returns
restorer:
    mov rax, SYS_rt_sigreturn
    syscall

section .data
align 16
; struct kernel_sigaction { handler; flags; restorer; mask; }
act:
    dq handler
    dq SA_SIGINFO | SA_RESTORER
    dq restorer
    dq 0

align 8
caught:  dd 0

msg:     db "caught SIGSEGV, rewrote RIP, and carried on", 10
msg_len  equ $ - msg
