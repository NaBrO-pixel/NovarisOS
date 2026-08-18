; tree64.s - a real Linux program that uses a directory tree as a tree.
;
; Everything the flat path table could not do:
;
;   - mkdir nested, where the inner one needs the outer to exist
;   - open a file through "..", which has to resolve rather than match
;   - getdents64, which needs a directory to have children
;   - rmdir refusing a directory that still has something in it
;
; Ordinary POSIX throughout, so it runs against a real filesystem on the
; host and a RAM one on the guest, and both have to agree. It works
; under /tmp and removes what it makes.

bits 64

SYS_read       equ 0
SYS_write      equ 1
SYS_open       equ 2
SYS_close      equ 3
SYS_mkdir      equ 83
SYS_rmdir      equ 84
SYS_unlink     equ 87
SYS_getdents64 equ 217
SYS_exit_group equ 231

STDOUT   equ 1

O_RDONLY equ 0x0000
O_RDWR   equ 0x0002
O_CREAT  equ 0x0040
O_TRUNC  equ 0x0200
O_DIRECTORY equ 0x10000

section .text
global _start

_start:
    ; mkdir /tmp/nvtree
    mov rax, SYS_mkdir
    lea rdi, [rel dir_outer]
    mov rsi, 0755o
    syscall
    test rax, rax
    js .fail_mkdir

    ; mkdir /tmp/nvtree/sub - needs the one above to exist
    mov rax, SYS_mkdir
    lea rdi, [rel dir_inner]
    mov rsi, 0755o
    syscall
    test rax, rax
    js .fail_mkdir

    ; Create a file two levels down.
    mov rax, SYS_open
    lea rdi, [rel filepath]
    mov rsi, O_RDWR | O_CREAT | O_TRUNC
    mov rdx, 0644o
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

    mov rax, SYS_close
    mov rdi, r12
    syscall

    ; Open the same file through "..", which a string compare cannot do.
    mov rax, SYS_open
    lea rdi, [rel dotdotpath]
    mov rsi, O_RDONLY
    xor rdx, rdx
    syscall
    test rax, rax
    js .fail_dotdot
    mov r12, rax

    mov rax, SYS_read
    mov rdi, r12
    lea rsi, [rel readbuf]
    mov rdx, payload_len
    syscall
    cmp rax, payload_len
    jne .fail_read

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

    ; Enumerate /tmp/nvtree and look for "sub" among the entries.
    mov rax, SYS_open
    lea rdi, [rel dir_outer]
    mov rsi, O_RDONLY | O_DIRECTORY
    xor rdx, rdx
    syscall
    test rax, rax
    js .fail_opendir
    mov r12, rax

    mov rax, SYS_getdents64
    mov rdi, r12
    lea rsi, [rel dirbuf]
    mov rdx, 1024
    syscall
    test rax, rax
    jle .fail_getdents
    mov r13, rax                    ; bytes of records

    mov rax, SYS_close
    mov rdi, r12
    syscall

    ; Walk the records looking for the name "sub".
    xor rbx, rbx                    ; offset into dirbuf
.scan:
    cmp rbx, r13
    jae .fail_nosub
    lea rsi, [rel dirbuf]
    add rsi, rbx
    movzx rcx, word [rsi + 16]      ; d_reclen
    test rcx, rcx
    jz .fail_nosub
    lea rdi, [rsi + 19]             ; d_name
    lea rsi, [rel name_sub]
    call streq
    test rax, rax
    jnz .found
    add rbx, rcx
    jmp .scan

.found:
    ; rmdir the outer directory while the inner one is still in it: it
    ; has to refuse, or "empty" means nothing.
    mov rax, SYS_rmdir
    lea rdi, [rel dir_outer]
    syscall
    test rax, rax
    jns .fail_rmdir_full

    ; Now tear it down in the right order.
    mov rax, SYS_unlink
    lea rdi, [rel filepath]
    syscall
    test rax, rax
    js .fail_cleanup

    mov rax, SYS_rmdir
    lea rdi, [rel dir_inner]
    syscall
    test rax, rax
    js .fail_cleanup

    mov rax, SYS_rmdir
    lea rdi, [rel dir_outer]
    syscall
    test rax, rax
    js .fail_cleanup

    mov rax, SYS_write
    mov rdi, STDOUT
    lea rsi, [rel msg]
    mov rdx, msg_len
    syscall

    mov rax, SYS_exit_group
    mov rdi, 83
    syscall

.fail_mkdir:      mov rdi, 60
                  jmp .die
.fail_open:       mov rdi, 61
                  jmp .die
.fail_write:      mov rdi, 62
                  jmp .die
.fail_dotdot:     mov rdi, 63
                  jmp .die
.fail_read:       mov rdi, 64
                  jmp .die
.fail_compare:    mov rdi, 65
                  jmp .die
.fail_opendir:    mov rdi, 66
                  jmp .die
.fail_getdents:   mov rdi, 67
                  jmp .die
.fail_nosub:      mov rdi, 68
                  jmp .die
.fail_rmdir_full: mov rdi, 69
                  jmp .die
.fail_cleanup:    mov rdi, 70
.die:
    mov rax, SYS_exit_group
    syscall

; streq(rdi, rsi) -> rax = 1 if equal.
;
; Placed after the failure labels on purpose: NASM scopes a local label
; to the last non-local one, so a function defined between _start and
; those labels quietly captures them and every jump to .fail_* fails to
; assemble.
; r8b/r9b rather than cl/ch: ch is the high byte of rcx, and the caller
; is holding d_reclen there. Clobbering it makes the scan step by a
; corrupted length and miss the entry it was looking for - which
; presents as "the directory does not contain what it contains".
streq:
    xor rax, rax
.next:
    mov r8b, [rdi]
    mov r9b, [rsi]
    cmp r8b, r9b
    jne .no
    test r8b, r8b
    jz .yes
    inc rdi
    inc rsi
    jmp .next
.yes:
    mov rax, 1
.no:
    ret

section .data
dir_outer:  db "/tmp/nvtree", 0
dir_inner:  db "/tmp/nvtree/sub", 0
filepath:   db "/tmp/nvtree/sub/leaf", 0
dotdotpath: db "/tmp/nvtree/sub/../sub/leaf", 0
name_sub:   db "sub", 0

payload:    db "a real tree, walked from the root", 10
payload_len equ $ - payload

msg:        db "made a tree, opened through .., listed it, tore it down", 10
msg_len     equ $ - msg

section .bss
readbuf: resb 128
dirbuf:  resb 1024
