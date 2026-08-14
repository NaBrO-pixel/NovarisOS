; isr64.s - low-level entry stubs for CPU exceptions and hardware IRQs,
; long-mode version.
;
; Same shape as the 32-bit isr.s - uniform frame, one common stub, one C
; dispatcher - but three details are different and each one is a fault
; that only shows up under load if it is got wrong:
;
;   * There is no pusha/popa in long mode. Fifteen registers get pushed
;     and popped by hand, and the order has to match registers64_t.
;
;   * The CPU always pushes ss:rsp, not just on a privilege change, so the
;     frame is the same size no matter which ring was interrupted.
;
;   * `iretq`, not `iret`. `iret` in long mode pops a 32-bit frame and
;     returns to a garbage address.
;
; Stack alignment: the CPU aligns rsp to 16 before pushing its 5-qword
; frame, so it is 8 (mod 16) on entry to the stub. The error code and
; vector make it 7 qwords, the fifteen registers bring it to 22, and
; 22 * 8 = 176 is a multiple of 16 - which is what the System V ABI
; requires at a `call`. Add or remove a push here and that stops being
; true, and the first SSE spill in a C handler faults.

%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0        ; dummy error code, so every frame looks alike
    push qword %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    ; the CPU has already pushed a real error code
    push qword %1
    jmp isr_common_stub
%endmacro

%macro IRQ 2
global irq%1
irq%1:
    push qword 0
    push qword %2
    jmp irq_common_stub
%endmacro

; Push in descending register order so that the lowest address ends up
; holding rax - see registers64_t, which lists them ascending.
%macro PUSH_ALL 0
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax
%endmacro

%macro POP_ALL 0
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
%endmacro

bits 64
section .text

; CPU exceptions 0-31. Vectors 8, 10-14, 17, 21 and 29-30 push a real
; error code; the rest get a zero from us.
ISR_NOERR 0    ; #DE divide error
ISR_NOERR 1    ; #DB debug
ISR_NOERR 2    ; NMI
ISR_NOERR 3    ; #BP breakpoint
ISR_NOERR 4    ; #OF overflow
ISR_NOERR 5    ; #BR bound range
ISR_NOERR 6    ; #UD invalid opcode
ISR_NOERR 7    ; #NM device not available
ISR_ERR   8    ; #DF double fault
ISR_NOERR 9    ; coprocessor segment overrun (legacy, never raised on x86-64)
ISR_ERR   10   ; #TS invalid TSS
ISR_ERR   11   ; #NP segment not present
ISR_ERR   12   ; #SS stack-segment fault
ISR_ERR   13   ; #GP general protection fault
ISR_ERR   14   ; #PF page fault
ISR_NOERR 15   ; reserved
ISR_NOERR 16   ; #MF x87 floating point
ISR_ERR   17   ; #AC alignment check
ISR_NOERR 18   ; #MC machine check
ISR_NOERR 19   ; #XM SIMD floating point
ISR_NOERR 20   ; #VE virtualization
ISR_ERR   21   ; #CP control protection
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29   ; #VC VMM communication
ISR_ERR   30   ; #SX security exception
ISR_NOERR 31

; The 8259s are remapped to 32-47 exactly as in the 32-bit kernel.
IRQ 0,  32
IRQ 1,  33
IRQ 2,  34
IRQ 3,  35
IRQ 4,  36
IRQ 5,  37
IRQ 6,  38
IRQ 7,  39
IRQ 8,  40
IRQ 9,  41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

extern isr64_handler
extern irq64_handler

isr_common_stub:
    PUSH_ALL
    mov rdi, rsp            ; System V: first argument is the frame pointer
    cld                     ; the ABI says handlers may assume DF is clear
    call isr64_handler
    POP_ALL
    add rsp, 16             ; discard the vector and error code
    iretq

irq_common_stub:
    PUSH_ALL
    mov rdi, rsp
    cld
    call irq64_handler
    POP_ALL
    add rsp, 16
    iretq

; Loading the task register. The TSS descriptor is 16 bytes in long mode,
; but `ltr` still takes a selector, so this is unchanged from 32-bit apart
; from the calling convention.
global tss64_flush
tss64_flush:
    mov ax, di
    ltr ax
    ret

; Reload the segment registers and get cs pointing at the new GDT. cs
; cannot be assigned, so it is changed with a far return: push the target
; selector and address, then lretq consumes both.
global gdt64_flush
gdt64_flush:
    lgdt [rdi]
    mov ax, 0x10            ; kernel data
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    pop rax                 ; the return address the far return will jump to
    push qword 0x08         ; kernel code selector
    push rax
    ; `o64 retf`, not `lretq`: NASM has no lretq mnemonic and would take
    ; the word for a label, assemble nothing at all, and fall through into
    ; whatever came next - which is precisely what it did here once. The
    ; o64 prefix is what makes this pop a 64-bit rip rather than a 32-bit
    ; one; without it the return address loses its top half.
    o64 retf

global idt64_flush
idt64_flush:
    lidt [rdi]
    ret

; uint64_t probe_read64(const void* addr)
;
; Reads eight bytes from an address that may not be mapped. If it faults,
; the page-fault handler points rip at probe_read64_recover, which returns
; zero instead - so a fault becomes a return value rather than a panic.
;
; This lives in assembly because the resume point has to be an address that
; is exactly what it looks like. The obvious C version - take &&label with
; a GCC labels-as-values extension and have the handler jump there - does
; not work at -O2, and fails in a way that looks like a kernel bug: the
; optimiser duplicates and reorders basic blocks, so the address of a label
; can be a *copy* of the block reached with different register state. It
; was tried here first, and resumed into the middle of an earlier test,
; which then re-ran and leaked a stack frame per iteration until the stack
; overflowed. Two real symbols and no optimiser involved is the fix, and it
; is the same shape as the exception tables a Unix kernel keeps for exactly
; this purpose.
global probe_read64
global probe_read64_recover
probe_read64:
    mov rax, [rdi]          ; the faulting instruction
    ret
probe_read64_recover:
    xor rax, rax            ; rsp still points at the return address
    ret
