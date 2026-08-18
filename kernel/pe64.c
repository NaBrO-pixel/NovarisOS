/* pe64.c - loading a 64-bit Windows executable. */

#include "pe64.h"
#include "paging64.h"
#include "pmm64.h"
#include "kstring.h"
#include "win32_64.h"

#define KERNEL_VMA  0xFFFFFFFF80000000ULL

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

typedef struct {
    uint32_t Characteristics;
    uint32_t TimeDateStamp;
    uint16_t MajorVersion;
    uint16_t MinorVersion;
    uint32_t Name;
    uint32_t Base;                  /* the ordinal the table starts at */
    uint32_t NumberOfFunctions;
    uint32_t NumberOfNames;
    uint32_t AddressOfFunctions;
    uint32_t AddressOfNames;
    uint32_t AddressOfNameOrdinals;
} __attribute__((packed)) export_dir_t;

/* Modules loaded into the current process, in load order. A real loader
 * keeps this per process next to the address space; this is a static for
 * the same reason uspace64.c's is, and pe64_reset_modules() is what
 * stands in for the process going away. */
#define MAX_MODULES 8

static struct {
    char     name[32];
    uint64_t base;
    uint32_t export_rva;
    uint32_t export_size;
} modules[MAX_MODULES];
static uint64_t module_count;

void pe64_reset_modules(void) { module_count = 0; }
uint64_t pe64_module_count(void) { return module_count; }

/* Windows compares module names case-insensitively, and import tables
 * spell them every possible way. */
static int name_matches(const char* a, const char* b) {
    return kstricmp(a, b) == 0;
}

/* Resolves an export in an already-loaded module. Must run with that
 * module's address space current, since the tables it walks are the
 * mapped ones rather than the file's.
 *
 * `dll` selects by name; passing 0 with a non-zero `base` selects by
 * base address instead, which is what GetProcAddress needs - it is
 * handed an HMODULE, not a name. */
static uint64_t module_export_ex(const char* dll, uint64_t want_base,
                                 const char* fn,
                                 uint32_t ordinal, int by_ordinal) {
    for (uint64_t m = 0; m < module_count; m++) {
        const export_dir_t* ed;
        const uint32_t* funcs;
        uint64_t base = modules[m].base;
        uint32_t idx;

        if (dll) {
            if (!name_matches(modules[m].name, dll)) continue;
        } else {
            if (base != want_base) continue;
        }
        if (!modules[m].export_size) return 0;

        ed = (const export_dir_t*)(base + modules[m].export_rva);
        funcs = (const uint32_t*)(base + ed->AddressOfFunctions);

        if (by_ordinal) {
            if (ordinal < ed->Base) return 0;
            idx = ordinal - ed->Base;
            if (idx >= ed->NumberOfFunctions) return 0;
        } else {
            const uint32_t* names = (const uint32_t*)(base + ed->AddressOfNames);
            const uint16_t* ords =
                (const uint16_t*)(base + ed->AddressOfNameOrdinals);
            uint32_t i;
            for (i = 0; i < ed->NumberOfNames; i++)
                if (kstrcmp((const char*)(base + names[i]), fn) == 0) break;
            if (i == ed->NumberOfNames) return 0;
            idx = ords[i];
            if (idx >= ed->NumberOfFunctions) return 0;
        }

        /* An export RVA that lands inside the export directory itself is
         * not code: it is a forwarder string, "OTHERDLL.OtherFunc", and
         * following it means loading that module too. Rejected rather
         * than returning a pointer to a string as if it were a
         * function - Chrome's DLLs use forwarders heavily. */
        if (funcs[idx] >= modules[m].export_rva &&
            funcs[idx] <  modules[m].export_rva + modules[m].export_size)
            return 0;

        return base + funcs[idx];
    }
    return 0;
}

static uint64_t module_export(const char* dll, const char* fn,
                              uint32_t ordinal, int by_ordinal) {
    return module_export_ex(dll, 0, fn, ordinal, by_ordinal);
}

uint64_t pe64_export_by_base(uint64_t base, const char* function) {
    return module_export_ex(0, base, function, 0, 0);
}

uint64_t pe64_module_base(const char* name) {
    for (uint64_t m = 0; m < module_count; m++)
        if (name_matches(modules[m].name, name)) return modules[m].base;
    return 0;
}

/* One import thunk, 24 bytes:
 *
 *   57              push rdi         see below
 *   56              push rsi
 *   48 89 CF        mov rdi, rcx     Windows arg 1 -> SysV arg 1
 *   48 89 D6        mov rsi, rdx     arg 2 (before rdx is overwritten)
 *   4C 89 C2        mov rdx, r8      arg 3
 *   4D 89 CA        mov r10, r9      arg 4 - r10, not rcx, because
 *                                    SYSCALL destroys rcx
 *   B8 nn nn nn nn  mov eax, number
 *   0F 05           syscall
 *   5E              pop rsi
 *   5F              pop rdi
 *   C3              ret
 *
 * The move order is not free: rsi <- rdx has to happen before rdx <- r8,
 * or the second argument is lost.
 *
 * The pushes are not optional either, and this is the trap in bridging
 * two ABIs rather than one: **rdi and rsi are callee-saved on Windows
 * and volatile on SysV**. A Windows program may keep a live value in
 * either across a call - mingw does exactly that, holding a pointer in
 * rsi across a GetStdHandle call - while the SysV C this syscall lands
 * in is entitled to destroy both. Without saving them the program
 * resumes with a register quietly rewritten, and what that looks like is
 * the *next* call receiving a plausible-but-wrong argument.
 *
 * The other direction of the same problem is already handled: Windows
 * also treats xmm6-xmm15 as callee-saved, and the kernel is compiled
 * -mno-sse (Milestone 51) so it cannot touch them at all.
 *
 * Pushing shifts rsp, which would matter for a function reading its
 * fifth and later arguments off the stack. Nothing here has one. */
#define THUNK_SIZE 24

static void write_thunk(uint8_t* p, uint32_t number) {
    static const uint8_t prologue[] = {
        0x57,
        0x56,
        0x48, 0x89, 0xCF,
        0x48, 0x89, 0xD6,
        0x4C, 0x89, 0xC2,
        0x4D, 0x89, 0xCA,
    };
    int i;
    for (i = 0; i < (int)sizeof(prologue); i++) p[i] = prologue[i];
    p[14] = 0xB8;
    p[15] = (uint8_t)(number      );
    p[16] = (uint8_t)(number >>  8);
    p[17] = (uint8_t)(number >> 16);
    p[18] = (uint8_t)(number >> 24);
    p[19] = 0x0F;
    p[20] = 0x05;
    p[21] = 0x5E;
    p[22] = 0x5F;
    p[23] = 0xC3;
}

static int map_pages(uint64_t start, uint64_t end, uint64_t* pages) {
    for (uint64_t va = start; va < end; va += PAGE64_SIZE) {
        uint64_t frame, existing;
        if (paging64_translate(va, &existing) == PAGING64_OK) continue;
        frame = pmm64_alloc_high();
        if (!frame) {
            if (frame) pmm64_free_frame(frame);
            return 0;
        }
        kmemset(phys64_to_virt(frame), 0, PAGE64_SIZE);
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

/* Everything except the CR3 switch, so that the executable and the DLLs
 * beside it share one implementation. Runs with `space` already
 * current. */
static int load_common(const void* image, uint64_t size, uint64_t bias,
                       const char* module_name, pe64_info_t* out) {
    const uint8_t* f = (const uint8_t*)image;
    uint32_t pe_off, n_sections, size_opt, size_headers, size_image;
    uint32_t entry_rva, i;
    uint64_t want_base, base, delta, thunk_va, thunk_next;
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

    base = want_base + bias;
    delta = bias;

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

    /* Registered before its own imports are resolved, which is harmless
     * and means the export table is readable the moment anything asks. */
    if (module_name) {
        if (module_count >= MAX_MODULES) { rc = PE64_NOMEM; goto done; }
        kstrlcpy(modules[module_count].name, module_name,
                 sizeof(modules[module_count].name));
        modules[module_count].base = base;
        if (n_dirs > DIR_EXPORT) {
            modules[module_count].export_rva  = dirs[DIR_EXPORT].VirtualAddress;
            modules[module_count].export_size = dirs[DIR_EXPORT].Size;
        } else {
            modules[module_count].export_rva  = 0;
            modules[module_count].export_size = 0;
        }
        module_count++;
    }

    /* Imports. A name the kernel provides becomes a thunk in the page
     * above the image; a name another loaded module exports becomes that
     * module's actual address. */
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
                const char* name = 0;
                uint32_t ordinal = 0;
                int by_ordinal = (*lookup & IMAGE_ORDINAL_FLAG64) != 0;
                uint64_t addr;
                int number;

                if (by_ordinal) {
                    ordinal = (uint32_t)(*lookup & 0xFFFF);
                } else {
                    /* IMAGE_IMPORT_BY_NAME: a 2-byte hint, then the name. */
                    name = (const char*)(base + (*lookup & 0x7FFFFFFF) + 2);
                }

                /* A real module that has been loaded wins: the import
                 * resolves to an actual address inside it, and the call
                 * is an ordinary ring-3 call with no thunk and no
                 * syscall in the middle. */
                addr = module_export(dll, name, ordinal, by_ordinal);
                if (addr) {
                    *iat = addr;
                    imports++;
                    continue;
                }

                /* Otherwise it has to be something the kernel provides,
                 * which is only ever by name - an ordinal is meaningless
                 * without the exporting DLL's table. */
                if (by_ordinal) { rc = PE64_BAD_IMPORT; goto done; }

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

/* The two public entry points differ only in whether the image's exports
 * are recorded and whether it is allowed to move. */
static int load_in_space(const void* image, uint64_t size,
                         vmspace64_t* space, uint64_t bias,
                         const char* module_name, pe64_info_t* out) {
    uint64_t saved_cr3;
    int rc;

    __asm__ __volatile__("mov %%cr3, %0" : "=r"(saved_cr3));
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(space->pml4_phys) : "memory");
    rc = load_common(image, size, bias, module_name, out);
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(saved_cr3) : "memory");
    return rc;
}

int pe64_load(const void* image, uint64_t size, vmspace64_t* space,
              pe64_info_t* out) {
    return load_in_space(image, size, space, 0, 0, out);
}

int pe64_load_dll(const void* image, uint64_t size, vmspace64_t* space,
                  const char* name, uint64_t bias, pe64_info_t* out) {
    return load_in_space(image, size, space, bias, name, out);
}

int pe64_load_dll_here(const void* image, uint64_t size, const char* name,
                       uint64_t bias, pe64_info_t* out) {
    /* No CR3 switch: the caller is a syscall from the process this
     * module belongs to, so its space is already loaded. */
    return load_common(image, size, bias, name, out);
}
