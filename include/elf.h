#ifndef ELF_H
#define ELF_H

#include <stdint.h>

/* ELF32 loading. Milestone 22 extended this from "static executables
 * only" to what a *dynamically* linked program needs from the kernel:
 * ET_DYN images loaded at a base of the kernel's choosing, and PT_INTERP
 * recognised so the real dynamic linker can be loaded alongside.
 *
 * Note what the kernel deliberately does *not* do, because it is the
 * whole shape of dynamic linking on Unix: no relocations, no symbol
 * resolution, no .so loading. Those are ld-linux.so.2's job. The kernel
 * loads two images - the program and its interpreter - hands the
 * interpreter a correct auxiliary vector, and gets out of the way. */

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
#define ET_DYN  3
#define EM_386  3

#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_PHDR    6

/* Returns 1 if `image` looks like a 32-bit little-endian ELF executable
 * we can actually load (ET_EXEC, EM_386) - used by the shell's `run`
 * command to pick between this loader and the older flat-binary path. */
int elf_is_valid(const uint8_t* image, uint32_t size);

/* What the loader learned about the image, for the initial process stack
 * the System V ABI says a program is entered with. glibc's startup reads
 * AT_PHDR/AT_PHNUM/AT_PHENT out of the auxiliary vector to find its own
 * program headers - among other things, that is how it locates PT_TLS. */
typedef struct {
    uint32_t entry;
    uint32_t phdr;    /* the program headers' address *in the process* */
    uint32_t phnum;
    uint32_t phent;
    uint32_t base;    /* the bias applied; 0 for a fixed-address ET_EXEC */
    /* The PT_INTERP path, empty for a static image. Non-empty means the
     * caller has to load that interpreter too and enter *it* rather than
     * this image's entry point. */
    char     interp[64];
} elf_info_t;

/* Maps every PT_LOAD segment at its ELF-specified virtual address
 * (PAGE_USER|PAGE_RW - we don't distinguish read-only/executable
 * mappings yet), copies segment data, and zero-fills the
 * memsz-minus-filesz tail (.bss). Returns the entry point in *out_entry,
 * or 0 and leaves *out_entry unchanged on failure. */
int elf_load(const uint8_t* image, uint32_t size, uint32_t* out_entry);

/* Same, but also reports what the initial process stack needs. */
int elf_load_info(const uint8_t* image, uint32_t size, elf_info_t* out);

/* Loads at `bias`, for a position-independent (ET_DYN) image. An ET_EXEC
 * image ignores the bias - its addresses are not negotiable. */
int elf_load_at(const uint8_t* image, uint32_t size, uint32_t bias,
                elf_info_t* out);

/* Reads the header without loading anything: is this an ELF this kernel
 * could run, and does it need an interpreter? */
int elf_peek(const uint8_t* image, uint32_t size, elf_info_t* out);

#endif
