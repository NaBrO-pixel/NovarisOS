#!/usr/bin/env python3
"""Packs a directory tree into Novaris's custom initrd archive format.

Layout (all little-endian):
    struct initrd_header       { uint32_t magic; uint32_t nfiles; }
    struct initrd_file_header[nfiles] {
        uint32_t magic; char name[124]; uint32_t offset; uint32_t length;
    }
    <raw file bytes, back to back, at the offsets above - offsets are
     relative to the very start of the archive, matching kernel/ramfs.c>

`name` is a path relative to the source directory and may contain '/'.
The kernel creates the directories on the way, so what the archive
describes is a tree rather than a heap of files.

That is Milestone 35's change, and the magic went from 'STLR' to 'STL2'
with it so that a stale image is refused rather than misread. Before it
the format had no directories and a 60-byte name field, so everything
shipped in the initrd landed at the root - including the whole of Wine,
which is why "/lib32/libc.so.6" used to resolve only through a fallback
that matched the last component of a path whose directories did not
exist. An operating system with Wine installed in it needs
/usr/lib/wine/i386-unix/ntdll.so to be at /usr/lib/wine/i386-unix, since
that is where Wine's own path arithmetic goes looking.

Usage:
    python3 mkinitrd.py <source_dir> <output.img>
"""
import os
import struct
import sys

MAGIC = 0x324C5453       # 'STL2'
FILE_MAGIC = 0xBEEFCAFE
NAME_MAX = 124


def pack(files, out_path):
    header = struct.pack('<II', MAGIC, len(files))
    entry_size = 4 + NAME_MAX + 4 + 4  # magic + name + offset + length
    data_start = len(header) + entry_size * len(files)

    entries = b''
    blobs = b''
    offset = data_start  # offsets are absolute (relative to archive start),
                          # matching kernel/ramfs.c's initrd_base + impl math
    for name, data in files:
        if len(name) >= NAME_MAX:
            raise ValueError(f'path too long for initrd: {name!r}')
        name_bytes = name.encode('ascii') + b'\x00' * (NAME_MAX - len(name))
        entries += struct.pack('<I', FILE_MAGIC) + name_bytes + struct.pack('<II', offset, len(data))
        blobs += data
        offset += len(data)

    with open(out_path, 'wb') as f:
        f.write(header)
        f.write(entries)
        f.write(blobs)


def collect(src_dir):
    """Every file under src_dir, as (relative path with '/', bytes).

    Sorted so that a build is reproducible, and so that the kernel meets a
    directory's own entries together rather than scattered."""
    files = []
    for dirpath, dirnames, filenames in os.walk(src_dir):
        dirnames.sort()
        for name in sorted(filenames):
            path = os.path.join(dirpath, name)
            if not os.path.isfile(path) or os.path.islink(path):
                continue
            rel = os.path.relpath(path, src_dir).replace(os.sep, '/')
            with open(path, 'rb') as f:
                files.append((rel, f.read()))
    return files


def main():
    if len(sys.argv) != 3:
        print(f'usage: {sys.argv[0]} <source_dir> <output.img>', file=sys.stderr)
        sys.exit(1)

    src_dir, out_path = sys.argv[1], sys.argv[2]
    files = collect(src_dir)

    pack(files, out_path)
    total = sum(len(d) for _, d in files)
    print(f'Packed {len(files)} file(s), {total} bytes, into {out_path}')
    for name, data in files:
        print(f'  {name}  ({len(data)} bytes)')


if __name__ == '__main__':
    main()
