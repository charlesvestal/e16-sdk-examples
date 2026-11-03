#!/usr/bin/env python3
"""Embed a file as a C array: embed_file.py IN OUT.c SYMBOL

Writes `const char SYMBOL[]` (the bytes, not NUL-terminated for the
length) and `const size_t SYMBOL_len`. Used for the mirror's web page
(src/mirror/mirror_page.h)."""
import sys

src, out, sym = sys.argv[1:4]
data = open(src, "rb").read()
with open(out, "w") as f:
    f.write(f"/* Generated from {src.rsplit('/', 1)[-1]} by tools/embed_file.py; don't edit. */\n")
    f.write("#include <stddef.h>\n\n")
    f.write(f"const char {sym}[{len(data) + 1}] = {{\n")
    for i in range(0, len(data), 16):
        f.write("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",\n")
    f.write("    0x00,\n};\n")
    f.write(f"const size_t {sym}_len = {len(data)};\n")
