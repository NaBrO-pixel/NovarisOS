#ifndef ELF64_H
#define ELF64_H

#include <stdint.h>
#include "vmspace64.h"

/* Enough of ELF64 to load a static executable into an address space.
 *
 * The 32-bit kernel's elf.c reads ELF32 and is not adaptable a field at a
 * time: the header, the program header and every offset in them are wider
 * here, and the two formats disagree about field *order* as well as size
 * (in ELF64 p_flags moves from after p_align to right after p_type). So
 * this is a separate reader rather than an ifdef through the old one. */

#define ELF64_OK           0
#define ELF64_NOT_ELF     -1
#define ELF64_WRONG_CLASS -2   /* a 32-bit ELF, or a wrong-endian one */
#define ELF64_WRONG_MACHINE -3
#define ELF64_NOT_EXEC    -4
#define ELF64_TRUNCATED   -5
#define ELF64_NOMEM       -6
#define ELF64_MAP_FAILED  -7

/* Loads every PT_LOAD segment of `image` into `space`, allocating and
 * mapping frames as it goes, and writes the entry point to entry_out.
 *
 * The frames it allocates are owned by the caller from here: vmspace64
 * frees a space's page tables but deliberately not the pages they point
 * at, since it cannot know what they are. */
int elf64_load(const void* image, uint64_t size, vmspace64_t* space,
               uint64_t* entry_out);

/* How many pages the last successful load mapped - reported so a test
 * can check that a segment with a .bss actually got more memory than the
 * file had bytes. */
uint64_t elf64_pages_mapped(void);

#endif
