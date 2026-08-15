; futex64.s - two threads handing off through a futex.
;
; The difference from thread64.s is that nothing here spins. That one's
; parent burned CPU until the timer happened to run the child; this one
; goes to sleep and is woken. A kernel that implemented FUTEX_WAIT as
; "return 0 immediately" would still pass thread64.s - the parent would
; just spin through the loop again - and cannot pass this one, because
; there is no loop to fall back into.
;
; The ordering is deliberately race-free, and that is what the value
; argument to FUTEX_WAIT is for. If the child wins the race and sets the
; flag first, the parent's FUTEX_WAIT sees the value has changed and
; returns -EAGAIN instead of sleeping forever with nobody left to wake
; it. Both orders end the same way.

bits 64

SYS_write      equ 1
SYS_mmap       equ 9
SYS_clone      equ 56
SYS_exit       equ 60
SYS_futex      equ 202
SYS_exit_group equ 231

STDOUT equ 1

FUTEX_WAIT_PRIVATE equ 128          ; 0 | FUTEX_PRIVATE_FLAG
FUTEX_WAKE_PRIVATE equ 129          ; 1 | FUTEX_PRIVATE_FLAG

CLONE_FLAGS equ 0x00010f00          ; VM|FS|FILES|SIGHAND|THREAD
STACK_SIZE  equ 65536

section .text
global _start

_start:
    mov rax, SYS_mmap
    xor rdi, rdi
    mov rsi, STACK_SIZE
    mov rdx, 3                      ; PROT_READ | PROT_WRITE
    mov r10, 0x22                   ; MAP_PRIVATE | MAP_ANONYMOUS
    mov r8, -1
    xor r9, r9
    syscall
    cmp rax, 0
    jle .fail

    add rax, STACK_SIZE
    and rax, -16
    mov rsi, rax

    mov rax, SYS_clone
    mov rdi, CLONE_FLAGS
    xor rdx, rdx
    xor r10, r10
    xor r8, r8
    syscall
    test rax, rax
    jz .child
    js .fail

    ; --- parent: sleep until the child hands over ---
.recheck:
    mov eax, [rel flag]
    test eax, eax
    jnz .woken                      ; the child got there first

    mov rax, SYS_futex
    lea rdi, [rel flag]
    mov rsi, FUTEX_WAIT_PRIVATE
    xor rdx, rdx                    ; expected value: still 0
    xor r10, r10                    ; no timeout
    xor r8, r8
    xor r9, r9
    syscall
    ; Woken (0) or -EAGAIN because the value moved; either way look
    ; again rather than trusting the wakeup, which is how futex is
    ; meant to be used.
    jmp .recheck

.woken:
    mov rax, SYS_write
    mov rdi, STDOUT
    lea rsi, [rel msg]
    mov rdx, msg_len
    syscall

    mov rax, SYS_exit_group
    mov rdi, 31
    syscall

    ; --- child: wait until the parent is really asleep, then hand over ---
    ;
    ; The obvious version - set the flag, wake once - is correct but
    ; races: if the child gets there first the parent never blocks at
    ; all, and a test that asserts "a thread really blocked" then fails
    ; through no fault of the kernel. It did, about one run in three.
    ;
    ; FUTEX_WAKE returns how many it woke, which is the kernel telling
    ; us whether anyone was actually asleep. Spinning on that until it
    ; returns non-zero makes "the parent has blocked" an observed fact
    ; rather than a hope. The flag is still 0 at that point, so the
    ; parent rechecks and blocks a second time - which is exactly what a
    ; waiter is supposed to do with a wakeup it cannot explain.
.child:
.try_wake:
    mov rax, SYS_futex
    lea rdi, [rel flag]
    mov rsi, FUTEX_WAKE_PRIVATE
    mov rdx, 1
    xor r10, r10
    xor r8, r8
    xor r9, r9
    syscall
    test rax, rax
    jz .try_wake                    ; nobody was blocked yet

    ; Now hand over for real.
    mov dword [rel flag], 1

    mov rax, SYS_futex
    lea rdi, [rel flag]
    mov rsi, FUTEX_WAKE_PRIVATE
    mov rdx, 1                      ; wake one waiter
    xor r10, r10
    xor r8, r8
    xor r9, r9
    syscall

    mov rax, SYS_exit               ; this thread only
    xor rdi, rdi
    syscall

.fail:
    mov rax, SYS_exit_group
    mov rdi, 83
    syscall

msg:     db "the parent slept on a futex and the child woke it", 10
msg_len  equ $ - msg

section .data
align 8
flag:    dd 0
