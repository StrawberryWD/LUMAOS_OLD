#!/usr/bin/env python3
"""
tests/day35_test_lmfs_vfs.py — DAY35 QEMU verification.

DAY35: "LMFS 注册进 VFS。" 验收："FAT 与 LMFS 可以同时存在。"

Boots the real lumaos-5.0.img headless, dumps the VGA text screen after
kmain()'s DAY35 test section runs (right after DAY32/33/34's, still
before text_shell_run()), and asserts:

  1. lmfs_vfs_mount() + vfs_register_mount() succeed and LMFS lands on
     mount slot "IDE3" (FAT12/16/32 already claimed IDE0/IDE1/IDE2).
  2. IDE0..IDE3 all resolve via vfs_lookup_mount() to the right
     fs->name ("fat12"/"fat16"/"fat32"/"lmfs") — i.e. FAT and LMFS
     genuinely coexist in the same mount table, not "last one wins".
  3. LMFS's root directory can be listed through the pure VFS
     interface (fs->readdir()), not just lmfs_readdir() directly —
     proving the adapter itself works end-to-end.
"""
import sys
sys.path.insert(0, "tests")
from lmfs_test_helpers import wait_for_marker

IMG = "lumaos-5.0.img"

screen = wait_for_marker(IMG, "day35",
                          marker="LMFS listed via generic fs->readdir()")

print(screen)
print("=" * 70)

checks = [
    ("DAY35 section ran",
     "DAY35: lmfs_vfs_mount()" in screen),
    ("registered as IDE3",
     'Registered as mount slot "IDE3"' in screen),
    ("IDE0 -> fat12 OK",
     "IDE0 -> fs->name=\"fat12\" OK" in screen),
    ("IDE1 -> fat16 OK",
     "IDE1 -> fs->name=\"fat16\" OK" in screen),
    ("IDE2 -> fat32 OK",
     "IDE2 -> fs->name=\"fat32\" OK" in screen),
    ("IDE3 -> lmfs OK",
     "IDE3 -> fs->name=\"lmfs\" OK" in screen),
    ("FAT and LMFS coexist message printed",
     "FAT and LMFS coexist in the VFS mount table" in screen),
    ("LMFS listed via generic fs->readdir()",
     "LMFS listed via generic fs->readdir()" in screen),
    ("no FAIL lines anywhere in the DAY35 section",
     "[lmfs] FAIL" not in screen),
]

ok = True
for name, passed in checks:
    print(("PASS" if passed else "FAIL") + "  " + name)
    ok = ok and passed

print("=" * 70)
print("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED")
sys.exit(0 if ok else 1)