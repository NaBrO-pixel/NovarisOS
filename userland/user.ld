ENTRY(_start)

/* Every Novaris user-mode program is a flat (non-relocatable) binary
 * loaded at this fixed virtual address - see USER_LOAD_VADDR in
 * kernel/process.c. Only one user program ever runs at a time (no
 * per-process address spaces yet), so sharing one address is fine. */
SECTIONS
{
    . = 0x40000000;

    .text : { *(.text) }
    .rodata : { *(.rodata) }
    .data : { *(.data) }
    .bss : { *(COMMON) *(.bss) }
}
