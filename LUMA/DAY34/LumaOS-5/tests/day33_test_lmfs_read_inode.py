#!/usr/bin/env python3
"""
tests/day33_test_lmfs_read_inode.py — DAY33 QEMU verification.

DAY33: "实现 lmfs_read_inode()。" 验收："能读取 inode 0。"

Boots the real lumaos-5.0.img headless, dumps the VGA text screen after
kmain()'s DAY33 test section runs (right after DAY32's, still before
text_shell_run()), and asserts all three lmfs_read_inode() edge cases
kmain.c exercises:

  1. inode 0 (root directory): type=DIR, file_size=1024 (1 block),
     direct[0]=1 — exactly what tools/mkfs.lmfs (Day 31, updated
     Day 34 to give root a real data block for "."/".." — see
     doc/lmfs.txt section 12) wrote.
  2. inode 1 (an unused slot): reads back as type=FREE.
  3. inode == inode_count (256, one past the valid range 0..255):
     lmfs_read_inode() must reject it (return -1).
"""
import sys
sys.path.insert(0, "tests")
from mon_helper import Session

IMG = "lumaos-5.0.img"

sess = Session(IMG, "day33", boot_wait=8)
try:
    screen = sess.dump_screen(wait=1.0)
finally:
    sess.close()

print(screen)
print("=" * 70)

checks = [
    ("DAY33 section ran",
     "DAY33: lmfs_read_inode()" in screen),
    ("inode 0 read OK",
     "inode 0 read OK" in screen),
    ("inode 0 type=DIR printed",
     "type=DIR(2)" in screen),
    ("inode 0 file_size=1024 (1 block) printed",
     "file_size=1024" in screen),
    ("inode 0 direct[0]=1 printed",
     "direct[0]=1" in screen),
    ("inode 1 (free slot) read OK",
     "inode 1 (free slot) read OK" in screen),
    ("inode 1 type=FREE printed",
     "type=FREE(0)" in screen),
    ("out-of-range inode correctly rejected",
     "out-of-range inode number correctly rejected" in screen),
    ("inode_count=256 referenced in OOB check",
     "inode_count=256" in screen),
    ("no FAIL lines anywhere in the DAY33 section",
     "[lmfs] FAIL" not in screen),
]

ok = True
for name, passed in checks:
    print(("PASS" if passed else "FAIL") + "  " + name)
    ok = ok and passed

print("=" * 70)
print("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED")
sys.exit(0 if ok else 1)