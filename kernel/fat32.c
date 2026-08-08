/* fat32.c - the FAT32 filesystem driver.
 *
 * Milestone 32, and the last structural thing between this kernel and a
 * real Wine installation. Milestone 31 ended by naming it: `wineboot`
 * runs `wine.inf` through setupapi to build C:\windows, the registry and
 * a user profile, which is several hundred files, and the filesystem it
 * had to build them in was an initrd read into RAM whole.
 *
 * The shape of the code follows the shape of FAT32, which is three
 * things:
 *
 *   - A boot sector (the BPB) saying how big a cluster is and where
 *     everything starts.
 *   - The file allocation table: one 32-bit entry per cluster, forming
 *     singly linked lists. Entry N holds "the cluster after N", or an
 *     end-of-chain marker, or 0 for free. That is the entire allocator.
 *   - Directories, which are ordinary files whose contents are 32-byte
 *     entries. A long filename is stored as extra entries *before* the
 *     real one, in reverse order, holding UCS-2 fragments.
 *
 * Two decisions worth stating up front.
 *
 * Nodes are cached, bytes are not. The first time anything looks inside a
 * directory its entries are read off the disk and turned into VFS nodes,
 * which then stay. File *contents* are never cached - a read goes to the
 * disk every time - unless something has to see the whole file at once
 * (mmap, or exec), which is what materialize() is for. This is the point
 * of the milestone: the cost of a filesystem becomes proportional to what
 * is used rather than to what exists. A prefix of four thousand files
 * costs about 700KB of nodes instead of forty megabytes of initrd.
 *
 * Metadata is written through, immediately. There is no journal and no
 * delayed allocation: a create writes the directory entry before it
 * returns, a write updates the size in the directory entry before it
 * returns. The only thing held back is the FAT sector the allocator is
 * working in, which fat32_sync() pushes out. That is slower than a real
 * driver and it means a machine that stops mid-write loses at most the
 * one operation in flight, which for a kernel with no fsck is the right
 * trade.
 */

#include "fat32.h"
#include "console.h"
#include "blockdev.h"
#include "vfs.h"
#include "kheap.h"
#include "kstring.h"
#include "posix.h"
#include "rtc.h"

/* --- on-disk layout ------------------------------------------------------ */

#define DIRENT_SIZE       32u
#define ATTR_READ_ONLY    0x01
#define ATTR_HIDDEN       0x02
#define ATTR_SYSTEM       0x04
#define ATTR_VOLUME_ID    0x08
#define ATTR_DIRECTORY    0x10
#define ATTR_ARCHIVE      0x20
#define ATTR_LFN          0x0F

#define ENTRY_FREE        0xE5
#define ENTRY_END         0x00

#define FAT_EOC           0x0FFFFFF8u   /* >= this is end of chain */
#define FAT_BAD           0x0FFFFFF7u
#define FAT_MASK          0x0FFFFFFFu

/* The header that marks a file as a symbolic link. See fat32.h. */
#define SYMLINK_MAGIC     "NVSYMLNK"
#define SYMLINK_MAGIC_LEN 8u

/* Little-endian loads. The disk is little-endian and so is the machine,
 * but the fields are unaligned inside a 32-byte entry and a compiler is
 * entitled to assume they are not - so they are assembled by hand. */
static uint16_t rd16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}
static void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* --- the mounted volume --------------------------------------------------
 *
 * One volume. A mount table is a real thing to want and this kernel does
 * not have one; what it has is a disk, and a disk is what the milestone
 * needed. Making it a struct rather than a scatter of globals is most of
 * the work of supporting several, if that day comes. */
typedef struct {
    blockdev_t* dev;
    vfs_node_t* root;

    uint32_t part_lba;            /* first sector of the volume */
    uint32_t sectors_per_cluster;
    uint32_t cluster_bytes;
    uint32_t reserved;
    uint32_t num_fats;
    uint32_t fat_sectors;
    uint32_t root_cluster;
    uint32_t fat_lba;             /* part_lba + reserved */
    uint32_t data_lba;            /* fat_lba + num_fats * fat_sectors */
    uint32_t total_clusters;      /* usable clusters, i.e. highest is +1 */
    uint32_t fsinfo_lba;          /* 0 when the volume has none */

    uint32_t free_hint;           /* where to start looking for a free one */
    uint32_t free_count;          /* 0xFFFFFFFF until counted */

    char label[12];
    int mounted;
} fat_vol_t;

static fat_vol_t vol;

/* One cached FAT sector. Chain walks and chain allocation both touch
 * consecutive entries, so a single sector holding 128 of them turns the
 * overwhelming majority of FAT accesses into no disk access at all. It is
 * written back to *every* copy of the FAT, because a volume whose two
 * FATs disagree is a volume every other operating system will refuse or
 * silently repair. */
static uint8_t  fat_cache[BLOCK_SECTOR_SIZE];
static uint32_t fat_cache_lba = 0;   /* 0 = nothing cached; the FAT never
                                      * starts at absolute sector 0 */
static int      fat_cache_dirty = 0;

/* A scratch sector for directory-entry edits, and a whole cluster for
 * directory scans. Both are file-scope rather than stack because the
 * kernel stack is 8KB and a cluster can be 32KB. */
static uint8_t  sec_buf[BLOCK_SECTOR_SIZE];
static uint8_t* clus_buf = 0;

static const vfs_ops_t fat_ops;

/* --- the FAT ------------------------------------------------------------- */

static int fat_cache_flush(void) {
    if (!fat_cache_dirty || !fat_cache_lba) return 0;
    for (uint32_t i = 0; i < vol.num_fats; i++) {
        uint32_t lba = fat_cache_lba + i * vol.fat_sectors;
        if (blockdev_write(vol.dev, lba, 1, fat_cache) != 0) return -EIO;
    }
    fat_cache_dirty = 0;
    return 0;
}

/* Brings the FAT sector holding `cluster` into the cache. */
static int fat_cache_load(uint32_t cluster) {
    uint32_t lba = vol.fat_lba + (cluster / (BLOCK_SECTOR_SIZE / 4));
    if (lba == fat_cache_lba) return 0;
    if (fat_cache_flush() != 0) return -EIO;
    if (blockdev_read(vol.dev, lba, 1, fat_cache) != 0) return -EIO;
    fat_cache_lba = lba;
    return 0;
}

static uint32_t fat_get(uint32_t cluster) {
    if (cluster < 2 || cluster >= vol.total_clusters + 2) return FAT_EOC;
    if (fat_cache_load(cluster) != 0) return FAT_EOC;
    uint32_t off = (cluster % (BLOCK_SECTOR_SIZE / 4)) * 4;
    return rd32(fat_cache + off) & FAT_MASK;
}

static void chain_memo_forget(void);

static int fat_set(uint32_t cluster, uint32_t value) {
    if (cluster < 2 || cluster >= vol.total_clusters + 2) return -EINVAL;

    /* Freeing a cluster or capping a chain can renumber or invalidate
     * whatever chain_nth() last remembered - a freed cluster can be handed
     * to another file, and a shortened chain has no Nth cluster any more.
     * Linking a cluster onto the end of a chain does neither, which is why
     * growing a file does not throw the memo away on every cluster. */
    if (value == 0 || value == FAT_MASK) chain_memo_forget();
    if (fat_cache_load(cluster) != 0) return -EIO;
    uint32_t off = (cluster % (BLOCK_SECTOR_SIZE / 4)) * 4;
    /* The top four bits of a FAT32 entry are reserved and must be left
     * exactly as they were found. Almost nothing looks at them, and the
     * one thing that does is a filesystem checker deciding whether the
     * volume was written by something that understood the format. */
    uint32_t keep = rd32(fat_cache + off) & 0xF0000000u;
    wr32(fat_cache + off, keep | (value & FAT_MASK));
    fat_cache_dirty = 1;
    return 0;
}

static uint32_t cluster_lba(uint32_t cluster) {
    return vol.data_lba + (cluster - 2) * vol.sectors_per_cluster;
}

/* Counts free clusters by walking the whole FAT. Done once, lazily, the
 * first time anything asks - a 64MB volume is 32 FAT sectors and a 4GB
 * one is 2048, so this is cheap at these sizes and would want the FSInfo
 * hint to be trusted at larger ones. */
static void count_free(void) {
    uint32_t free_n = 0;
    for (uint32_t c = 2; c < vol.total_clusters + 2; c++) {
        if (fat_get(c) == 0) free_n++;
    }
    vol.free_count = free_n;
}

/* Finds a free cluster, marks it as the end of a chain, and links it onto
 * `prev` when there is one. Returns 0 if the volume is full. */
static uint32_t alloc_cluster(uint32_t prev) {
    uint32_t start = vol.free_hint < 2 ? 2 : vol.free_hint;
    uint32_t limit = vol.total_clusters + 2;

    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t from = pass == 0 ? start : 2;
        uint32_t to   = pass == 0 ? limit : start;
        for (uint32_t c = from; c < to; c++) {
            if (fat_get(c) != 0) continue;
            if (fat_set(c, FAT_MASK) != 0) return 0;   /* end of chain */
            if (prev && fat_set(prev, c) != 0) return 0;
            vol.free_hint = c + 1;
            if (vol.free_count != 0xFFFFFFFFu && vol.free_count) vol.free_count--;
            return c;
        }
    }
    return 0;
}

/* Zeroes a freshly allocated cluster. A directory's cluster *must* be
 * zeroed, because a zero first byte is what marks the end of a directory
 * - without it a scan runs on into whatever the cluster held before and
 * invents files out of it. Doing it for data clusters too costs a write
 * and means a file with a hole reads as zeroes rather than as somebody
 * else's deleted data, which is a property worth paying for. */
static int zero_cluster(uint32_t cluster) {
    kmemset(clus_buf, 0, vol.cluster_bytes);
    return blockdev_write(vol.dev, cluster_lba(cluster),
                          vol.sectors_per_cluster, clus_buf);
}

static void free_chain(uint32_t cluster) {
    while (cluster >= 2 && cluster < FAT_BAD) {
        uint32_t next = fat_get(cluster);
        if (fat_set(cluster, 0) != 0) return;
        if (vol.free_count != 0xFFFFFFFFu) vol.free_count++;
        if (cluster < vol.free_hint) vol.free_hint = cluster;
        cluster = next;
    }
}

/* Where the last chain walk finished. See chain_nth(). */
static uint32_t memo_first, memo_index, memo_cluster;

static void chain_memo_forget(void) {
    memo_first = memo_index = memo_cluster = 0;
}

/* The `index`-th cluster of a chain, or 0 if the chain is shorter than
 * that. Never extends - growing is ensure_chain()'s job, and keeping the
 * two apart is what makes it possible to guarantee that a write which
 * cannot get all its space fails before writing any of it.
 *
 * The walk from the head is the obvious implementation, and on its own it
 * is what made writing a 48MB file impossible rather than merely slow.
 * Reading or writing a whole file asks for cluster 0, then 1, then 2, and
 * a fresh walk for each of those is quadratic: ninety thousand clusters
 * is four billion link steps, for a file that took under a second to
 * arrive over the network.
 *
 * So the last answer is remembered. A request for an index at or after
 * the one already found continues from there - one step per cluster,
 * which is what sequential access should cost. Anything else falls back
 * to the walk, and any change to a chain that could renumber it throws
 * the memo away, so this is a cache and never a second source of truth. */
static uint32_t chain_nth(uint32_t first, uint32_t index) {
    if (first < 2) return 0;

    uint32_t cur = first;
    uint32_t i = 0;
    if (memo_cluster >= 2 && memo_first == first && memo_index <= index) {
        cur = memo_cluster;
        i = memo_index;
    }

    for (; i < index; i++) {
        uint32_t next = fat_get(cur);
        if (next < 2 || next >= FAT_BAD) return 0;
        cur = next;
    }

    memo_first = first;
    memo_index = index;
    memo_cluster = cur;
    return cur;
}

/* How many clusters a chain has. Bounded by the volume, so a chain that
 * loops back on itself terminates here instead of hanging the machine. */
static uint32_t chain_length(uint32_t first) {
    if (first < 2) return 0;
    uint32_t n = 0;
    uint32_t cur = first;
    while (cur >= 2 && cur < FAT_BAD && n <= vol.total_clusters) {
        n++;
        cur = fat_get(cur);
    }
    return n;
}

/* Makes a node's chain at least `need` clusters long, allocating the head
 * if it had none. Every new cluster is zeroed, so a file that grows reads
 * as zeroes rather than as whatever was there before.
 *
 * All-or-nothing, and the rollback is the point rather than a nicety. A
 * partial extension that then failed would leave clusters marked in use
 * that no directory entry mentions - lost space, on a filesystem with no
 * checker to find it - and the volume would stay full for ever after one
 * program asked for too much. The host test writes a file bigger than the
 * disk and then checks that an ordinary small file still works. */
static int ensure_chain(vfs_node_t* node, uint32_t need) {
    if (!need) return 0;

    uint32_t had = chain_length(node->fs_cluster);
    if (had >= need) return 0;
    uint32_t head_was = node->fs_cluster;

    if (node->fs_cluster < 2) {
        uint32_t first = alloc_cluster(0);
        if (!first) return -ENOSPC;
        if (zero_cluster(first) != 0) { free_chain(first); return -EIO; }
        node->fs_cluster = first;
        node->fs_dirty = 1;
        had = 1;
    }

    uint32_t cur = chain_nth(node->fs_cluster, had - 1);
    if (!cur) return -EIO;

    for (uint32_t i = had; i < need; i++) {
        uint32_t next = alloc_cluster(cur);
        int bad = !next;
        if (!bad && zero_cluster(next) != 0) bad = 1;
        if (bad) {
            /* Back to exactly what was there before. */
            if (head_was < 2) {
                free_chain(node->fs_cluster);
                node->fs_cluster = head_was;
                node->fs_dirty = 1;
            } else {
                uint32_t keep = chain_nth(head_was, had - 1);
                if (keep) {
                    uint32_t rest = fat_get(keep);
                    fat_set(keep, FAT_MASK);
                    if (rest >= 2 && rest < FAT_BAD) free_chain(rest);
                }
            }
            return -ENOSPC;
        }
        cur = next;
    }
    return 0;
}

/* --- names ---------------------------------------------------------------
 *
 * The 8.3 short name is 11 bytes with no dot: "README  TXT". Everything
 * longer, or with characters DOS would not accept, needs long-filename
 * entries as well - and then the short name is a generated placeholder
 * that has to be unique in the directory, because that is still the name
 * the entry is found by. */

static int is_upper(char c) { return c >= 'A' && c <= 'Z'; }
static int is_lower(char c) { return c >= 'a' && c <= 'z'; }
static char to_upper(char c) { return is_lower(c) ? (char)(c - 32) : c; }
static char to_lower(char c) { return is_upper(c) ? (char)(c + 32) : c; }

/* The characters DOS allows in a short name, beyond letters and digits. */
static int short_char_ok(char c) {
    if (is_upper(c) || (c >= '0' && c <= '9')) return 1;
    const char* ok = "$%'-_@~`!(){}^#&";
    for (const char* p = ok; *p; p++) if (*p == c) return 1;
    return 0;
}

/* Lookups here are case-sensitive, and deliberately so. FAT itself is
 * not, but this volume is mounted inside a Unix filesystem and reached
 * through Unix paths - Wine, the dynamic linker and every program above
 * them expect "Makefile" and "makefile" to be different names. What the
 * driver does with case is confined to the 8.3 short name, which has no
 * case of its own. */
static int name_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* The checksum every long-filename entry carries, over the short name it
 * belongs to. It is what ties the two together: a filesystem that finds a
 * short entry whose checksum does not match the long entries in front of
 * it is supposed to ignore the long ones. */
static uint8_t short_checksum(const uint8_t* short11) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + short11[i]);
    }
    return sum;
}

/* NT's two spare bits in a directory entry: "the base is really
 * lowercase" and "the extension is really lowercase". They are why
 * "readme.txt" does not need long-filename entries even though its short
 * form is "README  TXT", and using them keeps ordinary names cheap. */
#define NT_LOWER_BASE 0x08
#define NT_LOWER_EXT  0x10

/* Tries to express `name` as a short name. Returns 1 if it fits, filling
 * `out11` and `ntflags`; 0 if the name needs LFN entries. */
static int make_short(const char* name, uint8_t* out11, uint8_t* ntflags) {
    uint32_t len = kstrlen(name);
    if (!len || len > 12) return 0;
    if (name_eq(name, ".") || name_eq(name, "..")) return 0;

    /* The extension is what follows the *last* dot, and a name with more
     * than one dot has no short form. */
    int dot = -1;
    uint32_t dots = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (name[i] == '.') { dot = (int)i; dots++; }
    }
    if (dots > 1) return 0;
    if (dot == 0) return 0;             /* ".profile" - no base */

    uint32_t baselen = dot < 0 ? len : (uint32_t)dot;
    uint32_t extlen  = dot < 0 ? 0 : len - (uint32_t)dot - 1;
    if (baselen == 0 || baselen > 8 || extlen > 3) return 0;

    int base_lower = 0, base_upper = 0, ext_lower = 0, ext_upper = 0;
    for (uint32_t i = 0; i < baselen; i++) {
        char c = name[i];
        if (is_lower(c)) base_lower = 1;
        else if (is_upper(c)) base_upper = 1;
        if (!short_char_ok(to_upper(c))) return 0;
    }
    for (uint32_t i = 0; i < extlen; i++) {
        char c = name[dot + 1 + i];
        if (is_lower(c)) ext_lower = 1;
        else if (is_upper(c)) ext_upper = 1;
        if (!short_char_ok(to_upper(c))) return 0;
    }
    /* Mixed case in either half cannot be expressed by one flag bit. */
    if (base_lower && base_upper) return 0;
    if (ext_lower && ext_upper) return 0;

    kmemset(out11, ' ', 11);
    for (uint32_t i = 0; i < baselen; i++) out11[i] = (uint8_t)to_upper(name[i]);
    for (uint32_t i = 0; i < extlen; i++) {
        out11[8 + i] = (uint8_t)to_upper(name[dot + 1 + i]);
    }
    *ntflags = (uint8_t)((base_lower ? NT_LOWER_BASE : 0) |
                         (ext_lower ? NT_LOWER_EXT : 0));
    return 1;
}

/* Turns an 11-byte short name back into "readme.txt". */
static void short_to_name(const uint8_t* in11, uint8_t ntflags, char* out) {
    uint32_t w = 0;
    for (uint32_t i = 0; i < 8; i++) {
        if (in11[i] == ' ') break;
        char c = (char)in11[i];
        out[w++] = (ntflags & NT_LOWER_BASE) ? to_lower(c) : c;
    }
    if (in11[8] != ' ') {
        out[w++] = '.';
        for (uint32_t i = 8; i < 11; i++) {
            if (in11[i] == ' ') break;
            char c = (char)in11[i];
            out[w++] = (ntflags & NT_LOWER_EXT) ? to_lower(c) : c;
        }
    }
    out[w] = '\0';
}

/* How many long-filename entries a name of `len` characters needs: 13
 * characters fit in each. */
static uint32_t lfn_slots_for(uint32_t len) {
    return (len + 12) / 13;
}

/* Where the 13 name characters live inside a long-filename entry: five at
 * offset 1, six at 14, two at 28. Split like that because the fields it
 * has to avoid - the attribute byte, the checksum, the first-cluster word
 * - are at the offsets an ordinary entry uses them, which is what lets an
 * old DOS skip these entries instead of choking on them. */
static const uint8_t lfn_offsets[13] = {
    1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
};

/* --- directory scanning ---------------------------------------------------
 *
 * A directory is a chain of clusters full of 32-byte entries, so
 * everything below iterates (cluster, sector, entry). The callback style
 * keeps that iteration in one place: scanning to build nodes, scanning to
 * find a free run of slots and scanning to delete are the same walk. */

typedef struct {
    uint32_t lba;        /* sector holding this entry */
    uint32_t off;        /* byte offset within that sector */
    uint32_t index;      /* entry number within the whole directory */
} dir_pos_t;

/* Reads the `index`-th 32-byte entry of a directory into `out`, and says
 * where it came from. Returns 0 on success, -ENOENT past the end. */
static int dir_entry_at(uint32_t first_cluster, uint32_t index,
                        uint8_t* out, dir_pos_t* pos) {
    uint32_t per_sector = BLOCK_SECTOR_SIZE / DIRENT_SIZE;
    uint32_t per_cluster = per_sector * vol.sectors_per_cluster;

    uint32_t cluster = chain_nth(first_cluster, index / per_cluster);
    if (!cluster) return -ENOENT;

    uint32_t within = index % per_cluster;
    uint32_t lba = cluster_lba(cluster) + within / per_sector;
    uint32_t off = (within % per_sector) * DIRENT_SIZE;

    if (blockdev_read(vol.dev, lba, 1, sec_buf) != 0) return -EIO;
    kmemcpy(out, sec_buf + off, DIRENT_SIZE);
    if (pos) { pos->lba = lba; pos->off = off; pos->index = index; }
    return 0;
}

/* Writes one 32-byte entry back, read-modify-write on its sector. */
static int dir_entry_put(const dir_pos_t* pos, const uint8_t* ent) {
    if (blockdev_read(vol.dev, pos->lba, 1, sec_buf) != 0) return -EIO;
    kmemcpy(sec_buf + pos->off, ent, DIRENT_SIZE);
    return blockdev_write(vol.dev, pos->lba, 1, sec_buf);
}

/* Patches the bytes of an existing entry in place - used to update a
 * file's size and first cluster after a write. */
static int dirent_update(vfs_node_t* node) {
    if (!node->fs_dirent_lba) return 0;
    if (blockdev_read(vol.dev, node->fs_dirent_lba, 1, sec_buf) != 0) return -EIO;
    uint8_t* e = sec_buf + node->fs_dirent_off;

    uint32_t raw_len = node->length;
    if (node->flags & VFS_SYMLINK) raw_len += SYMLINK_MAGIC_LEN;

    wr16(e + 20, (uint16_t)(node->fs_cluster >> 16));
    wr16(e + 26, (uint16_t)(node->fs_cluster & 0xFFFF));
    if (!(node->flags & VFS_DIRECTORY)) wr32(e + 28, raw_len);

    /* Modification time, so that `ls -l` on another machine and Wine's
     * own "is this file newer than that one" both see something real.
     * The RTC is the only clock this kernel has. */
    rtc_time_t t;
    rtc_read(&t);
    uint16_t fdate = (uint16_t)((((uint32_t)t.year - 1980u) & 0x7Fu) << 9 |
                                ((uint32_t)t.month & 0x0Fu) << 5 |
                                ((uint32_t)t.day & 0x1Fu));
    uint16_t ftime = (uint16_t)(((uint32_t)t.hour & 0x1Fu) << 11 |
                                ((uint32_t)t.minute & 0x3Fu) << 5 |
                                (((uint32_t)t.second / 2u) & 0x1Fu));
    wr16(e + 22, ftime);
    wr16(e + 24, fdate);
    wr16(e + 18, fdate);

    node->fs_dirty = 0;
    return blockdev_write(vol.dev, node->fs_dirent_lba, 1, sec_buf);
}

/* --- reading a directory into nodes -------------------------------------- */

static uint32_t fat_read_node(vfs_node_t* node, uint32_t offset, uint32_t size,
                              uint8_t* buffer);
static vfs_node_t* fat_readdir(vfs_node_t* node, uint32_t index);
static vfs_node_t* fat_finddir(vfs_node_t* node, const char* name);

/* Is this small SYSTEM-attributed file one of our symbolic links? Costs
 * one sector read, and only for files that have the attribute set at all
 * - nothing else on the volume does. */
static int looks_like_symlink(uint32_t cluster, uint32_t size) {
    if (cluster < 2) return 0;
    if (size <= SYMLINK_MAGIC_LEN || size > SYMLINK_MAGIC_LEN + VFS_SYMLINK_MAX) {
        return 0;
    }
    if (blockdev_read(vol.dev, cluster_lba(cluster), 1, sec_buf) != 0) return 0;
    return kmemcmp(sec_buf, SYMLINK_MAGIC, SYMLINK_MAGIC_LEN) == 0;
}

static void fat_init_node(vfs_node_t* n, uint32_t flags) {
    n->flags = flags;
    n->ops = &fat_ops;
    n->read = fat_read_node;
    n->readdir = (flags & VFS_DIRECTORY) ? fat_readdir : 0;
    n->finddir = (flags & VFS_DIRECTORY) ? fat_finddir : 0;
}

/* Reads every entry of a directory and instantiates a node for each.
 * Runs once per directory, the first time anything looks inside. */
static int populate(vfs_node_t* dir) {
    if (dir->populated) return 0;
    dir->populated = 1;   /* set first: a failure part-way leaves what was
                           * read rather than re-reading for ever */

    char lfn[VFS_NAME_MAX];
    uint32_t lfn_len = 0;
    uint32_t lfn_count = 0;
    uint8_t  lfn_sum = 0;
    uint32_t first_slot = 0;

    uint8_t ent[DIRENT_SIZE];
    dir_pos_t pos;

    for (uint32_t i = 0; ; i++) {
        if (dir_entry_at(dir->fs_cluster, i, ent, &pos) != 0) break;
        if (ent[0] == ENTRY_END) break;
        if (ent[0] == ENTRY_FREE) { lfn_count = 0; continue; }

        uint8_t attr = ent[11];
        if ((attr & ATTR_LFN) == ATTR_LFN) {
            /* Long-name entries come *before* their short entry and are
             * numbered from the end of the name backwards, so the last
             * fragment is seen first. The 0x40 bit marks that one. */
            uint32_t ord = ent[0] & 0x3Fu;
            if (ent[0] & 0x40u) {
                /* 20 fragments is 260 characters, which is the longest
                 * name the format allows; anything claiming more is
                 * corruption rather than a long name. */
                if (ord == 0 || ord > 20) { lfn_count = 0; continue; }
                lfn_count = ord;
                lfn_len = ord * 13;
                lfn_sum = ent[13];
                first_slot = i;
                kmemset(lfn, 0, sizeof(lfn));
            }
            if (!lfn_count || ord == 0 || ord > lfn_count) { lfn_count = 0; continue; }
            if (ent[13] != lfn_sum) { lfn_count = 0; continue; }

            uint32_t base = (ord - 1) * 13;
            for (uint32_t k = 0; k < 13; k++) {
                uint16_t ch = rd16(ent + lfn_offsets[k]);
                /* 0xFFFF is padding and 0 is the terminator. Anything
                 * outside ASCII becomes '?': this kernel's paths are
                 * bytes, and inventing a UTF-8 encoder here would be
                 * inventing it in the wrong place. */
                if (ch == 0x0000 || ch == 0xFFFF) {
                    if (base + k < lfn_len) lfn_len = base + k;
                    break;
                }
                if (base + k < VFS_NAME_MAX - 1) {
                    lfn[base + k] = (ch < 0x80) ? (char)ch : '?';
                }
            }
            continue;
        }

        if (attr & ATTR_VOLUME_ID) {
            /* The volume label lives in the root directory as an entry
             * with no name of its own. */
            if (dir == vol.root) {
                for (uint32_t k = 0; k < 11; k++) vol.label[k] = (char)ent[k];
                vol.label[11] = '\0';
                for (int k = 10; k >= 0 && vol.label[k] == ' '; k--) {
                    vol.label[k] = '\0';
                }
            }
            lfn_count = 0;
            continue;
        }

        char name[VFS_NAME_MAX];
        uint32_t slots = 1;
        /* The long name is used only when its checksum matches the short
         * entry it precedes and it fits a VFS name. A name too long for
         * this kernel falls back to the short one, which is unique on the
         * volume by construction - the file is still reachable, under a
         * name that is ugly rather than absent. */
        if (lfn_count && lfn_len < VFS_NAME_MAX && short_checksum(ent) == lfn_sum) {
            lfn[lfn_len] = '\0';
            kstrlcpy(name, lfn, VFS_NAME_MAX);
            slots = lfn_count + 1;
            (void)first_slot;
        } else {
            short_to_name(ent, ent[12], name);
        }
        lfn_count = 0;

        if (!name[0] || name_eq(name, ".") || name_eq(name, "..")) continue;

        uint32_t cluster = ((uint32_t)rd16(ent + 20) << 16) | rd16(ent + 26);
        uint32_t size = rd32(ent + 28);

        uint32_t flags;
        if (attr & ATTR_DIRECTORY) {
            flags = VFS_DIRECTORY;
            size = 0;
        } else if ((attr & ATTR_SYSTEM) && looks_like_symlink(cluster, size)) {
            flags = VFS_SYMLINK;
            size -= SYMLINK_MAGIC_LEN;
        } else {
            flags = VFS_FILE;
        }

        vfs_node_t* n = vfs_node_alloc();
        if (!n) break;               /* out of nodes - stop, do not spin */
        kstrlcpy(n->name, name, VFS_NAME_MAX);
        fat_init_node(n, flags);
        n->length = size;
        n->fs_cluster = cluster;
        n->fs_dirent_lba = pos.lba;
        n->fs_dirent_off = (uint16_t)pos.off;
        n->fs_slots = (uint16_t)slots;
        n->mode = (flags & VFS_DIRECTORY) ? 0755u
                : (flags & VFS_SYMLINK)   ? 0777u
                : (attr & ATTR_READ_ONLY) ? 0444u : 0644u;
        vfs_dir_append(dir, n);
    }
    return 0;
}

static vfs_node_t* fat_readdir(vfs_node_t* node, uint32_t index) {
    populate(node);
    vfs_node_t* c = node->first_child;
    while (c && index) { c = c->next_sibling; index--; }
    return c;
}

static vfs_node_t* fat_finddir(vfs_node_t* node, const char* name) {
    populate(node);
    for (vfs_node_t* c = node->first_child; c; c = c->next_sibling) {
        if (name_eq(c->name, name)) return c;
    }
    return 0;
}

/* --- reading and writing file bytes -------------------------------------- */

/* A symbolic link's target starts eight bytes into the file. Everything
 * below works in "raw" offsets, and this is the only place the difference
 * lives. */
static uint32_t link_off(const vfs_node_t* n) {
    return (n->flags & VFS_SYMLINK) ? SYMLINK_MAGIC_LEN : 0;
}

static int32_t fat_read_raw(vfs_node_t* node, uint32_t offset, uint32_t size,
                            uint8_t* buffer) {
    uint32_t done = 0;
    while (done < size) {
        uint32_t pos = offset + done;
        uint32_t cluster = chain_nth(node->fs_cluster, pos / vol.cluster_bytes);
        if (!cluster) break;

        uint32_t within = pos % vol.cluster_bytes;
        uint32_t lba = cluster_lba(cluster) + within / BLOCK_SECTOR_SIZE;
        uint32_t sec_off = within % BLOCK_SECTOR_SIZE;
        uint32_t chunk = BLOCK_SECTOR_SIZE - sec_off;
        if (chunk > size - done) chunk = size - done;

        if (sec_off == 0 && chunk == BLOCK_SECTOR_SIZE) {
            /* Whole sectors go straight into the caller's buffer, and as
             * many consecutive ones as the cluster still holds - which is
             * what makes reading a three-megabyte DLL one transfer per
             * cluster rather than one per sector. */
            uint32_t left_in_cluster =
                (vol.cluster_bytes - within) / BLOCK_SECTOR_SIZE;
            uint32_t want = (size - done) / BLOCK_SECTOR_SIZE;
            if (want > left_in_cluster) want = left_in_cluster;
            if (blockdev_read(vol.dev, lba, want, buffer + done) != 0) break;
            done += want * BLOCK_SECTOR_SIZE;
            continue;
        }

        if (blockdev_read(vol.dev, lba, 1, sec_buf) != 0) break;
        kmemcpy(buffer + done, sec_buf + sec_off, chunk);
        done += chunk;
    }
    return (int32_t)done;
}

static uint32_t fat_read_node(vfs_node_t* node, uint32_t offset, uint32_t size,
                              uint8_t* buffer) {
    if (node->flags & VFS_DIRECTORY) return 0;
    if (offset >= node->length) return 0;
    if (size > node->length - offset) size = node->length - offset;
    if (!size) return 0;

    int32_t got = fat_read_raw(node, offset + link_off(node), size, buffer);
    return got < 0 ? 0 : (uint32_t)got;
}

static int32_t fat_write_raw(vfs_node_t* node, uint32_t offset, uint32_t size,
                             const uint8_t* buffer) {
    /* Make sure the chain reaches the last byte before writing any of it.
     * A partial extension that then fails leaves a file whose size says
     * one thing and whose chain says another, and there is no fsck here
     * to notice. */
    uint32_t end = offset + size;
    uint32_t need = (end + vol.cluster_bytes - 1) / vol.cluster_bytes;
    int r = ensure_chain(node, need);
    if (r != 0) return r;

    uint32_t done = 0;
    while (done < size) {
        uint32_t pos = offset + done;
        uint32_t cluster = chain_nth(node->fs_cluster, pos / vol.cluster_bytes);
        if (!cluster) break;

        uint32_t within = pos % vol.cluster_bytes;
        uint32_t lba = cluster_lba(cluster) + within / BLOCK_SECTOR_SIZE;
        uint32_t sec_off = within % BLOCK_SECTOR_SIZE;
        uint32_t chunk = BLOCK_SECTOR_SIZE - sec_off;
        if (chunk > size - done) chunk = size - done;

        if (sec_off == 0 && chunk == BLOCK_SECTOR_SIZE) {
            uint32_t left_in_cluster =
                (vol.cluster_bytes - within) / BLOCK_SECTOR_SIZE;
            uint32_t want = (size - done) / BLOCK_SECTOR_SIZE;
            if (want > left_in_cluster) want = left_in_cluster;
            if (blockdev_write(vol.dev, lba, want, buffer + done) != 0) break;
            done += want * BLOCK_SECTOR_SIZE;
            continue;
        }

        /* A partial sector is read-modify-write. It has to be: the rest
         * of that sector belongs to the file too. */
        if (blockdev_read(vol.dev, lba, 1, sec_buf) != 0) break;
        kmemcpy(sec_buf + sec_off, buffer + done, chunk);
        if (blockdev_write(vol.dev, lba, 1, sec_buf) != 0) break;
        done += chunk;
    }
    return (int32_t)done;
}

static int32_t fat_write(vfs_node_t* node, uint32_t offset, uint32_t size,
                         const uint8_t* buffer) {
    if (node->flags & VFS_DIRECTORY) return -EISDIR;

    int32_t done = fat_write_raw(node, offset + link_off(node), size, buffer);
    if (done <= 0) return done == 0 ? -ENOSPC : done;

    if (offset + (uint32_t)done > node->length) {
        node->length = offset + (uint32_t)done;
    }

    /* Keep a materialised copy in step, so that a file being written
     * while it is mapped does not go stale. Only within the copy's own
     * capacity - beyond that the mapping does not reach either. */
    if (node->data) {
        uint32_t lim = node->capacity;
        if (offset < lim) {
            uint32_t n = (uint32_t)done;
            if (n > lim - offset) n = lim - offset;
            kmemcpy(node->data + offset, buffer, n);
        }
    }

    /* The FAT before the directory entry, and the order is the whole
     * point. ensure_chain() marks new clusters in the cached FAT sector;
     * dirent_update() makes the entry point at them. Write the entry
     * first and a machine that stops in between leaves a file whose
     * directory entry names a cluster the FAT still calls free - which is
     * exactly what `fsck.vfat -n` found here ("/AFTER.TXT contains a free
     * cluster"), and which on a reused volume would be two files sharing
     * one cluster. This way round the worst case is a chain nothing
     * points at: space lost until the volume is checked, rather than data
     * lost. */
    if (fat_cache_flush() != 0) return -EIO;
    if (dirent_update(node) != 0) return -EIO;
    return done;
}

static int32_t fat_truncate(vfs_node_t* node, uint32_t length) {
    if (node->flags & VFS_DIRECTORY) return -EISDIR;

    uint32_t raw = length + link_off(node);

    if (length > node->length) {
        /* Growing means the new bytes must read as zero. ensure_chain
         * zeroes every cluster it allocates, so the only region that can
         * still hold something is the tail of the last cluster the file
         * already had - which may be a previous, longer version of this
         * very file. */
        uint32_t old_raw = node->length + link_off(node);
        uint32_t old_alloc_end = chain_length(node->fs_cluster) *
                                 vol.cluster_bytes;
        uint32_t need = (raw + vol.cluster_bytes - 1) / vol.cluster_bytes;
        int r = ensure_chain(node, need);
        if (r != 0) return r;

        uint32_t gap_end = raw < old_alloc_end ? raw : old_alloc_end;
        if (gap_end > old_raw) {
            static uint8_t zeros[BLOCK_SECTOR_SIZE];
            kmemset(zeros, 0, sizeof(zeros));
            uint32_t p = old_raw;
            while (p < gap_end) {
                uint32_t n = gap_end - p;
                if (n > BLOCK_SECTOR_SIZE) n = BLOCK_SECTOR_SIZE;
                if (fat_write_raw(node, p, n, zeros) < 0) return -EIO;
                p += n;
            }
        }
    } else if (length < node->length) {
        /* Shrinking frees whole clusters past the new end. The head of
         * the chain is kept even for a zero-length file only when it is
         * needed; an empty file has no clusters at all, which is what the
         * on-disk format means by a first cluster of zero. */
        uint32_t keep = (raw + vol.cluster_bytes - 1) / vol.cluster_bytes;
        if (keep == 0) {
            free_chain(node->fs_cluster);
            node->fs_cluster = 0;
        } else {
            uint32_t last = chain_nth(node->fs_cluster, keep - 1);
            if (last) {
                uint32_t rest = fat_get(last);
                if (rest >= 2 && rest < FAT_BAD) {
                    fat_set(last, FAT_MASK);
                    free_chain(rest);
                }
            }
        }
    }

    node->length = length;
    /* Same order, same reason - see fat_write(). On a shrink it means the
     * chain is cut before the size follows, so an interrupted truncate
     * leaves a file longer than its chain; a checker resolves that by
     * shortening the file, which is what the operation was doing. */
    if (fat_cache_flush() != 0) return -EIO;
    if (dirent_update(node) != 0) return -EIO;
    return 0;
}

/* Brings the whole file into the heap, for mmap and exec.
 *
 * Page-aligned and rounded up to a page, which is what
 * vfs_make_mappable() would otherwise have to redo - and redoing it means
 * a second allocation the size of the file and a copy between them. On a
 * nine-megabyte shell32.dll that is nine megabytes of heap and a nine
 * megabyte memcpy saved, per DLL, and wineboot maps a few dozen. Setting
 * `mappable` here is what tells vfs_make_mappable there is nothing left
 * to do. */
static int fat_materialize(vfs_node_t* node) {
    if (node->data) return 1;

    uint32_t cap = ((node->length ? node->length : 1) + 4095u) & ~4095u;
    uint8_t* raw = (uint8_t*)kmalloc(cap + 4096u);
    if (!raw) return 0;
    uint8_t* buf = (uint8_t*)(((uint32_t)raw + 4095u) & ~4095u);

    if (node->length) {
        fat_read_raw(node, link_off(node), node->length, buf);
    }
    /* The tail of the last page has to read as zero: a PE image's last
     * section is almost never a whole page long, and the loader maps the
     * page anyway. */
    kmemset(buf + node->length, 0, cap - node->length);

    node->data = buf;
    node->data_base = raw;
    node->capacity = cap;
    node->mappable = 1;
    node->from_initrd = 0;
    vfs_heap_account((int32_t)cap);
    return 1;
}

/* --- creating and removing entries --------------------------------------- */

/* Finds `count` consecutive free entries in a directory, extending it by
 * a cluster if there are none. Returns the index of the first, or -1. */
static int32_t dir_find_slots(vfs_node_t* dir, uint32_t count) {
    uint32_t per_cluster =
        (BLOCK_SECTOR_SIZE / DIRENT_SIZE) * vol.sectors_per_cluster;
    uint32_t clusters = chain_length(dir->fs_cluster);
    uint32_t total = clusters * per_cluster;

    /* A run of `count` consecutive free entries. Free means either
     * deleted (0xE5) or never used (0x00) - and the first never-used
     * entry means every entry after it is free too, which is what makes
     * appending to a directory cheap. */
    uint32_t run = 0;
    uint8_t ent[DIRENT_SIZE];
    for (uint32_t i = 0; i < total; i++) {
        if (dir_entry_at(dir->fs_cluster, i, ent, 0) != 0) break;
        if (ent[0] == ENTRY_END) {
            if (run + (total - i) >= count) return (int32_t)(i - run);
            run += total - i;
            break;
        }
        if (ent[0] == ENTRY_FREE) {
            run++;
            if (run >= count) return (int32_t)(i + 1 - count);
        } else {
            run = 0;
        }
    }

    /* Not enough room, so the directory grows. The free tail already
     * found is reused, and only the shortfall costs new clusters. Every
     * new cluster is zeroed, so its entries all read as "end of
     * directory" - which is the invariant the scan above relies on. */
    uint32_t start = total - run;
    uint32_t shortfall = count - run;
    uint32_t add = (shortfall + per_cluster - 1) / per_cluster;
    if (ensure_chain(dir, clusters + add) != 0) return -1;
    if (dir->fs_dirty) dirent_update(dir);
    return (int32_t)start;
}

/* Writes a name's directory entries - the long ones and then the short
 * one - starting at `index`. */
static int dir_write_entries(vfs_node_t* dir, uint32_t index,
                             const char* name, const uint8_t* short11,
                             uint8_t ntflags, uint8_t attr, uint32_t cluster,
                             uint32_t size, uint32_t lfn_count,
                             dir_pos_t* short_pos) {
    uint8_t ent[DIRENT_SIZE];
    uint8_t sum = short_checksum(short11);
    uint32_t namelen = kstrlen(name);

    for (uint32_t s = 0; s < lfn_count; s++) {
        uint32_t ord = lfn_count - s;          /* written last-fragment first */
        kmemset(ent, 0, DIRENT_SIZE);
        ent[0] = (uint8_t)(ord | (s == 0 ? 0x40u : 0u));
        ent[11] = ATTR_LFN;
        ent[12] = 0;
        ent[13] = sum;
        /* ent[26..27] - the first-cluster field an old DOS would read -
         * must be zero in a long-name entry. kmemset already did it. */
        uint32_t base = (ord - 1) * 13;
        for (uint32_t k = 0; k < 13; k++) {
            uint16_t ch;
            if (base + k < namelen)      ch = (uint16_t)(uint8_t)name[base + k];
            else if (base + k == namelen) ch = 0x0000;
            else                          ch = 0xFFFF;
            wr16(ent + lfn_offsets[k], ch);
        }
        dir_pos_t p;
        uint8_t tmp[DIRENT_SIZE];
        if (dir_entry_at(dir->fs_cluster, index + s, tmp, &p) != 0) return -EIO;
        if (dir_entry_put(&p, ent) != 0) return -EIO;
    }

    kmemset(ent, 0, DIRENT_SIZE);
    kmemcpy(ent, short11, 11);
    ent[11] = attr;
    ent[12] = ntflags;
    wr16(ent + 20, (uint16_t)(cluster >> 16));
    wr16(ent + 26, (uint16_t)(cluster & 0xFFFF));
    wr32(ent + 28, size);

    rtc_time_t t;
    rtc_read(&t);
    uint16_t fdate = (uint16_t)((((uint32_t)t.year - 1980u) & 0x7Fu) << 9 |
                                ((uint32_t)t.month & 0x0Fu) << 5 |
                                ((uint32_t)t.day & 0x1Fu));
    uint16_t ftime = (uint16_t)(((uint32_t)t.hour & 0x1Fu) << 11 |
                                ((uint32_t)t.minute & 0x3Fu) << 5 |
                                (((uint32_t)t.second / 2u) & 0x1Fu));
    wr16(ent + 14, ftime);
    wr16(ent + 16, fdate);
    wr16(ent + 18, fdate);
    wr16(ent + 22, ftime);
    wr16(ent + 24, fdate);

    dir_pos_t p;
    uint8_t tmp[DIRENT_SIZE];
    if (dir_entry_at(dir->fs_cluster, index + lfn_count, tmp, &p) != 0) return -EIO;
    if (dir_entry_put(&p, ent) != 0) return -EIO;
    if (short_pos) *short_pos = p;
    return 0;
}

/* Is this 11-byte short name already used in the directory? */
static int short_name_taken(vfs_node_t* dir, const uint8_t* short11) {
    uint8_t ent[DIRENT_SIZE];
    for (uint32_t i = 0; ; i++) {
        if (dir_entry_at(dir->fs_cluster, i, ent, 0) != 0) break;
        if (ent[0] == ENTRY_END) break;
        if (ent[0] == ENTRY_FREE) continue;
        if ((ent[11] & ATTR_LFN) == ATTR_LFN) continue;
        if (kmemcmp(ent, short11, 11) == 0) return 1;
    }
    return 0;
}

/* Builds "LONGNA~1" style short names until one is unused. The tail is
 * what makes it unique, and it has to be, because the short name is still
 * how the entry is identified on disk. */
static int generate_short(vfs_node_t* dir, const char* name, uint8_t* out11) {
    /* The extension of the long name, uppercased, keeps working: a tool
     * that only reads short names still sees a .DLL as a .DLL. */
    int dot = -1;
    uint32_t len = kstrlen(name);
    for (uint32_t i = 0; i < len; i++) if (name[i] == '.') dot = (int)i;

    uint8_t base[8];
    kmemset(base, ' ', sizeof(base));
    uint32_t bw = 0;
    for (uint32_t i = 0; i < len && bw < 6; i++) {
        if (dot >= 0 && (int)i >= dot) break;
        char c = to_upper(name[i]);
        if (c == ' ' || c == '.') continue;
        base[bw++] = short_char_ok(c) ? (uint8_t)c : (uint8_t)'_';
    }
    if (!bw) base[bw++] = '_';

    uint8_t ext[3];
    kmemset(ext, ' ', sizeof(ext));
    if (dot >= 0) {
        uint32_t ew = 0;
        for (uint32_t i = (uint32_t)dot + 1; i < len && ew < 3; i++) {
            char c = to_upper(name[i]);
            ext[ew++] = short_char_ok(c) ? (uint8_t)c : (uint8_t)'_';
        }
    }

    for (uint32_t n = 1; n < 1000000u; n++) {
        char tail[8];
        uint32_t tl = 0;
        uint32_t v = n;
        char digits[7];
        uint32_t dn = 0;
        while (v) { digits[dn++] = (char)('0' + v % 10); v /= 10; }
        if (!dn) digits[dn++] = '0';
        tail[tl++] = '~';
        while (dn) tail[tl++] = digits[--dn];

        kmemset(out11, ' ', 11);
        uint32_t keep = tl > 8 ? 0 : 8 - tl;
        if (keep > bw) keep = bw;
        for (uint32_t i = 0; i < keep; i++) out11[i] = base[i];
        for (uint32_t i = 0; i < tl && keep + i < 8; i++) {
            out11[keep + i] = (uint8_t)tail[i];
        }
        for (uint32_t i = 0; i < 3; i++) out11[8 + i] = ext[i];

        if (!short_name_taken(dir, out11)) return 1;
    }
    return 0;
}

/* Why a create failed, said out loud.
 *
 * Milestone 35. `mkdir /disk/.wine` returning nothing is what Wine sees as
 * "chdir to /disk/.wine : No such file or directory", three layers up and
 * with no clue in it, and there are five different ways to get there. A
 * line naming which one turns an afternoon into a minute. */
static vfs_node_t* create_failed(const char* why, const char* name) {
    terminal_writestring_color("[fat32] ", VGA_COLOR_LIGHT_RED);
    terminal_writestring("cannot create \"");
    terminal_writestring(name);
    terminal_writestring("\": ");
    terminal_writestring(why);
    terminal_writestring("\n");
    return 0;
}

static vfs_node_t* fat_create(vfs_node_t* dir, const char* name,
                              uint32_t flags) {
    if (kstrlen(name) >= VFS_NAME_MAX) return 0;
    populate(dir);

    uint8_t short11[11];
    uint8_t ntflags = 0;
    uint32_t lfn_count = 0;

    if (make_short(name, short11, &ntflags) && !short_name_taken(dir, short11)) {
        lfn_count = 0;
    } else {
        if (!generate_short(dir, name, short11)) {
            return create_failed("no free 8.3 short name", name);
        }
        ntflags = 0;
        lfn_count = lfn_slots_for(kstrlen(name));
    }

    int32_t slot = dir_find_slots(dir, lfn_count + 1);
    if (slot < 0) return create_failed("no room in the directory", name);

    uint32_t cluster = 0;
    uint8_t attr = ATTR_ARCHIVE;
    if (flags & VFS_DIRECTORY) {
        /* A directory always has at least one cluster, because it has to
         * hold "." and "..". */
        cluster = alloc_cluster(0);
        if (!cluster) return create_failed("no free cluster", name);
        if (zero_cluster(cluster) != 0) {
            free_chain(cluster);
            return create_failed("write error zeroing its cluster", name);
        }
        attr = ATTR_DIRECTORY;
    } else if (flags & VFS_SYMLINK) {
        attr = ATTR_SYSTEM | ATTR_ARCHIVE;
    }

    dir_pos_t short_pos;
    if (dir_write_entries(dir, (uint32_t)slot, name, short11, ntflags, attr,
                          cluster, 0, lfn_count, &short_pos) != 0) {
        if (cluster) free_chain(cluster);
        return create_failed("write error storing its directory entry", name);
    }

    vfs_node_t* n = vfs_node_alloc();
    if (!n) {
        if (cluster) free_chain(cluster);
        return 0;
    }
    kstrlcpy(n->name, name, VFS_NAME_MAX);
    fat_init_node(n, flags);
    n->length = 0;
    n->fs_cluster = cluster;
    n->fs_dirent_lba = short_pos.lba;
    n->fs_dirent_off = (uint16_t)short_pos.off;
    n->fs_slots = (uint16_t)(lfn_count + 1);
    n->mode = (flags & VFS_DIRECTORY) ? 0755u
            : (flags & VFS_SYMLINK)   ? 0777u : 0644u;
    n->populated = (flags & VFS_DIRECTORY) ? 1 : 0;   /* freshly made: empty */
    vfs_dir_append(dir, n);

    if (flags & VFS_DIRECTORY) {
        /* "." and "..", which every directory but the root must have.
         * ".." on a child of the root records cluster 0, not the root's
         * real cluster - that is the format's rule, not a shortcut. */
        uint8_t ent[DIRENT_SIZE];
        dir_pos_t p;
        uint8_t tmp[DIRENT_SIZE];

        /* A date of zero is not "no date", it is month 0 and day 0 - an
         * impossible date that a checker reports. These two entries are
         * written by hand rather than through dir_write_entries(), so
         * they need the stamp applying by hand too. */
        rtc_time_t t;
        rtc_read(&t);
        uint16_t fdate = (uint16_t)((((uint32_t)t.year - 1980u) & 0x7Fu) << 9 |
                                    ((uint32_t)t.month & 0x0Fu) << 5 |
                                    ((uint32_t)t.day & 0x1Fu));
        uint16_t ftime = (uint16_t)(((uint32_t)t.hour & 0x1Fu) << 11 |
                                    ((uint32_t)t.minute & 0x3Fu) << 5 |
                                    (((uint32_t)t.second / 2u) & 0x1Fu));

        kmemset(ent, 0, DIRENT_SIZE);
        kmemset(ent, ' ', 11);
        ent[0] = '.';
        ent[11] = ATTR_DIRECTORY;
        wr16(ent + 14, ftime); wr16(ent + 16, fdate); wr16(ent + 18, fdate);
        wr16(ent + 22, ftime); wr16(ent + 24, fdate);
        wr16(ent + 20, (uint16_t)(cluster >> 16));
        wr16(ent + 26, (uint16_t)(cluster & 0xFFFF));
        if (dir_entry_at(cluster, 0, tmp, &p) == 0) dir_entry_put(&p, ent);

        uint32_t up = (dir == vol.root) ? 0 : dir->fs_cluster;
        kmemset(ent, 0, DIRENT_SIZE);
        kmemset(ent, ' ', 11);
        ent[0] = '.'; ent[1] = '.';
        ent[11] = ATTR_DIRECTORY;
        wr16(ent + 14, ftime); wr16(ent + 16, fdate); wr16(ent + 18, fdate);
        wr16(ent + 22, ftime); wr16(ent + 24, fdate);
        wr16(ent + 20, (uint16_t)(up >> 16));
        wr16(ent + 26, (uint16_t)(up & 0xFFFF));
        if (dir_entry_at(cluster, 1, tmp, &p) == 0) dir_entry_put(&p, ent);
    } else if (flags & VFS_SYMLINK) {
        /* The magic header, so the node reads back as a link after a
         * remount. Written raw, since link_off() would skip it. */
        if (fat_write_raw(n, 0, SYMLINK_MAGIC_LEN,
                          (const uint8_t*)SYMLINK_MAGIC) < 0) {
            vfs_dir_remove(dir, n);
            vfs_node_release(n);
            return 0;
        }
        dirent_update(n);
    }

    fat_cache_flush();
    return n;
}

/* Marks a name's entries free. The bytes of the file are not touched -
 * freeing the cluster chain is what actually releases the space. */
static int dir_free_entries(vfs_node_t* dir, vfs_node_t* victim) {
    /* Work backwards from the short entry, which is the one whose
     * location was recorded. The long entries directly precede it. */
    uint8_t ent[DIRENT_SIZE];
    dir_pos_t pos;

    for (uint32_t i = 0; ; i++) {
        if (dir_entry_at(dir->fs_cluster, i, ent, &pos) != 0) break;
        if (ent[0] == ENTRY_END) break;
        if (pos.lba == victim->fs_dirent_lba &&
            pos.off == victim->fs_dirent_off) {
            uint32_t slots = victim->fs_slots ? victim->fs_slots : 1;
            uint32_t start = i + 1 >= slots ? i + 1 - slots : 0;
            for (uint32_t k = start; k <= i; k++) {
                uint8_t e[DIRENT_SIZE];
                dir_pos_t p;
                if (dir_entry_at(dir->fs_cluster, k, e, &p) != 0) return -EIO;
                e[0] = ENTRY_FREE;
                if (dir_entry_put(&p, e) != 0) return -EIO;
            }
            return 0;
        }
    }
    return -ENOENT;
}

static int32_t fat_unlink(vfs_node_t* dir, const char* name, int want_dir) {
    populate(dir);
    vfs_node_t* victim = fat_finddir(dir, name);
    if (!victim) return -ENOENT;

    int is_dir = (victim->flags & VFS_DIRECTORY) != 0;
    if (want_dir && !is_dir) return -ENOTDIR;
    if (!want_dir && is_dir) return -EISDIR;
    if (is_dir) {
        populate(victim);
        if (victim->first_child) return -ENOTEMPTY;
    }

    int32_t r = dir_free_entries(dir, victim);
    if (r != 0) return r;

    if (victim->fs_cluster >= 2) free_chain(victim->fs_cluster);
    victim->fs_cluster = 0;
    victim->fs_dirent_lba = 0;

    vfs_dir_remove(dir, victim);
    victim->unlinked = 1;
    if (victim->refs == 0) vfs_node_release(victim);
    fat_cache_flush();
    return 0;
}

static int32_t fat_rename(vfs_node_t* olddir, const char* oldname,
                          vfs_node_t* newdir, const char* newname) {
    populate(olddir);
    populate(newdir);

    vfs_node_t* moving = fat_finddir(olddir, oldname);
    if (!moving) return -ENOENT;
    if (kstrlen(newname) >= VFS_NAME_MAX) return -ENAMETOOLONG;

    vfs_node_t* victim = fat_finddir(newdir, newname);
    if (victim) {
        if (victim == moving) return 0;
        if (victim->flags & VFS_DIRECTORY) return -EISDIR;
        int32_t r = fat_unlink(newdir, newname, 0);
        if (r != 0) return r;
    }

    /* A rename on FAT is: write a new set of entries pointing at the same
     * cluster chain, then free the old ones. The chain is never touched,
     * so no data moves however far the file travels in the tree. */
    uint8_t short11[11];
    uint8_t ntflags = 0;
    uint32_t lfn_count = 0;
    if (make_short(newname, short11, &ntflags) &&
        !short_name_taken(newdir, short11)) {
        lfn_count = 0;
    } else {
        if (!generate_short(newdir, newname, short11)) return -ENOSPC;
        ntflags = 0;
        lfn_count = lfn_slots_for(kstrlen(newname));
    }

    int32_t slot = dir_find_slots(newdir, lfn_count + 1);
    if (slot < 0) return -ENOSPC;

    uint8_t attr = (moving->flags & VFS_DIRECTORY) ? ATTR_DIRECTORY
                 : (moving->flags & VFS_SYMLINK)   ? (ATTR_SYSTEM | ATTR_ARCHIVE)
                                                   : ATTR_ARCHIVE;
    uint32_t raw_len = moving->length;
    if (moving->flags & VFS_SYMLINK) raw_len += SYMLINK_MAGIC_LEN;
    if (moving->flags & VFS_DIRECTORY) raw_len = 0;

    dir_pos_t short_pos;
    if (dir_write_entries(newdir, (uint32_t)slot, newname, short11, ntflags,
                          attr, moving->fs_cluster, raw_len, lfn_count,
                          &short_pos) != 0) {
        return -EIO;
    }

    int32_t r = dir_free_entries(olddir, moving);
    if (r != 0) return r;

    vfs_dir_remove(olddir, moving);
    kstrlcpy(moving->name, newname, VFS_NAME_MAX);
    moving->fs_dirent_lba = short_pos.lba;
    moving->fs_dirent_off = (uint16_t)short_pos.off;
    moving->fs_slots = (uint16_t)(lfn_count + 1);
    vfs_dir_append(newdir, moving);

    /* A directory that moved has a ".." pointing at its old parent. */
    if (moving->flags & VFS_DIRECTORY) {
        uint8_t ent[DIRENT_SIZE];
        dir_pos_t p;
        if (dir_entry_at(moving->fs_cluster, 1, ent, &p) == 0 &&
            ent[0] == '.' && ent[1] == '.') {
            uint32_t up = (newdir == vol.root) ? 0 : newdir->fs_cluster;
            wr16(ent + 20, (uint16_t)(up >> 16));
            wr16(ent + 26, (uint16_t)(up & 0xFFFF));
            dir_entry_put(&p, ent);
        }
    }

    fat_cache_flush();
    return 0;
}

static void fat_forget(vfs_node_t* node) {
    (void)node;   /* nothing hangs off a FAT node but the fields in it */
}

static const vfs_ops_t fat_ops = {
    fat_write,
    fat_truncate,
    fat_create,
    fat_unlink,
    fat_rename,
    fat_materialize,
    fat_forget,
    0,              /* open: a file on disk is what it is */
    0,              /* ioctl: not a device */
};

/* --- mounting ------------------------------------------------------------- */

/* Reads and validates a FAT32 boot sector at `lba`. */
static int read_bpb(blockdev_t* dev, uint32_t lba) {
    uint8_t bs[BLOCK_SECTOR_SIZE];
    if (blockdev_read(dev, lba, 1, bs) != 0) return -EIO;
    if (rd16(bs + 510) != 0xAA55u) return -EINVAL;

    uint32_t bps = rd16(bs + 11);
    uint32_t spc = bs[13];
    uint32_t reserved = rd16(bs + 14);
    uint32_t nfats = bs[16];
    uint32_t root_entries = rd16(bs + 17);
    uint32_t fat16_size = rd16(bs + 22);
    uint32_t total16 = rd16(bs + 19);
    uint32_t total32 = rd32(bs + 32);
    uint32_t fat32_size = rd32(bs + 36);
    uint32_t root_cluster = rd32(bs + 44);
    uint32_t fsinfo = rd16(bs + 48);

    /* The four things that make it FAT32 rather than FAT16 or noise. */
    if (bps != BLOCK_SECTOR_SIZE) return -EINVAL;
    if (spc == 0 || spc > 128 || (spc & (spc - 1))) return -EINVAL;
    if (nfats == 0 || nfats > 4) return -EINVAL;
    if (root_entries != 0 || fat16_size != 0) return -EINVAL;
    if (fat32_size == 0 || reserved == 0) return -EINVAL;
    if (root_cluster < 2) return -EINVAL;

    uint32_t total = total32 ? total32 : total16;
    if (!total) return -EINVAL;

    vol.dev = dev;
    vol.part_lba = lba;
    vol.sectors_per_cluster = spc;
    vol.cluster_bytes = spc * BLOCK_SECTOR_SIZE;
    vol.reserved = reserved;
    vol.num_fats = nfats;
    vol.fat_sectors = fat32_size;
    vol.root_cluster = root_cluster;
    vol.fat_lba = lba + reserved;
    vol.data_lba = vol.fat_lba + nfats * fat32_size;
    vol.fsinfo_lba = fsinfo ? lba + fsinfo : 0;

    if (total <= reserved + nfats * fat32_size) return -EINVAL;
    uint32_t data_sectors = total - (reserved + nfats * fat32_size);
    vol.total_clusters = data_sectors / spc;
    if (!vol.total_clusters) return -EINVAL;

    /* The FAT has to be big enough to hold an entry per cluster. A volume
     * where it is not was built wrong, and trusting the cluster count
     * over the FAT's size would mean allocating clusters whose entries
     * live off the end of the table. */
    if (vol.total_clusters + 2 > fat32_size * (BLOCK_SECTOR_SIZE / 4)) {
        vol.total_clusters = fat32_size * (BLOCK_SECTOR_SIZE / 4) - 2;
    }
    return 0;
}

/* An MBR is four 16-byte entries at offset 446, each with a type byte.
 * 0x0B and 0x0C are FAT32 (CHS and LBA addressed respectively); 0x0E is
 * FAT16 LBA and is not accepted, because this driver would misread it. */
static int find_partition(blockdev_t* dev, uint32_t* lba_out) {
    uint8_t mbr[BLOCK_SECTOR_SIZE];
    if (blockdev_read(dev, 0, 1, mbr) != 0) return 0;
    if (rd16(mbr + 510) != 0xAA55u) return 0;

    for (uint32_t i = 0; i < 4; i++) {
        const uint8_t* e = mbr + 446 + i * 16;
        uint8_t type = e[4];
        uint32_t start = rd32(e + 8);
        uint32_t count = rd32(e + 12);
        if ((type == 0x0B || type == 0x0C) && start && count) {
            *lba_out = start;
            return 1;
        }
    }
    return 0;
}

int fat32_mount(vfs_node_t* dir, blockdev_t* dev) {
    if (!dir || !dev) return -EINVAL;
    if (!(dir->flags & VFS_DIRECTORY)) return -ENOTDIR;
    if (dir->first_child) return -EBUSY;
    if (vol.mounted) return -EBUSY;

    kmemset(&vol, 0, sizeof(vol));

    /* An image with a partition table gets the first FAT32 partition; one
     * without gets mounted whole. Both are things the build produces, and
     * distinguishing them is two reads. */
    uint32_t lba = 0;
    int r;
    if (find_partition(dev, &lba)) {
        r = read_bpb(dev, lba);
    } else {
        r = read_bpb(dev, 0);
    }
    if (r != 0) return r;

    clus_buf = (uint8_t*)kmalloc(vol.cluster_bytes);
    if (!clus_buf) return -ENOMEM;

    fat_cache_lba = 0;
    fat_cache_dirty = 0;
    vol.free_count = 0xFFFFFFFFu;
    vol.free_hint = 2;
    vol.label[0] = '\0';

    /* The mount point becomes the volume's root: same name, same parent,
     * new behaviour. */
    dir->fs_cluster = vol.root_cluster;
    dir->populated = 0;
    fat_init_node(dir, VFS_DIRECTORY);
    dir->mode = 0755u;
    vol.root = dir;
    vol.mounted = 1;

    /* Read the root now rather than on first use, so that a volume this
     * driver cannot make sense of fails at mount time where it can be
     * reported, instead of looking like an empty disk. */
    populate(dir);

    /* The FSInfo block's free-cluster hint, if the volume has one and it
     * is plausible. It is only a hint - the specification says so - and a
     * wrong one costs a scan, not correctness. */
    if (vol.fsinfo_lba) {
        uint8_t fi[BLOCK_SECTOR_SIZE];
        if (blockdev_read(dev, vol.fsinfo_lba, 1, fi) == 0 &&
            rd32(fi) == 0x41615252u && rd32(fi + 484) == 0x61417272u) {
            uint32_t freec = rd32(fi + 488);
            uint32_t next = rd32(fi + 492);
            if (freec <= vol.total_clusters) vol.free_count = freec;
            if (next >= 2 && next < vol.total_clusters + 2) vol.free_hint = next;
        }
    }
    return 0;
}

int fat32_mount_auto(void) {
    if (vol.mounted) return 0;
    vfs_node_t* mp = vfs_lookup("/disk");
    if (!mp) return -ENOENT;

    for (uint32_t i = 0; i < blockdev_count(); i++) {
        blockdev_t* dev = blockdev_get(i);
        if (!dev) continue;
        if (fat32_mount(mp, dev) == 0) return 0;
    }
    return -ENODEV;
}

int fat32_mounted(void) { return vol.mounted; }

const char* fat32_device_name(void) {
    return vol.mounted && vol.dev ? vol.dev->name : "";
}

const char* fat32_volume_label(void) { return vol.label; }

uint32_t fat32_cluster_bytes(void) { return vol.cluster_bytes; }

void fat32_space(uint32_t* total, uint32_t* freec) {
    if (!vol.mounted) {
        if (total) *total = 0;
        if (freec) *freec = 0;
        return;
    }
    if (vol.free_count == 0xFFFFFFFFu) count_free();
    if (total) *total = vol.total_clusters;
    if (freec) *freec = vol.free_count;
}

int fat32_sync(void) {
    if (!vol.mounted) return 0;
    if (fat_cache_flush() != 0) return -EIO;

    if (vol.fsinfo_lba) {
        uint8_t fi[BLOCK_SECTOR_SIZE];
        if (blockdev_read(vol.dev, vol.fsinfo_lba, 1, fi) == 0 &&
            rd32(fi) == 0x41615252u) {
            wr32(fi + 488, vol.free_count);
            wr32(fi + 492, vol.free_hint);
            if (blockdev_write(vol.dev, vol.fsinfo_lba, 1, fi) != 0) return -EIO;
        }
    }
    return 0;
}

/* Drops a subtree of cached nodes. Used by unmount; refuses if anything
 * still holds a reference, because a descriptor open on a volume that has
 * gone away is exactly the dangling pointer this project spent Milestone
 * 30 removing. */
static int drop_subtree(vfs_node_t* dir) {
    for (vfs_node_t* c = dir->first_child; c; c = c->next_sibling) {
        if (c->refs) return -EBUSY;
        if ((c->flags & VFS_DIRECTORY) && drop_subtree(c) != 0) return -EBUSY;
    }
    vfs_node_t* c = dir->first_child;
    while (c) {
        vfs_node_t* next = c->next_sibling;
        vfs_node_release(c);
        c = next;
    }
    dir->first_child = 0;
    return 0;
}

int fat32_unmount(void) {
    if (!vol.mounted) return -EINVAL;
    if (vol.root->refs) return -EBUSY;
    if (drop_subtree(vol.root) != 0) return -EBUSY;
    fat32_sync();

    vfs_node_t* mp = vol.root;
    mp->ops = 0;
    mp->fs_cluster = 0;
    mp->populated = 0;
    mp->read = 0;
    /* Back to an ordinary empty ramfs directory. The readdir and finddir
     * hooks a ramfs directory uses are private to ramfs.c, so the mount
     * point is rebuilt by making a fresh one beside it and swapping the
     * name over - which is what this does by simply asking vfs_create for
     * a new node and moving the name. */
    vfs_node_t* parent = mp->parent;
    char name[VFS_NAME_MAX];
    kstrlcpy(name, mp->name, VFS_NAME_MAX);
    vfs_dir_remove(parent, mp);
    vfs_node_release(mp);
    vfs_create(parent, name, VFS_DIRECTORY);

    if (clus_buf) { kfree(clus_buf); clus_buf = 0; }
    kmemset(&vol, 0, sizeof(vol));
    fat_cache_lba = 0;
    fat_cache_dirty = 0;
    return 0;
}
