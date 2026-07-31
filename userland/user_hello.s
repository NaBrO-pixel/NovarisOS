; user_hello.s - the "hello world from ring 3" demo program.
;
; Assembled as a raw flat binary (not ELF) and embedded directly into the
; kernel image as a byte array (see user_hello_blob.c) - there's no
; filesystem or ELF loader yet (that's Milestone 6), so process.c just
; copies these bytes straight into a mapped page and jumps to the start.
;
; ORG must match USER_PROGRAM_VADDR in kernel/process.c, since this is a
; flat (non-relocatable) binary - the `mov ebx, msg` below bakes in an
; absolute address that only makes sense once mapped there.
;
; To regenerate user_hello_blob.c after editing this file:
;   nasm -f bin userland/user_hello.s -o userland/user_hello.bin
;   (then re-run the embedding step - see comment at the top of
;   kernel/user_hello_blob.c)

BITS 32
ORG 0x40000000

_start:
    mov eax, 4              ; Linux SYS_write
    mov ebx, 1              ; fd 1 = stdout
    mov ecx, msg
    mov edx, msg_len
    int 0x80

    mov eax, 1              ; Linux SYS_exit
    xor ebx, ebx
    int 0x80

.halt_loop:
    jmp .halt_loop           ; should never be reached: sys_exit doesn't return

msg: db "Hello from ring 3 user mode!", 10, 0
msg_len equ $ - msg - 1   ; Linux write() takes a length, not a
                          ; NUL-terminated string; the -1 drops the
                          ; terminator, which is only there because
                          ; the old Novaris SYS_WRITE needed it.

