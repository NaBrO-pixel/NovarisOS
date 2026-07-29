; task_b.s - tiny demo user-mode program for the Milestone 9 preemptive
; scheduler: loops printing a short tag with a busy-wait spin between
; each print, so a single scheduler time slice can't race through all
; iterations before the timer ever gets a chance to preempt it - without
; the spin, a tight print loop finishes so fast relative to a 10-20ms
; slice that one task can grab a large head start before the others get
; scheduled at all. Multiple copies of this program (task_a/b/c) running
; concurrently produce visibly interleaved output if - and only if - the
; kernel is really switching between them on a timer.
;
; ORG must match the load address scheduler_spawn_flat() is given for
; this task in kernel/shell.c's `multitask` command (0x50200000) - flat,
; non-relocatable binary, same convention as userland/user_hello.s.
;
; To regenerate kernel/task_b_blob.c after editing this file:
;   nasm -f bin userland/task_b.s -o userland/task_b.bin
;   python3 userland/embed_tasks.py

BITS 32
ORG 0x50200000

_start:
    mov esi, 12          ; iteration count
.loop:
    mov eax, 1            ; SYS_WRITE
    mov ebx, msg
    int 0x80

    mov edi, 3000000      ; busy-wait spin between prints (see comment
.spin:                     ; above) - long enough that one scheduler
    dec edi                ; time slice covers only a fraction of it
    jnz .spin

    dec esi
    jnz .loop

    mov eax, 0             ; SYS_EXIT
    int 0x80

.halt_loop:
    jmp .halt_loop          ; should never be reached: sys_exit doesn't return

msg: db "[B]", 10, 0
