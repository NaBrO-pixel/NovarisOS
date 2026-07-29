; gdt_flush.s - loads a new GDT and reloads every segment register.
;
; The CPU caches segment selector values in "hidden" parts of the segment
; registers, so just calling LGDT isn't enough - CS can only be reloaded via
; a far jump/return, and the data segment registers need an explicit mov.

global gdt_flush
gdt_flush:
    mov eax, [esp + 4]   ; pointer to our gdt_ptr struct, passed as arg
    lgdt [eax]            ; load the new GDT

    mov ax, 0x10           ; 0x10 = offset of the kernel data selector (entry 2)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    jmp 0x08:.flush        ; 0x08 = offset of the kernel code selector (entry 1)
                            ; far jump forces CS to reload with the new selector
.flush:
    ret
