; thread_demo.s - Milestone 16's evidence that threads share memory.
;
; ONE copy of this image is loaded, and TWO scheduler tasks are pointed at
; it: one at thread0 (the load address) and one at thread1 (a fixed offset
; so the kernel can name it without parsing anything). They get separate
; kernel stacks, separate user stacks and separate register contexts, and
; the same page directory - which is the entire definition of a thread
; here.
;
; Both threads increment the same `counter` word. Since they are two
; different tasks running in one address space, that word is literally the
; same physical memory, and after both have exited the kernel reads it
; back: 2 * ITERATIONS means both really were incrementing one shared
; variable. Anything less would mean they had been given private copies.
;
; The printed tags interleave for the same reason task_a/b/c's do - the
; timer preempts them mid-spin.
;
; ORG must match the load address kernel/shell.c uses for `threadtest`.
;
; To regenerate kernel/thread_demo_blob.c after editing this file:
;   nasm -f bin userland/thread_demo.s -o userland/thread_demo.bin
;   python3 userland/embed_tasks.py

BITS 32
ORG 0x51000000

; Linux/i386 syscall numbers - Milestone 18 adopted the real ABI, see
; include/posix.h.
%define SYS_EXIT   1
%define SYS_WRITE  4
%define ITERATIONS 8

; --- thread 0 entry: the load address itself ----------------------------
thread0:
    mov ebp, msg0
    jmp body

; --- thread 1 entry: load address + 0x40 --------------------------------
; A fixed offset rather than a computed one, because the kernel has to
; know it (THREAD1_ENTRY_OFFSET in kernel/shell.c) and a flat binary
; carries no symbol table to look it up in.
    times 0x40-($-$$) db 0
thread1:
    mov ebp, msg1
    jmp body

; --- the body both threads run ------------------------------------------
; ebp holds this thread's message. It survives `int 0x80` because the ISR
; saves and restores every register (pusha), so there is no need to keep
; reloading it.
body:
    mov esi, ITERATIONS
.loop:
    ; The shared write. `lock` is not strictly required on a single core -
    ; a preemption can only land between whole instructions - but it says
    ; what this line is for.
    lock inc dword [counter]

    mov eax, SYS_WRITE
    mov ebx, 1              ; fd 1 = stdout
    mov ecx, ebp            ; this thread's message
    mov edx, MSG_LEN
    int 0x80

    ; Busy-wait so a single ~20ms time slice cannot run the whole loop
    ; before the timer ever gets a chance to preempt - same reasoning as
    ; userland/task_a.s.
    mov edi, 2000000
.spin:
    dec edi
    jnz .spin

    dec esi
    jnz .loop

    mov eax, SYS_EXIT
    xor ebx, ebx
    int 0x80

.hang:
    jmp .hang               ; sys_exit does not return

msg0: db "[thread 0]", 10
msg1: db "[thread 1]", 10
MSG_LEN equ 11          ; both messages are the same length

; The shared counter, parked at a fixed offset for the same reason
; thread1's entry is: the kernel reads it back through
; load address + THREAD_COUNTER_OFFSET once both threads have exited.
    times 0x200-($-$$) db 0
counter: dd 0
