#ifndef ELF_H
#define ELF_H

#include <stdint.h>

/* Just enough of ELF32 to load a static executable's PT_LOAD segments -
 * no dynamic linking (no PT_DYNAMIC, no relocations, no .so loading)
 * yet. That's real, separately-scoped follow-up work (see ROADMAP.md's
 * Milestone 8 notes) - Wine specifically needs shared-library loading,
 * which this deliberately doesn't attempt. */

#define EI_NIDENT 16

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed)) Elf32_Phdr;

#define ELFMAG0 0x7F
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

#define ELFCLASS32 1
#define ELFDATA2LSB 1

#define ET_EXEC 2
#define EM_386  3

#define PT_LOAD 1

/* Returns 1 if `image` looks like a 32-bit little-endian ELF executable
 * we can actually load (ET_EXEC, EM_386) - used by the shell's `run`
 * command to pick between this loader and the older flat-binary path. */
int elf_is_valid(const uint8_t* image, uint32_t size);

/* Maps every PT_LOAD segment at its ELF-specified virtual address
 * (PAGE_USER|PAGE_RW - we don't distinguish read-only/executable
 * mappings yet), copies segment data, and zero-fills the
 * memsz-minus-filesz tail (.bss). Returns the entry point in *out_entry,
 * or 0 and leaves *out_entry unchanged on failure. */
int elf_load(const uint8_t* image, uint32_t size, uint32_t* out_entry);

#endif
