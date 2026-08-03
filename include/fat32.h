#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include "blockdev.h"
#include "vfs.h"

/* FAT32 - Milestone 32's filesystem.
 *
 * Chosen for the reason Milestone 6 first considered it: it is the only
 * filesystem that is both simple enough to write from scratch and
 * universally readable, so the disk image this project builds can be
 * inspected, edited and repaired from any other machine. That matters
 * more than it sounds - a Wine prefix is hundreds of files, and being
 * able to `mount -o loop` the image on the host and look is the
 * difference between debugging a filesystem and debugging a filesystem
 * through a serial port.
 *
 * What it supports: reading and writing files of any size the volume
 * holds, growing and truncating them, creating and removing files and
 * directories, renaming within the volume, long filenames in both
 * directions, and symbolic links.
 *
 * Symbolic links are not a FAT feature; FAT has no link type at all. They
 * are stored the way every other system that has had to put a Unix
 * symlink on a DOS filesystem stores one - a small file with the SYSTEM
 * attribute set and a magic header, here "NVSYMLNK" followed by the
 * target path. Nothing else on the volume sets SYSTEM, so the check costs
 * one sector read for the handful of files that do, and a link written
 * here reads back as an ordinary small file anywhere else rather than as
 * corruption.
 */

/* Mounts the FAT32 volume on `dev` onto the directory `dir`, which must
 * be an empty ramfs directory. `dir` *becomes* the volume's root: it
 * keeps its name and its place in the tree and takes on the filesystem's
 * behaviour, which is why no indirection is needed anywhere else and
 * vfs_path_of() keeps working unchanged.
 *
 * If the device has an MBR partition table the first FAT32 partition is
 * used; a device with a boot sector but no partition table is mounted
 * whole, which is what a "superfloppy" image is and what the build
 * produces.
 *
 * Returns 0, or a negative errno. */
int fat32_mount(vfs_node_t* dir, blockdev_t* dev);

/* Mounts the first disk carrying a FAT32 volume onto /disk. Called at
 * boot; a machine with no disk is not an error and this quietly does
 * nothing. Returns 0 on a successful mount. */
int fat32_mount_auto(void);

/* Whether anything is mounted, and where it came from. */
int fat32_mounted(void);
const char* fat32_device_name(void);
const char* fat32_volume_label(void);

/* Space accounting, for the shell's `df`. Sizes in clusters; multiply by
 * fat32_cluster_bytes(). `free` is counted by scanning the FAT the first
 * time it is asked for and then kept up to date. */
void fat32_space(uint32_t* total_clusters, uint32_t* free_clusters);
uint32_t fat32_cluster_bytes(void);

/* Pushes the FAT sector cache and the FSInfo block out to the disk.
 * Every metadata change is already written through, so this only matters
 * for the one sector the allocator keeps hot. */
int fat32_sync(void);

/* Unmounts: flushes, drops every cached node, and returns the mount point
 * to being an empty ramfs directory. Returns 0, or a negative errno if
 * anything still holds a descriptor open on the volume. */
int fat32_unmount(void);

#endif
