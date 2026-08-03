/* blockdev.c - the registry of block devices, and the bounds checks that
 * belong in one place rather than in every driver above it.
 *
 * Milestone 32. There is very little here on purpose: the interesting
 * code is the driver below (kernel/ata.c) and the filesystem above
 * (kernel/fat32.c), and this is the seam between them that lets the
 * second be tested without the first.
 */

#include "blockdev.h"
#include "posix.h"
#include "kstring.h"

static blockdev_t* devices[BLOCK_MAX_DEVICES];
static uint32_t ndevices = 0;
static uint32_t stat_reads = 0;
static uint32_t stat_writes = 0;

int blockdev_register(blockdev_t* dev) {
    if (!dev || ndevices >= BLOCK_MAX_DEVICES) return -1;
    dev->present = 1;
    devices[ndevices] = dev;
    return (int)ndevices++;
}

blockdev_t* blockdev_get(uint32_t index) {
    if (index >= ndevices) return 0;
    return devices[index];
}

blockdev_t* blockdev_find(const char* name) {
    if (!name) return 0;
    for (uint32_t i = 0; i < ndevices; i++) {
        if (devices[i] && kstrcmp(devices[i]->name, name) == 0) return devices[i];
    }
    return 0;
}

uint32_t blockdev_count(void) { return ndevices; }

void blockdev_reset(void) {
    for (uint32_t i = 0; i < BLOCK_MAX_DEVICES; i++) devices[i] = 0;
    ndevices = 0;
    stat_reads = stat_writes = 0;
}

/* The one check worth centralising: a read that runs off the end of the
 * device. A filesystem with a corrupt cluster chain will ask for sector
 * four billion sooner or later, and an ATA controller handed a nonsense
 * LBA does something device-specific rather than something safe. */
static int in_range(blockdev_t* dev, uint32_t lba, uint32_t count) {
    if (!dev || !dev->present || !count) return 0;
    if (lba >= dev->sectors) return 0;
    if (count > dev->sectors - lba) return 0;
    return 1;
}

int blockdev_read(blockdev_t* dev, uint32_t lba, uint32_t count, void* buf) {
    if (!in_range(dev, lba, count) || !buf || !dev->read) return -EIO;
    int r = dev->read(dev, lba, count, buf);
    if (r == 0) stat_reads += count;
    return r;
}

int blockdev_write(blockdev_t* dev, uint32_t lba, uint32_t count,
                   const void* buf) {
    if (!in_range(dev, lba, count) || !buf || !dev->write) return -EIO;
    int r = dev->write(dev, lba, count, buf);
    if (r == 0) stat_writes += count;
    return r;
}

void blockdev_stats(uint32_t* reads, uint32_t* writes) {
    if (reads) *reads = stat_reads;
    if (writes) *writes = stat_writes;
}
