; Minimal PE32 test binary for Novaris's PE loader (see kernel/pe.c).
;
; This is NOT a real Windows program - it has zero imports, so it can't
; call any Win32 API. It exists purely to exercise pe_load(): a real
; MZ/PE-header, section table, and entry point, assembled straight to a
; win32 COFF object and linked with no CRT and no import table. It talks
; to Novaris directly using Novaris's own int 0x80 syscall convention
; (see kernel/syscall.c) - which a real Windows binary would have no
; idea about - just to prove bytes loaded from the PE's sections are
; really what gets executed after pe_load() hands off to the entry point.
bits 32

section .text
global start
start:
    mov eax, 1          ; SYS_WRITE
    mov ebx, msg
    int 0x80
    mov eax, 0          ; SYS_EXIT
    int 0x80

section .data
msg: db "Hello from a loaded .exe! (PE32 header/section loader test - see ROADMAP.md Milestone 8b; this is not a real Win32 program)", 0
