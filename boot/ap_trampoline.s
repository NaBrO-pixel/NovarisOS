; ap_trampoline.s - where an application processor starts.
;
; A CPU woken by INIT-SIPI-SIPI begins in 16-bit real mode at the page the
; SIPI named - 0x8000 here - with nothing set up at all. This takes it to
; long mode the same way boot64.s took the bootstrap processor, then jumps
; into the kernel's higher half at the address smp64.c left in the
; parameter block at the end of the page.
;
; Assembled as a flat binary (nasm -f bin) and copied to 0x8000 by
; smp64.c, which fills in the parameter block before every SIPI: the CR3
; to use (a small table with an identity map of the low 2MB beside the
; kernel's own upper half, below 4GB so a 32-bit mov reaches it), the
; stack, the C function to call and the CPU's number.

bits 16
org 0x8000

ap_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    lgdt [gdt_ptr]
    mov eax, cr0
    or eax, 1                       ; CR0.PE
    mov cr0, eax
    jmp 0x08:pm32

bits 32
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov eax, cr4
    or eax, (1 << 5)                ; CR4.PAE
    mov cr4, eax

    mov eax, [param_cr3]
    mov cr3, eax

    mov ecx, 0xC0000080             ; IA32_EFER
    rdmsr
    or eax, (1 << 8) | (1 << 0)     ; LME, and SCE for SYSCALL
    wrmsr

    mov eax, cr0
    or eax, (1 << 31) | (1 << 0)    ; PG | PE
    mov cr0, eax
    jmp 0x18:lm64

bits 64
lm64:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax

    mov rsp, [param_stack]
    mov rdi, [param_cpu]
    mov rax, [param_entry]
    call rax                        ; does not return
.hang:
    cli
    hlt
    jmp .hang

align 16
gdt:
    dq 0
    dq 0x00CF9A000000FFFF           ; 0x08: 32-bit code
    dq 0x00CF92000000FFFF           ; 0x10: data
    dq 0x00AF9A000000FFFF           ; 0x18: 64-bit code
gdt_end:
gdt_ptr:
    dw gdt_end - gdt - 1
    dd gdt

; The parameter block, at a fixed place: the last 32 bytes of the page.
times 4096 - 32 - ($ - $$) db 0
param_cr3:   dq 0
param_stack: dq 0
param_entry: dq 0
param_cpu:   dq 0
