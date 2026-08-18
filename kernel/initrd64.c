/* initrd64.c - reading files out of the archive GRUB loaded for us. */

#include "initrd64.h"
#include "kstring.h"
#include "pmm64.h"

#include "paging64.h"

#define INITRD_MAGIC 0x324C5453u         /* 'STL2' */
#define FILE_MAGIC   0xBEEFCAFEu
#define NAME_MAX     124

typedef struct {
    uint32_t magic;
    uint32_t nfiles;
} __attribute__((packed)) initrd_header_t;

typedef struct {
    uint32_t magic;
    char     name[NAME_MAX];
    uint32_t offset;
    uint32_t length;
} __attribute__((packed)) initrd_file_t;

static const uint8_t*        archive;    /* through the direct map */
static const initrd_file_t*  files;
static uint64_t              nfiles;
static uint64_t              archive_len;

/* Where GRUB put the archive, or 0,0 if it did not load one. Reads the
 * multiboot info through the boot identity map, which is still live this
 * early - so this works no matter how high the module itself sits. */
static void module_range(const multiboot_info_t* mbi,
                         uint64_t* start, uint64_t* end) {
    const multiboot_module_t* mods;

    *start = *end = 0;
    if (!(mbi->flags & MULTIBOOT_INFO_MODS) || mbi->mods_count == 0) return;

    mods = (const multiboot_module_t*)(uint64_t)mbi->mods_addr;
    *start = mods[0].mod_start;
    *end   = mods[0].mod_end;
}

/* Split out of initrd64_init because of an ordering problem that only
 * appeared once the machine had more than a gigabyte of RAM (Milestone
 * 66): the archive has to be reserved before anything allocates, but it
 * can only be *read* through the direct map, and building the direct map
 * allocates page tables. So: reserve here, build the map, then init.
 *
 * GRUB put the archive in ordinary RAM, which the frame allocator has
 * just been told is free. Without this it gets handed out as a page like
 * any other and the initrd is quietly overwritten by whatever allocates
 * next - which presents as a corrupt archive much later, nowhere near
 * the cause. */
void initrd64_reserve(const multiboot_info_t* mbi) {
    uint64_t start, end;
    module_range(mbi, &start, &end);
    if (end > start) pmm64_reserve_region(start, end);
}

int initrd64_init(const multiboot_info_t* mbi) {
    const initrd_header_t* hdr;
    uint64_t start, end;

    archive = 0;
    files = 0;
    nfiles = 0;
    archive_len = 0;

    module_range(mbi, &start, &end);
    if (end <= start) return INITRD64_NO_MODULE;

    /* Through the direct map, so that the archive stays readable once a
     * process address space is loaded and the identity map is gone - and
     * so that where GRUB chose to put it stops mattering. With 2GB of
     * RAM it lands above the old 1GB window, and before this milestone
     * that was the difference between a filesystem and none. */
    archive     = (const uint8_t*)phys64_to_virt(start);
    archive_len = end - start;

    if (archive_len < sizeof(initrd_header_t)) return INITRD64_BAD_MAGIC;

    hdr = (const initrd_header_t*)archive;
    /* Milestone 35 changed this magic from 'STLR' to 'STL2' precisely so
     * that a stale image is refused rather than misread. */
    if (hdr->magic != INITRD_MAGIC) return INITRD64_BAD_MAGIC;

    nfiles = hdr->nfiles;
    if (sizeof(initrd_header_t) + nfiles * sizeof(initrd_file_t)
            > archive_len) {
        nfiles = 0;
        return INITRD64_BAD_MAGIC;
    }

    files = (const initrd_file_t*)(archive + sizeof(initrd_header_t));
    return INITRD64_OK;
}

int initrd64_open(const char* name, const void** data, uint64_t* length) {
    if (!files) return INITRD64_NO_MODULE;

    for (uint64_t i = 0; i < nfiles; i++) {
        if (files[i].magic != FILE_MAGIC) continue;
        /* kstrncmp rather than kstrcmp: the name field is fixed-width and
         * only NUL-terminated when the name is shorter than it. */
        if (kstrncmp(files[i].name, name, NAME_MAX) != 0) continue;

        /* An offset or length past the end of the archive would hand the
         * caller a pointer into whatever follows it in memory. */
        if ((uint64_t)files[i].offset + files[i].length > archive_len)
            return INITRD64_BAD_MAGIC;

        if (data)   *data   = archive + files[i].offset;
        if (length) *length = files[i].length;
        return INITRD64_OK;
    }
    return INITRD64_NO_MODULE;
}

uint64_t initrd64_file_count(void) { return nfiles; }
uint64_t initrd64_size(void)       { return archive_len; }

const char* initrd64_name(uint64_t index) {
    if (!files || index >= nfiles) return 0;
    return files[index].name;
}
