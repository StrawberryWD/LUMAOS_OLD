#!/usr/bin/env bash
# build.sh — LumaOS 5.0
#
# DAY1 changes (LumaOS 5.0 roadmap - Week 1, Day 1):
#   - MAX_LOAD: 65536 (64 KB) -> 131072 (128 KB), matching loader.asm's
#     move to a 256-sector (two-call) LBA read. This gives headroom
#     for the upcoming filesystem/driver code (~40-50 KB) while
#     keeping total kernel.bin size comfortably under ~100 KB.
#
# DAY2 changes (LumaOS 5.0 roadmap - Week 1, Day 2):
#   - Added -Ikernel/drivers/block so ide.h resolves the same way
#     keyboard.h/mouse.h already do via -Ikernel/drivers.
#   - Compile kernel/drivers/block/ide.c and link ide.o into kernel.elf.
#
# DAY5 changes (LumaOS 5.0 roadmap - Week 1, Day 5):
#   - Compile kernel/drivers/block/mbr.c and link mbr.o into
#     kernel.elf. No new include path needed: mbr.h lives alongside
#     ide.h in kernel/drivers/block, already covered by the
#     -Ikernel/drivers/block flag added on Day 2.
#
# DAY6 changes (LumaOS 5.0 roadmap - Week 1, Day 6):
#   - Compile kernel/drivers/block/block.c and link block.o into
#     kernel.elf. block.h/block.c live alongside ide.h/mbr.h in
#     kernel/drivers/block, already covered by the
#     -Ikernel/drivers/block flag added on Day 2 — no new include
#     path needed. This is the generic block_device_t layer: kmain.c
#     now calls ide0_block_device()->read/->write instead of
#     ide_read_sectors()/ide_write_sectors() directly.
#
# DAY9 changes (LumaOS 5.0 roadmap - Week 2, Day 9):
#   - Added -Ikernel/fs/fat so fat12.h resolves the same way
#     ide.h/mbr.h do via -Ikernel/drivers/block.
#   - Compile kernel/fs/fat/fat12.c and link fat12.o into kernel.elf.
#     fat12.c includes kernel/fs/vfs/vfs.h's sibling directory
#     kernel/fs/fat directly (no new -I needed beyond the one just
#     added); it does NOT yet touch kernel/fs/vfs/vfs.h itself — that
#     wiring starts Day 13.
#
# DAY11 changes (LumaOS 5.0 roadmap - Week 2, Day 11):
#   - tests/fat12-bpb.bin (LBA 2500, Day9/10's fixture) only ever
#     contained a boot sector — the FAT table region behind it was
#     whatever was already zeroed on the disk image, so it could never
#     exercise fat12_next_cluster()'s 1.5-byte/odd-even packing logic
#     with a real, non-trivial cluster chain (see kmain.c's DAY11
#     header comment for the full rationale). A second, larger fixture
#     — tests/fat12-with-file.img — is now dd'd onto the image too, at
#     LBA 5000: a location chosen to sit comfortably past LBA 2500's
#     fixture (which is only 1 sector) with a wide safety margin, so
#     the new fixture's own size (2880 sectors = the full 1.44MB image
#     it was built as) can never overlap it or anything else already
#     on the disk (see the full existing-LBA-usage list above this
#     comment block). tests/fat12-bpb.bin at LBA 2500 is left
#     completely untouched — Day 9/10's test code and fixture keep
#     working exactly as before.
#   - Because tests/fat12-with-file.img is a full 2880-sector (1.44MB)
#     image landing at LBA 5000, the disk image built below must now
#     be at least 5000+2880 = 7880 sectors, up from the previous 2880.
#     MAX_LOAD (the 128KB kernel.bin size cap from Day 1) is a
#     completely separate limit — kernel.bin still loads via the
#     loader's fixed-LBA/fixed-count sector reads (LBA 6, 256 sectors,
#     see loader.asm) which know nothing about the overall disk image
#     size, so growing the image doesn't touch that limit or require
#     any loader.asm change. Bumped to 8000 sectors (a round number
#     with a bit of headroom past the strict 7880 minimum, for
#     whatever the next fixture/test data turns out to need) via the
#     `dd if=/dev/zero ... count=2880` line below, now count=8000.
#
# DAY12 changes (LumaOS 5.0 roadmap - Week 2, Day 12):
#   - No build.sh changes needed. fat12_readdir() (kernel/fs/fat/fat12.c)
#     and its fat12_dirlist_entry_t/FAT12_DIRLIST_MAX_ENTRIES declarations
#     (kernel/fs/fat/fat12.h) are new code inside the SAME fat12.c/fat12.h
#     pair build.sh has already been compiling since Day 9 (see that day's
#     entry above: "kernel/fs/fat/fat12.c and link fat12.o into
#     kernel.elf") — no new source file, no new -I include path, no new
#     link input. kmain.c's DAY12 test section (calls fat12_readdir()
#     against the same tests/fat12-with-file.img fixture already embedded
#     at LBA 5000 by the DAY11 section below) is likewise just new code
#     inside the kmain.c build.sh has compiled since the project's first
#     day. tests/qemu_dump_console.py (new this Day) is a standalone dev/
#     test helper — a headless QEMU-monitor VGA-text-buffer dump script —
#     never invoked by build.sh and not part of the kernel image in any
#     way; see that file's own header comment for what it's for.
#
# DAY11-FOLLOWUP changes (generating fixtures instead of checking them
# in):
#   - Both tests/fat12-bpb.bin and tests/fat12-with-file.img started
#     out as binary files committed straight into the repo. A binary
#     blob can't be code-reviewed the way a generation recipe can, and
#     it silently goes stale if fat12.c's assumptions about it (e.g.
#     BIGFILE.BIN's cluster count, or which cluster it starts at) ever
#     change without someone remembering to regenerate it by hand.
#     Both are now generated by gen_fat12_fixtures() below, using
#     `mkfs.fat -F 12` + `mtools` (mcopy), only when the file is
#     missing — so this is a no-op on repeat builds, and a human who
#     deliberately hand-edits either fixture for a specific test case
#     doesn't have that overwritten by the next `./build.sh`. If
#     mkfs.fat/mtools aren't installed, generation is skipped with a
#     warning and LBA 2500/5000 are left zeroed, same as the old
#     "file not found" fallback.
#
# DAY14 changes (LumaOS 5.0 roadmap - Week 2, Day 14):
#   - Compile kernel/shell/cmd/ls.c and link cmd_ls.o into kernel.elf.
#     No new -I include path needed: ls.c reaches vfs.h via
#     "../../fs/vfs/vfs.h" (a relative include, same pattern
#     fat12_vfs.c already uses for "../vfs/vfs.h"), not through
#     CFLAGS's -Ikernel/fs/vfs entry (that entry exists so *.c files
#     can #include "vfs.h" unqualified from within kernel/fs/fat/ etc;
#     kernel/shell/cmd/ is two directories away so a relative path is
#     simpler here). kmain.c's tail "for(;;) hlt;" idle loop is
#     replaced with a real text-mode shell loop (text_shell_run(),
#     kernel/core/kmain.c) — no build.sh change needed for that beyond
#     already compiling kmain.c and shell.o/cmd_*.o, all of which were
#     already being built (see DAY13 above) since window1.o's
#     terminal_init()/terminal_puts()/terminal_newline() never touched
#     graphics hardware in the first place — see kmain.c's DAY14 header
#     comment for the full rationale.
#
# DAY9-FOLLOWUP changes (verifying fat12_parse_bpb() against a REAL
# FAT12 volume, not just LumaOS's own non-FAT LBA 0):
#   - tests/fat12-bpb.bin (512 bytes; see DAY11-FOLLOWUP below — as of
#     Day 11 this is generated by build.sh itself rather than checked
#     into the repo) is the boot sector of a `mkfs.fat -F 12` 1.44MB
#     image — a genuine FAT12 BPB with BS_FilSysType == "FAT12   ",
#     unlike LBA 0 which is LumaOS's own boot/mbr.asm bootloader. It's
#     dd'd onto the disk image at LBA 2500 below, a location chosen to
#     never overlap mbr.bin (LBA 0), loader.bin (LBA 1-5), kernel.bin
#     (LBA 6-261), or the Day 4 write/readback test's LBA 2000 (see
#     kmain.c's DAY6 header comment for why LBA 2000 itself must stay
#     untouched) — all comfortably inside the 2880-sector/1.44MB
#     image built below.
#   - kmain.c's DAY9-FOLLOWUP section reads LBA 2500 and runs it
#     through the exact same fat12_parse_bpb() call used on LBA 0,
#     so both the "this is NOT a valid FAT12 BPB" (LBA 0) and "this
#     IS a valid FAT12 BPB, geometry computed correctly" (LBA 2500)
#     paths are exercised and visible on one boot, without needing a
#     second disk image or a different QEMU invocation.
#
# STAGE1 changes (carried over from 4.1):
#   - Version string 4.1 -> 4.2 -> 5.0
#   - Back buffer is now at 0x00300000 (3 MB); the old 0x50000 address
#     is gone.  The BGA LFB at 0xE0000000 is accessed via the flat 4 GB
#     GDT data segment — no linker changes needed.
#   - Added -Ikernel/drivers to CFLAGS so keyboard.h / mouse.h resolve
#     from any subdirectory.
#
# FIX ENTRY-2: -fno-exceptions + -fno-asynchronous-unwind-tables kept
#   to eliminate .eh_frame bloat.
#
# DAY32 changes (LumaOS 5.0 roadmap - Month 2/Week 5, Day 32):
#   - Added -Ikernel/fs/lmfs so lmfs.h resolves the same way fat12.h/
#     vfs.h do via -Ikernel/fs/fat / -Ikernel/fs/vfs, for the new
#     kernel/fs/lmfs/lmfs.{h,c} (lmfs_mount(), see that file's own
#     header comment — this is LMFS's first real kernel-side C code,
#     Day 29-31 having only produced doc/lmfs.txt and the host-side
#     tools/mkfs.lmfs).
#   - Compile kernel/fs/lmfs/lmfs.c and link lmfs.o into kernel.elf.
#   - gen_lmfs_fixture() runs tools/mkfs.lmfs (Day 31) to produce
#     tests/lmfs-fixture.img (2880 sectors / 1.44MB, same size as the
#     default `mkfs.lmfs` output), embedded onto the disk image at LBA
#     100000 — chosen the same way every previous fixture's LBA was
#     (DAY9-FOLLOWUP/DAY11/DAY16/DAY17 comments above): comfortably
#     past the previous last fixture (tests/fat32-with-file.img, LBA
#     30000, 70000 sectors, ending exactly at LBA 100000) with zero
#     overlap. This is also why the disk image itself grows from
#     100000 to 102880 sectors below (100000 + 2880, mirroring the
#     DAY17 comment's own "fixture start + fixture length = new total"
#     reasoning for the previous grow). kmain.c's own DAY32 section
#     mounts this fixture via lmfs_mount(disk, 100000, ...).
#   - kernel/drivers/keyboard.c: no change made this build for the
#     "修复键盘Delete键功能" half of Day 32's task line — every Delete-
#     key code path already found in this codebase (text_shell_run in
#     kmain.c, kernel/shell/cmd/write.c's editor, kernel/gui/window1.c's
#     GUI terminal) passed tests/day28_test_month_review.py's Delete-
#     key scenarios A-D unmodified before today's LMFS work started;
#     see this day's chat reply for the specific ask for clarification
#     on what remaining bug this line refers to.

set -euo pipefail

CC="${CC:-gcc}"
AS="${AS:-nasm}"
LD="${LD:-ld}"
OBJCOPY="${OBJCOPY:-objcopy}"
BUILD="build"
IMG="lumaos-5.0.img"
KERNEL_BASE="0x00010000"

CFLAGS="-m32 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -O2 \
  -fno-exceptions -fno-asynchronous-unwind-tables \
  -Ikernel/arch -Ikernel/core -Ikernel/drivers -Ikernel/drivers/block \
  -Ikernel/gui -Ikernel/shell -Ikernel/lib -Ikernel/fs/fat -Ikernel/fs/vfs \
  -Ikernel/fs/lmfs"

mkdir -p "$BUILD"
die() { echo "ERROR: $1"; exit 1; }

echo "=== LumaOS 5.0 Build ==="

# ---- Boot sector & loader ----
$AS -f bin boot/mbr.asm    -o "$BUILD/mbr.bin"    || die "mbr.asm"
$AS -f bin boot/loader.asm -o "$BUILD/loader.bin"  || die "loader.asm"
echo "[boot] mbr.bin  loader.bin"

# ---- ASM kernel objects ----
$AS -f elf32 kernel/arch/entry.asm       -o "$BUILD/entry.o"        || die "entry.asm"
$AS -f elf32 kernel/arch/interrupts.asm  -o "$BUILD/interrupts_s.o"  || die "interrupts.asm"
echo "[asm]  entry.o  interrupts_s.o"

cc_compile() {
    local src="$1" obj="$BUILD/$2.o"
    $CC $CFLAGS -c "$src" -o "$obj" || die "$src"
    echo "[cc]   $src"
}

# ---- Arch ----
cc_compile kernel/arch/gdt.c         gdt
cc_compile kernel/arch/idt.c         idt
cc_compile kernel/arch/pic.c         pic
cc_compile kernel/arch/interrupts.c  interrupts_c

# ---- Core ----
cc_compile kernel/core/console.c     console
cc_compile kernel/core/panic.c       panic
cc_compile kernel/core/kmain.c       kmain

# ---- Drivers ----
cc_compile kernel/drivers/vga_gfx.c  vga_gfx
cc_compile kernel/drivers/vga.c      vga
cc_compile kernel/drivers/keyboard.c keyboard
cc_compile kernel/drivers/mouse.c    mouse

# ---- Drivers / Block (DAY2) ----
cc_compile kernel/drivers/block/ide.c  ide

# ---- Drivers / Block (DAY5) ----
cc_compile kernel/drivers/block/mbr.c  mbr

# ---- Drivers / Block (DAY6) ----
cc_compile kernel/drivers/block/block.c  block

# ---- FS / FAT (DAY9) ----
# DAY15: fat_common.c compiled first — fat12.c's own fat12_parse_bpb()
# now calls into it (fat_parse_bpb_common()/fat_compute_geometry()),
# see kernel/fs/fat/fat_common.h's header comment for the full
# "what moved out of fat12.h and why" rationale. Compile order doesn't
# matter for a plain cc-then-ld build like this one (each .c is its
# own translation unit; symbol resolution happens at link time), but
# fat_common is listed first here since it's the lower-level piece
# fat12.c now depends on, matching this section's existing top-to-
# bottom "foundation before what's built on it" ordering.
cc_compile kernel/fs/fat/fat_common.c  fat_common
cc_compile kernel/fs/fat/fat12.c  fat12

# ---- FS / FAT16 (DAY16) ----
# fat16.c depends on fat_common.c (fat_parse_bpb_common(),
# fat_compute_geometry(), fat_readdir_fixed_root()) the same way
# fat12.c does — listed right after it for the same "foundation
# before what's built on it" ordering as fat12.c's own DAY15 comment
# above. No new -I include path: fat16.h lives alongside fat12.h in
# kernel/fs/fat, already covered by the DAY9 -Ikernel/fs/fat entry.
cc_compile kernel/fs/fat/fat16.c  fat16

# ---- FS / FAT32 (DAY17) ----
# fat32.c does NOT depend on fat_common.c's fat_compute_geometry() (see
# fat32.h's header comment for why — FAT32 needs its own geometry math
# using BPB_FATSz32), but it does use fat_parse_bpb_common()/
# fat_decode_short_name(), so it's still listed after fat_common.c for
# the same "foundation before what's built on it" readability ordering.
cc_compile kernel/fs/fat/fat32.c  fat32

# ---- FS / VFS + FAT12 adapter (DAY13) ----
cc_compile kernel/fs/vfs/vfs.c          vfs
cc_compile kernel/fs/fat/fat12_vfs.c    fat12_vfs

# ---- FS / FAT16 VFS adapter + auto-detect dispatcher (DAY16) ----
# fat16_vfs.c mirrors fat12_vfs.c's role for FAT16 (see fat16_vfs.h's
# header comment). fat_vfs.c is the new fat_vfs_mount() dispatcher
# (kernel/fs/fat/fat_vfs.h) that vfs.c's vfs_mount("IDE0") now calls
# instead of fat12_vfs_mount() directly — it #includes both
# fat12_vfs.h and fat16_vfs.h, so it's compiled after both adapters
# are known to exist, though (as with the rest of this section) plain
# cc-then-ld compilation doesn't actually require a particular order.
cc_compile kernel/fs/fat/fat16_vfs.c    fat16_vfs
cc_compile kernel/fs/fat/fat_vfs.c      fat_vfs

# ---- FS / FAT32 VFS adapter (DAY17) ----
cc_compile kernel/fs/fat/fat32_vfs.c    fat32_vfs

# ---- FS / LMFS (DAY32) ----
# lmfs.c is LMFS's first real kernel-side C code (Day 29-31 only
# produced doc/lmfs.txt + the host-side tools/mkfs.lmfs) — see this
# file's DAY32 header comment above and kernel/fs/lmfs/lmfs.c's own
# header comment for lmfs_mount()'s contract. Not yet wired into the
# VFS `struct filesystem` layer (that would mirror FAT12's own
# Day 10 -> Day 13 gap) — kmain.c's DAY32 test section below calls
# lmfs_mount() directly, same standalone-handle pattern fat12_mount()
# used between Day 10 and Day 13.
cc_compile kernel/fs/lmfs/lmfs.c        lmfs

# ---- GUI ----
cc_compile kernel/gui/window1.c      window1
cc_compile kernel/gui/window2.c      window2

# ---- Shell ----
cc_compile kernel/shell/shell.c          shell
cc_compile kernel/shell/cmd/help.c       cmd_help
cc_compile kernel/shell/cmd/clear.c      cmd_clear
cc_compile kernel/shell/cmd/version.c    cmd_version
cc_compile kernel/shell/cmd/type.c       cmd_type
cc_compile kernel/shell/cmd/echo.c       cmd_echo
cc_compile kernel/shell/cmd/color.c      cmd_color
cc_compile kernel/shell/cmd/shutdown.c   cmd_shutdown
cc_compile kernel/shell/cmd/reboot.c     cmd_reboot

# ---- Shell / DAY14 ----
#   cmd_ls (kernel/shell/cmd/ls.c) walks vfs_root via
#   vfs_root->fs->readdir() — needs kernel/fs/vfs already on the
#   include path, added by the DAY9 -Ikernel/fs/vfs CFLAGS entry above.
cc_compile kernel/shell/cmd/ls.c         cmd_ls

# ---- Shell / DAY19 (DAY21: cat.c -> show.c, cmd_cat -> cmd_show;
#      DAY27: show.c removed outright — see kernel/shell/shell.c's own
#      DAY27 comment; cmd_read.c below is the command that remains) ----

# ---- Shell / DAY24 ----
#   cmd_create (kernel/shell/cmd/create.c) calls fat_create_file()
#   (kernel/fs/fat/fat_vfs.c) — needs kernel/fs/fat already on the
#   include path (DAY9 -Ikernel/fs/fat CFLAGS entry above) for
#   fat_vfs.h, same as kernel/fs/vfs for vfs.h.
cc_compile kernel/shell/cmd/create.c     cmd_create

# ---- Shell / DAY25 ----
#   cmd_write (kernel/shell/cmd/write.c) calls fat_write()/
#   fat_create_file()/fat_truncate() (DAY27) and drives the real VGA
#   console directly (console_*, kernel/core/console.c) for its
#   interactive multi-line editor — see that file's own header comment
#   for why. cmd_read (kernel/shell/cmd/read.c) is the former `show`
#   command's own read logic plus shell_resolve_path()'s bare-filename
#   fallback (DAY27: now the ONLY read-a-file command, `show` having
#   been removed). cmd_cd (kernel/shell/cmd/cd.c) switches vfs_root
#   between mounted volumes. Same include-path needs as cmd_create.c
#   above.
cc_compile kernel/shell/cmd/write.c      cmd_write
cc_compile kernel/shell/cmd/read.c       cmd_read
cc_compile kernel/shell/cmd/cd.c         cmd_cd

# ---- Shell / DAY26 ----
#   cmd_rm (kernel/shell/cmd/rm.c) calls fat_delete_file()
#   (kernel/fs/fat/fat_vfs.c) — same include-path needs as
#   cmd_create.c/cmd_write.c above.
cc_compile kernel/shell/cmd/rm.c         cmd_rm

# ---- Lib ----
cc_compile kernel/lib/string.c  string
cc_compile kernel/lib/printf.c  printf
cc_compile kernel/lib/font.c    font

echo "[ld]   linking..."
$LD -m elf_i386 -Ttext "$KERNEL_BASE" \
    "$BUILD/entry.o" \
    "$BUILD/interrupts_s.o" \
    "$BUILD/gdt.o" "$BUILD/idt.o" "$BUILD/pic.o" "$BUILD/interrupts_c.o" \
    "$BUILD/console.o" "$BUILD/panic.o" "$BUILD/kmain.o" \
    "$BUILD/vga_gfx.o" "$BUILD/vga.o" \
    "$BUILD/keyboard.o" "$BUILD/mouse.o" \
    "$BUILD/ide.o" \
    "$BUILD/mbr.o" \
    "$BUILD/block.o" \
    "$BUILD/fat_common.o" \
    "$BUILD/fat12.o" \
    "$BUILD/fat16.o" \
    "$BUILD/fat32.o" \
    "$BUILD/vfs.o" \
    "$BUILD/fat12_vfs.o" \
    "$BUILD/fat16_vfs.o" \
    "$BUILD/fat_vfs.o" \
    "$BUILD/fat32_vfs.o" \
    "$BUILD/lmfs.o" \
    "$BUILD/window1.o" "$BUILD/window2.o" \
    "$BUILD/shell.o" \
    "$BUILD/cmd_help.o" "$BUILD/cmd_clear.o" "$BUILD/cmd_version.o" \
    "$BUILD/cmd_type.o" "$BUILD/cmd_echo.o" "$BUILD/cmd_color.o" \
    "$BUILD/cmd_shutdown.o" "$BUILD/cmd_reboot.o" "$BUILD/cmd_ls.o" \
    "$BUILD/cmd_create.o" \
    "$BUILD/cmd_write.o" "$BUILD/cmd_read.o" "$BUILD/cmd_cd.o" \
    "$BUILD/cmd_rm.o" \
    "$BUILD/string.o" "$BUILD/printf.o" "$BUILD/font.o" \
    -o "$BUILD/kernel.elf" || die "link"

$OBJCOPY -O binary "$BUILD/kernel.elf" "$BUILD/kernel.bin" || die "objcopy"
SZ=$(stat -c%s "$BUILD/kernel.bin" 2>/dev/null || stat -f%z "$BUILD/kernel.bin")
echo "[ld]   kernel.bin = $SZ bytes"

# DAY1: 65536 -> 131072 (matches loader.asm al=255 sector read)
MAX_LOAD=131072
if [ "$SZ" -gt "$MAX_LOAD" ]; then
    echo "WARNING: kernel.bin ($SZ bytes) exceeds loader capacity ($MAX_LOAD bytes)!"
    echo "         Consider splitting font data or compressing the kernel."
fi

# DAY17: 30000 -> 100000 sectors, so the disk image is also big enough
# to hold tests/fat32-with-file.img (70000 sectors) landing at LBA
# 30000 without truncating it. 30000 was chosen the same way LBA 5000
# (Day 11) and LBA 10000 (Day 16) were — comfortably past the previous
# last fixture (tests/fat16-with-file.img, LBA 10000, 20000 sectors,
# ending at LBA 30000) with zero overlap risk. See this file's DAY17
# comment further below for why the FAT32 fixture itself needs to be
# 70000 sectors (~35MB): FAT32 requires at least 65525 clusters, well
# past what a small image can offer even with 1 sector/cluster.
# 30000 (fixture start) + 70000 (fixture length) = 100000, hence the
# new count below.
dd if=/dev/zero           of="$IMG" bs=512 count=102880 status=none
dd if="$BUILD/mbr.bin"    of="$IMG" conv=notrunc bs=512 seek=0 status=none
dd if="$BUILD/loader.bin" of="$IMG" conv=notrunc bs=512 seek=1 status=none
dd if="$BUILD/kernel.bin" of="$IMG" conv=notrunc bs=512 seek=6 status=none

# DAY9-FOLLOWUP: bake a real FAT12 boot sector (from `mkfs.fat -F 12`,
# generated below into tests/fat12-bpb.bin) onto the image at LBA 2500, so
# kmain.c can run fat12_parse_bpb() against an actual valid BPB in
# addition to LBA 0's (expected-invalid) result. See this file's
# DAY9-FOLLOWUP comment above for why LBA 2500 is a safe location.
#
# DAY11-FOLLOWUP: this fixture, and DAY11's own tests/fat12-with-file.img
# below, used to be binary files checked straight into the repo — but a
# checked-in binary can't be inspected in a diff/review the way generated-
# by-a-documented-recipe can, and it silently goes stale if fat12.c's
# expectations ever change. Both are now generated right here by build.sh
# instead, from `mkfs.fat`/`mtools`, only regenerated when missing (so a
# human who deliberately hand-edits either file for a specific test case
# doesn't have that overwritten on the next build). Needs mkfs.fat and
# mtools (mcopy) — see gen_fat12_fixtures() below for the fallback if
# they're not installed.
gen_fat12_fixtures() {
    if ! command -v mkfs.fat >/dev/null 2>&1 || ! command -v mcopy >/dev/null 2>&1; then
        echo "WARNING: mkfs.fat and/or mtools (mcopy) not found on PATH —"
        echo "         cannot generate tests/fat12-bpb.bin or"
        echo "         tests/fat12-with-file.img. Install dosfstools + mtools,"
        echo "         or supply these two files yourself under tests/."
        return
    fi

    mkdir -p tests
    local TMP
    TMP=$(mktemp -d)

    # ---- tests/fat12-bpb.bin: just a boot sector (Day 9/10's fixture,
    # mounted at LBA 2500). A plain freshly-formatted 1.44MB FAT12 image's
    # first 512 bytes — the FAT table/data region past it is intentionally
    # NOT included (Day 9/10 never reads past the boot sector; DAY9-FOLLOWUP
    # comment above covers why). ----
    if [ ! -f "tests/fat12-bpb.bin" ]; then
        dd if=/dev/zero of="$TMP/bpb-src.img" bs=512 count=2880 status=none
        mkfs.fat -F 12 -n LUMABPB "$TMP/bpb-src.img" >/dev/null
        dd if="$TMP/bpb-src.img" of="tests/fat12-bpb.bin" bs=512 count=1 status=none
        echo "[gen]  generated tests/fat12-bpb.bin (mkfs.fat -F 12 boot sector)"
    fi

    # ---- tests/fat12-with-file.img: a full 1.44MB FAT12 volume with real
    # files, for Day 11's fat12_next_cluster() to walk a genuine multi-
    # cluster chain (a boot-sector-only fixture can't exercise that — see
    # kmain.c's DAY11 header comment). BIGFILE.BIN is 10240 bytes = exactly
    # 20 512-byte clusters on this 1-sector-per-cluster volume, long enough
    # to print all 10 of the roadmap's requested cluster numbers without
    # hitting end-of-chain. Its content (byte i = i mod 256) and
    # SMALL.BIN's (byte i = (i*7+3) mod 256) are arbitrary but FIXED
    # patterns, chosen only so the files are reproducible byte-for-byte
    # across machines/runs — fat12_next_cluster() itself only cares about
    # the FAT table, never file content. On a freshly-formatted, otherwise
    # empty volume mtools allocates clusters contiguously, so BIGFILE.BIN
    # lands at cluster 2 (the first legal data cluster) with clusters
    # 2..21 all contiguous — this is exactly what kmain.c's DAY11 section
    # and header comment document and cross-validate against (expected
    # fat12_next_cluster() output starting from cluster 2: 3,4,5,6,7,8,9,
    # 10,11,12). If mtools/mkfs.fat versions ever change this allocation
    # behavior, kmain.c's printed values would need re-deriving against
    # the new fixture — see kmain.c's DAY11 comment for the cross-
    # validation method (independent Python FAT-table parse + a host-
    # compiled fat12.c test) to redo that.
    rm -f "tests/fat12-with-file.img"; if [ "${LUMAOS_KEEP_FIXTURES:-0}" != "1" ] || [ ! -f "tests/fat12-with-file.img" ]; then
        dd if=/dev/zero of="tests/fat12-with-file.img" bs=512 count=2880 status=none
        mkfs.fat -F 12 -n LUMATEST "tests/fat12-with-file.img" >/dev/null

        python3 -c "
import sys
data = bytes(i % 256 for i in range(20 * 512))
open('$TMP/bigfile.bin', 'wb').write(data)
data = bytes((i * 7 + 3) % 256 for i in range(3 * 512))
open('$TMP/small.bin', 'wb').write(data)
" || die "python3 required to generate fixture file content"

        mcopy -i "tests/fat12-with-file.img" "$TMP/bigfile.bin" ::BIGFILE.BIN
        mcopy -i "tests/fat12-with-file.img" "$TMP/small.bin"   ::SMALL.BIN
        echo "[gen]  generated tests/fat12-with-file.img (mkfs.fat -F 12 + mcopy, BIGFILE.BIN=20 clusters)"

        # ---- DAY21 addition: HELLO.TXT, a real human-readable ASCII text
        # file in this same root directory (计划.md's own DAY21 acceptance
        # text: "一个根目录下的文本文件（用 Python 提前写入）" — "a text file
        # in the root directory, written ahead of time via Python"). Unlike
        # BIGFILE.BIN/SMALL.BIN above (synthetic i%256 / (i*7+3)%256 byte
        # patterns, chosen only to be deterministic and cross-validatable,
        # never meant to be read as text), this file exists specifically so
        # `show IDE0/HELLO.TXT` produces legible output on the QEMU text
        # screen — the natural way to eyeball-verify Day 21's cat->show
        # rename actually still works end-to-end, the same role SMALL.BIN
        # played for Day 19/20's interactive checks (tests/
        # day19_test_cat_interactive.py, tests/day20_test_cat_full.py).
        # mcopy'd AFTER BIGFILE.BIN/SMALL.BIN above so their existing
        # cluster numbers (independently cross-validated in kmain.c's
        # DAY11 comment) are completely undisturbed — mtools allocates
        # clusters to new files in call order on an otherwise-untouched
        # volume, so HELLO.TXT simply lands in whatever clusters come
        # after SMALL.BIN's, without shifting anything already there.
        python3 -c "
text = ('LumaOS Day 21 test file.\r\n'
        'Hello from the FAT12 root directory!\r\n'
        'The show command replaces cat.\r\n'
        'LumaOS can read TXT file now!\r\n'
        '                       LumaOS 5.0.21\r\n')
open('$TMP/hello.txt', 'wb').write(text.encode('ascii'))
" || die "python3 required to generate fixture file content"
        mcopy -i "tests/fat12-with-file.img" "$TMP/hello.txt" ::HELLO.TXT
        echo "[gen]  generated HELLO.TXT inside tests/fat12-with-file.img (DAY21 text-file fixture)"
    fi

    rm -rf "$TMP"
}
gen_fat12_fixtures

if [ -f "tests/fat12-bpb.bin" ]; then
    dd if="tests/fat12-bpb.bin" of="$IMG" conv=notrunc bs=512 seek=2500 status=none
    echo "[img]  embedded tests/fat12-bpb.bin at LBA 2500 (real FAT12 BPB)"
else
    echo "WARNING: tests/fat12-bpb.bin not found — LBA 2500 will be left zeroed"
fi

# DAY11: bake a real FAT12 volume WITH an actual multi-cluster file
# onto the image at LBA 5000, for fat12_next_cluster() to walk. Unlike
# tests/fat12-bpb.bin above (boot sector only), this fixture is the
# full 2880-sector (1.44MB) image, since the cluster-chain walker
# needs the FAT table region, root directory region, and data region
# all present and consistent with each other. See kmain.c's DAY11
# header comment for how the fixture was built and independently
# cross-validated (expected cluster sequence vs. fat12_next_cluster()
# output).
if [ -f "tests/fat12-with-file.img" ]; then
    dd if="tests/fat12-with-file.img" of="$IMG" conv=notrunc bs=512 seek=5000 status=none
    echo "[img]  embedded tests/fat12-with-file.img at LBA 5000 (FAT12 volume w/ multi-cluster file)"
else
    echo "WARNING: tests/fat12-with-file.img not found — LBA 5000 will be left zeroed"
fi

# DAY16 (Week 3, Day 16 of the 5.0 filesystem roadmap):
#   "实现 FAT16 解析（FAT16 的根目录在数据区之前，簇号 16 位）。挂载一个
#    FAT16 镜像（可以用 mkfs.fat -F 16 制作），ls 能正确列出文件。"
#
#   Same "generate, don't check in a binary" approach DAY11-FOLLOWUP
#   established for the FAT12 fixtures, applied to a new FAT16 one.
#   The key difference from tests/fat12-with-file.img: FAT16 needs
#   the volume's TOTAL CLUSTER COUNT to land in [4085, 65524] (below
#   4085, mkfs.fat itself would format the volume as FAT12 instead,
#   regardless of the `-F 16` request — this is the real FAT spec
#   rule this project's own fat_compute_geometry()/fat_type_t
#   classification implements, see fat_common.h's DAY15 comment). A
#   1.44MB image sized like the FAT12 fixtures (2880 sectors, 1
#   sector/cluster) only has ~2850 usable clusters — squarely in FAT12
#   range, NOT big enough for `mkfs.fat -F 16` to honor. `-s 1` (force
#   1 sector = 512 bytes per cluster, same as the FAT12 fixture, so
#   fat16_next_cluster()'s cluster numbers stay simple/predictable)
#   plus a 20000-sector (~10MB) image gives ~19800 usable clusters —
#   comfortably mid-range, independently confirmed by decoding the
#   resulting BPB by hand (BPB_BytesPerSec=512, BPB_SecPerClus=1,
#   BPB_RsvdSecCnt=1, BPB_NumFATs=2, BPB_RootEntCnt=512,
#   BPB_FATSz16=78, BS_FilSysType="FAT16   ") before this recipe was
#   written.
gen_fat16_fixtures() {
    if ! command -v mkfs.fat >/dev/null 2>&1 || ! command -v mcopy >/dev/null 2>&1; then
        echo "WARNING: mkfs.fat and/or mtools (mcopy) not found on PATH —"
        echo "         cannot generate tests/fat16-with-file.img."
        echo "         Install dosfstools + mtools, or supply this file"
        echo "         yourself under tests/."
        return
    fi

    # DAY17-FOLLOWUP: this used to skip generation whenever
    # tests/fat16-with-file.img already existed (silently keeping
    # whatever was there — meant to protect a hand-edited fixture, but
    # in practice just meant "the generator's own output" and the
    # generator disagreeing after any change to the recipe below,
    # unless you remembered to `rm` it first). Regenerates by default
    # now; set LUMAOS_KEEP_FIXTURES=1 if you really do have a
    # hand-edited tests/fat16-with-file.img you don't want touched.
    if [ -f "tests/fat16-with-file.img" ] && [ "${LUMAOS_KEEP_FIXTURES:-0}" = "1" ]; then
        return
    fi
    rm -f "tests/fat16-with-file.img"

    mkdir -p tests
    local TMP
    TMP=$(mktemp -d)

    # ---- tests/fat16-with-file.img: a full 20000-sector (~10MB) FAT16
    # volume with two real files, so fat16_readdir() has an actual root
    # directory to list (this Day's acceptance test is readdir/ls, not
    # the cluster-chain walker — unlike Day 11's FAT12 fixture, BIGFILE.BIN's
    # exact cluster layout doesn't need to be independently cross-validated
    # here, since no test in this Day's scope calls fat16_next_cluster()
    # against it). BIGFILE.BIN/SMALL.BIN reuse the exact same byte patterns
    # (byte i = i mod 256 / byte i = (i*7+3) mod 256) as the FAT12 fixture's
    # same-named files purely for consistency across fixtures — the two
    # images are otherwise completely unrelated volumes at different LBAs. ----
    dd if=/dev/zero of="tests/fat16-with-file.img" bs=512 count=20000 status=none
    mkfs.fat -F 16 -s 1 -n LUMA16 "tests/fat16-with-file.img" >/dev/null

    python3 -c "
data = bytes(i % 256 for i in range(20 * 512))
open('$TMP/bigfile16.bin', 'wb').write(data)
data = bytes((i * 7 + 3) % 256 for i in range(3 * 512))
open('$TMP/small16.bin', 'wb').write(data)
" || die "python3 required to generate fixture file content"

    mcopy -i "tests/fat16-with-file.img" "$TMP/bigfile16.bin" ::BIGFILE.BIN
    mcopy -i "tests/fat16-with-file.img" "$TMP/small16.bin"   ::SMALL.BIN
    echo "[gen]  generated tests/fat16-with-file.img (mkfs.fat -F 16 -s 1 + mcopy, ~19800 clusters)"

    rm -rf "$TMP"
}
gen_fat16_fixtures

# Bake the FAT16 volume onto the image at LBA 10000 — see this file's
# DAY16 comment above (and the disk-image-size DAY16 comment further
# up) for why this LBA and why 20000 sectors.
if [ -f "tests/fat16-with-file.img" ]; then
    dd if="tests/fat16-with-file.img" of="$IMG" conv=notrunc bs=512 seek=10000 status=none
    echo "[img]  embedded tests/fat16-with-file.img at LBA 10000 (FAT16 volume w/ BIGFILE.BIN/SMALL.BIN)"
else
    echo "WARNING: tests/fat16-with-file.img not found — LBA 10000 will be left zeroed"
fi

# DAY17 (Week 3, Day 17 of the 5.0 filesystem roadmap):
#   "实现 FAT32 解析（注意 BPB_RootClus，根目录是一个普通簇链，且存在
#    FSInfo 结构）。挂载 FAT32 镜像（mkfs.fat -F 32），ls 能工作。"
#
#   Same generate-fixture-then-embed recipe as DAY16's FAT16 fixture,
#   sized for FAT32 this time. FAT32 needs total cluster count >=
#   65525 (below that, mkfs.fat -F 32 refuses and falls back to
#   FAT16) — a 20000-sector image like the FAT16 fixture only has
#   ~19800 clusters, nowhere near enough. `-s 1` (1 sector/cluster,
#   same reasoning as the FAT16 fixture) plus a 70000-sector (~35MB)
#   image gives ~68874 usable clusters — independently confirmed by
#   decoding the resulting BPB by hand (BPB_BytesPerSec=512,
#   BPB_SecPerClus=1, BPB_RsvdSecCnt=32, BPB_NumFATs=2,
#   BPB_FATSz32=539, BPB_RootClus=2, BPB_FSInfo=1, BPB_BkBootSec=6,
#   BS_FilSysType="FAT32   ", plus a real FSInfo sector with valid
#   0x41615252/0x61417272/0xAA550000 signatures) before this recipe
#   was written.
gen_fat32_fixtures() {
    if ! command -v mkfs.fat >/dev/null 2>&1 || ! command -v mcopy >/dev/null 2>&1; then
        echo "WARNING: mkfs.fat and/or mtools (mcopy) not found on PATH —"
        echo "         cannot generate tests/fat32-with-file.img."
        echo "         Install dosfstools + mtools, or supply this file"
        echo "         yourself under tests/."
        return
    fi

    # DAY17-FOLLOWUP: same regenerate-by-default change as
    # gen_fat16_fixtures() above — see its comment for the rationale.
    if [ -f "tests/fat32-with-file.img" ] && [ "${LUMAOS_KEEP_FIXTURES:-0}" = "1" ]; then
        return
    fi
    rm -f "tests/fat32-with-file.img"

    mkdir -p tests
    local TMP
    TMP=$(mktemp -d)

    dd if=/dev/zero of="tests/fat32-with-file.img" bs=512 count=70000 status=none
    mkfs.fat -F 32 -s 1 -n LUMA32 "tests/fat32-with-file.img" >/dev/null

    python3 -c "
data = bytes(i % 256 for i in range(20 * 512))
open('$TMP/bigfile32.bin', 'wb').write(data)
data = bytes((i * 7 + 3) % 256 for i in range(3 * 512))
open('$TMP/small32.bin', 'wb').write(data)
" || die "python3 required to generate fixture file content"

    mcopy -i "tests/fat32-with-file.img" "$TMP/bigfile32.bin" ::FAT32BIG.BIN
    mcopy -i "tests/fat32-with-file.img" "$TMP/small32.bin"   ::SMALLFIL.BIN
    echo "[gen]  generated tests/fat32-with-file.img (mkfs.fat -F 32 -s 1 + mcopy, ~68874 clusters)"

    rm -rf "$TMP"
}
gen_fat32_fixtures

# Bake the FAT32 volume onto the image at LBA 30000 — see this file's
# DAY17 comment above (and the disk-image-size DAY17 comment further
# up) for why this LBA and why 70000 sectors.
if [ -f "tests/fat32-with-file.img" ]; then
    dd if="tests/fat32-with-file.img" of="$IMG" conv=notrunc bs=512 seek=30000 status=none
    echo "[img]  embedded tests/fat32-with-file.img at LBA 30000 (FAT32 volume w/ FAT32BIG.BIN/SMALLFIL.BIN)"
else
    echo "WARNING: tests/fat32-with-file.img not found — LBA 30000 will be left zeroed"
fi

# DAY32 (Week 5, Day 32 of the 5.0 filesystem roadmap):
#   "C 端实现 lmfs_mount(dev)。" 验收："能读取超级块并验证版本/容量。"
#
#   tools/mkfs.lmfs (Day 31, Python) generates a real LMFS volume the
#   same way gen_fat12_fixtures() etc. above generate real FAT volumes
#   — regenerated on every build (same "delete then regenerate unless
#   LUMAOS_KEEP_FIXTURES=1" convention as the FAT fixtures) so kmain.c's
#   lmfs_mount() test below always exercises a byte-correct, freshly
#   -formatted LMFS image, not a stale hand-edited one.
gen_lmfs_fixture() {
    if [ -f "tests/lmfs-fixture.img" ] && [ "${LUMAOS_KEEP_FIXTURES:-0}" = "1" ]; then
        return
    fi
    rm -f "tests/lmfs-fixture.img"
    mkdir -p tests
    python3 tools/mkfs.lmfs -o "tests/lmfs-fixture.img" || die "tools/mkfs.lmfs"
    echo "[gen]  generated tests/lmfs-fixture.img (tools/mkfs.lmfs, 2880 sectors)"
}
gen_lmfs_fixture

# Bake the LMFS volume onto the image at LBA 100000 — see this file's
# DAY32 header comment above for why this LBA (comfortably past the
# FAT32 fixture, which ends exactly at LBA 100000) and why the image
# grew to 102880 sectors.
if [ -f "tests/lmfs-fixture.img" ]; then
    dd if="tests/lmfs-fixture.img" of="$IMG" conv=notrunc bs=512 seek=100000 status=none
    echo "[img]  embedded tests/lmfs-fixture.img at LBA 100000 (LMFS volume, root inode only)"
else
    echo "WARNING: tests/lmfs-fixture.img not found — LBA 100000 will be left zeroed"
fi

echo "[img]  $IMG ($(stat -c%s "$IMG" 2>/dev/null || stat -f%z "$IMG") bytes)"
echo "=== Done ==="