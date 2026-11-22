#!/usr/bin/env python3
"""
tests/day37_test_lmfs_alloc_inode.py — DAY37 QEMU verification.

DAY37: "实现 inode 分配器。" 验收："创建 inode 后返回合法 inode 号。"

Boots the real lumaos-5.0.img headless, and asserts kmain()'s DAY37
test section (3 consecutive lmfs_alloc_inode() calls, all requesting
LMFS_TYPE_FILE, against the LBA 100000 fixture whose inode bitmap only
has inode 0/root pre-allocated by mkfs.lmfs) shows:

  1. Three consecutive allocations returning distinct, non-zero,
     strictly increasing inode numbers (1, 2, 3 given this fixture's
     fresh state).
  2. Each newly allocated inode, read back via lmfs_read_inode(),
     correctly initialized: type=FILE, file_size=0, all block pointers
     zero — not just an untouched bitmap flip.
  3. Each allocated inode's bit independently re-read straight off
     disk (bypassing lmfs_alloc_inode() itself) and confirmed set.
"""
import sys
sys.path.insert(0, "tests")
from lmfs_test_helpers import wait_for_marker

IMG = "lumaos-5.0.img"

screen = wait_for_marker(IMG, "day37",
                          marker="inode bitmap correctly updated on disk")

print(screen)
print("=" * 70)

checks = [
    ("DAY37 section ran",
     "DAY37: lmfs_alloc_inode()" in screen),
    ("call #1 -> inode 1",
     "call #1 -> inode 1" in screen),
    ("call #2 -> inode 2",
     "call #2 -> inode 2" in screen),
    ("call #3 -> inode 3",
     "call #3 -> inode 3" in screen),
    ("3 distinct/non-zero/increasing inode numbers message",
     "3 distinct, non-zero, increasing inode numbers allocated" in screen),
    ("inode 1 content: type=1 file_size=0 pointers_zero=1",
     "inode 1: type=1 file_size=0 pointers_zero=1" in screen),
    ("inode 2 content: type=1 file_size=0 pointers_zero=1",
     "inode 2: type=1 file_size=0 pointers_zero=1" in screen),
    ("inode 3 content: type=1 file_size=0 pointers_zero=1",
     "inode 3: type=1 file_size=0 pointers_zero=1" in screen),
    ("newly allocated inodes correctly initialized message",
     "newly allocated inodes correctly initialized (type=FILE, empty)"
     in screen),
    ("bit for inode 1 on disk = 1",
     "bit for inode 1 on disk = 1" in screen),
    ("bit for inode 2 on disk = 1",
     "bit for inode 2 on disk = 1" in screen),
    ("bit for inode 3 on disk = 1",
     "bit for inode 3 on disk = 1" in screen),
    ("inode bitmap correctly updated on disk",
     "inode bitmap correctly updated on disk" in screen),
    ("no FAIL lines anywhere in the DAY37 section",
     "[lmfs] FAIL" not in screen),
]

ok = True
for name, passed in checks:
    print(("PASS" if passed else "FAIL") + "  " + name)
    ok = ok and passed

print("=" * 70)
print("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED")
sys.exit(0 if ok else 1)