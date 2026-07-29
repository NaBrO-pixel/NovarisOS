; isr.s - low-level entry stubs for CPU exceptions and hardware IRQs.
;
; The CPU pushes different things depending on the vector: some exceptions
; push an error code, some don't. To keep the C-side registers_t struct
; layout consistent, every stub without a real error code pushes a dummy 0
; so the stack always looks the same by the time we reach the C dispatcher.

%macro ISR_NOERR 1
global isr%1
isr%1:
    cli
    push dword 0        ; dummy error code
    push dword %1        ; interrupt number
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    cli
    ; CPU already pushed a real error code here
    push dword %1
    jmp isr_common_stub
%endmacro

%macro IRQ 2
global irq%1
irq%1:
    cli
    push dword 0
    push dword %2
    jmp irq_common_stub
%endmacro

; CPU exceptions 0-31. Vectors 8, 10-14, 17 push a real error code;
; everything else gets a dummy 0 from us so the frame stays uniform.
ISR_NOERR 0    ; Divide-by-zero
ISR_NOERR 1    ; Debug
ISR_NOERR 2    ; NMI
ISR_NOERR 3    ; Breakpoint
ISR_NOERR 4    ; Overflow
ISR_NOERR 5    ; Bound range exceeded
ISR_NOERR 6    ; Invalid opcode
ISR_NOERR 7    ; Device not available
ISR_ERR   8    ; Double fault
ISR_NOERR 9    ; Coprocessor segment overrun (legacy)
ISR_ERR   10   ; Invalid TSS
ISR_ERR   11   ; Segment not present
ISR_ERR   12   ; Stack-segment fault
ISR_ERR   13   ; General protection fault
ISR_ERR   14   ; Page fault
ISR_NOERR 15   ; Reserved
ISR_NOERR 16   ; x87 floating point exception
ISR_ERR   17   ; Alignment check
ISR_NOERR 18   ; Machine check
ISR_NOERR 19   ; SIMD floating point exception
ISR_NOERR 20   ; Virtualization exception
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; Syscall interrupt (int 0x80). Ring-3 code needs to be allowed to invoke
; this directly, unlike every gate above - its IDT entry gets DPL=3 in
; idt.c, while every other gate stays DPL=0 so user code can't fake a CPU
; exception or hardware IRQ.
ISR_NOERR 128

; Win32 emulation entry (int 0x81), also DPL=3. Separate from 0x80 so the
; OS's own syscall convention and the Win32 thunk convention never have to
; share a dispatcher - see include/win32.h.
ISR_NOERR 129

; Hardware IRQs 0-15, remapped by the PIC driver to vectors 32-47.
IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

extern isr_handler
extern irq_handler

; Milestone 9 scheduler hook (see kernel/scheduler.c). Both stubs below
; call into C (isr_handler for int 0x80/exceptions, irq_handler for
; hardware IRQs) and, on the way back out, now check this global before
; popping registers and iret-ing. Non-zero means "a context switch was
; decided while we were in C - resume from this kernel esp instead of
; the one we entered on". scheduler_tick() (called from the IRQ0/PIT
; handler) and scheduler_exit_current() (called from the SYS_EXIT
; syscall handler) are the only two places that ever set it; both are
; reached through this same path, which is exactly why one hook here
; covers both. Left at 0 by default, so every interrupt that isn't
; triggering a switch behaves exactly as it did before this milestone.
extern scheduler_next_esp

; All four segment registers are saved and restored individually, not just
; ds. Until Milestone 10 the epilogue reloaded ds/es/fs/gs from the single
; saved ds value, which was harmless while every user selector was the
; same - but a Win32 program runs with fs pointing at its TEB (selector
; 0x33, see kernel/win32.c), and clobbering that with the data selector on
; the way out of the first timer interrupt would break every fs:[...]
; access the C runtime makes afterwards. The push order below fixes the
; layout of the first four fields of registers_t in include/idt.h.
isr_common_stub:
    pusha                ; edi,esi,ebp,esp,ebx,edx,ecx,eax
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10          ; load kernel data segment for our own use
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp             ; pass pointer to the registers_t we just built
    call isr_handler
    add esp, 4

    mov eax, [scheduler_next_esp]
    test eax, eax
    jz .isr_no_switch
    mov dword [scheduler_next_esp], 0
    mov esp, eax
.isr_no_switch:

    pop gs
    pop fs
    pop es
    pop ds

    popa
    add esp, 8            ; clean up error code + int number pushed earlier
    sti
    iret

irq_common_stub:
    pusha
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp
    call irq_handler
    add esp, 4

    mov eax, [scheduler_next_esp]
    test eax, eax
    jz .irq_no_switch
    mov dword [scheduler_next_esp], 0
    mov esp, eax
.irq_no_switch:

    pop gs
    pop fs
    pop es
    pop ds

    popa
    add esp, 8
    sti
    iret
