#!/usr/bin/env python3
"""Regenerates kernel/*_blob.c from the assembled flat binaries in this
directory (the scheduler demo's three tasks, plus Milestone 16's thread demo).
Same idea as embed.py, generalized for the scheduler demo's three tasks.

Usage:
    nasm -f bin userland/task_a.s -o userland/task_a.bin
    nasm -f bin userland/task_b.s -o userland/task_b.bin
    nasm -f bin userland/task_c.s -o userland/task_c.bin
    python3 userland/embed_tasks.py
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))

def embed(name):
    src = os.path.join(HERE, f"{name}.bin")
    dst = os.path.join(HERE, "..", "kernel", f"{name}_blob.c")
    with open(src, "rb") as f:
        data = f.read()
    with open(dst, "w") as f:
        f.write(f"/* GENERATED from userland/{name}.bin - do not edit by hand.\n")
        f.write(" * Regenerate with:\n")
        f.write(f" *   nasm -f bin userland/{name}.s -o userland/{name}.bin\n")
        f.write(" *   python3 userland/embed_tasks.py */\n\n")
        f.write(f'#include "{name}.h"\n\n')
        f.write(f"const unsigned char {name}_bin[] = {{\n")
        for i in range(0, len(data), 12):
            chunk = data[i:i + 12]
            f.write("    " + ", ".join("0x%02x" % b for b in chunk) + ",\n")
        f.write("};\n\n")
        f.write(f"const unsigned int {name}_bin_len = sizeof({name}_bin);\n")
    print(f"wrote {dst} ({len(data)} bytes)")

for n in ("task_a", "task_b", "task_c", "thread_demo"):
    embed(n)
