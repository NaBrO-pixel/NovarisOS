; scheduler_asm.s - the two asm helpers scheduler.c needs to block a
; kernel-mode caller until a whole batch of scheduled processes has run
; to completion, mirroring process_asm.s's process_run_user_mode() /
; process_exit_to_kernel() pair for the single-process blocking case.
;
; scheduler_bootstrap_save_and_jump(first_esp): saves the caller's
; kernel esp/ebp (so we can find our way back), then jumps into the
; first process via the same pop/popa/iret sequence isr.s's shared stub
; epilogue uses - see scheduler.c's file-level comment for why a
; never-run process's synthetic frame and a real preempted one share
; this exact code path.
;
; scheduler_return_to_caller(): called only from scheduler_exit_current()
; when the last scheduled process exits. Restores the saved esp/ebp and
; executes the back half of a normal function epilogue, so control
; returns to scheduler_run_until_idle() as if scheduler_bootstrap_save_
; and_jump() had just returned normally - even though we got here from
; deep inside a totally different process's syscall handler call chain.

global scheduler_bootstrap_save_and_jump
global scheduler_return_to_caller
extern scheduler_resume_esp
extern scheduler_resume_ebp

section .text

; void scheduler_bootstrap_save_and_jump(uint32_t first_esp)
scheduler_bootstrap_save_and_jump:
    push ebp
    mov ebp, esp

    ; Same reasoning as process_asm.s: we come back through
    ; scheduler_return_to_caller from inside an interrupt handler, so the
    ; callee-saved registers have to be preserved here and restored there.
    push ebx
    push esi
    push edi

    mov [scheduler_resume_esp], esp
    mov [scheduler_resume_ebp], ebp

    mov eax, [ebp+8]     ; first_esp

    cli
    mov esp, eax

    ; Four separate segment registers, matching the frame isr.s builds and
    ; scheduler.c synthesizes (see registers_t in include/idt.h).
    pop gs
    pop fs
    pop es
    pop ds

    popa                  ; edi,esi,ebp,(esp slot),ebx,edx,ecx,eax
    add esp, 8             ; skip int_no, err_code
    iret
    ; Execution continues in ring 3 at the first process's entry point -
    ; we never reach past here directly.

; void scheduler_return_to_caller(void) -- called only from
; scheduler_exit_current(), while still running on whichever kernel
; stack happened to be current (irrelevant - we're about to abandon it).
scheduler_return_to_caller:
    mov esp, [scheduler_resume_esp]
    mov ebp, [scheduler_resume_ebp]
    sti
    pop edi
    pop esi
    pop ebx
    pop ebp    ; restores scheduler_run_until_idle()'s original ebp
    ret        ; returns to scheduler_run_until_idle(), right after the
               ; scheduler_bootstrap_save_and_jump() call
