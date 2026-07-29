#ifndef INITRD_H
#define INITRD_H

#include <stdint.h>

/* Parses Novaris's custom initrd archive format (packed by
 * userland/mkinitrd.py - see that file for the exact byte layout) and
 * populates vfs_root with a flat root directory, one node per file
 * (no subdirectories yet). `addr` must already be mapped (identity or
 * otherwise) and readable before calling this. */
void initrd_init(uint32_t addr, uint32_t size);

#endif
