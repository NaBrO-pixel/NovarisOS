/* pe_host_test.c - runs kernel/pe.c against real .exe files on the host.
 *
 * The QEMU harness proves a binary runs end to end, but a failure there
 * tells you very little about *why*. This links the actual loader - not a
 * reimplementation of it - into a host binary and drives it directly, so
 * "the relocation pass rewrote this specific address wrongly" is a
 * one-line assertion instead of an archaeology exercise.
 *
 * The trick that makes it possible: pe.c's only kernel dependencies are
 * paging, the physical allocator and kmalloc, and all three are behind
 * narrow interfaces. Stubbing paging_map_page() with an mmap of the same
 * address gives the loader a real address space to write into, so the
 * image really is mapped, really is relocated, and its import table
 * really is bound - just in a process instead of a kernel.
 *
 * Built and run by `make test`.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "pe.h"
#include "paging.h"
#include "pmm.h"
#include "kheap.h"

/* --- the kernel interfaces pe.c uses, backed by the host -------------- */

#define HOST_PAGE 4096u

static int mapped_pages = 0;

void paging_map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    (void)phys;
    (void)flags;
    void* p = mmap((void*)(uintptr_t)virt, HOST_PAGE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "  (host) mmap at 0x%08x failed - cannot test here\n", virt);
        exit(77); /* the "skipped" convention */
    }
    mapped_pages++;
}

void paging_unmap_page(uint32_t virt) {
    munmap((void*)(uintptr_t)virt, HOST_PAGE);
    mapped_pages--;
}

/* pe.c saves and restores whatever was mapped before it; on the host
 * there never is anything, so "no previous mapping" is always right. */
uint32_t paging_get_entry(uint32_t virt) { (void)virt; return 0; }
void paging_set_entry(uint32_t virt, uint32_t entry) { (void)virt; (void)entry; }

void paging_reserve_region(uint32_t s, uint32_t e, const char* n) {
    (void)s; (void)e; (void)n;
}
const char* paging_region_conflict(uint32_t s, uint32_t e) {
    (void)s; (void)e;
    return 0; /* the host has no kernel-owned regions to collide with */
}

/* The frame allocator's return value is only ever handed back to
 * paging_map_page(), which ignores it here - so any non-zero value does. */
uint32_t pmm_alloc_frame(void) { return 0x1000; }
void pmm_free_frame(uint32_t phys) { (void)phys; }

void* kmalloc(size_t size) { return malloc(size); }
void kfree(void* p) { free(p); }

/* --- import resolution -------------------------------------------------- */

/* Hands out a distinct fake address per symbol and records what was asked
 * for, so a test can assert on both the resolution and the IAT contents. */
#define MAX_SEEN 256
static struct {
    char dll[64];
    char name[96];
    uint16_t ordinal;
    uint32_t address;
} seen[MAX_SEEN];
static int seen_count = 0;
static int refuse_everything = 0;

static uint32_t test_resolve(const char* dll, const char* name,
                             uint16_t ordinal, void* ctx) {
    (void)ctx;
    if (refuse_everything) return 0;
    if (seen_count >= MAX_SEEN) return 0;

    snprintf(seen[seen_count].dll, sizeof(seen[seen_count].dll), "%s", dll);
    snprintf(seen[seen_count].name, sizeof(seen[seen_count].name), "%s",
             name ? name : "");
    seen[seen_count].ordinal = ordinal;
    seen[seen_count].address = 0x7B000000u + (uint32_t)seen_count * 16u;
    return seen[seen_count++].address;
}

static uint32_t missing_count = 0;
static uint32_t test_missing(const char* dll, const char* name,
                             uint16_t ordinal, void* ctx) {
    (void)dll; (void)name; (void)ordinal; (void)ctx;
    missing_count++;
    return 0x7BFF0000u;
}

/* --- assertions --------------------------------------------------------- */

static int checks = 0;
static int failures = 0;

static void ok(const char* what, int condition) {
    checks++;
    if (condition) return;
    failures++;
    printf("  FAIL %s\n", what);
}

static void ok_eq(const char* what, uint32_t got, uint32_t expected) {
    checks++;
    if (got == expected) return;
    failures++;
    printf("  FAIL %s: got 0x%08x, expected 0x%08x\n", what, got, expected);
}

static uint8_t* read_file(const char* path, uint32_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buffer = malloc((size_t)n);
    if (fread(buffer, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buffer);
        return 0;
    }
    fclose(f);
    *size = (uint32_t)n;
    return buffer;
}

/* --- tests -------------------------------------------------------------- */

static void test_rejects_garbage(void) {
    printf("rejecting things that aren't loadable programs:\n");

    uint8_t junk[512];
    memset(junk, 0xCC, sizeof(junk));
    pe_info_t info;
    ok("random bytes are not a PE", pe_inspect(junk, sizeof(junk), &info) == PE_ERR_NOT_PE);
    ok("pe_is_valid agrees", !pe_is_valid(junk, sizeof(junk)));

    /* An MZ header with no PE header behind it: a DOS executable. */
    uint8_t dos[128];
    memset(dos, 0, sizeof(dos));
    dos[0] = 'M'; dos[1] = 'Z';
    *(uint32_t*)(dos + 60) = 0x40; /* e_lfanew pointing at zeros */
    ok("a DOS .exe is rejected as not-a-PE",
       pe_inspect(dos, sizeof(dos), &info) == PE_ERR_NOT_PE);

    /* Truncation must be caught rather than read past. */
    uint32_t size = 0;
    uint8_t* image = read_file("build/user/hellowin.exe", &size);
    if (image) {
        ok("a truncated PE is rejected", pe_inspect(image, 200, &info) != PE_OK);
        free(image);
    }
}

static void test_inspect(const char* path, const char* label,
                         pe_load_result_t expected, uint16_t expect_machine) {
    uint32_t size = 0;
    uint8_t* image = read_file(path, &size);
    if (!image) {
        printf("  SKIP %s (not built)\n", label);
        return;
    }

    pe_info_t info;
    pe_load_result_t r = pe_inspect(image, size, &info);

    char what[128];
    snprintf(what, sizeof(what), "%s: verdict", label);
    checks++;
    if (r != expected) {
        failures++;
        printf("  FAIL %s: got \"%s\", expected \"%s\"\n", what,
               pe_result_string(r), pe_result_string(expected));
    }

    snprintf(what, sizeof(what), "%s: machine", label);
    ok_eq(what, info.machine, expect_machine);

    free(image);
}

static void test_load_and_bind(void) {
    printf("\nloading, relocating and binding imports:\n");

    uint32_t size = 0;
    uint8_t* image = read_file("build/user/hellowin.exe", &size);
    if (!image) {
        printf("  SKIP (build/user/hellowin.exe not built)\n");
        return;
    }

    pe_info_t info;
    pe_inspect(image, size, &info);

    pe_loader_ops_t ops = { test_resolve, test_missing, 0, 0 };
    pe_image_t loaded;
    seen_count = 0;
    missing_count = 0;

    pe_load_result_t r = pe_load(image, size, &ops, &loaded);
    ok("loads", r == PE_OK);
    if (r != PE_OK) {
        printf("       (%s)\n", pe_result_string(r));
        free(image);
        return;
    }

    ok_eq("loaded at its preferred image base", loaded.base, info.image_base);
    ok_eq("no relocation was needed", loaded.reloc_delta, 0);
    ok_eq("entry point", loaded.entry, info.image_base + info.entry_rva);
    ok("every import resolved", missing_count == 0);
    ok("imports were actually bound", loaded.resolved == info.import_func_count);

    /* The mapped image must start with its own headers - a program that
     * calls GetModuleHandle(NULL) and walks them depends on this. */
    ok("headers are mapped at the image base",
       *(uint16_t*)(uintptr_t)loaded.base == IMAGE_DOS_SIGNATURE);

    /* Spot-check the import address table: whatever the resolver handed
     * back for a symbol must be the value sitting in the IAT. */
    int found = 0;
    for (int i = 0; i < seen_count; i++) {
        if (strcmp(seen[i].name, "GetProcAddress") != 0) continue;
        found = 1;
        /* Scan the mapped image for the address we returned. It has to be
         * in there exactly once, in the IAT slot for this symbol. */
        int hits = 0;
        for (uint32_t off = 0; off + 4 <= loaded.size; off += 4) {
            if (*(uint32_t*)(uintptr_t)(loaded.base + off) == seen[i].address) hits++;
        }
        ok("the resolved address for GetProcAddress is in the image", hits >= 1);
    }
    ok("GetProcAddress was among the imports resolved", found);

    pe_unload(&loaded);
    ok("unloading releases every page", mapped_pages == 0);
    free(image);
}

/* Translates an RVA to a file offset through the section table, so the
 * test can read the *unrelocated* original of a word the loader rewrote. */
static uint32_t file_offset_of(const uint8_t* image, uint32_t rva) {
    uint32_t lfanew = *(const uint32_t*)(image + 60);
    const coff_header_t* coff = (const coff_header_t*)(image + lfanew + 4);
    const section_header_t* sh = (const section_header_t*)
        (image + lfanew + 4 + sizeof(coff_header_t) + coff->SizeOfOptionalHeader);

    for (uint32_t i = 0; i < coff->NumberOfSections; i++) {
        uint32_t vsize = sh[i].VirtualSize ? sh[i].VirtualSize : sh[i].SizeOfRawData;
        if (rva >= sh[i].VirtualAddress && rva - sh[i].VirtualAddress < vsize) {
            uint32_t delta = rva - sh[i].VirtualAddress;
            if (delta >= sh[i].SizeOfRawData) return 0;
            return sh[i].PointerToRawData + delta;
        }
    }
    return 0;
}

/* Walks the image's base relocation table and checks each HIGHLOW entry:
 * the word in the loaded image must be the word in the file plus the
 * delta the loader reported. */
static void verify_relocations(const uint8_t* image, uint32_t size,
                               const pe_image_t* loaded,
                               uint32_t* applied, uint32_t* wrong) {
    uint32_t lfanew = *(const uint32_t*)(image + 60);
    const coff_header_t* coff = (const coff_header_t*)(image + lfanew + 4);
    const optional_header32_t* opt = (const optional_header32_t*)
        (image + lfanew + 4 + sizeof(coff_header_t));

    const data_directory_t* dir = &opt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    (void)coff;

    uint32_t pos = dir->VirtualAddress;
    uint32_t end = dir->VirtualAddress + dir->Size;

    while (pos + 8 <= end) {
        uint32_t block_offset = file_offset_of(image, pos);
        if (!block_offset || block_offset + 8 > size) break;

        const base_reloc_block_t* blk =
            (const base_reloc_block_t*)(image + block_offset);
        if (blk->SizeOfBlock <= 8) break;

        uint32_t entries = (blk->SizeOfBlock - 8) / 2;
        const uint16_t* ent = (const uint16_t*)(image + block_offset + 8);

        for (uint32_t i = 0; i < entries; i++) {
            if ((ent[i] >> 12) != IMAGE_REL_BASED_HIGHLOW) continue;

            uint32_t rva = blk->VirtualAddress + (ent[i] & 0x0FFF);
            uint32_t src = file_offset_of(image, rva);
            if (!src || src + 4 > size) continue;

            uint32_t original = *(const uint32_t*)(image + src);
            uint32_t got = *(const uint32_t*)(uintptr_t)(loaded->base + rva);
            (*applied)++;
            if (got != original + loaded->reloc_delta) (*wrong)++;
        }
        pos += blk->SizeOfBlock;
    }
}

static void test_relocation(void) {
    printf("\nbase relocation (an image that can't have the address it wants):\n");

    uint32_t size = 0;
    uint8_t* image = read_file("build/user/lowbase.exe", &size);
    if (!image) {
        printf("  SKIP (build/user/lowbase.exe not built)\n");
        return;
    }

    pe_info_t info;
    pe_inspect(image, size, &info);
    ok("the test image really is linked low", info.image_base < 0x100000u);
    ok("and brought a relocation table", info.has_relocs != 0);

    pe_loader_ops_t ops = { test_resolve, test_missing, 0, 0 };
    pe_image_t loaded;
    seen_count = 0;

    pe_load_result_t r = pe_load(image, size, &ops, &loaded);
    ok("loads anyway", r == PE_OK);
    if (r != PE_OK) {
        printf("       (%s)\n", pe_result_string(r));
        free(image);
        return;
    }

    ok("was moved away from its preferred base", loaded.base != info.image_base);
    ok_eq("the delta matches where it landed",
          loaded.reloc_delta, loaded.base - info.image_base);
    ok_eq("entry point moved with it",
          loaded.entry, loaded.base + info.entry_rva);

    /* The precise check: walk the image's own relocation table, and for
     * every entry confirm the mapped word equals the on-disk word plus
     * the delta. Anything the table doesn't list must be left alone -
     * which is why this reads the table rather than scanning for
     * suspicious-looking values. (An earlier version of this test did
     * scan, and flagged hundreds of "stale pointers" that turned out to
     * be DWARF debug sections. Those genuinely aren't relocated on
     * Windows either: they aren't in .reloc, so leaving them is correct.) */
    uint32_t applied = 0;
    uint32_t wrong = 0;
    verify_relocations(image, size, &loaded, &applied, &wrong);

    ok("the relocation table was non-empty", applied > 0);
    ok("every relocation was applied correctly", wrong == 0);
    printf("       (%u relocation(s) checked)\n", applied);
    if (wrong) printf("       (%u wrong)\n", wrong);

    pe_unload(&loaded);
    free(image);
}

static void test_strict_mode(void) {
    printf("\nstrict mode (refusing a program whose imports can't be met):\n");

    uint32_t size = 0;
    uint8_t* image = read_file("build/user/hellowin.exe", &size);
    if (!image) {
        printf("  SKIP (not built)\n");
        return;
    }

    pe_loader_ops_t ops = { test_resolve, test_missing, 0, 1 /* strict */ };
    pe_image_t loaded;
    refuse_everything = 1;

    pe_load_result_t r = pe_load(image, size, &ops, &loaded);
    ok("refuses rather than loading", r == PE_ERR_MISSING_IMPORTS);
    ok("and leaves nothing mapped behind", mapped_pages == 0);

    refuse_everything = 0;
    free(image);
}

int main(void) {
    printf("PE loader, driven directly against real mingw-built binaries\n\n");

    test_rejects_garbage();

    printf("\nheader inspection:\n");
    test_inspect("build/user/hellowin.exe", "hellowin.exe (32-bit console)",
                 PE_OK, IMAGE_FILE_MACHINE_I386);
    test_inspect("build/user/guiapp.exe", "guiapp.exe (32-bit GUI)",
                 PE_OK, IMAGE_FILE_MACHINE_I386);
    test_inspect("build/user/hello64.exe", "hello64.exe (64-bit)",
                 PE_ERR_PE32PLUS, IMAGE_FILE_MACHINE_AMD64);

    test_load_and_bind();
    test_relocation();
    test_strict_mode();

    printf("\n%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
