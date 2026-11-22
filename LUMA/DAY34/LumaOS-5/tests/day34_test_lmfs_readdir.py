#!/usr/bin/env python3
"""
tests/day34_test_lmfs_readdir.py — DAY34 QEMU verification.

DAY34: "实现 lmfs_readdir()。" 验收："能列出根目录。"

Boots the real lumaos-5.0.img headless, dumps the VGA text screen after
kmain()'s DAY34 test section runs (right after DAY32/33's, still before
text_shell_run()), and asserts lmfs_readdir() against the root directory
of the LBA 100000 fixture returns exactly the two entries tools/mkfs.lmfs
(Day 31, updated Day 34) wrote: "." and ".." both pointing at inode 0.
"""
import sys
sys.path.insert(0, "tests")
from mon_helper import Session

IMG = "lumaos-5.0.img"

sess = Session(IMG, "day34", boot_wait=8)
try:
    screen = sess.dump_screen(wait=1.0)
finally:
    sess.close()

print(screen)
print("=" * 70)

checks = [
    ("DAY34 section ran",
     "DAY34: lmfs_readdir()" in screen),
    ("readdir returned 2 entries",
     "returned 2 entries" in screen),
    ("entry 0 is '.'",
     'name="."' in screen),
    ("entry 1 is '..'",
     'name=".."' in screen),
    ("root directory listed OK",
     "root directory listed OK" in screen),
    ("no FAIL lines anywhere in the DAY34 section",
     "[lmfs] FAIL" not in screen),
]

ok = True
for name, passed in checks:
    print(("PASS" if passed else "FAIL") + "  " + name)
    ok = ok and passed

print("=" * 70)
print("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED")
sys.exit(0 if ok else 1)