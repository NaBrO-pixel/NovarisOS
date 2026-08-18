; mmapfile64.s - a real Linux program that maps a file and reads it
; through the mapping.
;
; This is how a loader loads: ld.so maps an ELF's segments, and Wine
; maps a PE's. Nothing here has done it - every image so far was copied
; into memory by the kernel, not mapped by the program.
;
; Two things get proved, and the second is the one that is easy to fake:
;
;   1. The bytes visible through the mapping are the file's bytes. A
;      kernel that handed back a zeroed anonymous page would return a
;      perfectly good pointer and fail this.
;
;   2. MAP_PRIVATE means private. Writing through the mapping must NOT
;      change the file, so the same offset read with read(2) afterwards
;      still holds the original byte. A kernel that shared the frame
;      instead of copying passes (1) and fails this.

bits 64

SYS_read       equ 0
SYS_write      equ 1
SYS_open       equ 2
SYS_close      equ 3
SYS_lseek      equ 8
SYS_mmap       equ 9
SYS_munmap     equ 11
SYS_unlink     equ 87
SYS_exit_group equ 231

STDOUT   equ 1

O_RDWR   equ 0x0002
O_CREAT  equ 0x0040
O_TRUNC  equ 0x0200
SEEK_SET equ 0

PROT_READ   equ 0x1
PROT_WRITE  equ 0x2
MAP_PRIVATE equ 0x02

MAPLEN equ 4096

section .text
global _start

_start:
    ; Create the file and put known bytes in it.
    mov rax, SYS_open
    lea rdi, [rel path]
    mov rsi, O_RDWR | O_CREAT | O_TRUNC
    mov rdx, 0600o
    syscall
    test rax, rax
    js .fail_open
    mov r12, rax

    mov rax, SYS_write
    mov rdi, r12
    lea rsi, [rel payload]
    mov rdx, payload_len
    syscall
    cmp rax, payload_len
    jne .fail_write

    ; mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0)
    mov rax, SYS_mmap
    xor rdi, rdi
    mov rsi, MAPLEN
    mov rdx, PROT_READ | PROT_WRITE
    mov r10, MAP_PRIVATE
    mov r8, r12
    xor r9, r9
    syscall
    cmp rax, 0
    jle .fail_mmap
    mov r13, rax                    ; the mapping

    ; (1) the mapping holds the file's bytes
    lea rsi, [rel payload]
    mov rdi, r13
    mov rcx, payload_len
.cmp_loop:
    mov al, [rsi]
    cmp al, [rdi]
    jne .fail_contents
    inc rsi
    inc rdi
    dec rcx
    jnz .cmp_loop

    ; (2) MAP_PRIVATE: writing the mapping must not reach the file
    mov byte [r13], 'X'

    mov rax, SYS_lseek
    mov rdi, r12
    xor rsi, rsi
    mov rdx, SEEK_SET
    syscall

    mov rax, SYS_read
    mov rdi, r12
    lea rsi, [rel readbuf]
    mov rdx, 1
    syscall
    cmp rax, 1
    jne .fail_read

    mov al, [rel readbuf]
    cmp al, 'X'
    je .fail_private                ; the write reached the file
    mov al, [rel readbuf]
    cmp al, [rel payload]
    jne .fail_private               ; and it should still be the original

    mov rax, SYS_munmap
    mov rdi, r13
    mov rsi, MAPLEN
    syscall

    mov rax, SYS_close
    mov rdi, r12
    syscall

    mov rax, SYS_unlink
    lea rdi, [rel path]
    syscall

    mov rax, SYS_write
    mov rdi, STDOUT
    lea rsi, [rel msg]
    mov rdx, msg_len
    syscall

    mov rax, SYS_exit_group
    mov rdi, 61
    syscall

.fail_open:     mov rdi, 91
                jmp .die
.fail_write:    mov rdi, 92
                jmp .die
.fail_mmap:     mov rdi, 93
                jmp .die
.fail_contents: mov rdi, 94
                jmp .die
.fail_read:     mov rdi, 95
                jmp .die
.fail_private:  mov rdi, 96
.die:
    mov rax, SYS_exit_group
    syscall

section .data
path:    db "/tmp/novaris-mmap-test", 0
payload: db "mapped from a file, not conjured from zeros", 10
payload_len equ $ - payload

msg:     db "mapped a file, read it through the mapping, kept it private", 10
msg_len  equ $ - msg

section .bss
readbuf: resb 16
