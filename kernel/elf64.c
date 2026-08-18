/* elf64.c - loading a static x86-64 executable into an address space. */

#include "elf64.h"
#include "paging64.h"
#include "pmm64.h"
#include "kstring.h"

#define KERNEL_VMA   0xFFFFFFFF80000000ULL

#define PT_LOAD 1
#define PF_X    0x1
#define PF_W    0x2

#define EM_X86_64 62
#define ET_EXEC   2
#define ET_DYN    3

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

/* p_flags sits here, between p_type and p_offset. In ELF32 it is the
 * second-to-last field. Getting this wrong reads the segment's alignment
 * as its permissions, which produces a mapping that looks plausible. */
typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) elf64_phdr_t;

static uint64_t pages_mapped;

uint64_t elf64_pages_mapped(void) { return pages_mapped; }

static inline void* window(uint64_t phys) {
    return phys64_to_virt(phys);
}

#define PT_INTERP 3

int elf64_interp(const void* image, uint64_t size, char* buf, uint64_t n) {
    const uint8_t* f = (const uint8_t*)image;
    const elf64_ehdr_t* eh = (const elf64_ehdr_t*)image;
    int i;

    if (size < sizeof(elf64_ehdr_t)) return 0;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E') return 0;

    for (i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t* ph =
            (const elf64_phdr_t*)(f + eh->e_phoff +
                                  (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_INTERP) continue;
        if (ph->p_offset + ph->p_filesz > size) return 0;
        if (ph->p_filesz == 0 || ph->p_filesz > n) return 0;
        kmemcpy(buf, f + ph->p_offset, ph->p_filesz);
        buf[ph->p_filesz - 1] = 0;      /* it is NUL-terminated already */
        return 1;
    }
    return 0;
}

int elf64_load(const void* image, uint64_t size, vmspace64_t* space,
               elf64_info_t* out) {
    return elf64_load_at(image, size, space, 0, out);
}

int elf64_load_at(const void* image, uint64_t size, vmspace64_t* space,
                  uint64_t bias, elf64_info_t* out) {
    const uint8_t* base = (const uint8_t*)image;
    const elf64_ehdr_t* eh = (const elf64_ehdr_t*)image;
    uint64_t saved_cr3;
    uint64_t phdr_va = 0, brk_start = 0;
    int i, rc = ELF64_OK;

    pages_mapped = 0;

    if (size < sizeof(elf64_ehdr_t)) return ELF64_TRUNCATED;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') return ELF64_NOT_ELF;
    if (eh->e_ident[4] != 2)  return ELF64_WRONG_CLASS;   /* ELFCLASS64 */
    if (eh->e_ident[5] != 1)  return ELF64_WRONG_CLASS;   /* little endian */
    if (eh->e_machine != EM_X86_64) return ELF64_WRONG_MACHINE;
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) return ELF64_NOT_EXEC;
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size)
        return ELF64_TRUNCATED;

    /* Everything below maps into `space`, so run in it rather than
     * switching per page. The kernel's own half is identical in every
     * space, so the code, its stack and the physical window it copies
     * through are all still where they were. */
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(saved_cr3));
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(space->pml4_phys) : "memory");

    for (i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t* ph =
            (const elf64_phdr_t*)(base + eh->e_phoff +
                                  (uint64_t)i * eh->e_phentsize);
        uint64_t start, end, va, flags, vaddr;

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        /* Everything below works in loaded addresses, not file ones.
         * An ET_DYN image asks to be placed at 0 and expects the loader
         * to choose, so the bias is what makes it real. */
        vaddr = ph->p_vaddr + bias;
        if (ph->p_offset + ph->p_filesz > size) { rc = ELF64_TRUNCATED; break; }
        if (ph->p_filesz > ph->p_memsz)         { rc = ELF64_TRUNCATED; break; }

        /* The program headers are mapped wherever the segment that
         * contains them in the file was mapped. Worked out rather than
         * assumed to be load_base + e_phoff, which is only true because
         * ld happens to put the first segment at file offset 0. */
        if (!phdr_va &&
            eh->e_phoff >= ph->p_offset &&
            eh->e_phoff < ph->p_offset + ph->p_filesz)
            phdr_va = vaddr + (eh->e_phoff - ph->p_offset);

        if (vaddr + ph->p_memsz > brk_start)
            brk_start = vaddr + ph->p_memsz;

        start = vaddr & ~(PAGE64_SIZE - 1);
        end   = (vaddr + ph->p_memsz + PAGE64_SIZE - 1)
                & ~(PAGE64_SIZE - 1);

        flags = PAGE64_PRESENT | PAGE64_USER;
        if (ph->p_flags & PF_W) flags |= PAGE64_WRITE;

        for (va = start; va < end; va += PAGE64_SIZE) {
            uint64_t frame, copy_from, copy_to;
            uint8_t* dst;

            /* Two segments commonly share the page where one ends and
             * the next begins. Re-mapping it would strand the frame that
             * is already there and lose the bytes already written into
             * it, so an existing mapping is reused. */
            if (paging64_translate(va, &frame) == PAGING64_OK) {
                frame &= ~(PAGE64_SIZE - 1);
                /* The shared page needs the union of both permissions. */
                if (flags & PAGE64_WRITE)
                    paging64_map(va, frame, flags);
            } else {
                frame = pmm64_alloc_high();
                if (!frame) { rc = ELF64_NOMEM; break; }
                if (paging64_map(va, frame, flags) != PAGING64_OK) {
                    pmm64_free_frame(frame);
                    rc = ELF64_MAP_FAILED;
                    break;
                }
                /* Zero first, which is what gives a segment its .bss:
                 * p_memsz beyond p_filesz is defined to read as zero. */
                kmemset(window(frame), 0, PAGE64_SIZE);
                pages_mapped++;
            }

            dst = (uint8_t*)window(frame);

            /* The part of this page the file actually has bytes for. */
            copy_from = va > vaddr ? va : vaddr;
            copy_to   = va + PAGE64_SIZE;
            if (copy_to > vaddr + ph->p_filesz)
                copy_to = vaddr + ph->p_filesz;

            if (copy_to > copy_from)
                kmemcpy(dst + (copy_from - va),
                        base + ph->p_offset + (copy_from - vaddr),
                        copy_to - copy_from);
        }
        if (rc != ELF64_OK) break;
    }

    __asm__ __volatile__("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");

    if (rc == ELF64_OK && out) {
        /* Big enough for a real interpreter path - too small a buffer
         * would make elf64_interp fail and report a dynamic image as
         * static. */
        char probe[128];
        out->entry     = eh->e_entry + bias;
        out->phdr_va   = phdr_va;
        out->phent     = eh->e_phentsize;
        out->phnum     = eh->e_phnum;
        out->brk_start = (brk_start + PAGE64_SIZE - 1)
                         & ~(PAGE64_SIZE - 1);
        out->base       = bias;
        out->is_dyn     = (eh->e_type == ET_DYN);
        out->has_interp = elf64_interp(image, size, probe, sizeof(probe))
                          ? 1 : 0;
    }
    return rc;
}
