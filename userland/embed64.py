#!/usr/bin/env python3
"""Turns a binary into a C array the 64-bit kernel can link in.

Unlike userland/embed.py, this is run from Makefile.amd64 on every build
and writes into build64/ rather than into kernel/. A generated file that
is checked in is a generated file that can disagree with its source; this
one cannot, because it does not outlive the build directory.

Usage:
    embed64.py <input-binary> <output.c> <symbol>
"""
import os
import sys

if len(sys.argv) != 4:
    sys.exit("usage: embed64.py <input> <output.c> <symbol>")

src, dst, symbol = sys.argv[1], sys.argv[2], sys.argv[3]

with open(src, "rb") as fh:
    data = fh.read()

with open(dst, "w") as fh:
    fh.write("/* GENERATED from %s - do not edit.\n" % os.path.basename(src))
    fh.write(" * Regenerated on every build by userland/embed64.py. */\n\n")
    fh.write("#include <stdint.h>\n\n")
    fh.write("const unsigned char %s[] = {\n" % symbol)
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        fh.write("    " + ", ".join("0x%02x" % b for b in chunk) + ",\n")
    fh.write("};\n\n")
    fh.write("const unsigned long %s_len = sizeof(%s);\n" % (symbol, symbol))

print("  embedded %s (%d bytes) as %s" % (os.path.basename(src),
                                          len(data), symbol))
