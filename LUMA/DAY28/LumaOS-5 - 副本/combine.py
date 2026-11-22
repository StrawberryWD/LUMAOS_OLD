#!/usr/bin/env python3
# combine.py - LumaOS 4.0
# Collects every source file into a single all_files.txt for sharing/archiving.

import os

EXTS   = {".asm", ".c", ".h", ".py", ".sh"}
OUTPUT = "all_files.txt"
SKIP   = {"all_files.txt", "lumaos-4.0.img"}

with open(OUTPUT, "w", encoding="utf-8") as out:
    for root, dirs, files in os.walk("."):
        # Skip build directory and hidden dirs
        dirs[:] = [d for d in sorted(dirs)
                   if d != "build" and not d.startswith(".")]
        for filename in sorted(files):
            if filename in SKIP:
                continue
            _, ext = os.path.splitext(filename)
            if ext.lower() not in EXTS:
                continue
            path = os.path.join(root, filename)
            out.write(f"{path}\n")
            out.write("___________\n")
            try:
                with open(path, "r", encoding="utf-8", errors="ignore") as f:
                    out.write(f.read())
            except Exception as e:
                out.write(f"[cannot read: {e}]")
            out.write("\n\n====\n\n")

print(f"Written: {OUTPUT}")
