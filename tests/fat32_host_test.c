/* fat32_host_test.c - drives kernel/fat32.c against a real disk image on
 * the host.
 *
 * Milestone 32. The FAT32 driver is the most intricate code in this tree
 * and the one whose failures are least legible from inside a VM: a
 * mis-set cluster is a file that reads as somebody else's bytes three
 * operations later, and the serial log says nothing at all. So the driver
 * is written against a block-device interface (include/blockdev.h) and
 * this test hands it a device backed by an ordinary file - the same
 * driver, the same code paths, a failure that prints a line number.
 *
 * The image it works on is built by tools/mkfat32.py, which is a
 * completely separate implementation of the same format. That is the
 * point of the arrangement: the writer and the reader agreeing is
 * evidence, where a driver reading back its own output is not. What the
 * driver then writes is checked by unmounting, remounting and looking
 * again, which is the only definition of "persisted" that means anything.
 *
 * Built and run by `make test`.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blockdev.h"
#include "fat32.h"
#include "vfs.h"
#include "posix.h"
#include "rtc.h"

/* --- the kernel interfaces the driver uses, backed by the host -------- */

void* kmalloc(size_t n) { return malloc(n ? n : 1); }
void kfree(void* p) { free(p); }

void rtc_read(rtc_time_t* out) {
    /* Fixed, so that a directory entry written by this test is
     * byte-comparable between runs. */
    out->year = 2026; out->month = 1; out->day = 1;
    out->hour = 0; out->minute = 0; out->second = 0; out->weekday = 4;
}
uint32_t rtc_unix_time(void) { return 1767225600u; }
int32_t rtc_days_from_civil(int32_t y, uint32_t m, uint32_t d) {
    (void)y; (void)m; (void)d; return 0;
}

/* --- a block device that is a file ------------------------------------ */

static FILE* disk_file = 0;

static int file_read(blockdev_t* dev, uint32_t lba, uint32_t count, void* buf) {
    (void)dev;
    if (fseek(disk_file, (long)lba * BLOCK_SECTOR_SIZE, SEEK_SET) != 0) return -1;
    if (fread(buf, BLOCK_SECTOR_SIZE, count, disk_file) != count) return -1;
    return 0;
}

static int file_write(blockdev_t* dev, uint32_t lba, uint32_t count,
                      const void* buf) {
    (void)dev;
    if (fseek(disk_file, (long)lba * BLOCK_SECTOR_SIZE, SEEK_SET) != 0) return -1;
    if (fwrite(buf, BLOCK_SECTOR_SIZE, count, disk_file) != count) return -1;
    fflush(disk_file);
    return 0;
}

static blockdev_t file_dev;

/* --- assertions -------------------------------------------------------- */

static int checks = 0;
static int failures = 0;

static void ok(int cond, const char* what) {
    checks++;
    if (cond) {
        printf("  ok    %s\n", what);
    } else {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

static void ok_eq(uint32_t got, uint32_t want, const char* what) {
    checks++;
    if (got == want) {
        printf("  ok    %s (%u)\n", what, got);
    } else {
        failures++;
        printf("  FAIL  %s: got %u, wanted %u\n", what, got, want);
    }
}

static void ok_str(const char* got, const char* want, const char* what) {
    checks++;
    if (got && strcmp(got, want) == 0) {
        printf("  ok    %s (\"%s\")\n", what, got);
    } else {
        failures++;
        printf("  FAIL  %s: got \"%s\", wanted \"%s\"\n", what,
               got ? got : "(null)", want);
    }
}

/* --- mounting ---------------------------------------------------------- */

/* A fake initrd header with the wrong magic, so ramfs_init() builds an
 * empty root - which is exactly what is wanted here. */
static uint8_t fake_initrd[64];

static void mount_image(const char* path) {
    disk_file = fopen(path, "r+b");
    if (!disk_file) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    fseek(disk_file, 0, SEEK_END);
    long size = ftell(disk_file);

    blockdev_reset();
    memset(&file_dev, 0, sizeof(file_dev));
    file_dev.read = file_read;
    file_dev.write = file_write;
    file_dev.sectors = (uint32_t)(size / BLOCK_SECTOR_SIZE);
    strcpy(file_dev.name, "img0");
    blockdev_register(&file_dev);

    memset(fake_initrd, 0, sizeof(fake_initrd));
    ramfs_init((uint32_t)(uintptr_t)fake_initrd, sizeof(fake_initrd));

    vfs_node_t* mp = vfs_lookup("/disk");
    if (!mp) { fprintf(stderr, "no /disk mount point\n"); exit(1); }
    int r = fat32_mount(mp, &file_dev);
    if (r != 0) {
        fprintf(stderr, "fat32_mount failed: %d\n", r);
        exit(1);
    }
}

static void remount_image(const char* path) {
    int r = fat32_unmount();
    if (r != 0) {
        fprintf(stderr, "fat32_unmount failed: %d\n", r);
        exit(1);
    }
    fclose(disk_file);
    mount_image(path);
}

/* --- helpers ----------------------------------------------------------- */

static char rdbuf[262144];

static uint32_t slurp(const char* path) {
    vfs_node_t* n = vfs_lookup(path);
    if (!n) return 0xFFFFFFFFu;
    uint32_t got = vfs_read(n, 0, n->length < sizeof(rdbuf) ? n->length
                                                           : sizeof(rdbuf) - 1,
                            (uint8_t*)rdbuf);
    rdbuf[got] = '\0';
    return got;
}

static uint32_t count_children(const char* path) {
    vfs_node_t* d = vfs_lookup(path);
    if (!d) return 0xFFFFFFFFu;
    uint32_t n = 0;
    while (vfs_readdir(d, n)) n++;
    return n;
}

/* --- the tests --------------------------------------------------------- */

/* What mkfat32.py put there, read back by the driver. If these fail the
 * two implementations disagree about the format itself, and nothing below
 * is worth looking at. */
static void test_read_prebuilt(void) {
    printf("\nreading what tools/mkfat32.py wrote:\n");

    ok(vfs_lookup("/disk/readme.txt") != 0, "a short 8.3 name resolves");
    ok_eq(slurp("/disk/readme.txt"), 20, "its length");
    ok_str(rdbuf, "hello from the disk\n", "its contents");

    ok(vfs_lookup("/disk/a-very-long-file-name-indeed.dat") != 0,
       "a long filename resolves through its LFN entries");
    ok_eq(slurp("/disk/a-very-long-file-name-indeed.dat"), 28,
          "the long-named file's length");

    ok(vfs_lookup("/disk/sub/deeper/note.txt") != 0,
       "a path three directories deep resolves");
    ok_eq(slurp("/disk/sub/deeper/note.txt"), 7, "the nested file's length");

    /* A file larger than one cluster, so the chain really is walked. */
    vfs_node_t* blob = vfs_lookup("/disk/blob.bin");
    ok(blob != 0, "a multi-cluster file resolves");
    ok_eq(blob ? blob->length : 0, 200000, "its length");

    /* Read it in one go and in pieces, and check the pieces agree - a
     * chain walked from the start every time and a chain walked from an
     * offset are different code paths. */
    if (blob) {
        static uint8_t whole[200000];
        uint32_t got = vfs_read(blob, 0, sizeof(whole), whole);
        ok_eq(got, 200000, "read in one call");

        int same = 1;
        for (uint32_t off = 0; off < 200000; off += 4093) {
            uint8_t piece[4093];
            uint32_t want = 200000 - off < 4093 ? 200000 - off : 4093;
            uint32_t n = vfs_read(blob, off, want, piece);
            if (n != want || memcmp(piece, whole + off, want) != 0) {
                same = 0;
                break;
            }
        }
        ok(same, "reading it in odd-sized pieces gives the same bytes");
    }
}

static void test_symlinks(void) {
    printf("\nsymbolic links:\n");

    vfs_node_t* l = vfs_lookup_nofollow("/disk/sub/link-to-readme");
    ok(l != 0, "a link resolves without being followed");
    ok(l && (l->flags & VFS_SYMLINK), "and reports itself as a link");

    char target[VFS_SYMLINK_MAX];
    int32_t n = l ? vfs_readlink(l, target, sizeof(target) - 1) : -1;
    if (n > 0) target[n] = '\0';
    ok_str(n > 0 ? target : "", "../readme.txt", "readlink gives the target");

    /* Following it must land on the file the target names. */
    vfs_node_t* through = vfs_lookup("/disk/sub/link-to-readme");
    ok(through == vfs_lookup("/disk/readme.txt"),
       "following the link reaches the file it names");

    ok_eq(slurp("/disk/sub/link-to-readme"), 20, "reading through the link");
    ok_str(rdbuf, "hello from the disk\n", "and getting the file's contents");

    /* An absolute target, and one that is a directory. */
    vfs_node_t* abs = vfs_lookup("/disk/rootlink");
    ok(abs == vfs_lookup("/disk"), "an absolute link target resolves");
}

static void test_create_and_write(void) {
    printf("\ncreating and writing:\n");

    vfs_node_t* dir = vfs_lookup("/disk");
    vfs_node_t* f = vfs_create(dir, "written.txt", VFS_FILE);
    ok(f != 0, "create a file");

    const char* msg = "the driver wrote this";
    int32_t w = vfs_write(f, 0, (uint32_t)strlen(msg), (const uint8_t*)msg);
    ok_eq((uint32_t)w, (uint32_t)strlen(msg), "write returns what it wrote");
    ok_eq(f->length, (uint32_t)strlen(msg), "the length is updated");

    ok_eq(slurp("/disk/written.txt"), (uint32_t)strlen(msg), "read it back");
    ok_str(rdbuf, msg, "the bytes match");

    /* A write that crosses a cluster boundary, then one that reaches well
     * past it, so the chain has to be extended more than once. */
    vfs_node_t* big = vfs_create(dir, "big-file-with-a-long-name.bin", VFS_FILE);
    ok(big != 0, "create a file with a name needing LFN entries");

    static uint8_t pattern[70000];
    for (uint32_t i = 0; i < sizeof(pattern); i++) {
        pattern[i] = (uint8_t)(i * 31u + (i >> 8));
    }
    w = vfs_write(big, 0, sizeof(pattern), pattern);
    ok_eq((uint32_t)w, sizeof(pattern), "write 70000 bytes across many clusters");

    static uint8_t back[70000];
    ok_eq(vfs_read(big, 0, sizeof(back), back), sizeof(back), "read them back");
    ok(memcmp(back, pattern, sizeof(pattern)) == 0, "every byte survived");

    /* An overwrite in the middle, which is the read-modify-write path. */
    const char* patch = "PATCHED";
    vfs_write(big, 33333, 7, (const uint8_t*)patch);
    vfs_read(big, 33330, 16, back);
    ok(memcmp(back + 3, patch, 7) == 0, "a partial-sector overwrite lands");
    ok(back[0] == pattern[33330] && back[10] == pattern[33340],
       "and leaves its neighbours alone");
}

static void test_truncate(void) {
    printf("\ntruncating:\n");

    vfs_node_t* f = vfs_lookup("/disk/big-file-with-a-long-name.bin");
    ok(f != 0, "the file is still there");
    if (!f) return;

    ok_eq((uint32_t)vfs_truncate(f, 100), 0, "truncate to 100 bytes");
    ok_eq(f->length, 100, "the length follows");
    ok_eq(vfs_read(f, 0, sizeof(rdbuf), (uint8_t*)rdbuf), 100,
          "a read stops at the new end");

    /* Growing past the end must read as zeroes, including the part that
     * falls inside a cluster the file already had - where the old bytes
     * are still physically present. */
    ok_eq((uint32_t)vfs_truncate(f, 5000), 0, "grow it back to 5000");
    static uint8_t z[5000];
    memset(z, 0xAA, sizeof(z));
    ok_eq(vfs_read(f, 0, sizeof(z), z), 5000, "read the grown file");
    int zeroed = 1;
    for (uint32_t i = 100; i < 5000; i++) if (z[i]) { zeroed = 0; break; }
    ok(zeroed, "everything past the old end reads as zero");
}

static void test_directories(void) {
    printf("\ndirectories:\n");

    vfs_node_t* dir = vfs_lookup("/disk");
    vfs_node_t* made = vfs_create(dir, "newdir", VFS_DIRECTORY);
    ok(made != 0, "mkdir");
    ok(vfs_lookup("/disk/newdir") == made, "and it resolves");

    vfs_node_t* inner = vfs_create(made, "inner.txt", VFS_FILE);
    ok(inner != 0, "create a file inside it");
    vfs_write(inner, 0, 5, (const uint8_t*)"abcde");

    ok_eq((uint32_t)vfs_unlink(dir, "newdir", 1), (uint32_t)-ENOTEMPTY,
          "rmdir refuses a directory with something in it");

    /* Enough entries to force the directory past its first cluster. On a
     * 512-byte-cluster volume that is 16 entries, and each long name here
     * costs several - so this really does cross the boundary. */
    for (int i = 0; i < 60; i++) {
        char name[64];
        snprintf(name, sizeof(name), "generated-entry-number-%02d.txt", i);
        vfs_node_t* n = vfs_create(made, name, VFS_FILE);
        if (!n) { ok(0, "creating 60 long-named entries"); return; }
        vfs_write(n, 0, (uint32_t)strlen(name), (const uint8_t*)name);
    }
    ok_eq(count_children("/disk/newdir"), 61,
          "60 more entries, so the directory grew past one cluster");
}

static void test_rename_and_unlink(void) {
    printf("\nrenaming and removing:\n");

    ok_eq((uint32_t)vfs_rename("/disk/written.txt", "/disk/renamed.txt"), 0,
          "rename within a directory");
    ok(vfs_lookup("/disk/written.txt") == 0, "the old name is gone");
    ok_eq(slurp("/disk/renamed.txt"), 21, "the new name has the contents");

    ok_eq((uint32_t)vfs_rename("/disk/renamed.txt",
                               "/disk/sub/moved-with-a-long-name.txt"), 0,
          "rename into another directory");
    ok(vfs_lookup("/disk/renamed.txt") == 0, "gone from the old directory");
    ok_eq(slurp("/disk/sub/moved-with-a-long-name.txt"), 21,
          "and readable in the new one");

    vfs_node_t* dir = vfs_lookup("/disk");
    ok_eq((uint32_t)vfs_unlink(dir, "big-file-with-a-long-name.bin", 0), 0,
          "unlink a file whose name used LFN entries");
    ok(vfs_lookup("/disk/big-file-with-a-long-name.bin") == 0,
       "and it is gone");
}

/* The only test that proves anything about a filesystem: close it, open
 * it again, and see whether what was written is still there. */
static void test_persistence(const char* path) {
    printf("\nafter an unmount and remount:\n");

    uint32_t before_total = 0, before_free = 0;
    fat32_space(&before_total, &before_free);

    remount_image(path);

    ok_eq(slurp("/disk/sub/moved-with-a-long-name.txt"), 21,
          "the renamed, moved file is still there");
    ok_str(rdbuf, "the driver wrote this", "with its contents");

    ok(vfs_lookup("/disk/big-file-with-a-long-name.bin") == 0,
       "the unlinked file is still gone");
    ok_eq(count_children("/disk/newdir"), 61,
          "the grown directory still has all its entries");

    vfs_node_t* f = vfs_lookup("/disk/newdir/generated-entry-number-42.txt");
    ok(f != 0, "an entry past the directory's first cluster resolves");
    ok_eq(slurp("/disk/newdir/generated-entry-number-42.txt"), 29,
          "and its contents came back");

    /* A link created by the driver rather than by the image builder. */
    vfs_node_t* d = vfs_lookup("/disk");
    ok_eq((uint32_t)vfs_symlink(d, "made-link", "sub/deeper/note.txt"), 0,
          "the driver can create a symlink");
    remount_image(path);
    vfs_node_t* l = vfs_lookup_nofollow("/disk/made-link");
    ok(l && (l->flags & VFS_SYMLINK),
       "and it reads back as a link after a remount");
    ok(vfs_lookup("/disk/made-link") == vfs_lookup("/disk/sub/deeper/note.txt"),
       "pointing where it was told to");

    uint32_t after_total = 0, after_free = 0;
    fat32_space(&after_total, &after_free);
    ok_eq(after_total, before_total, "the volume is the same size");
    ok(after_free > 0, "and has free space left");
}

static void test_full_volume(void) {
    printf("\nrunning out of space:\n");

    /* A file bigger than the volume. The interesting property is not that
     * it fails - it is that the filesystem is still usable afterwards. */
    vfs_node_t* dir = vfs_lookup("/disk");
    vfs_node_t* hog = vfs_create(dir, "hog.bin", VFS_FILE);
    ok(hog != 0, "create the file");

    uint32_t total = 0, freec = 0;
    fat32_space(&total, &freec);
    uint32_t too_big = (freec + 16) * fat32_cluster_bytes();

    int32_t r = vfs_truncate(hog, too_big);
    ok(r < 0, "truncating past the end of the volume fails");

    vfs_node_t* after = vfs_create(dir, "after.txt", VFS_FILE);
    ok(after != 0, "the filesystem still works afterwards");
    if (after) {
        vfs_write(after, 0, 3, (const uint8_t*)"ok\n");
        ok_eq(slurp("/disk/after.txt"), 3, "and can still store a file");
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s DISK.img\n", argv[0]);
        return 2;
    }
    printf("FAT32 driver, driven directly against a real disk image\n");

    mount_image(argv[1]);
    uint32_t total = 0, freec = 0;
    fat32_space(&total, &freec);
    printf("\nmounted: %u clusters of %u bytes (%u free), label \"%s\"\n",
           total, fat32_cluster_bytes(), freec, fat32_volume_label());

    test_read_prebuilt();
    test_symlinks();
    test_create_and_write();
    test_truncate();
    test_directories();
    test_rename_and_unlink();
    test_persistence(argv[1]);
    test_full_volume();

    /* Leave the volume tidy: the FSInfo block's free-cluster count is a
     * hint the format lets drift, and pushing it out here means
     * `fsck.vfat -n` on the image this test leaves behind finds nothing
     * at all to correct - which is a stronger statement than "the driver
     * agrees with itself". */
    fat32_sync();

    printf("\n%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
