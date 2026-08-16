; fs64.s - a real Linux program that creates a file, writes it, reads it
; back, and removes it.
;
; Ordinary POSIX, which is the point: it runs on Linux against a real
; filesystem and on Novaris against a RAM one, and both have to agree.
; It writes under /tmp and unlinks what it made, so a host run leaves
; nothing behind.
;
; The read-back compare is what makes this worth running. A filesystem
; that accepted the write and returned zeros would pass every syscall
; return check and fail here.

bits 64

SYS_read       equ 0
SYS_write      equ 1
SYS_open       equ 2
SYS_close      equ 3
SYS_lseek      equ 8
SYS_unlink     equ 87
SYS_exit_group equ 231

STDOUT   equ 1

O_WRONLY equ 0x0001
O_RDWR   equ 0x0002
O_CREAT  equ 0x0040
O_TRUNC  equ 0x0200

SEEK_SET equ 0

section .text
global _start

_start:
    ; open("/tmp/novaris-fs-test", O_RDWR|O_CREAT|O_TRUNC, 0600)
    mov rax, SYS_open
    lea rdi, [rel path]
    mov rsi, O_RDWR | O_CREAT | O_TRUNC
    mov rdx, 0600o
    syscall
    test rax, rax
    js .fail_open
    mov r12, rax                    ; keep the fd

    ; write(fd, payload, payload_len)
    mov rax, SYS_write
    mov rdi, r12
    lea rsi, [rel payload]
    mov rdx, payload_len
    syscall
    cmp rax, payload_len
    jne .fail_write

    ; lseek(fd, 0, SEEK_SET) - rewind and read our own bytes back
    mov rax, SYS_lseek
    mov rdi, r12
    xor rsi, rsi
    mov rdx, SEEK_SET
    syscall
    test rax, rax
    jnz .fail_seek

    mov rax, SYS_read
    mov rdi, r12
    lea rsi, [rel readbuf]
    mov rdx, payload_len
    syscall
    cmp rax, payload_len
    jne .fail_read

    ; Compare. A filesystem that stored nothing and returned zeros gets
    ; caught exactly here and nowhere earlier.
    lea rsi, [rel payload]
    lea rdi, [rel readbuf]
    mov rcx, payload_len
.cmp_loop:
    mov al, [rsi]
    cmp al, [rdi]
    jne .fail_compare
    inc rsi
    inc rdi
    dec rcx
    jnz .cmp_loop

    mov rax, SYS_close
    mov rdi, r12
    syscall

    ; Reading a file that has been removed must fail, which is how we
    ; know unlink did something rather than returning 0 politely.
    mov rax, SYS_unlink
    lea rdi, [rel path]
    syscall
    test rax, rax
    jnz .fail_unlink

    mov rax, SYS_open
    lea rdi, [rel path]
    xor rsi, rsi                    ; O_RDONLY, no O_CREAT
    xor rdx, rdx
    syscall
    test rax, rax
    jns .fail_still_there           ; it opened, so it was not removed

    mov rax, SYS_write
    mov rdi, STDOUT
    lea rsi, [rel msg]
    mov rdx, msg_len
    syscall

    mov rax, SYS_exit_group
    mov rdi, 53
    syscall

.fail_open:        mov rdi, 71
                   jmp .die
.fail_write:       mov rdi, 72
                   jmp .die
.fail_seek:        mov rdi, 73
                   jmp .die
.fail_read:        mov rdi, 74
                   jmp .die
.fail_compare:     mov rdi, 75
                   jmp .die
.fail_unlink:      mov rdi, 76
                   jmp .die
.fail_still_there: mov rdi, 77
.die:
    mov rax, SYS_exit_group
    syscall

section .data
path:    db "/tmp/novaris-fs-test", 0
payload: db "a writable filesystem, proved by reading it back", 10
payload_len equ $ - payload

msg:     db "wrote a file, read it back, and removed it", 10
msg_len  equ $ - msg

section .bss
readbuf: resb 128
