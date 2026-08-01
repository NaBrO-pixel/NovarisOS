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
    elf_info_t info;
    if (!elf_load_info(image, size, &info)) return 0;
    *out_entry = info.entry;
    return 1;
}

int elf_load_info(const uint8_t* image, uint32_t size, elf_info_t* out) {
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
            /* Segments routinely share a page - a GNU_RELRO tail and the
             * data segment after it, most commonly - and mapping a fresh
             * frame over one already populated would silently discard the
             * earlier segment's bytes. Every binary with more than two
             * PT_LOADs depends on this check. */
            if (paging_get_entry(va) & PAGE_PRESENT) continue;
            uint32_t phys = pmm_alloc_frame();
            if (!phys) return 0; /* out of memory */
            paging_map_page(va, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER);
        }

        /* Zero only the part no file bytes cover - the memsz-filesz .bss
         * tail. Zeroing the whole segment first would be simpler, and
         * would wipe a preceding segment that shares this one's first
         * page. */
        uint8_t* dest = (uint8_t*)ph[i].p_vaddr;
        const uint8_t* src = image + ph[i].p_offset;
        for (uint32_t b = 0; b < ph[i].p_filesz; b++) dest[b] = src[b];
        for (uint32_t b = ph[i].p_filesz; b < ph[i].p_memsz; b++) dest[b] = 0;
    }

    out->entry = eh->e_entry;
    out->phnum = eh->e_phnum;
    out->phent = eh->e_phentsize;
    /* Where the program headers landed *in the process*. They are only
     * reachable if some PT_LOAD covered them, which is the usual layout
     * (the first segment starts at file offset 0); 0 says "not mapped",
     * and the auxv builder omits AT_PHDR in that case rather than
     * pointing glibc at nothing. */
    out->phdr = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (eh->e_phoff >= ph[i].p_offset &&
            eh->e_phoff < ph[i].p_offset + ph[i].p_filesz) {
            out->phdr = ph[i].p_vaddr + (eh->e_phoff - ph[i].p_offset);
            break;
        }
    }
    return 1;
}
