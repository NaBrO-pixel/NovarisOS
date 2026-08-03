#!/usr/bin/env python3
"""Builds a FAT32 disk image, optionally populated from a directory.

Milestone 32. The kernel's FAT32 driver (kernel/fat32.c) can create every
structure this writes, so in principle the build could format a blank disk
at boot. It does not, for two reasons worth stating:

  - A disk image the *host* can mount is worth a great deal. `mount -o
    loop` on the result of this script, or `fsck.vfat -n`, is an
    independent check on the driver that no amount of self-consistency
    testing provides.

  - Populating a prefix from the initrd means shipping it in the initrd,
    which is exactly the thing this milestone exists to stop doing.

Reproducible on purpose: a fixed timestamp and a deterministic allocation
order, so that two runs over the same tree give byte-identical images and
a diff means something changed.

Usage:
    mkfat32.py OUT.img --size 256M [--label NAME] [--mbr] [DIR]

Symbolic links in DIR are written as this kernel stores them: a file with
the SYSTEM attribute whose contents are "NVSYMLNK" followed by the target.
See include/fat32.h.
"""

import argparse
import os
import struct
import sys

SECTOR = 512
DIRENT = 32

ATTR_READ_ONLY = 0x01
ATTR_HIDDEN = 0x02
ATTR_SYSTEM = 0x04
ATTR_VOLUME_ID = 0x08
ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE = 0x20
ATTR_LFN = 0x0F

FAT_EOC = 0x0FFFFFFF
SYMLINK_MAGIC = b"NVSYMLNK"

# 1980-01-01 00:00:00, the earliest a FAT timestamp can express. Fixed so
# the image is reproducible; nothing in this project reads it back for
# anything but display.
FIXED_DATE = (1 << 5) | 1
FIXED_TIME = 0

SHORT_OK = set(b"$%'-_@~`!(){}^#&")


def parse_size(text):
    mult = 1
    t = text.strip().upper()
    if t.endswith("K"):
        mult, t = 1024, t[:-1]
    elif t.endswith("M"):
        mult, t = 1024 * 1024, t[:-1]
    elif t.endswith("G"):
        mult, t = 1024 * 1024 * 1024, t[:-1]
    return int(t) * mult


def choose_spc(total_sectors, reserved, num_fats):
    """Sectors per cluster.

    Picked to keep the cluster count above 65525, which is what makes the
    volume a FAT32 one rather than a FAT16 wearing FAT32's boot sector.
    Larger clusters mean a smaller FAT and fewer chain walks, so the
    largest that still clears the bar wins.
    """
    best = 1
    for spc in (1, 2, 4, 8, 16, 32, 64):
        # The FAT has to hold an entry per cluster, so its size and the
        # cluster count are mutually dependent; solve it by iterating.
        fat_sectors = 1
        for _ in range(8):
            data = total_sectors - reserved - num_fats * fat_sectors
            if data <= 0:
                break
            clusters = data // spc
            need = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
            if need == fat_sectors:
                break
            fat_sectors = need
        data = total_sectors - reserved - num_fats * fat_sectors
        if data <= 0:
            continue
        clusters = data // spc
        if clusters > 65525:
            best = spc
    return best


def fat_sectors_for(total_sectors, reserved, num_fats, spc):
    fat_sectors = 1
    for _ in range(16):
        data = total_sectors - reserved - num_fats * fat_sectors
        clusters = max(data // spc, 0)
        need = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
        if need == fat_sectors:
            break
        fat_sectors = need
    return fat_sectors


def lfn_checksum(short11):
    s = 0
    for c in short11:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s


# Where the 13 name characters live inside a long-name entry. Same table
# as kernel/fat32.c, and for the same reason: the offsets dodge the fields
# an old DOS would interpret.
LFN_OFFSETS = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)


def short_char(c):
    b = ord(c.upper())
    if (0x41 <= b <= 0x5A) or (0x30 <= b <= 0x39) or b in SHORT_OK:
        return chr(b)
    return "_"


def make_short(name, taken):
    """An 8.3 name, plus the NT case flags and whether LFN entries are
    needed. Mirrors make_short()/generate_short() in kernel/fat32.c."""
    base, _, ext = name.rpartition(".")
    if not base:
        base, ext = name, ""

    def simple(part, limit):
        if len(part) > limit:
            return None
        lower = any(c.islower() for c in part)
        upper = any(c.isupper() for c in part)
        if lower and upper:
            return None
        for c in part:
            if short_char(c) != c.upper():
                return None
        return lower

    if "." not in name or name.count(".") == 1:
        bl = simple(base, 8)
        el = simple(ext, 3)
        if base and bl is not None and el is not None:
            s11 = (base.upper().ljust(8) + ext.upper().ljust(3)).encode("ascii")
            if s11 not in taken:
                nt = (0x08 if bl else 0) | (0x10 if el else 0)
                return s11, nt, False

    stem = "".join(short_char(c) for c in base if c not in " .")[:6] or "_"
    ext3 = "".join(short_char(c) for c in ext)[:3]
    for n in range(1, 1000000):
        tail = "~%d" % n
        keep = max(0, 8 - len(tail))
        cand = (stem[:keep] + tail).ljust(8) + ext3.ljust(3)
        s11 = cand.encode("ascii")
        if s11 not in taken:
            return s11, 0, True
    raise RuntimeError("no free short name for %r" % name)


class Image:
    def __init__(self, total_sectors, label, mbr):
        self.mbr = mbr
        self.part_lba = 2048 if mbr else 0
        self.total_sectors = total_sectors
        self.vol_sectors = total_sectors - self.part_lba
        self.reserved = 32
        self.num_fats = 2
        self.spc = choose_spc(self.vol_sectors, self.reserved, self.num_fats)
        self.fat_sectors = fat_sectors_for(
            self.vol_sectors, self.reserved, self.num_fats, self.spc
        )
        self.data_start = self.reserved + self.num_fats * self.fat_sectors
        self.clusters = (self.vol_sectors - self.data_start) // self.spc
        # The FAT is sized in whole sectors, so it can end up able to
        # address slightly fewer clusters than the data area holds. The
        # table is the authority - kernel/fat32.c clamps the same way -
        # and the volume is then declared *shorter* so that the two agree
        # exactly. Leaving the declared length alone is what made
        # `fsck.vfat -n` say "filesystem has 129024 clusters but only
        # space for 129022 FAT entries": true, and a real defect, since
        # anything that trusted the boot sector over the table would
        # allocate two clusters whose entries live off the end of it.
        self.clusters = min(self.clusters, self.fat_sectors * SECTOR // 4 - 2)
        self.vol_sectors = self.data_start + self.clusters * self.spc
        if self.clusters < 4:
            raise SystemExit("image too small to hold a filesystem")
        self.cluster_bytes = self.spc * SECTOR
        self.label = (label or "NOVARIS")[:11].upper().ljust(11)

        self.buf = bytearray(total_sectors * SECTOR)
        self.fat = [0] * (self.clusters + 2)
        self.fat[0] = 0x0FFFFFF8
        self.fat[1] = 0x0FFFFFFF
        self.next_free = 2

    # --- allocation ------------------------------------------------------
    def alloc(self, count):
        if self.next_free + count > self.clusters + 2:
            raise SystemExit(
                "image full: needs more than %d clusters" % self.clusters
            )
        chain = list(range(self.next_free, self.next_free + count))
        self.next_free += count
        for i, c in enumerate(chain):
            self.fat[c] = FAT_EOC if i == len(chain) - 1 else chain[i + 1]
        return chain

    def cluster_offset(self, cluster):
        lba = self.part_lba + self.data_start + (cluster - 2) * self.spc
        return lba * SECTOR

    def write_chain(self, chain, data):
        for i, c in enumerate(chain):
            off = self.cluster_offset(c)
            piece = data[i * self.cluster_bytes : (i + 1) * self.cluster_bytes]
            self.buf[off : off + len(piece)] = piece

    def store(self, data):
        """Writes file contents, returning the first cluster (0 if empty)."""
        if not data:
            return 0
        n = (len(data) + self.cluster_bytes - 1) // self.cluster_bytes
        chain = self.alloc(n)
        self.write_chain(chain, data)
        return chain[0]

    # --- directories -----------------------------------------------------
    @staticmethod
    def dirent(short11, attr, nt, cluster, size):
        e = bytearray(DIRENT)
        e[0:11] = short11
        e[11] = attr
        e[12] = nt
        struct.pack_into("<H", e, 14, FIXED_TIME)
        struct.pack_into("<H", e, 16, FIXED_DATE)
        struct.pack_into("<H", e, 18, FIXED_DATE)
        struct.pack_into("<H", e, 20, (cluster >> 16) & 0xFFFF)
        struct.pack_into("<H", e, 22, FIXED_TIME)
        struct.pack_into("<H", e, 24, FIXED_DATE)
        struct.pack_into("<H", e, 26, cluster & 0xFFFF)
        struct.pack_into("<I", e, 28, size)
        return bytes(e)

    @staticmethod
    def lfn_entries(name, short11):
        sum_ = lfn_checksum(short11)
        chars = name.encode("utf-16-le")
        n = (len(name) + 12) // 13
        out = []
        for ord_ in range(n, 0, -1):
            e = bytearray(DIRENT)
            e[0] = ord_ | (0x40 if ord_ == n else 0)
            e[11] = ATTR_LFN
            e[13] = sum_
            base = (ord_ - 1) * 13
            for k in range(13):
                i = base + k
                if i < len(name):
                    ch = struct.unpack_from("<H", chars, i * 2)[0]
                elif i == len(name):
                    ch = 0x0000
                else:
                    ch = 0xFFFF
                struct.pack_into("<H", e, LFN_OFFSETS[k], ch)
            out.append(bytes(e))
        return b"".join(out)

    def build_dir(self, path, parent_cluster, is_root):
        try:
            names = sorted(os.listdir(path)) if path else []
        except OSError as exc:
            raise SystemExit("cannot read %s: %s" % (path, exc))

        # The size of a directory is known before its contents are, which
        # is what lets its cluster be allocated first - and it has to be
        # first, because every child directory's ".." records it.
        taken = set()
        planned = []
        for name in names:
            short11, nt, need_lfn = make_short(name, taken)
            taken.add(short11)
            planned.append((name, short11, nt, need_lfn))

        slots = 0 if is_root else 2
        if is_root:
            slots += 1  # the volume label entry
        for name, _s, _n, need_lfn in planned:
            slots += ((len(name) + 12) // 13 if need_lfn else 0) + 1

        nbytes = max(slots * DIRENT, 1)
        nclusters = max(
            (nbytes + self.cluster_bytes - 1) // self.cluster_bytes, 1
        )
        chain = self.alloc(nclusters)
        me = chain[0]

        out = bytearray()
        if is_root:
            out += self.dirent(
                self.label.encode("ascii"), ATTR_VOLUME_ID, 0, 0, 0
            )
        else:
            out += self.dirent(b".          ", ATTR_DIRECTORY, 0, me, 0)
            # A child of the root records 0, not the root's real cluster.
            up = 0 if parent_cluster == self.root_cluster else parent_cluster
            out += self.dirent(b"..         ", ATTR_DIRECTORY, 0, up, 0)

        for name, short11, nt, need_lfn in planned:
            full = os.path.join(path, name)
            if os.path.islink(full):
                target = os.readlink(full).encode("utf-8")
                data = SYMLINK_MAGIC + target
                first = self.store(data)
                attr, size = ATTR_SYSTEM | ATTR_ARCHIVE, len(data)
            elif os.path.isdir(full):
                first = self.build_dir(full, me, False)
                attr, size = ATTR_DIRECTORY, 0
            else:
                with open(full, "rb") as fh:
                    data = fh.read()
                first = self.store(data)
                attr, size = ATTR_ARCHIVE, len(data)

            if need_lfn:
                out += self.lfn_entries(name, short11)
            out += self.dirent(short11, attr, nt, first, size)

        self.write_chain(chain, bytes(out))
        return me

    # --- the fixed structures -------------------------------------------
    def write_boot_sector(self):
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xeb\x58\x90"          # JMP over the BPB, as DOS expects
        bs[3:11] = b"NOVARIS "
        struct.pack_into("<H", bs, 11, SECTOR)
        bs[13] = self.spc
        struct.pack_into("<H", bs, 14, self.reserved)
        bs[16] = self.num_fats
        struct.pack_into("<H", bs, 17, 0)   # root entries: FAT32 has none
        struct.pack_into("<H", bs, 19, 0)   # 16-bit total: too big, see +32
        bs[21] = 0xF8                        # "fixed disk"
        struct.pack_into("<H", bs, 22, 0)   # 16-bit FAT size: FAT32 uses +36
        struct.pack_into("<H", bs, 24, 63)
        struct.pack_into("<H", bs, 26, 255)
        struct.pack_into("<I", bs, 28, self.part_lba)
        struct.pack_into("<I", bs, 32, self.vol_sectors)
        struct.pack_into("<I", bs, 36, self.fat_sectors)
        struct.pack_into("<H", bs, 40, 0)   # flags: FATs mirrored
        struct.pack_into("<H", bs, 42, 0)   # version
        struct.pack_into("<I", bs, 44, self.root_cluster)
        struct.pack_into("<H", bs, 48, 1)   # FSInfo sector
        struct.pack_into("<H", bs, 50, 6)   # backup boot sector
        bs[64] = 0x80
        bs[66] = 0x29                        # extended boot signature
        struct.pack_into("<I", bs, 67, 0x4E4F5641)
        bs[71:82] = self.label.encode("ascii")
        bs[82:90] = b"FAT32   "
        struct.pack_into("<H", bs, 510, 0xAA55)

        for lba in (self.part_lba, self.part_lba + 6):
            self.buf[lba * SECTOR : lba * SECTOR + SECTOR] = bs

    def write_fsinfo(self):
        fi = bytearray(SECTOR)
        struct.pack_into("<I", fi, 0, 0x41615252)
        struct.pack_into("<I", fi, 484, 0x61417272)
        struct.pack_into("<I", fi, 488, self.clusters + 2 - self.next_free)
        struct.pack_into("<I", fi, 492, self.next_free)
        struct.pack_into("<H", fi, 510, 0xAA55)
        lba = self.part_lba + 1
        self.buf[lba * SECTOR : lba * SECTOR + SECTOR] = fi

    def write_fats(self):
        table = bytearray(self.fat_sectors * SECTOR)
        for i, v in enumerate(self.fat):
            struct.pack_into("<I", table, i * 4, v & 0x0FFFFFFF)
        for f in range(self.num_fats):
            lba = self.part_lba + self.reserved + f * self.fat_sectors
            self.buf[lba * SECTOR : lba * SECTOR + len(table)] = table

    def write_mbr(self):
        mbr = bytearray(SECTOR)
        part = bytearray(16)
        part[0] = 0x80                        # bootable
        part[1:4] = b"\xfe\xff\xff"           # CHS: "use LBA instead"
        part[4] = 0x0C                        # FAT32 LBA
        part[5:8] = b"\xfe\xff\xff"
        struct.pack_into("<I", part, 8, self.part_lba)
        struct.pack_into("<I", part, 12, self.vol_sectors)
        mbr[446:462] = part
        struct.pack_into("<H", mbr, 510, 0xAA55)
        self.buf[0:SECTOR] = mbr

    def build(self, staging):
        # The root has to land on cluster 2: nothing requires it, but every
        # tool in the world expects it and it makes the image comparable.
        self.root_cluster = 2
        root = self.build_dir(staging, 2, True)
        assert root == 2, "the root directory did not land on cluster 2"
        if self.mbr:
            self.write_mbr()
        self.write_boot_sector()
        self.write_fats()
        self.write_fsinfo()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("out")
    ap.add_argument("dir", nargs="?", default=None,
                    help="directory whose contents populate the image")
    ap.add_argument("--size", default="256M")
    ap.add_argument("--label", default="NOVARIS")
    ap.add_argument("--mbr", action="store_true",
                    help="write an MBR partition table instead of a bare "
                         "volume; the driver reads both")
    args = ap.parse_args()

    total_sectors = parse_size(args.size) // SECTOR
    if total_sectors < 128:
        raise SystemExit("--size is far too small")

    img = Image(total_sectors, args.label, args.mbr)
    img.build(args.dir)

    with open(args.out, "wb") as fh:
        fh.write(img.buf)

    used = img.next_free - 2
    print(
        "Wrote %s: %d sectors, %d bytes/cluster, %d clusters "
        "(%d used, %.1fMB free)"
        % (
            args.out,
            total_sectors,
            img.cluster_bytes,
            img.clusters,
            used,
            (img.clusters - used) * img.cluster_bytes / 1048576.0,
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
