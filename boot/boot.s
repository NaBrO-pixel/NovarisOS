; boot.s - Multiboot entry point + higher-half bootstrap.
;
; GRUB loads us in 32-bit protected mode with paging OFF, at our physical
; load address (1MB) - see linker.ld. But the rest of the kernel is
; *linked* to run at 0xC0000000+1MB. So before we can jump into any
; higher-half C code, we build a small boot-time page directory that maps
; the low 8MB of physical memory both identity (0-8MB, so things like the
; VGA text buffer at 0xB8000 and the Multiboot info struct keep working)
; and aliased at 0xC0000000-0xC0800000 (same physical memory, high
; addresses), enable paging, then jump to the higher-half entry point.
; kernel/paging.c replaces this coarse 4MB-page bootstrap mapping with a
; real 4KB-granularity page directory shortly after kernel_main starts.

MBALIGN  equ  1 << 0            ; align loaded modules on page boundaries
MEMINFO  equ  1 << 1            ; provide memory map
VIDMODE  equ  1 << 2            ; request a graphics mode (see header fields below)
FLAGS    equ  MBALIGN | MEMINFO | VIDMODE
MAGIC    equ  0x1BADB002        ; magic number GRUB looks for
CHECKSUM equ -(MAGIC + FLAGS)   ; checksum required by the spec

KERNEL_VIRTUAL_BASE equ 0xC0000000
KERNEL_PAGE_NUMBER   equ (KERNEL_VIRTUAL_BASE >> 22)  ; PDE index for 0xC0000000

; The Multiboot header must be within the first 8KB of the kernel file
; and 4-byte aligned, so GRUB can find it and know this is a valid kernel.
section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM
    ; Video mode request fields (only present because VIDMODE is set):
    ; mode_type 0 = linear graphics (not text mode); width/height/depth
    ; are a *request* - GRUB/VBE may grant something else, so
    ; kernel_main always reads back what it actually got from mbi.
    dd 0            ; mode_type: linear graphics
    dd 1024         ; width
    dd 768          ; height
    dd 32           ; depth (bits per pixel)

; Boot-time page directory: identity-maps physical 0-8MB (two 4MB pages,
; needs PSE) and aliases the same physical memory at 0xC0000000-0xC0800000.
; This needs real content (not zero-initialized), so it lives in .data
; rather than .bss.
section .data
align 4096
global BootPageDirectory
BootPageDirectory:
    ; PDE 0-1: identity map physical 0-8MB.
    ; Flags: present (bit0), read/write (bit1), page size = 4MB (bit7).
    dd 0x00000083
    dd 0x00400083
    times (KERNEL_PAGE_NUMBER - 2) dd 0
    ; PDE 768-769: alias the same physical 0-8MB at 0xC0000000+.
    dd 0x00000083
    dd 0x00400083
    times (1024 - KERNEL_PAGE_NUMBER - 2) dd 0

; Reserve 16KB for our stack. The CPU has no stack when we start, and we
; need one before calling any C function.
section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

section .text
global _start
extern kernel_main
_start:
    ; We are still executing at our *physical* load address here (paging
    ; is off), so anything touched before paging is enabled must use a
    ; physical address - hence subtracting KERNEL_VIRTUAL_BASE, which the
    ; linker/assembler folds into a link-time constant.
    mov ecx, (BootPageDirectory - KERNEL_VIRTUAL_BASE)
    mov cr3, ecx

    ; Enable PSE (4MB pages) in CR4.
    mov ecx, cr4
    or ecx, 0x00000010
    mov cr4, ecx

    ; Enable paging (CR0.PG). Protected mode is already on courtesy of GRUB.
    mov ecx, cr0
    or ecx, 0x80000000
    mov cr0, ecx

    ; EIP is still the low physical address at this point (identity-mapped,
    ; so execution continues fine), but everything from here on should run
    ; at the higher-half linked addresses. `mov ecx, higher_half_start`
    ; loads the label's real linked (high) address as an immediate; the
    ; jmp then forces EIP into the alias mapping.
    mov ecx, higher_half_start
    jmp ecx

higher_half_start:
    ; eax (multiboot magic) and ebx (multiboot info pointer) were set by
    ; GRUB before we touched anything above, and we never clobbered them.
    mov esp, stack_top
    push ebx                ; multiboot info struct pointer (arg to kernel_main)
    push eax                ; multiboot magic value (arg to kernel_main)

    cli                      ; disable interrupts until we set up an IDT
    call kernel_main

    ; kernel_main should never return, but if it does, halt forever
    cli
.hang:
    hlt
    jmp .hang
