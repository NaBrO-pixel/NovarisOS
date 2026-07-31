; win32_callback_asm.s - the ring0 -> ring3 -> ring0 round trip for a
; Win32 API implementation that has to call a function belonging to the
; program (a qsort comparator, an atexit handler). See
; include/win32_callback.h for what this is for and kernel/win32_callback.c
; for the C side that drives it.
;
; This is a near-mirror of process_asm.s, and for the same reason: the
; trick depends on controlling the exact stack layout at entry, which is
; not something to leave to a C compiler's prologue. The difference is
; that process_run_user_mode() enters a whole program and there is exactly
; one of it in flight, so it can save its resume context in two globals.
; A callback nests - a comparator may call qsort again - so the resume
; context is passed in by address, and the C side keeps a stack of them.

global win32_callback_enter
global win32_callback_leave

; Which selector fs gets on the way into ring 3. Shared with
; process_asm.s: while a Win32 program runs this is 0x33, so fs addresses
; its TEB. A callback runs inside that same program, so it wants exactly
; the same value - reusing the global rather than hardcoding 0x33 keeps
; the two in step.
extern user_fs_selector

section .text

; uint32_t win32_callback_enter(uint32_t entry, uint32_t user_esp,
;                               win32_cb_frame_t* frame)
;
; Saves the kernel context into *frame, then drops to ring 3 at `entry`
; with `user_esp`. Does not return here - control comes back through
; win32_callback_leave below, which makes this look like an ordinary
; blocking call that returns the callback's eax.
win32_callback_enter:
    push ebp
    mov ebp, esp

    ; Same reasoning as process_run_user_mode: we come back from deep
    ; inside an interrupt handler, so the callee-saved registers have to
    ; be preserved here or our caller's locals silently rot.
    push ebx
    push esi
    push edi

    ; frame->esp / frame->ebp. Done before the segment registers are
    ; reloaded, while ds still addresses kernel data.
    mov eax, [ebp+16]
    mov [eax+0], esp
    mov [eax+4], ebp

    cli

    ; Ring-3 data selectors. This clobbers dx, and eax/ecx are reloaded
    ; from the stack afterwards rather than trusted to survive - the
    ; [ebp+n] reads go through ss, which is still the kernel's.
    mov dx, 0x23
    mov ds, dx
    mov es, dx
    mov gs, dx
    mov dx, [user_fs_selector]
    mov fs, dx

    mov eax, [ebp+8]    ; entry
    mov ecx, [ebp+12]   ; user_esp

    push dword 0x23     ; SS  = user data | RPL3
    push ecx            ; ESP = the frame win32_callback_call built
    pushfd
    pop edx
    or edx, 0x200       ; force IF on, so the timer can still preempt
    push edx            ; EFLAGS
    push dword 0x1B     ; CS  = user code | RPL3
    push eax            ; EIP = the program's callback
    iret
    ; Ring 3 from here. Never falls through.

; void win32_callback_leave(win32_cb_frame_t* frame, uint32_t retval)
;
; Called from the int 0x82 handler, running on the *nested* trap's kernel
; stack. Switches back to the esp win32_callback_enter saved and finishes
; that function's epilogue, so it returns `retval` to its caller. The
; nested trap frame is simply abandoned: we are not going back to the
; ring-3 context that raised it, the callback is over.
win32_callback_leave:
    mov ecx, [esp+4]    ; frame
    mov eax, [esp+8]    ; retval -> win32_callback_enter's return value
    mov esp, [ecx+0]
    mov ebp, [ecx+4]
    sti
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret
