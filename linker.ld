ENTRY(_start)

/* Higher-half kernel: the kernel is *linked* to run at 0xC0000000 + 1MB
 * (KERNEL_VIRTUAL_BASE), but GRUB still loads it at physical 1MB, same
 * as before. AT() below tells the linker each section's load address
 * (LMA) is its link address (VMA) minus KERNEL_VIRTUAL_BASE, so the file
 * on disk is laid out at the low physical addresses GRUB expects, while
 * every symbol the compiler emits resolves to the high addresses the
 * kernel actually runs at once boot.s enables paging. See boot.s for the
 * bootstrap page directory that makes this addressing scheme valid. */
KERNEL_VIRTUAL_BASE = 0xC0000000;

SECTIONS
{
    . = KERNEL_VIRTUAL_BASE + 0x00100000;

    _kernel_virtual_start  = .;
    _kernel_physical_start = . - KERNEL_VIRTUAL_BASE;

    .multiboot ALIGN(4K) : AT(ADDR(.multiboot) - KERNEL_VIRTUAL_BASE) {
        *(.multiboot)
    }

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VIRTUAL_BASE) {
        *(.text)
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VIRTUAL_BASE) {
        *(.rodata)
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VIRTUAL_BASE) {
        *(.data)
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VIRTUAL_BASE) {
        *(COMMON)
        *(.bss)
    }

    _kernel_virtual_end  = .;
    _kernel_physical_end = . - KERNEL_VIRTUAL_BASE;
}
