; syscall64.s - getting into ring 3, and back out of it.
;
; Three pieces: the trampoline that drops to ring 3, the SYSCALL entry
; point the CPU jumps to on the way back in, and a tiny ring-3 program to
; prove both work.
;
; The awkward part of SYSCALL is what it does *not* do. It performs no
; stack switch: on entry rsp is still the user's, so the first thing the
; handler can safely do is move off it, and it cannot push anything until
; it has. It also destroys rcx and r11, which is where it puts the return
; rip and rflags, so those two have to be preserved by hand.
;
; A multiprocessor kernel does the stack switch with swapgs and a per-CPU
; block reached through GS. This one is single-CPU and single-threaded
; during bring-up, so it uses RIP-relative globals instead; swapgs is what
; this turns into when there is more than one CPU to tell apart.

bits 64

UDATA_SEL equ 0x18
UCODE_SEL equ 0x20
RPL3      equ 3

SYS64_EXIT equ 1

section .text

extern syscall64_dispatch

; ---------------------------------------------------------------------
; void enter_user_mode64(uint64_t user_rip /*rdi*/, uint64_t user_rsp /*rsi*/)
;
; Returns when the ring-3 program makes a SYS64_EXIT call, as if it were
; an ordinary call that took a long time. The callee-saved registers are
; pushed here and popped by the exit path, so the C either side of it
; sees a normal function call.
; ---------------------------------------------------------------------
global enter_user_mode64
enter_user_mode64:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [rel kernel_return_rsp], rsp

    ; The frame iretq consumes, top of stack last: SS, RSP, RFLAGS, CS, RIP.
    push qword UDATA_SEL | RPL3
    push rsi                        ; user rsp
    push qword 0x202                ; RFLAGS: IF set, bit 1 always set
    push qword UCODE_SEL | RPL3
    push rdi                        ; user rip
    iretq

; ---------------------------------------------------------------------
; The SYSCALL entry point. rcx = return rip, r11 = return rflags, and the
; arguments follow Linux's convention: rax = number, then rdi, rsi, rdx.
; Only the number and one argument are used during bring-up.
; ---------------------------------------------------------------------
global syscall64_entry
syscall64_entry:
    ; Still on the user's stack here. Nothing may be pushed until after
    ; the next two instructions.
    mov [rel saved_user_rsp], rsp
    lea rsp, [rel syscall_stack_top]

    push rcx                        ; user rip, destroyed by SYSCALL
    push r11                        ; user rflags, likewise
    push rax                        ; the number, for the exit check below

    ; syscall64_dispatch(nr /*rdi*/, arg1 /*rsi*/). The incoming values
    ; are the other way round, so they cross over.
    mov rsi, rdi
    mov rdi, rax
    call syscall64_dispatch         ; result in rax

    pop rcx                         ; the number again
    cmp rcx, SYS64_EXIT
    je .leave_ring3

    pop r11
    pop rcx
    mov rsp, [rel saved_user_rsp]
    o64 sysret

    ; SYS64_EXIT: do not go back to ring 3. Restore the stack
    ; enter_user_mode64 was called on and return to its caller, with rax
    ; still holding whatever the dispatcher returned.
.leave_ring3:
    mov rsp, [rel kernel_return_rsp]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

; ---------------------------------------------------------------------
; The ring-3 program. Position independent by construction, so it runs at
; whatever address it is copied to.
;
; It reads its own CS and hands it to the kernel, which is the only
; unforgeable evidence that this really is ring 3: a copy of this code
; running at ring 0 would report 0x08, not 0x23. Then it feeds the value
; the kernel returned into the exit call, which proves SYSRET came back
; here with a result rather than the program never resuming.
; ---------------------------------------------------------------------
global user_test_code
global user_test_code_end
user_test_code:
    mov rax, 2                      ; SYS64_ECHO
    mov rdi, cs                     ; 0x23 if this is really ring 3
    syscall                         ; -> rax = rdi + 0x1111
    mov rdi, rax
    mov rax, 1                      ; SYS64_EXIT, with that value
    syscall
.hang:
    jmp .hang                       ; unreachable: exit does not return
user_test_code_end:

section .bss
align 16
saved_user_rsp:     resq 1
kernel_return_rsp:  resq 1

; The stack the entry stub switches to. Reached with `lea`, so it is the
; address itself rather than a pointer to be filled in - one less thing
; that can be left uninitialised on the path where the CPU is still on a
; ring-3 stack and cannot report anything.
align 16
syscall_stack_bottom:
    resb 16384
syscall_stack_top:
