#ifndef INITRD64_H
#define INITRD64_H

#include <stdint.h>
#include "multiboot.h"

/* The initrd, read-only.
 *
 * This is the 64-bit tree's first filesystem, and it is deliberately the
 * smallest thing that deserves the name: GRUB loads one archive as a
 * Multiboot module and this reads files out of it. No disk driver, no
 * FAT32, no writing. The 32-bit tree's ramfs.c is 877 lines because it
 * is writable and backed by a real disk; none of that is needed to
 * answer the question this unblocks, which is "can a program load a
 * library from a file at run time rather than having it compiled in".
 *
 * The archive format is the one userland/mkinitrd.py already writes, so
 * the same tooling serves both trees. Layout, all little-endian:
 *
 *   { u32 magic 'STL2'; u32 nfiles; }
 *   nfiles x { u32 magic; char name[124]; u32 offset; u32 length; }
 *   raw file bytes, offsets relative to the start of the archive
 *
 * `name` may contain '/', so the archive describes a tree. This reader
 * does not build directories - it matches whole paths - because nothing
 * here lists a directory yet.
 */

#define INITRD64_OK        0
#define INITRD64_NO_MODULE -1
#define INITRD64_BAD_MAGIC -2
/* No longer returned. Kept so the number is never reused: until
 * Milestone 66 an archive above 1GB was unreadable, and a stale caller
 * comparing against -3 should not silently start matching something
 * else. */
#define INITRD64_TOO_HIGH  -3

/* Reserves the archive's frames so the allocator cannot hand them out.
 * Must run immediately after pmm64_init, before anything allocates, and
 * before initrd64_init - which needs the direct map, which needs the
 * allocator. Reads the Multiboot info through the boot identity map. */
void initrd64_reserve(const multiboot_info_t* mbi);

/* Must run after paging64_physmap_init: the archive is read through the
 * direct map, and may sit anywhere in RAM. */
int initrd64_init(const multiboot_info_t* mbi);

/* Exact path match. `data` comes back pointing into the archive itself,
 * which lives in the kernel's half of every address space, so it stays
 * readable from inside a process. */
int initrd64_open(const char* name, const void** data, uint64_t* length);

uint64_t    initrd64_file_count(void);
const char* initrd64_name(uint64_t index);
uint64_t    initrd64_size(void);

#endif
