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

/* What the loader learned, and what a Linux process needs told to it on
 * its initial stack. AT_PHDR in particular is not optional for glibc: it
 * walks its own program headers at startup to find PT_TLS and PT_GNU_RELRO,
 * so it has to be told where they were mapped. */
typedef struct {
    uint64_t entry;      /* e_entry                                  */
    uint64_t phdr_va;    /* where the program headers landed, AT_PHDR */
    uint64_t phent;      /* AT_PHENT                                  */
    uint64_t phnum;      /* AT_PHNUM                                  */
    uint64_t brk_start;  /* first page past the last segment          */
} elf64_info_t;

/* Loads every PT_LOAD segment of `image` into `space`, allocating and
 * mapping frames as it goes.
 *
 * The frames it allocates are owned by the caller from here: vmspace64
 * frees a space's page tables but deliberately not the pages they point
 * at, since it cannot know what they are. */
int elf64_load(const void* image, uint64_t size, vmspace64_t* space,
               elf64_info_t* out);

/* How many pages the last successful load mapped - reported so a test
 * can check that a segment with a .bss actually got more memory than the
 * file had bytes. */
uint64_t elf64_pages_mapped(void);

#endif
