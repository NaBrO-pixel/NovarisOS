; idt_flush.s - loads the IDTR with LIDT. Unlike the GDT, no segment
; registers need reloading here - LIDT alone is enough.

global idt_flush
idt_flush:
    mov eax, [esp + 4]   ; pointer to our idt_ptr struct, passed as arg
    lidt [eax]
    ret
