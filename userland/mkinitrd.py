#!/usr/bin/env python3
"""Packs a directory of files into Novaris's custom initrd archive format.

Layout (all little-endian):
    struct initrd_header       { uint32_t magic; uint32_t nfiles; }
    struct initrd_file_header[nfiles] {
        uint32_t magic; char name[60]; uint32_t offset; uint32_t length;
    }
    <raw file bytes, back to back, at the offsets above - offsets are
     relative to the very start of the archive, matching kernel/initrd.c>

Usage:
    python3 mkinitrd.py <source_dir> <output.img>
"""
import os
import struct
import sys

MAGIC = 0x53544C52       # 'STLR'
FILE_MAGIC = 0xBEEFCAFE
NAME_MAX = 60


def pack(files, out_path):
    header = struct.pack('<II', MAGIC, len(files))
    entry_size = 4 + NAME_MAX + 4 + 4  # magic + name + offset + length
    data_start = len(header) + entry_size * len(files)

    entries = b''
    blobs = b''
    offset = data_start  # offsets are absolute (relative to archive start),
                          # matching kernel/initrd.c's initrd_base + impl math
    for name, data in files:
        if len(name) >= NAME_MAX:
            raise ValueError(f'filename too long for initrd: {name!r}')
        name_bytes = name.encode('ascii') + b'\x00' * (NAME_MAX - len(name))
        entries += struct.pack('<I', FILE_MAGIC) + name_bytes + struct.pack('<II', offset, len(data))
        blobs += data
        offset += len(data)

    with open(out_path, 'wb') as f:
        f.write(header)
        f.write(entries)
        f.write(blobs)


def main():
    if len(sys.argv) != 3:
        print(f'usage: {sys.argv[0]} <source_dir> <output.img>', file=sys.stderr)
        sys.exit(1)

    src_dir, out_path = sys.argv[1], sys.argv[2]
    files = []
    for name in sorted(os.listdir(src_dir)):
        path = os.path.join(src_dir, name)
        if os.path.isfile(path):
            with open(path, 'rb') as f:
                files.append((name, f.read()))

    pack(files, out_path)
    print(f'Packed {len(files)} file(s) into {out_path}:')
    for name, data in files:
        print(f'  {name}  ({len(data)} bytes)')


if __name__ == '__main__':
    main()
