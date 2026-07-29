#include "initrd.h"
#include "vfs.h"

/* Archive format (see userland/mkinitrd.py, the host-side packer that
 * produces this):
 *
 *   struct initrd_header  { uint32_t magic; uint32_t nfiles; }
 *   struct initrd_file_header[nfiles] {
 *       uint32_t magic; char name[60]; uint32_t offset; uint32_t length;
 *   }
 *   <raw file data, back to back, at the offsets above, relative to the
 *    very start of the archive - not the end of the header table>
 */
#define INITRD_MAGIC      0x53544C52u /* 'STLR' */
#define INITRD_FILE_MAGIC 0xBEEFCAFEu
#define INITRD_MAX_FILES  32
#define INITRD_NAME_MAX   60

typedef struct initrd_header {
    uint32_t magic;
    uint32_t nfiles;
} __attribute__((packed)) initrd_header_t;

typedef struct initrd_file_header {
    uint32_t magic;
    char name[INITRD_NAME_MAX];
    uint32_t offset;
    uint32_t length;
} __attribute__((packed)) initrd_file_header_t;

static vfs_node_t root_node;
static vfs_node_t file_nodes[INITRD_MAX_FILES];
static uint32_t nfiles = 0;
static uint32_t initrd_base = 0;

static uint32_t initrd_read(vfs_node_t* node, uint32_t offset, uint32_t size,
                             uint8_t* buffer) {
    if (offset >= node->length) return 0;
    if (offset + size > node->length) size = node->length - offset;

    const uint8_t* src = (const uint8_t*)(initrd_base + node->impl + offset);
    for (uint32_t i = 0; i < size; i++) buffer[i] = src[i];
    return size;
}

static vfs_node_t* initrd_readdir(vfs_node_t* node, uint32_t index) {
    (void)node;
    if (index >= nfiles) return 0;
    return &file_nodes[index];
}

static int names_equal(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static vfs_node_t* initrd_finddir(vfs_node_t* node, const char* name) {
    (void)node;
    for (uint32_t i = 0; i < nfiles; i++) {
        if (names_equal(file_nodes[i].name, name)) return &file_nodes[i];
    }
    return 0;
}

void initrd_init(uint32_t addr, uint32_t size) {
    (void)size;
    initrd_base = addr;
    nfiles = 0;

    initrd_header_t* hdr = (initrd_header_t*)addr;
    if (hdr->magic == INITRD_MAGIC) {
        nfiles = hdr->nfiles;
        if (nfiles > INITRD_MAX_FILES) nfiles = INITRD_MAX_FILES;

        initrd_file_header_t* entries =
            (initrd_file_header_t*)(addr + sizeof(initrd_header_t));

        for (uint32_t i = 0; i < nfiles; i++) {
            vfs_node_t* n = &file_nodes[i];

            uint32_t len = 0;
            while (len < INITRD_NAME_MAX && entries[i].name[len] != '\0') {
                n->name[len] = entries[i].name[len];
                len++;
            }
            if (len >= VFS_NAME_MAX) len = VFS_NAME_MAX - 1;
            n->name[len] = '\0';

            n->flags = VFS_FILE;
            n->length = entries[i].length;
            n->impl = entries[i].offset;
            n->read = initrd_read;
            n->readdir = 0;
            n->finddir = 0;
        }
    }
    /* If the magic didn't match, we still mount an (empty) root rather
     * than leaving vfs_root NULL, so `ls` reports "no files" instead of
     * "no filesystem" - a missing/corrupt initrd is a build problem, not
     * something the shell should have to special-case. */

    root_node.name[0] = '/';
    root_node.name[1] = '\0';
    root_node.flags = VFS_DIRECTORY;
    root_node.length = 0;
    root_node.impl = 0;
    root_node.read = 0;
    root_node.readdir = initrd_readdir;
    root_node.finddir = initrd_finddir;

    vfs_root = &root_node;
}
