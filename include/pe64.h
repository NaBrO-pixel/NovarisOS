#ifndef PE64_H
#define PE64_H

#include <stdint.h>
#include "vmspace64.h"

/* PE32+ - the format chrome.exe is in.
 *
 * Separate from the 32-bit pe.c for the same reason elf64.c is separate
 * from elf.c, and more so: PE32+ is not PE32 with wider fields. The
 * optional header's magic changes, BaseOfData disappears entirely (which
 * shifts every field after it), ImageBase becomes 64-bit, and the thunk
 * arrays in the import table go from 4 bytes to 8 with the
 * import-by-ordinal flag moving from bit 31 to bit 63.
 *
 * The calling convention is the other half of the problem. Windows
 * x86-64 passes arguments in rcx, rdx, r8, r9 with 32 bytes of shadow
 * space; this kernel is compiled for SysV and its syscalls take rdi,
 * rsi, rdx, r10. Rather than compile the kernel's Win32 layer twice, the
 * loader writes a small thunk per import that shuffles the registers and
 * issues a syscall - which is also what makes an imported function a
 * ring-3 thing that traps, rather than a kernel address a ring-3 program
 * would fault on. */

#define PE64_OK            0
#define PE64_NOT_PE       -1
#define PE64_NOT_PE32PLUS -2
#define PE64_WRONG_MACHINE -3
#define PE64_TRUNCATED    -4
#define PE64_NOMEM        -5
#define PE64_MAP_FAILED   -6
#define PE64_BAD_IMPORT   -7

typedef struct {
    uint64_t image_base;   /* where it was actually put                */
    uint64_t entry;        /* absolute address of the entry point      */
    uint64_t image_size;   /* SizeOfImage                              */
    uint64_t imports;      /* how many import thunks were written      */
    uint64_t relocs;       /* how many base relocations were applied   */
    uint64_t pages;        /* pages mapped, image and thunks together  */
} pe64_info_t;

int pe64_load(const void* image, uint64_t size, vmspace64_t* space,
              pe64_info_t* out);

#endif
