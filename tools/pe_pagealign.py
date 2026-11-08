#!/usr/bin/env python3
"""pe_pagealign.py IN OUT - rewrite a PE so every section starts on a page.

Why: Wine maps a DLL's sections straight from the file only when a
section's file offset is a multiple of the page size; otherwise it
reads the section into anonymous memory (map_file_into_view in
dlls/ntdll/unix/virtual.c falls back to read()). Toolchains write PE
files with FileAlignment 0x200, so chrome.dll's 284MB .text sits at file
offset 0x600, and every Chromium process read its own 320MB copy:
millions of page faults, gigabytes of memory, most of a run's time under
emulation.

Rewritten with every section at a page-aligned file offset, the same
bytes at the same RVAs, Wine can mmap the sections, and NovarisOS shares
one copy of the pages between every process that maps them.

What changes, and nothing else:
  - each section's PointerToRawData (now page-aligned) and SizeOfRawData
    (rounded up to a page, zero-padded - what the loader zero-fills
    anyway, and never past the section's virtual size rounded up);
  - FileAlignment and SizeOfHeaders, to 0x1000;
  - the file offsets in the debug directory (PointerToRawData of each
    entry), moved with the section they point into;
  - the certificate table (Authenticode), dropped: it covers the old
    bytes and its offset would be wrong, and nothing on this system
    checks it;
  - the header checksum, recomputed.
Data after the last section that is not the certificate (an "overlay")
is kept, after the sections.

A file that is already aligned is copied unchanged. Anything this does
not understand is an error, and the input is left alone.
"""

import struct
import sys

PAGE = 0x1000


def align(v, a):
    return (v + a - 1) & ~(a - 1)


def checksum(data, csum_off):
    """The PE header checksum: 16-bit one's-complement sum of the file
    with the checksum field taken as zero, plus the file's length."""
    from array import array
    n = len(data)
    words = array("H")
    words.frombytes(bytes(data[:n - (n % 2)]))
    if sys.byteorder != "little":
        words.byteswap()
    words[csum_off // 2] = 0
    words[csum_off // 2 + 1] = 0
    total = sum(words)
    if n % 2:
        total += data[-1]
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (total + n) & 0xFFFFFFFF


def main(src, dst):
    data = bytearray(open(src, "rb").read())
    if data[:2] != b"MZ":
        raise SystemExit("%s: not a PE file" % src)
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("%s: no PE signature" % src)
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic == 0x20B:
        dd_off = opt + 112                       # PE32+
    elif magic == 0x10B:
        dd_off = opt + 96                        # PE32
    else:
        raise SystemExit("%s: unknown optional header magic %#x" % (src, magic))
    ndd = struct.unpack_from("<I", data, dd_off - 4)[0]
    sec_off = opt + opt_size
    sect_align = struct.unpack_from("<I", data, opt + 32)[0]
    if sect_align < PAGE:
        raise SystemExit("%s: SectionAlignment %#x below a page; left alone" % (src, sect_align))

    secs = []
    for i in range(nsec):
        o = sec_off + 40 * i
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, o + 8)
        secs.append([o, vsize, va, rsize, raw])

    if all(s[4] % PAGE == 0 for s in secs if s[3]):
        open(dst, "wb").write(data)
        print("pe_pagealign: %s already page-aligned" % src)
        return

    # The certificate table: a file offset, not an RVA.
    cert_off = cert_size = 0
    if ndd > 4:
        cert_off, cert_size = struct.unpack_from("<II", data, dd_off + 8 * 4)

    raw_end = max((s[4] + s[3] for s in secs if s[3]), default=0)
    headers_end = sec_off + 40 * nsec
    new_headers = align(headers_end, PAGE)
    out = bytearray(new_headers)
    out[:headers_end] = data[:headers_end]

    moves = []                                   # (old_start, old_end, delta)
    pos = new_headers
    for s in secs:
        o, vsize, va, rsize, raw = s
        if not rsize:
            struct.pack_into("<I", out, o + 20, 0)
            continue
        new_size = align(rsize, PAGE)
        cap = align(max(vsize, rsize), PAGE)
        if new_size > cap:
            new_size = cap
        chunk = data[raw:raw + rsize]
        out += chunk + bytes(new_size - len(chunk))
        moves.append((raw, raw + rsize, pos - raw))
        struct.pack_into("<I", out, o + 16, new_size)   # SizeOfRawData
        struct.pack_into("<I", out, o + 20, pos)        # PointerToRawData
        pos += new_size

    # Overlay: anything after the sections that is not the certificate.
    overlay = data[raw_end:]
    if cert_size and cert_off >= raw_end:
        before = data[raw_end:cert_off]
        after = data[cert_off + cert_size:]
        overlay = before + after
    if overlay.strip(b"\0"):
        out += overlay

    def moved(off):
        for a, b, d in moves:
            if a <= off < b:
                return off + d
        return off

    # The debug directory's entries carry file offsets of their data.
    if ndd > 6:
        dbg_rva, dbg_size = struct.unpack_from("<II", data, dd_off + 8 * 6)
        if dbg_rva and dbg_size:
            for o, vsize, va, rsize, raw in secs:
                if va <= dbg_rva < va + max(vsize, rsize):
                    new_raw = struct.unpack_from("<I", out, o + 20)[0]
                    base = new_raw + (dbg_rva - va)
                    for e in range(dbg_size // 28):
                        p = base + 28 * e + 24
                        old = struct.unpack_from("<I", out, p)[0]
                        if old:
                            struct.pack_into("<I", out, p, moved(old))
                    break

    struct.pack_into("<I", out, opt + 36, PAGE)          # FileAlignment
    struct.pack_into("<I", out, opt + 60, new_headers)   # SizeOfHeaders
    if ndd > 4:
        struct.pack_into("<II", out, dd_off + 8 * 4, 0, 0)  # no certificate
    struct.pack_into("<I", out, opt + 64, 0)
    struct.pack_into("<I", out, opt + 64, checksum(out, opt + 64))

    open(dst, "wb").write(out)
    print("pe_pagealign: %s: %d sections page-aligned, %d -> %d bytes" %
          (src, len(moves), len(data), len(out)))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: pe_pagealign.py IN OUT")
    main(sys.argv[1], sys.argv[2])
