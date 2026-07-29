#include "elf.h"
#include "paging.h"
#include "pmm.h"

int elf_is_valid(const uint8_t* image, uint32_t size) {
    if (size < sizeof(Elf32_Ehdr)) return 0;
    const Elf32_Ehdr* eh = (const Elf32_Ehdr*)image;

    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
        eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3) {
        return 0;
    }
    if (eh->e_ident[4] != ELFCLASS32 || eh->e_ident[5] != ELFDATA2LSB) return 0;
    if (eh->e_type != ET_EXEC || eh->e_machine != EM_386) return 0;
    return 1;
}

int elf_load(const uint8_t* image, uint32_t size, uint32_t* out_entry) {
    if (!elf_is_valid(image, size)) return 0;
    const Elf32_Ehdr* eh = (const Elf32_Ehdr*)image;

    if (eh->e_phoff + (uint32_t)eh->e_phnum * eh->e_phentsize > size) return 0;
    const Elf32_Phdr* ph = (const Elf32_Phdr*)(image + eh->e_phoff);

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_offset + ph[i].p_filesz > size) return 0;

        uint32_t seg_start = ph[i].p_vaddr & ~0xFFFu;
        uint32_t seg_end = (ph[i].p_vaddr + ph[i].p_memsz + 0xFFFu) & ~0xFFFu;

        for (uint32_t va = seg_start; va < seg_end; va += 4096) {
            uint32_t phys = pmm_alloc_frame();
            if (!phys) return 0; /* out of memory */
            paging_map_page(va, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER);
        }

        /* Zero the whole segment first (covers the memsz-filesz .bss
         * tail), then copy in the file's bytes. */
        uint8_t* dest = (uint8_t*)ph[i].p_vaddr;
        for (uint32_t b = 0; b < ph[i].p_memsz; b++) dest[b] = 0;
        const uint8_t* src = image + ph[i].p_offset;
        for (uint32_t b = 0; b < ph[i].p_filesz; b++) dest[b] = src[b];
    }

    *out_entry = eh->e_entry;
    return 1;
}
