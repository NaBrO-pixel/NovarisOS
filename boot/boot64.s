; boot64.s - Multiboot entry point + long-mode (x86-64) bootstrap.
;
; GRUB hands us a 32-bit protected-mode machine with paging off, loaded at
; our physical load address (1MB). Long mode cannot be entered with paging
; off, so unlike the 32-bit boot.s this file has to build real page tables
; before it can switch: there is no "enable long mode and set up memory
; later" ordering available.
;
; The sequence the manual requires, and the order is not negotiable:
;   1. CR4.PAE on          (long mode is PAE-only)
;   2. CR3 = PML4          (the tables must already be valid here)
;   3. EFER.LME on         (long mode *enabled*, not yet active)
;   4. CR0.PG on           (paging on; this is what makes LMA go active)
;   5. far jump through a GDT whose code segment has the L bit
;
; Only after step 5 is the CPU executing 64-bit instructions.
;
; The kernel is linked at 0xFFFFFFFF80000000 (the top 2GB, which is what
; -mcmodel=kernel means and why that address and not 0xC0000000). This file
; is different: `.boot.*` is linked at its physical address, so GRUB can
; jump to _start with paging off and the early code can address its own
; data without a translation existing yet. See linker64.ld.

MBALIGN  equ  1 << 0            ; align loaded modules on page boundaries
MEMINFO  equ  1 << 1            ; provide memory map
VIDMODE  equ  1 << 2            ; request a graphics mode
FLAGS    equ  MBALIGN | MEMINFO | VIDMODE
MAGIC    equ  0x1BADB002
CHECKSUM equ -(MAGIC + FLAGS)

KERNEL_VMA equ 0xFFFFFFFF80000000

PG_PRESENT equ 1 << 0
PG_WRITE   equ 1 << 1
PG_HUGE    equ 1 << 7           ; PS: a PD entry maps a 2MB page, not a PT

CODE64_SEL equ 0x08
DATA64_SEL equ 0x10

; Which slots the higher-half address decodes to. 0xFFFFFFFF80000000
; splits as PML4[511] -> PDPT[510] -> PD[0], and those three constants are
; the whole reason the mapping below looks the way it does.
PML4_HIGH  equ 511
PDPT_HIGH  equ 510

bits 32
section .boot.text progbits alloc exec nowrite align=16

; The Multiboot header must be in the first 8KB of the file and 4-byte
; aligned. The five zero dwords are the a.out kludge fields: flag bit 16 is
; clear so GRUB ignores their contents, but they still occupy offsets 12-31,
; and the video request fields the spec puts at offset 32 are only read
; correctly if they are actually at offset 32.
align 8
multiboot_header:
    dd MAGIC
    dd FLAGS
    dd CHECKSUM
    dd 0, 0, 0, 0, 0        ; header_addr, load_addr, load_end, bss_end, entry
    dd 0                    ; mode_type: 0 = linear graphics
    dd 1024                 ; width  \  a request, not a guarantee - the
    dd 768                  ; height  } kernel reads back what it was
    dd 32                   ; depth  /  actually given from the mbi

global _start
_start:
    cli
    ; GRUB's handoff lives in eax/ebx and nothing below preserves them, so
    ; stash both before touching anything. These are physical addresses and
    ; paging is off, which is exactly why .boot.data is linked low.
    mov [mb_magic], eax
    mov [mb_info], ebx

    mov esp, boot_stack_top

    call check_cpuid
    call check_long_mode
    call build_page_tables

    ; --- the five steps, in order ------------------------------------
    mov eax, cr4
    or eax, 1 << 5                  ; CR4.PAE
    mov cr4, eax

    mov eax, pml4
    mov cr3, eax

    mov ecx, 0xC0000080             ; IA32_EFER
    rdmsr
    or eax, 1 << 8                  ; EFER.LME
    wrmsr

    mov eax, cr0
    or eax, (1 << 31) | (1 << 0)    ; CR0.PG | CR0.PE
    mov cr0, eax

    ; The CPU is in compatibility mode until a far jump reloads CS with a
    ; descriptor whose L bit is set. lgdt's 32-bit operand form is a 2-byte
    ; limit and a 4-byte base, which is fine because the GDT is below 4GB.
    lgdt [gdt64_ptr32]
    jmp CODE64_SEL:long_mode_start

; --- CPUID availability: flip EFLAGS.ID and see whether it sticks -------
check_cpuid:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx                        ; restore the original EFLAGS either way
    popfd
    cmp eax, ecx
    je .missing
    ret
.missing:
    mov esi, msg_no_cpuid
    jmp boot_panic

; --- long mode support, via the extended CPUID leaf ---------------------
check_long_mode:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001             ; is the leaf we need even implemented?
    jb .missing
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29               ; LM
    jz .missing
    ret
.missing:
    mov esi, msg_no_long_mode
    jmp boot_panic

; --- the tables ---------------------------------------------------------
; Three tables and 2MB pages, which is all the bootstrap needs: one PD of
; 512 huge pages covers the first 1GB of physical memory. Both the identity
; window and the higher-half window point at that same PD, so the kernel
; sees one gigabyte of physical memory at two addresses. The identity half
; is what keeps _start executing across the CR0.PG write; the high half is
; where the kernel is linked. paging.c replaces all of this later.
build_page_tables:
    mov edi, pml4
    xor eax, eax
    mov ecx, (3 * 4096) / 4
    cld
    rep stosd

    mov eax, pdpt
    or eax, PG_PRESENT | PG_WRITE
    mov [pml4], eax                         ; identity: virt 0
    mov [pml4 + PML4_HIGH * 8], eax         ; higher half

    mov eax, pd
    or eax, PG_PRESENT | PG_WRITE
    mov [pdpt], eax                         ; identity: first 1GB
    mov [pdpt + PDPT_HIGH * 8], eax         ; 0xFFFFFFFF80000000

    mov edi, pd
    mov eax, PG_PRESENT | PG_WRITE | PG_HUGE
    mov ecx, 512
.fill:
    mov [edi], eax                  ; low half: frame address + flags
    mov dword [edi + 4], 0          ; high half: below 4GB, so zero
    add eax, 0x200000
    add edi, 8
    loop .fill
    ret

; --- failure path -------------------------------------------------------
; No console exists yet and the video mode is not text, so the only way to
; say anything is COM1, configured here from scratch because GRUB makes no
; promises about it. esi = message.
boot_panic:
    mov dx, 0x3F9
    xor al, al
    out dx, al                      ; interrupts off: we poll
    mov dx, 0x3FB
    mov al, 0x80
    out dx, al                      ; DLAB on
    mov dx, 0x3F8
    mov al, 0x03
    out dx, al                      ; divisor low = 3 => 38400 baud
    mov dx, 0x3F9
    xor al, al
    out dx, al                      ; divisor high
    mov dx, 0x3FB
    mov al, 0x03
    out dx, al                      ; DLAB off, 8N1
    cld
.next:
    lodsb
    test al, al
    jz .done
    call .putc
    jmp .next
.done:
    cli
.hang:
    hlt
    jmp .hang
.putc:
    mov ah, al                      ; the byte to send, parked
.wait:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20                   ; LSR.THR empty
    jz .wait
    mov al, ah
    mov dx, 0x3F8
    out dx, al
    ret

; --- 64-bit from here ---------------------------------------------------
bits 64
long_mode_start:
    mov ax, DATA64_SEL
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Long mode guarantees SSE2 exists, but it still has to be turned on
    ; before any instruction uses it - and the C below is compiled without
    ; -mno-sse, so gcc will use xmm registers for ordinary struct copies.
    ; Clearing CR0.EM and setting CR0.MP says "there is an FPU, do not trap
    ; on it"; the CR4 bits say the OS is prepared to save the SSE state.
    mov rax, cr0
    and ax, 0xFFFB                  ; ~CR0.EM
    or ax, 1 << 1                   ; CR0.MP
    mov cr0, rax
    mov rax, cr4
    or ax, (1 << 9) | (1 << 10)     ; CR4.OSFXSR | CR4.OSXMMEXCPT
    mov cr4, rax

    ; Leave the low identity addresses behind: everything from here is
    ; linked high, so get RIP there with an absolute 64-bit jump.
    mov rax, higher_half_start
    jmp rax

section .text
bits 64
extern kernel_main
higher_half_start:
    mov rsp, stack_top
    xor rbp, rbp                    ; terminate any stack walk cleanly

    ; System V puts the first two integer arguments in rdi and rsi. Both
    ; values were saved as 32-bit quantities and both are physical
    ; addresses below 4GB, so the zero-extending 32-bit loads are correct.
    mov edi, [mb_magic]             ; magic
    mov esi, [mb_info]              ; multiboot_info_t*
    call kernel_main

    cli
.hang:
    hlt
    jmp .hang

; --- descriptors --------------------------------------------------------
; In long mode the base and limit of a code/data descriptor are ignored;
; what still matters is P, S, the executable bit, and L. This GDT exists
; only to make the far jump legal - gdt.c builds the real one, with a TSS.
section .boot.data progbits alloc noexec write align=4096

pml4: times 4096 db 0
pdpt: times 4096 db 0
pd:   times 4096 db 0

mb_magic: dd 0
mb_info:  dd 0

align 8
gdt64:
    dq 0                                            ; null
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)        ; code64: exec, S, P, L
    dq (1<<41) | (1<<44) | (1<<47)                  ; data:   RW,   S, P
gdt64_end:
gdt64_ptr32:
    dw gdt64_end - gdt64 - 1
    dd gdt64

align 16
boot_stack_bottom:
    times 4096 db 0
boot_stack_top:

msg_no_cpuid:     db "novaris: CPUID is not available; this CPU is too old to boot a 64-bit kernel", 10, 0
msg_no_long_mode: db "novaris: this CPU does not support long mode (x86-64)", 10, 0

; The kernel's own stack, in the higher half. 16KB, same as the 32-bit
; kernel used, and 16-byte aligned because the ABI requires it.
section .bss
align 16
stack_bottom:
    resb 16384
stack_top:
