; ap_blob.s - boot/ap_trampoline.s, assembled flat, carried in the kernel.
; smp64.c copies it to 0x8000 before starting each application processor.

section .rodata
global ap_trampoline_bin
global ap_trampoline_len
align 16
ap_trampoline_bin:
    incbin "build64/ap_trampoline.bin"
ap_trampoline_end:
align 8
ap_trampoline_len:
    dq ap_trampoline_end - ap_trampoline_bin
