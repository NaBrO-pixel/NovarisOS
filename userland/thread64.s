; thread64.s - a real Linux x86-64 program that creates a thread.
;
; Raw syscalls, no libc. glibc's pthread_create wants a great deal more
; of the ABI than exists yet (futex, robust lists, a signal for
; cancellation), and what is being tested here is clone itself: two
; threads sharing one address space, both getting the CPU.
;
; The parent spins on a memory location until the child writes to it,
; which only completes if the scheduler actually switches between them -
; a kernel that created the thread but never ran it would hang here, and
; the test's timeout would catch that rather than passing quietly.
;
; The child does not exit. Thread exit is not implemented (see
; ROADMAP.md), so it parks in a loop and the parent's exit ends the run.

bits 64

SYS_write equ 1
SYS_mmap  equ 9
SYS_clone equ 56
SYS_exit  equ 60                    ; ends this THREAD only

; exit_group, not exit. exit(60) ends the calling THREAD; with the child
; parked in a loop the process would stay alive and the run would hang -
; which is exactly what running this on Linux did before the number was
; corrected. Novaris does not yet make that distinction (see ROADMAP.md),
; so only the host run could have caught it.
SYS_exit_group equ 231

STDOUT    equ 1

; CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD
CLONE_FLAGS equ 0x00010f00

STACK_SIZE  equ 65536
MAGIC       equ 0x5A5A5A5A

section .text
global _start

_start:
    ; A stack for the child, from the kernel rather than from .bss, so
    ; that mmap is exercised as part of it.
    mov rax, SYS_mmap
    xor rdi, rdi                    ; addr: let the kernel choose
    mov rsi, STACK_SIZE
    mov rdx, 3                      ; PROT_READ | PROT_WRITE
    mov r10, 0x22                   ; MAP_PRIVATE | MAP_ANONYMOUS
    mov r8, -1                      ; no fd
    xor r9, r9
    syscall
    cmp rax, 0
    jle .fail_mmap

    ; Stacks grow down, so the child starts at the far end. Kept
    ; 16-byte aligned, which the ABI requires and which anything using
    ; SSE will enforce the hard way.
    add rax, STACK_SIZE
    and rax, -16
    mov rsi, rax                    ; child stack

    mov rax, SYS_clone
    mov rdi, CLONE_FLAGS
    xor rdx, rdx                    ; parent_tid
    xor r10, r10                    ; child_tid
    xor r8, r8                      ; tls
    syscall

    test rax, rax
    jz .child                       ; 0 means this is the child
    js .fail_clone                  ; negative means it failed

    ; --- parent ---
    ; Spin until the child writes. Nothing here yields, so this only
    ; ends if the timer preempts the parent and runs the child.
.wait:
    mov eax, [rel shared]
    cmp eax, MAGIC
    jne .wait

    mov rax, SYS_write
    mov rdi, STDOUT
    lea rsi, [rel msg_ok]
    mov rdx, msg_ok_len
    syscall

    mov rax, SYS_exit_group
    mov rdi, 23                     ; the status the kernel checks for
    syscall

    ; --- child ---
    ; Writes to memory the parent is reading, which is the point: the two
    ; share one address space.
.child:
    mov dword [rel shared], MAGIC
    ; exit, not exit_group: this ends the CHILD and leaves the parent
    ; running. If the two were the same thing the parent would never get
    ; to print, and on Linux the process would die with status 0.
    mov rax, SYS_exit
    xor rdi, rdi
    syscall
.park:
    jmp .park                       ; unreachable

.fail_mmap:
    mov rax, SYS_exit_group
    mov rdi, 81
    syscall

.fail_clone:
    mov rax, SYS_exit_group
    mov rdi, 82
    syscall

msg_ok:     db "the child thread ran and shared memory with its parent", 10
msg_ok_len  equ $ - msg_ok

section .data
align 8
shared:     dd 0
