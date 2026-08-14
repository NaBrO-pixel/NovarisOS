ENTRY(_start)

/* The 64-bit kernel lives at -2GB, not at 0xC0000000 like the 32-bit one.
 * That is not a preference: -mcmodel=kernel tells gcc it may assume every
 * symbol is reachable by a sign-extended 32-bit displacement, which is only
 * true in the top 2GB of the address space. Build with that model and link
 * anywhere else and the result miscompiles silently. */
KERNEL_VMA = 0xFFFFFFFF80000000;

SECTIONS
{
    . = 0x00100000;
    _kernel_physical_start = .;

    /* The bootstrap is linked at its physical address, VMA == LMA. GRUB
     * enters with paging off, so _start has to be somewhere the CPU can
     * reach without a page table, and the early code has to be able to
     * address its own page tables the same way. Everything after this
     * point is linked high and is unreachable until CR0.PG is set. */
    .boot.text ALIGN(4K) : {
        *(.boot.text)
    }

    .boot.data ALIGN(4K) : {
        *(.boot.data)
    }

    . = ALIGN(4K);
    _boot_end = .;

    /* Same trick as the 32-bit linker script, one address higher up: VMA
     * jumps to the higher half, AT() keeps the load addresses contiguous
     * and low, so the ELF GRUB loads is still a low-memory image. */
    . = _boot_end + KERNEL_VMA;
    _kernel_virtual_start = .;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA) {
        *(.text)
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA) {
        *(.rodata)
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data)
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA) {
        *(COMMON)
        *(.bss)
    }

    _kernel_virtual_end  = .;
    _kernel_physical_end = . - KERNEL_VMA;

    /DISCARD/ : {
        *(.eh_frame)
        *(.comment)
        *(.note .note.*)
    }
}
