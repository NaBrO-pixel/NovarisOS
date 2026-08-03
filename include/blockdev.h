#ifndef BLOCKDEV_H
#define BLOCKDEV_H

#include <stdint.h>

/* A block device: something that stores 512-byte sectors and can be asked
 * for them by number.
 *
 * Milestone 32. This exists as an interface rather than as "the functions
 * in ata.c" for one concrete reason: the FAT32 driver above it is the
 * most intricate code in this tree, and testing it against real hardware
 * means booting a machine. With a block device behind a vtable, the host
 * test in tests/fat32_host_test.c hands the *same* driver a device backed
 * by a file on the host, and every path through it - cluster chains, long
 * names, growing a directory, freeing a chain - is exercised where a
 * failure prints a line number instead of a triple fault.
 *
 * 512 is hard-coded on purpose. Every device this kernel will meet uses
 * it, FAT32 volumes made by anything use it, and a variable that only
 * ever holds one value is a place for a bug to hide. */
#define BLOCK_SECTOR_SIZE 512u

/* How many devices can be registered. Four is the whole of legacy IDE:
 * primary and secondary bus, master and slave on each. */
#define BLOCK_MAX_DEVICES 4

typedef struct blockdev {
    /* Both return 0 on success and a negative errno on failure. `count`
     * is in sectors, and a short transfer is a failure rather than a
     * partial success - a filesystem cannot do anything useful with half
     * a cluster. */
    int (*read)(struct blockdev* dev, uint32_t lba, uint32_t count, void* buf);
    int (*write)(struct blockdev* dev, uint32_t lba, uint32_t count,
                 const void* buf);

    uint32_t sectors;      /* device size, in 512-byte sectors */
    void*    impl;         /* the driver's own handle for this device */
    char     name[16];     /* "ata0", "ata1", ... - for `mount` and `df` */
    int      present;
} blockdev_t;

/* The registry. Drivers add devices at boot; the filesystem layer and the
 * shell look them up by index or by name. */
int          blockdev_register(blockdev_t* dev);
blockdev_t*  blockdev_get(uint32_t index);
blockdev_t*  blockdev_find(const char* name);
uint32_t     blockdev_count(void);
void         blockdev_reset(void);

/* Convenience wrappers that check the device is there and the range is
 * inside it, so no driver above this line has to. */
int blockdev_read(blockdev_t* dev, uint32_t lba, uint32_t count, void* buf);
int blockdev_write(blockdev_t* dev, uint32_t lba, uint32_t count,
                   const void* buf);

/* Counters, for the shell's `df`. Reads and writes are counted in
 * sectors, which is the unit that costs time. */
void blockdev_stats(uint32_t* reads, uint32_t* writes);

#endif
