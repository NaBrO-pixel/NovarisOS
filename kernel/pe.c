#include "pe.h"
#include "paging.h"
#include "pmm.h"
#include "kheap.h"
#include "kstring.h"

/* See the long comment at the top of pe.h before touching this file. */

#define PAGE_SIZE 4096u

/* Where an image goes when it can't have the address it asked for. Any
 * PE with a base relocation table can be moved here; images built
 * /DYNAMICBASE (the mingw-w64 and MSVC default for years now) all can.
 * Well clear of the flat-binary load address (0x40000000), the Win32
 * arenas (0x5C000000 and up) and the kernel's own space (0xC0000000+). */
#define PE_RELOC_ARENA_START 0x30000000u
#define PE_RELOC_ARENA_END   0x38000000u
static uint32_t reloc_arena_next = PE_RELOC_ARENA_START;

/* Nothing may be loaded below this - everything under 1MB is real-mode /
 * BIOS / VGA memory the kernel still uses. */
#define PE_MIN_LOAD_ADDR 0x00100000u
#define PE_MAX_LOAD_ADDR 0xC0000000u /* kernel space starts here */

const char* pe_result_string(pe_load_result_t r) {
    switch (r) {
        case PE_OK:                   return "ok";
        case PE_ERR_NOT_PE:           return "not a PE executable";
        case PE_ERR_BAD_HEADER:       return "malformed PE headers";
        case PE_ERR_PE32PLUS:         return "64-bit (PE32+) binary";
        case PE_ERR_BAD_MACHINE:      return "built for another CPU architecture";
        case PE_ERR_IS_DLL:           return "a DLL, not a program";
        case PE_ERR_DOTNET:           return ".NET/CLR managed binary";
        case PE_ERR_BAD_SECTION:      return "a section points outside the file";
        case PE_ERR_TOO_LARGE:        return "image too large to map";
        case PE_ERR_OUT_OF_MEMORY:    return "out of physical memory";
        case PE_ERR_NO_ADDRESS_SPACE: return "needs an address range that is in use";
        case PE_ERR_BAD_IMPORTS:      return "unreadable import directory";
        case PE_ERR_MISSING_IMPORTS:  return "imports an API this OS doesn't provide";
    }
    return "unknown error";
}

/* --- header parsing ---------------------------------------------------- */

/* Locates the optional header, checking every offset against `size` on
 * the way. Returns 0 and sets *err if anything doesn't fit or add up. */
static const optional_header32_t* pe_headers(const uint8_t* image, uint32_t size,
                                             const coff_header_t** out_coff,
                                             pe_load_result_t* err) {
    *err = PE_ERR_NOT_PE;
    if (size < sizeof(dos_header_t)) return 0;

    const dos_header_t* dos = (const dos_header_t*)image;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;

    if ((uint64_t)dos->e_lfanew + 4 + sizeof(coff_header_t) > size) return 0;

    const uint32_t* pe_sig = (const uint32_t*)(image + dos->e_lfanew);
    if (*pe_sig != IMAGE_NT_SIGNATURE) return 0;

    /* Past this point we know it's meant to be a PE, so failures become
     * "malformed" rather than "not a PE at all". */
    *err = PE_ERR_BAD_HEADER;

    const coff_header_t* coff = (const coff_header_t*)(image + dos->e_lfanew + 4);
    if (out_coff) *out_coff = coff;

    uint32_t opt_off = dos->e_lfanew + 4 + (uint32_t)sizeof(coff_header_t);
    if ((uint64_t)opt_off + 2 > size) return 0;

    const optional_header32_t* opt = (const optional_header32_t*)(image + opt_off);
    if (opt->Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        *err = PE_ERR_PE32PLUS;
        return 0;
    }
    if (opt->Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return 0;
    if (coff->SizeOfOptionalHeader < sizeof(optional_header32_t)) return 0;
    if ((uint64_t)opt_off + sizeof(optional_header32_t) > size) return 0;

    *err = PE_OK;
    return opt;
}

static const section_header_t* pe_sections(const uint8_t* image, uint32_t size,
                                           const coff_header_t* coff) {
    uint32_t lfanew = ((const dos_header_t*)image)->e_lfanew;
    uint32_t off = lfanew + 4 + (uint32_t)sizeof(coff_header_t) +
                   coff->SizeOfOptionalHeader;
    if ((uint64_t)off + (uint64_t)coff->NumberOfSections * sizeof(section_header_t) > size) {
        return 0;
    }
    return (const section_header_t*)(image + off);
}

static const data_directory_t* pe_dir(const optional_header32_t* opt, uint32_t index) {
    if (opt->NumberOfRvaAndSizes <= index) return 0;
    const data_directory_t* d = &opt->DataDirectory[index];
    if (d->VirtualAddress == 0 || d->Size == 0) return 0;
    return d;
}

int pe_is_valid(const uint8_t* image, uint32_t size) {
    if (size < sizeof(dos_header_t)) return 0;
    const dos_header_t* dos = (const dos_header_t*)image;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    if ((uint64_t)dos->e_lfanew + 4 > size) return 0;
    return *(const uint32_t*)(image + dos->e_lfanew) == IMAGE_NT_SIGNATURE;
}

/* Maps an RVA to a file offset via the section table. Returns 0 if the
 * RVA isn't covered by any section - a file offset of 0 is the MZ magic,
 * never real section content, so it doubles as the failure value. */
static uint32_t rva_to_offset(const section_header_t* sh, uint32_t num_sections,
                              uint32_t rva) {
    for (uint32_t i = 0; i < num_sections; i++) {
        uint32_t vsize = sh[i].VirtualSize ? sh[i].VirtualSize : sh[i].SizeOfRawData;
        if (rva >= sh[i].VirtualAddress && rva - sh[i].VirtualAddress < vsize) {
            uint32_t delta = rva - sh[i].VirtualAddress;
            if (delta >= sh[i].SizeOfRawData) return 0; /* inside the .bss tail */
            return sh[i].PointerToRawData + delta;
        }
    }
    return 0;
}

/* Bounds-checked file pointer: `need` bytes starting at `off` must be
 * inside the buffer. */
static const uint8_t* file_ptr(const uint8_t* image, uint32_t size,
                               uint32_t off, uint32_t need) {
    if (off == 0) return 0;
    if ((uint64_t)off + need > size) return 0;
    return image + off;
}

static const uint8_t* rva_ptr(const uint8_t* image, uint32_t size,
                              const section_header_t* sh, uint32_t num_sections,
                              uint32_t rva, uint32_t need) {
    return file_ptr(image, size, rva_to_offset(sh, num_sections, rva), need);
}

/* A NUL-terminated string read out of the file, with the terminator
 * required to be inside the buffer. */
static const char* rva_str(const uint8_t* image, uint32_t size,
                           const section_header_t* sh, uint32_t num_sections,
                           uint32_t rva) {
    uint32_t off = rva_to_offset(sh, num_sections, rva);
    if (off == 0 || off >= size) return 0;
    for (uint32_t i = off; i < size; i++) {
        if (image[i] == '\0') return (const char*)(image + off);
    }
    return 0; /* runs off the end unterminated */
}

pe_load_result_t pe_inspect(const uint8_t* image, uint32_t size, pe_info_t* out) {
    if (out) kmemset(out, 0, sizeof(*out));

    pe_load_result_t err;
    const coff_header_t* coff = 0;
    const optional_header32_t* opt = pe_headers(image, size, &coff, &err);
    if (!opt) {
        if (out && coff && err != PE_ERR_NOT_PE) {
            out->valid = 1;
            out->machine = coff->Machine;
            out->characteristics = coff->Characteristics;
            out->is_pe32plus = (err == PE_ERR_PE32PLUS);
            out->is_dll = (coff->Characteristics & IMAGE_FILE_DLL) != 0;
        }
        return err;
    }

    const section_header_t* sh = pe_sections(image, size, coff);
    if (!sh) return PE_ERR_BAD_SECTION;
    uint32_t ns = coff->NumberOfSections;

    if (out) {
        out->valid = 1;
        out->machine = coff->Machine;
        out->characteristics = coff->Characteristics;
        out->is_dll = (coff->Characteristics & IMAGE_FILE_DLL) != 0;
        out->relocs_stripped = (coff->Characteristics & IMAGE_FILE_RELOCS_STRIPPED) != 0;
        out->has_relocs = pe_dir(opt, IMAGE_DIRECTORY_ENTRY_BASERELOC) != 0 &&
                          !out->relocs_stripped;
        out->is_dotnet = pe_dir(opt, IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR) != 0;
        out->subsystem = opt->Subsystem;
        out->image_base = opt->ImageBase;
        out->size_of_image = opt->SizeOfImage;
        out->entry_rva = opt->AddressOfEntryPoint;
        out->num_sections = ns;
        out->stack_reserve = opt->SizeOfStackReserve;

        /* Import counts for the summary line. A truncated import
         * directory isn't fatal to *inspection* - pe_visit_imports() and
         * pe_load() are the ones that report it as an error. */
        const data_directory_t* imp = pe_dir(opt, IMAGE_DIRECTORY_ENTRY_IMPORT);
        if (imp) {
            for (uint32_t i = 0;; i++) {
                const import_descriptor_t* d = (const import_descriptor_t*)
                    rva_ptr(image, size, sh, ns,
                            imp->VirtualAddress + i * (uint32_t)sizeof(import_descriptor_t),
                            sizeof(import_descriptor_t));
                if (!d || (d->Name == 0 && d->FirstThunk == 0)) break;
                out->import_dll_count++;

                uint32_t thunk_rva = d->OriginalFirstThunk ? d->OriginalFirstThunk
                                                           : d->FirstThunk;
                for (uint32_t j = 0;; j++) {
                    const uint32_t* t = (const uint32_t*)
                        rva_ptr(image, size, sh, ns, thunk_rva + j * 4, 4);
                    if (!t || *t == 0) break;
                    out->import_func_count++;
                }
            }
        }
    }

    if (coff->Machine != IMAGE_FILE_MACHINE_I386) return PE_ERR_BAD_MACHINE;
    if (coff->Characteristics & IMAGE_FILE_DLL) return PE_ERR_IS_DLL;
    if (pe_dir(opt, IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR)) return PE_ERR_DOTNET;
    if (opt->SizeOfImage == 0) return PE_ERR_BAD_HEADER;
    if ((opt->SizeOfImage + PAGE_SIZE - 1) / PAGE_SIZE > PE_MAX_IMAGE_PAGES) {
        return PE_ERR_TOO_LARGE;
    }
    if (opt->AddressOfEntryPoint >= opt->SizeOfImage) return PE_ERR_BAD_HEADER;

    /* Every section's raw data must be inside the file, and every section
     * must live inside SizeOfImage. Checking here means pe_load() can copy
     * without re-validating each one. */
    for (uint32_t i = 0; i < ns; i++) {
        if (sh[i].SizeOfRawData > 0 &&
            (uint64_t)sh[i].PointerToRawData + sh[i].SizeOfRawData > size) {
            return PE_ERR_BAD_SECTION;
        }
        uint32_t vsize = sh[i].VirtualSize ? sh[i].VirtualSize : sh[i].SizeOfRawData;
        if ((uint64_t)sh[i].VirtualAddress + vsize > opt->SizeOfImage) {
            return PE_ERR_BAD_SECTION;
        }
    }

    return PE_OK;
}

pe_load_result_t pe_visit_imports(const uint8_t* image, uint32_t size,
                                  pe_import_visitor_t visit, void* ctx) {
    pe_load_result_t err;
    const coff_header_t* coff = 0;
    const optional_header32_t* opt = pe_headers(image, size, &coff, &err);
    if (!opt) return err;

    const section_header_t* sh = pe_sections(image, size, coff);
    if (!sh) return PE_ERR_BAD_SECTION;
    uint32_t ns = coff->NumberOfSections;

    const data_directory_t* imp = pe_dir(opt, IMAGE_DIRECTORY_ENTRY_IMPORT);
    if (!imp) return PE_OK; /* importing nothing is perfectly legal */

    for (uint32_t i = 0;; i++) {
        const import_descriptor_t* d = (const import_descriptor_t*)
            rva_ptr(image, size, sh, ns,
                    imp->VirtualAddress + i * (uint32_t)sizeof(import_descriptor_t),
                    sizeof(import_descriptor_t));
        if (!d) return PE_ERR_BAD_IMPORTS;
        if (d->Name == 0 && d->FirstThunk == 0) break; /* terminator */

        const char* dll = rva_str(image, size, sh, ns, d->Name);
        if (!dll) return PE_ERR_BAD_IMPORTS;

        uint32_t thunk_rva = d->OriginalFirstThunk ? d->OriginalFirstThunk
                                                   : d->FirstThunk;
        for (uint32_t j = 0;; j++) {
            const uint32_t* t = (const uint32_t*)
                rva_ptr(image, size, sh, ns, thunk_rva + j * 4, 4);
            if (!t) return PE_ERR_BAD_IMPORTS;
            if (*t == 0) break;

            if (*t & IMAGE_ORDINAL_FLAG32) {
                visit(dll, 0, (uint16_t)(*t & 0xFFFF), ctx);
            } else {
                /* Hint/name entry: a 2-byte hint, then the name. */
                const char* name = rva_str(image, size, sh, ns, *t + 2);
                if (!name) return PE_ERR_BAD_IMPORTS;
                visit(dll, name, 0, ctx);
            }
        }
    }
    return PE_OK;
}

/* --- loading ----------------------------------------------------------- */

/* Bounds-checked pointer into the *mapped* image. Once the image is in
 * memory an RVA is just base+rva, but a corrupt table could still point
 * past the end of what we mapped, so every dereference goes through here. */
static void* img_ptr(const pe_image_t* img, uint32_t rva, uint32_t need) {
    if ((uint64_t)rva + need > img->size) return 0;
    return (void*)(img->base + rva);
}

static char* img_str(const pe_image_t* img, uint32_t rva) {
    if (rva >= img->size) return 0;
    char* s = (char*)(img->base + rva);
    uint32_t max = img->size - rva;
    for (uint32_t i = 0; i < max; i++) {
        if (s[i] == '\0') return s;
    }
    return 0;
}

static int region_is_available(uint32_t base, uint32_t bytes) {
    if (base < PE_MIN_LOAD_ADDR) return 0;
    if ((uint64_t)base + bytes > PE_MAX_LOAD_ADDR) return 0;
    return paging_region_conflict(base, base + bytes) == 0;
}

/* Picks where the image actually goes: its preferred ImageBase if that
 * range is free, otherwise a fresh slot in the relocation arena - which
 * is only possible if it brought a relocation table. */
static uint32_t choose_base(const pe_info_t* info, uint32_t bytes) {
    if (info->image_base != 0 && region_is_available(info->image_base, bytes)) {
        return info->image_base;
    }
    if (!info->has_relocs) return 0;

    uint32_t candidate = (reloc_arena_next + 0xFFFFu) & ~0xFFFFu; /* 64KB align */
    if ((uint64_t)candidate + bytes > PE_RELOC_ARENA_END) return 0;
    if (!region_is_available(candidate, bytes)) return 0;
    reloc_arena_next = candidate + bytes;
    return candidate;
}

static void unmap_range(pe_image_t* img, uint32_t upto_page) {
    for (uint32_t i = 0; i < upto_page; i++) {
        uint32_t va = img->base + i * PAGE_SIZE;
        uint32_t pte = paging_get_entry(va);
        paging_unmap_page(va);
        if (pte & PAGE_PRESENT) pmm_free_frame(pte & ~0xFFFu);
        if (img->saved_ptes) paging_set_entry(va, img->saved_ptes[i]);
    }
}

/* Applies the base relocation table for a load address that differs from
 * ImageBase. Everything is read out of the mapped image, so the table has
 * already moved with it. */
static pe_load_result_t apply_relocations(pe_image_t* img,
                                          const optional_header32_t* opt) {
    if (img->reloc_delta == 0) return PE_OK;

    const data_directory_t* dir = pe_dir(opt, IMAGE_DIRECTORY_ENTRY_BASERELOC);
    if (!dir) return PE_ERR_NO_ADDRESS_SPACE;

    uint32_t pos = dir->VirtualAddress;
    uint64_t end = (uint64_t)dir->VirtualAddress + dir->Size;
    if (end > img->size) end = img->size;

    while ((uint64_t)pos + sizeof(base_reloc_block_t) <= end) {
        base_reloc_block_t* blk =
            (base_reloc_block_t*)img_ptr(img, pos, sizeof(base_reloc_block_t));
        if (!blk) break;
        if (blk->SizeOfBlock <= sizeof(base_reloc_block_t)) break;

        uint32_t entries = (blk->SizeOfBlock - (uint32_t)sizeof(base_reloc_block_t)) / 2;
        uint16_t* ent = (uint16_t*)img_ptr(img,
                            pos + (uint32_t)sizeof(base_reloc_block_t), entries * 2);
        if (!ent) break;

        for (uint32_t i = 0; i < entries; i++) {
            uint32_t type = ent[i] >> 12;
            uint32_t rva = blk->VirtualAddress + (ent[i] & 0x0FFF);

            switch (type) {
                case IMAGE_REL_BASED_ABSOLUTE:
                    break; /* padding entry, by design a no-op */

                case IMAGE_REL_BASED_HIGHLOW: {
                    uint32_t* p = (uint32_t*)img_ptr(img, rva, 4);
                    if (p) *p += img->reloc_delta;
                    break;
                }
                case IMAGE_REL_BASED_HIGH: {
                    uint16_t* p = (uint16_t*)img_ptr(img, rva, 2);
                    if (p) *p = (uint16_t)(*p + (img->reloc_delta >> 16));
                    break;
                }
                case IMAGE_REL_BASED_LOW: {
                    uint16_t* p = (uint16_t*)img_ptr(img, rva, 2);
                    if (p) *p = (uint16_t)(*p + (img->reloc_delta & 0xFFFF));
                    break;
                }
                case IMAGE_REL_BASED_HIGHADJ: {
                    /* Consumes the *next* entry as a 16-bit addend. */
                    if (i + 1 >= entries) break;
                    uint16_t* p = (uint16_t*)img_ptr(img, rva, 2);
                    if (p) {
                        uint32_t v = ((uint32_t)*p << 16) + ent[i + 1];
                        v += img->reloc_delta;
                        *p = (uint16_t)((v + 0x8000) >> 16);
                    }
                    i++;
                    break;
                }
                default:
                    /* DIR64 and friends can't legally appear in a 32-bit
                     * image. Skipping an unknown type is safer than
                     * guessing and corrupting a word. */
                    break;
            }
        }
        pos += blk->SizeOfBlock;
    }
    return PE_OK;
}

static pe_load_result_t bind_imports(pe_image_t* img,
                                     const optional_header32_t* opt,
                                     const pe_loader_ops_t* ops) {
    const data_directory_t* dir = pe_dir(opt, IMAGE_DIRECTORY_ENTRY_IMPORT);
    if (!dir) return PE_OK;

    for (uint32_t i = 0;; i++) {
        import_descriptor_t* d = (import_descriptor_t*)
            img_ptr(img, dir->VirtualAddress + i * (uint32_t)sizeof(import_descriptor_t),
                    sizeof(import_descriptor_t));
        if (!d) return PE_ERR_BAD_IMPORTS;
        if (d->Name == 0 && d->FirstThunk == 0) break;

        const char* dll = img_str(img, d->Name);
        if (!dll || d->FirstThunk == 0) return PE_ERR_BAD_IMPORTS;

        /* Names come from OriginalFirstThunk when it's present. When it
         * isn't (bound imports, and some older linkers), FirstThunk still
         * holds the name entries until we overwrite them - so each entry
         * is read before that same slot is written, never after. */
        uint32_t name_rva = d->OriginalFirstThunk ? d->OriginalFirstThunk
                                                  : d->FirstThunk;

        for (uint32_t j = 0;; j++) {
            uint32_t* name_slot = (uint32_t*)img_ptr(img, name_rva + j * 4, 4);
            uint32_t* iat_slot = (uint32_t*)img_ptr(img, d->FirstThunk + j * 4, 4);
            if (!name_slot || !iat_slot) return PE_ERR_BAD_IMPORTS;
            uint32_t entry = *name_slot;
            if (entry == 0) break;

            const char* fname = 0;
            uint16_t ordinal = 0;
            if (entry & IMAGE_ORDINAL_FLAG32) {
                ordinal = (uint16_t)(entry & 0xFFFF);
            } else {
                fname = img_str(img, entry + 2); /* skip the 2-byte hint */
                if (!fname) return PE_ERR_BAD_IMPORTS;
            }

            uint32_t addr = 0;
            if (ops && ops->resolve_import) {
                addr = ops->resolve_import(dll, fname, ordinal, ops->ctx);
            }

            if (addr) {
                img->resolved++;
            } else {
                if (ops && ops->strict) return PE_ERR_MISSING_IMPORTS;
                img->unresolved++;
                if (ops && ops->missing_import) {
                    addr = ops->missing_import(dll, fname, ordinal, ops->ctx);
                }
            }
            *iat_slot = addr;
        }
    }
    return PE_OK;
}

/* Records the TLS callback array and zeroes the TLS index slot. The
 * callbacks are user-mode code, so they can't be run from here -
 * kernel/win32.c arranges for them to run in ring 3 ahead of the entry
 * point (see the startup trampoline it builds). */
static void setup_tls(pe_image_t* img, const optional_header32_t* opt) {
    const data_directory_t* dir = pe_dir(opt, IMAGE_DIRECTORY_ENTRY_TLS);
    if (!dir) return;

    tls_directory32_t* tls = (tls_directory32_t*)
        img_ptr(img, dir->VirtualAddress, sizeof(tls_directory32_t));
    if (!tls) return;

    /* AddressOfIndex and AddressOfCallBacks are absolute addresses that
     * the relocation pass has already fixed up, not RVAs - so they're
     * range-checked against the loaded image rather than against size. */
    if (tls->AddressOfIndex >= img->base &&
        tls->AddressOfIndex <= img->base + img->size - 4) {
        *(uint32_t*)tls->AddressOfIndex = 0; /* single-threaded: TLS slot 0 */
    }

    if (tls->AddressOfCallBacks >= img->base &&
        tls->AddressOfCallBacks <= img->base + img->size - 4) {
        /* An empty callback list is a single null pointer. */
        if (*(uint32_t*)tls->AddressOfCallBacks != 0) {
            img->tls_callbacks = tls->AddressOfCallBacks;
        }
    }
}

pe_load_result_t pe_load(const uint8_t* image, uint32_t size,
                         const pe_loader_ops_t* ops, pe_image_t* out) {
    kmemset(out, 0, sizeof(*out));

    pe_info_t info;
    pe_load_result_t r = pe_inspect(image, size, &info);
    if (r != PE_OK) return r;

    const coff_header_t* coff = 0;
    pe_load_result_t err;
    const optional_header32_t* opt = pe_headers(image, size, &coff, &err);
    if (!opt) return err;
    const section_header_t* sh = pe_sections(image, size, coff);
    if (!sh) return PE_ERR_BAD_SECTION;

    uint32_t bytes = (opt->SizeOfImage + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t pages = bytes / PAGE_SIZE;

    uint32_t base = choose_base(&info, bytes);
    if (!base) return PE_ERR_NO_ADDRESS_SPACE;

    out->base = base;
    out->size = bytes;
    out->pages = 0;
    out->reloc_delta = base - opt->ImageBase;
    out->subsystem = opt->Subsystem;
    out->stack_reserve = opt->SizeOfStackReserve;

    out->saved_ptes = (uint32_t*)kmalloc(pages * sizeof(uint32_t));
    if (!out->saved_ptes) return PE_ERR_OUT_OF_MEMORY;

    /* Map the whole SizeOfImage range in one piece, saving whatever was
     * mapped there beforehand (see paging.h). Sections are copied into
     * the zeroed result, which is also how the gaps between sections and
     * every .bss tail end up correctly zero-filled with no special case. */
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t va = base + i * PAGE_SIZE;
        out->saved_ptes[i] = paging_get_entry(va);

        uint32_t phys = pmm_alloc_frame();
        if (!phys) {
            unmap_range(out, i);
            kfree(out->saved_ptes);
            out->saved_ptes = 0;
            return PE_ERR_OUT_OF_MEMORY;
        }
        paging_map_page(va, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER);
        out->pages = i + 1;
    }

    kmemset((void*)base, 0, bytes);

    if (opt->SectionAlignment < PAGE_SIZE) {
        /* "Raw-mapped" image: a section alignment below a page means the
         * file is laid out 1:1 with memory, so copy it wholesale instead
         * of section by section. */
        uint32_t n = size < bytes ? size : bytes;
        kmemcpy((void*)base, image, n);
    } else {
        /* Headers first: Windows maps SizeOfHeaders bytes at the image
         * base, and programs really do read them back after calling
         * GetModuleHandle(NULL). */
        uint32_t hdr_bytes = opt->SizeOfHeaders;
        if (hdr_bytes > size) hdr_bytes = size;
        if (hdr_bytes > bytes) hdr_bytes = bytes;
        kmemcpy((void*)base, image, hdr_bytes);

        for (uint32_t i = 0; i < coff->NumberOfSections; i++) {
            uint32_t vsize = sh[i].VirtualSize ? sh[i].VirtualSize
                                               : sh[i].SizeOfRawData;
            if (vsize == 0 || sh[i].SizeOfRawData == 0) continue;

            uint32_t copy = sh[i].SizeOfRawData < vsize ? sh[i].SizeOfRawData : vsize;
            /* pe_inspect() already proved these bounds; belt and braces. */
            if ((uint64_t)sh[i].VirtualAddress + copy > bytes) continue;
            if ((uint64_t)sh[i].PointerToRawData + copy > size) continue;

            kmemcpy((void*)(base + sh[i].VirtualAddress),
                    image + sh[i].PointerToRawData, copy);
        }
    }

    r = apply_relocations(out, opt);
    if (r != PE_OK) { pe_unload(out); return r; }

    r = bind_imports(out, opt, ops);
    if (r != PE_OK) { pe_unload(out); return r; }

    setup_tls(out, opt);

    out->entry = base + opt->AddressOfEntryPoint;
    return PE_OK;
}

void pe_unload(pe_image_t* img) {
    if (!img) return;

    if (img->pages > 0) unmap_range(img, img->pages);
    if (img->saved_ptes) {
        kfree(img->saved_ptes);
        img->saved_ptes = 0;
    }
    img->pages = 0;
    img->base = 0;
    img->entry = 0;
}
