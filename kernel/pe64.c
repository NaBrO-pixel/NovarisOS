/* pe64.c - loading a 64-bit Windows executable. */

#include "pe64.h"
#include "paging64.h"
#include "pmm64.h"
#include "kstring.h"
#include "win32_64.h"

#define KERNEL_VMA  0xFFFFFFFF80000000ULL
#define PHYS_WINDOW 0x40000000ULL

#define IMAGE_FILE_MACHINE_AMD64 0x8664
#define PE32PLUS_MAGIC           0x20B

#define DIR_EXPORT  0
#define DIR_IMPORT  1
#define DIR_BASERELOC 5

#define IMAGE_ORDINAL_FLAG64 (1ULL << 63)

#define REL_BASED_ABSOLUTE 0
#define REL_BASED_DIR64    10

typedef struct {
    uint32_t VirtualAddress;
    uint32_t Size;
} __attribute__((packed)) data_dir_t;

typedef struct {
    char     Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
} __attribute__((packed)) section_hdr_t;

typedef struct {
    uint32_t OriginalFirstThunk;
    uint32_t TimeDateStamp;
    uint32_t ForwarderChain;
    uint32_t Name;
    uint32_t FirstThunk;
} __attribute__((packed)) import_desc_t;

typedef struct {
    uint32_t VirtualAddress;
    uint32_t SizeOfBlock;
} __attribute__((packed)) reloc_block_t;

/* One import thunk, 20 bytes:
 *
 *   48 89 CF        mov rdi, rcx     Windows arg 1 -> SysV arg 1
 *   48 89 D6        mov rsi, rdx     arg 2 (before rdx is overwritten)
 *   4C 89 C2        mov rdx, r8      arg 3
 *   4D 89 CA        mov r10, r9      arg 4 - r10, not rcx, because
 *                                    SYSCALL destroys rcx
 *   B8 nn nn nn nn  mov eax, number
 *   0F 05           syscall
 *   C3              ret
 *
 * The move order is not free: rsi <- rdx has to happen before rdx <- r8,
 * or the second argument is lost. */
#define THUNK_SIZE 20

static void write_thunk(uint8_t* p, uint32_t number) {
    static const uint8_t prologue[] = {
        0x48, 0x89, 0xCF,
        0x48, 0x89, 0xD6,
        0x4C, 0x89, 0xC2,
        0x4D, 0x89, 0xCA,
    };
    int i;
    for (i = 0; i < (int)sizeof(prologue); i++) p[i] = prologue[i];
    p[12] = 0xB8;
    p[13] = (uint8_t)(number      );
    p[14] = (uint8_t)(number >>  8);
    p[15] = (uint8_t)(number >> 16);
    p[16] = (uint8_t)(number >> 24);
    p[17] = 0x0F;
    p[18] = 0x05;
    p[19] = 0xC3;
}

static int map_pages(uint64_t start, uint64_t end, uint64_t* pages) {
    for (uint64_t va = start; va < end; va += PAGE64_SIZE) {
        uint64_t frame, existing;
        if (paging64_translate(va, &existing) == PAGING64_OK) continue;
        frame = pmm64_alloc_frame();
        if (!frame || frame >= PHYS_WINDOW) {
            if (frame) pmm64_free_frame(frame);
            return 0;
        }
        kmemset((void*)(KERNEL_VMA + frame), 0, PAGE64_SIZE);
        /* Mapped writable and executable throughout. Honouring the
         * sections' own characteristics would need NX, and would break
         * the import thunks, which live in a page this loader writes and
         * the program then calls. */
        if (paging64_map(va, frame,
                         PAGE64_PRESENT | PAGE64_WRITE | PAGE64_USER)
                != PAGING64_OK) {
            pmm64_free_frame(frame);
            return 0;
        }
        (*pages)++;
    }
    return 1;
}

int pe64_load(const void* image, uint64_t size, vmspace64_t* space,
              pe64_info_t* out) {
    const uint8_t* f = (const uint8_t*)image;
    uint32_t pe_off, n_sections, size_opt, size_headers, size_image;
    uint32_t entry_rva, i;
    uint64_t want_base, base, delta, saved_cr3, thunk_va, thunk_next;
    uint64_t pages = 0, imports = 0, relocs = 0;
    const data_dir_t* dirs;
    const section_hdr_t* sections;
    uint32_t opt, n_dirs;
    int rc = PE64_OK;

    if (size < 0x40 || f[0] != 'M' || f[1] != 'Z') return PE64_NOT_PE;
    pe_off = *(const uint32_t*)(f + 0x3C);
    if (pe_off + 24 > size) return PE64_TRUNCATED;
    if (f[pe_off] != 'P' || f[pe_off + 1] != 'E' ||
        f[pe_off + 2] || f[pe_off + 3]) return PE64_NOT_PE;

    if (*(const uint16_t*)(f + pe_off + 4) != IMAGE_FILE_MACHINE_AMD64)
        return PE64_WRONG_MACHINE;
    n_sections = *(const uint16_t*)(f + pe_off + 6);
    size_opt   = *(const uint16_t*)(f + pe_off + 20);

    opt = pe_off + 24;
    if (*(const uint16_t*)(f + opt) != PE32PLUS_MAGIC)
        return PE64_NOT_PE32PLUS;

    /* PE32+ offsets. BaseOfData does not exist here, which is what moves
     * everything after it relative to PE32. */
    entry_rva    = *(const uint32_t*)(f + opt + 16);
    want_base    = *(const uint64_t*)(f + opt + 24);
    size_image   = *(const uint32_t*)(f + opt + 56);
    size_headers = *(const uint32_t*)(f + opt + 60);
    n_dirs       = *(const uint32_t*)(f + opt + 108);
    dirs         = (const data_dir_t*)(f + opt + 112);
    sections     = (const section_hdr_t*)(f + opt + size_opt);

    if (opt + size_opt + n_sections * sizeof(section_hdr_t) > size)
        return PE64_TRUNCATED;

    base = want_base;              /* no ASLR: loaded where it asked */
    delta = base - want_base;

    __asm__ __volatile__("mov %%cr3, %0" : "=r"(saved_cr3));
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(space->pml4_phys) : "memory");

    /* The image, plus one page after it for the import thunks. */
    thunk_va = (base + size_image + PAGE64_SIZE - 1) & ~(PAGE64_SIZE - 1);
    if (!map_pages(base, thunk_va + PAGE64_SIZE, &pages)) {
        rc = PE64_NOMEM;
        goto done;
    }

    if (size_headers > size) { rc = PE64_TRUNCATED; goto done; }
    kmemcpy((void*)base, f, size_headers);

    for (i = 0; i < n_sections; i++) {
        const section_hdr_t* s = &sections[i];
        uint64_t dst = base + s->VirtualAddress;
        if (s->SizeOfRawData == 0) continue;      /* .bss: already zero */
        if ((uint64_t)s->PointerToRawData + s->SizeOfRawData > size) {
            rc = PE64_TRUNCATED;
            goto done;
        }
        kmemcpy((void*)dst, f + s->PointerToRawData, s->SizeOfRawData);
    }

    /* Base relocations. Not needed while the image loads where it asked,
     * but chrome.exe will not, and a loader that silently skips them
     * produces a program that runs until the first absolute address. */
    if (delta && n_dirs > DIR_BASERELOC && dirs[DIR_BASERELOC].Size) {
        uint64_t p   = base + dirs[DIR_BASERELOC].VirtualAddress;
        uint64_t end = p + dirs[DIR_BASERELOC].Size;
        while (p + sizeof(reloc_block_t) <= end) {
            const reloc_block_t* b = (const reloc_block_t*)p;
            uint32_t n, k;
            if (b->SizeOfBlock < sizeof(reloc_block_t)) break;
            n = (b->SizeOfBlock - sizeof(reloc_block_t)) / 2;
            for (k = 0; k < n; k++) {
                uint16_t e = *(const uint16_t*)(p + sizeof(reloc_block_t)
                                                + k * 2);
                uint16_t type = e >> 12;
                uint64_t at = base + b->VirtualAddress + (e & 0xFFF);
                if (type == REL_BASED_DIR64) {
                    *(uint64_t*)at += delta;
                    relocs++;
                } else if (type != REL_BASED_ABSOLUTE) {
                    /* 32-bit relocation types cannot be represented in a
                     * 64-bit image that has moved more than 2GB; a real
                     * loader rejects the image rather than truncating. */
                    rc = PE64_BAD_IMPORT;
                    goto done;
                }
            }
            p += b->SizeOfBlock;
        }
    }

    /* Imports. Every name is resolved to a thunk in the page above the
     * image, and the thunk's address is written into the IAT. */
    thunk_next = thunk_va;
    if (n_dirs > DIR_IMPORT && dirs[DIR_IMPORT].Size) {
        const import_desc_t* d =
            (const import_desc_t*)(base + dirs[DIR_IMPORT].VirtualAddress);
        for (; d->Name; d++) {
            const char* dll = (const char*)(base + d->Name);
            uint64_t* lookup = (uint64_t*)(base + (d->OriginalFirstThunk
                                                   ? d->OriginalFirstThunk
                                                   : d->FirstThunk));
            uint64_t* iat = (uint64_t*)(base + d->FirstThunk);

            for (; *lookup; lookup++, iat++) {
                const char* name;
                int number;

                if (*lookup & IMAGE_ORDINAL_FLAG64) {
                    /* Importing by ordinal needs the exporting DLL's
                     * export table, which nothing here has. */
                    rc = PE64_BAD_IMPORT;
                    goto done;
                }
                /* IMAGE_IMPORT_BY_NAME: a 2-byte hint, then the name. */
                name = (const char*)(base + (*lookup & 0x7FFFFFFF) + 2);

                number = win32_64_resolve(dll, name);
                if (number < 0) { rc = PE64_BAD_IMPORT; goto done; }

                if (thunk_next + THUNK_SIZE > thunk_va + PAGE64_SIZE) {
                    rc = PE64_NOMEM;
                    goto done;
                }
                write_thunk((uint8_t*)thunk_next, (uint32_t)number);
                *iat = thunk_next;
                thunk_next += THUNK_SIZE;
                imports++;
            }
        }
    }

done:
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");

    if (rc == PE64_OK && out) {
        out->image_base = base;
        out->entry      = base + entry_rva;
        out->image_size = size_image;
        out->imports    = imports;
        out->relocs     = relocs;
        out->pages      = pages;
    }
    return rc;
}
