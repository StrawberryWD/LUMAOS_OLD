#!/usr/bin/env python3
"""
tests/day36_test_lmfs_alloc_block.py — DAY36 QEMU verification.

DAY36: "实现数据块 bitmap 分配器。" 验收："连续分配多个块，bitmap 正确变化。"

Boots the real lumaos-5.0.img headless, and asserts kmain()'s DAY36
test section (3 consecutive lmfs_alloc_block() calls against the LBA
100000 fixture, whose data bitmap only has blocks 0/1 pre-allocated by
mkfs.lmfs) shows:

  1. Three consecutive allocations returning distinct, non-zero,
     strictly increasing block numbers (2, 3, 4 given this fixture's
     fresh state).
  2. Each allocated block's bit independently re-read straight off
     disk (bypassing lmfs_alloc_block() itself) and confirmed set —
     not just trusting the function's return value.
"""
import sys
sys.path.insert(0, "tests")
from lmfs_test_helpers import wait_for_marker

IMG = "lumaos-5.0.img"

screen = wait_for_marker(IMG, "day36",
                          marker="data bitmap correctly updated on disk")

print(screen)
print("=" * 70)

checks = [
    ("DAY36 section ran",
     "DAY36: lmfs_alloc_block()" in screen),
    ("call #1 -> block 2",
     "call #1 -> block 2" in screen),
    ("call #2 -> block 3",
     "call #2 -> block 3" in screen),
    ("call #3 -> block 4",
     "call #3 -> block 4" in screen),
    ("3 distinct/non-zero/increasing block numbers message",
     "3 distinct, non-zero, increasing block numbers allocated" in screen),
    ("bit for block 2 on disk = 1",
     "bit for block 2 on disk = 1" in screen),
    ("bit for block 3 on disk = 1",
     "bit for block 3 on disk = 1" in screen),
    ("bit for block 4 on disk = 1",
     "bit for block 4 on disk = 1" in screen),
    ("data bitmap correctly updated on disk",
     "data bitmap correctly updated on disk" in screen),
    ("no FAIL lines anywhere in the DAY36 section",
     "[lmfs] FAIL" not in screen),
]

ok = True
for name, passed in checks:
    print(("PASS" if passed else "FAIL") + "  " + name)
    ok = ok and passed

print("=" * 70)
print("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED")
sys.exit(0 if ok else 1)