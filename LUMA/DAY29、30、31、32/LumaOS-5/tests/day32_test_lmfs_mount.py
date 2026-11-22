#!/usr/bin/env python3
"""
tests/day32_test_lmfs_mount.py — DAY32 QEMU verification.

DAY32: "C 端实现 lmfs_mount(dev)。" 验收："能读取超级块并验证版本/容量。"

Boots the real lumaos-5.0.img headless (same technique as
day21/24/25/26/27/28's own tests, via tests/mon_helper.py's Session),
waits for kmain()'s DAY32 test section (the very last thing kmain()
prints before handing off to text_shell_run()) to run, dumps the VGA
text screen, and asserts:

  1. The real fixture mount (LBA 100000, tests/lmfs-fixture.img,
     generated fresh by build.sh's gen_lmfs_fixture() via Day 31's
     tools/mkfs.lmfs) succeeds and prints back the exact values
     mkfs.lmfs wrote: version 1.0, inode_count=256,
     data_block_count=1422, magic=0x4c4d4653.
  2. The negative-path mount (LBA 0, LumaOS's own mbr.asm sector, not
     an LMFS volume) correctly fails and does NOT print "FAIL".
"""
import sys
sys.path.insert(0, "tests")
from mon_helper import Session

IMG = "lumaos-5.0.img"

sess = Session(IMG, "day32", boot_wait=8)
try:
    screen = sess.dump_screen(wait=1.0)
finally:
    sess.close()

print(screen)
print("=" * 70)

checks = [
    ("LMFS mounted",
     "LMFS mounted" in screen),
    ("version 1.0 printed",
     "version=1.0" in screen),
    ("inode_count=256 printed",
     "inode_count=256" in screen),
    ("data_block_count=1422 printed",
     "data_block_count=1422" in screen),
    ("magic 0x4c4d4653 printed",
     "0x4c4d4653" in screen.lower()),
    ("LBA 0 negative path did NOT report FAIL",
     "[lmfs] FAIL" not in screen),
    ("LBA 0 negative path reported the expected NOTE",
     "mount failed as expected" in screen),
]

ok = True
for name, passed in checks:
    print(("PASS" if passed else "FAIL") + "  " + name)
    ok = ok and passed

print("=" * 70)
print("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED")
sys.exit(0 if ok else 1)