/* initrd64.c - reading files out of the archive GRUB loaded for us. */

#include "initrd64.h"
#include "kstring.h"
#include "pmm64.h"

#define KERNEL_VMA   0xFFFFFFFF80000000ULL
#define PHYS_WINDOW  0x40000000ULL       /* what boot64.s maps at KERNEL_VMA */

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

static const uint8_t*        archive;    /* through the KERNEL_VMA window */
static const initrd_file_t*  files;
static uint64_t              nfiles;
static uint64_t              archive_len;

int initrd64_init(const multiboot_info_t* mbi) {
    const multiboot_module_t* mods;
    const initrd_header_t* hdr;
    uint64_t start, end;

    archive = 0;
    files = 0;
    nfiles = 0;
    archive_len = 0;

    if (!(mbi->flags & MULTIBOOT_INFO_MODS) || mbi->mods_count == 0)
        return INITRD64_NO_MODULE;

    /* The identity mapping is still live here, so a physical address is
     * a usable pointer - which is the whole reason this runs early. */
    mods = (const multiboot_module_t*)(uint64_t)mbi->mods_addr;
    start = mods[0].mod_start;
    end   = mods[0].mod_end;

    if (end <= start) return INITRD64_NO_MODULE;

    /* Everything after this point addresses the archive through the
     * higher half, so that it stays readable once a process address
     * space is loaded and the identity map is gone. That window covers
     * the first gigabyte only. */
    if (end > PHYS_WINDOW) return INITRD64_TOO_HIGH;

    /* GRUB put the archive in ordinary RAM, which the frame allocator
     * has just been told is free. Without this it gets handed out as a
     * page like any other and the initrd is quietly overwritten by
     * whatever allocates next - which presents as a corrupt archive
     * much later, nowhere near the cause. */
    pmm64_reserve_region(start, end);

    archive     = (const uint8_t*)(KERNEL_VMA + start);
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
