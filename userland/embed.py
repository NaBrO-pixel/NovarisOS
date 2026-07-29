#!/usr/bin/env python3
"""Regenerates kernel/user_hello_blob.c from userland/user_hello.bin.

Usage:
    nasm -f bin userland/user_hello.s -o userland/user_hello.bin
    python3 userland/embed.py
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "user_hello.bin")
DST = os.path.join(HERE, "..", "kernel", "user_hello_blob.c")

with open(SRC, "rb") as f:
    data = f.read()

with open(DST, "w") as f:
    f.write("/* GENERATED from userland/user_hello.bin - do not edit by hand.\n")
    f.write(" * Regenerate with:\n")
    f.write(" *   nasm -f bin userland/user_hello.s -o userland/user_hello.bin\n")
    f.write(" *   python3 userland/embed.py */\n\n")
    f.write('#include "user_hello.h"\n\n')
    f.write("const unsigned char user_hello_bin[] = {\n")
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        f.write("    " + ", ".join("0x%02x" % b for b in chunk) + ",\n")
    f.write("};\n\n")
    f.write("const unsigned int user_hello_bin_len = sizeof(user_hello_bin);\n")

print(f"wrote {DST} ({len(data)} bytes)")
