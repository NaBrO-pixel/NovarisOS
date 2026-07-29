; process_asm.s - the ring0<->ring3 transition.
;
; process_run_user_mode(entry, user_stack_top) drops into ring 3 via the
; classic iret trick, and never returns through a normal `ret` - instead,
; when the user program's sys_exit syscall fires, the kernel calls
; process_exit_to_kernel below, which restores the exact esp/ebp we saved
; on the way in and executes the missing half of a normal function
; epilogue (pop ebp; ret). The net effect: from the perspective of
; whoever called process_run_user_mode, it looks like an ordinary
; function call that blocks until the user program exits.
;
; We hand-write this in assembly (like gdt_flush.s/idt_flush.s already do
; for LGDT/LIDT) instead of using inline asm in a C function, because this
; trick depends on controlling the exact stack layout at entry - something
; a C compiler's own prologue (and -O2's frame-pointer omission) would
; make fragile to rely on.

global process_run_user_mode
global process_exit_to_kernel
extern kernel_resume_esp
extern kernel_resume_ebp
; Which selector fs gets on the way into ring 3. 0x23 (flat user data) for
; ordinary programs; the Win32 layer sets 0x33 so fs addresses the running
; program's TEB. See process_set_user_fs() in process.c.
extern user_fs_selector

section .text

; void process_run_user_mode(uint32_t entry, uint32_t user_stack_top)
process_run_user_mode:
    push ebp
    mov ebp, esp

    ; Save the callee-saved registers the way a normal function would.
    ; This matters more than it looks: control comes back through
    ; process_exit_to_kernel from deep inside an interrupt handler, with
    ; ebx/esi/edi holding whatever *that* call chain left in them. Without
    ; restoring them here, any variable our caller kept in a callee-saved
    ; register across this call silently becomes garbage after the user
    ; program exits - which is exactly the kind of bug that shows up as
    ; one wrong value somewhere unrelated, long after the fact.
    push ebx
    push esi
    push edi

    ; Save kernel context: this esp is what process_exit_to_kernel restores
    ; to unwind back to our caller (see the comment there).
    mov [kernel_resume_esp], esp
    mov [kernel_resume_ebp], ebp

    mov eax, [ebp+8]   ; entry
    mov ecx, [ebp+12]  ; user_stack_top

    cli

    ; Load user-mode data segment selectors (0x20 = user data GDT entry,
    ; | 3 for RPL 3). This clobbers ax, so eax/ecx get reloaded from the
    ; stack args below rather than trusted to survive.
    mov dx, 0x23
    mov ds, dx
    mov es, dx
    mov gs, dx
    mov dx, [user_fs_selector]
    mov fs, dx

    mov eax, [ebp+8]
    mov ecx, [ebp+12]

    push dword 0x23      ; SS = user data selector | RPL3
    push ecx              ; ESP = user_stack_top
    pushfd
    pop edx
    or edx, 0x200          ; force IF set, so the timer can still preempt ring 3
    push edx               ; EFLAGS
    push dword 0x1B        ; CS = user code selector (0x18) | RPL3
    push eax               ; EIP = entry
    iret
    ; Execution continues in ring 3 - we never reach past here directly.

; void process_exit_to_kernel(void)  -- called only from the sys_exit
; syscall handler, while still running with the kernel's interrupt stack
; (loaded automatically via the TSS when the ring-3 `int 0x80` fired).
process_exit_to_kernel:
    mov esp, [kernel_resume_esp]
    mov ebp, [kernel_resume_ebp]
    sti
    ; The whole epilogue of process_run_user_mode lives here, because the
    ; function itself never falls through to one - the only way back out
    ; of ring 3 is this path.
    pop edi
    pop esi
    pop ebx
    pop ebp    ; restores our caller's original ebp (mirrors `mov ebp,esp` above)
    ret        ; returns to whoever called process_run_user_mode
