/* kernel/fs/vfs/vfs.c
 * LumaOS 5.0 - VFS Mount Entry Point
 *
 * DAY13 (Week 2, Day 13 of the 5.0 filesystem roadmap):
 *   "将 FAT12 挂接到 VFS：注册 fat12_filesystem_ops。实现
 *    vfs_mount("IDE0")。调用 vfs_mount 后，vfs_root 指针指向 FAT
 *    根目录。"
 *
 *   See vfs.h's DAY13 doc comment for the full contract of vfs_root/
 *   vfs_mount(). This file is the implementation: it is the ONE place
 *   in the codebase that both knows about a concrete block device
 *   (kernel/drivers/block/block.h) and a concrete filesystem backend
 *   (kernel/fs/fat/fat12_vfs.h) — everything else either stays below
 *   the block_device_t abstraction (drivers) or above the vfs_node_t
 *   abstraction (Day 14's planned cmd_ls and later shell commands).
 *
 * ---- NOTE ON THE DEVICE STRING: "IDE0", not "IDE" or "/dev/hda" ----
 *
 *   This went through two corrections before landing here:
 *
 *     1. An early draft used a Unix-style device path, "/dev/hda".
 *        Wrong: LumaOS's own userspace namespace document (计划.md's
 *        file-tree diagram) never uses Unix /dev paths anywhere — the
 *        top level of the physical-disk namespace is `IDE/` itself, a
 *        peer of `USB/`, `IMG/`, `ISO/`:
 *
 *            LumaOS
 *            ├── IDE/
 *            │   ├── Boot/
 *            │   ├── System/  (Kernel/ SysApp/ UserControler/ ...)
 *            │   ├── User/    (Desktop/ Documents/ Downloads/ ...)
 *            │   ├── Administrator/ (Index/ Desktop/ Documents/ App/)
 *            │   └── Public/
 *            ├── USB/DISK_1/, USB/DISK_2/
 *            ├── IMG/DISK_1/, ...
 *            └── ISO/LumaOS/, ...
 *
 *        So the device string was changed to plain "IDE".
 *
 *     2. Plain "IDE" was ALSO wrong, for a different reason: it can't
 *        tell two physical IDE drives apart. This project's block
 *        layer already numbers its device accessor — Day 6's
 *        block.h/block.c only expose `ide0_block_device()`, not a
 *        bare `ide_block_device()` — anticipating a future
 *        `ide1_block_device()` for a second physical drive (secondary
 *        ATA channel / a slave drive). Mirroring that numbering into
 *        the VFS device-name namespace means "IDE0", "IDE1", "IDE2"
 *        map 1:1 onto however many physical IDE drives end up probed,
 *        auto-numbered in probe order — the same pattern real OSes
 *        use (Linux's /dev/sda, /dev/sdb, ...; this project's own
 *        USB/DISK_1, USB/DISK_2 convention for the SAME reason,
 *        multiple like-devices need a way to stay distinguishable).
 *
 *   So `"IDE0"` is what vfs_mount() recognizes TODAY — it is the only
 *   device string that resolves to anything, because
 *   ide0_block_device() is the only accessor block.c currently
 *   implements (no ide1_block_device() exists yet, so there is
 *   nothing for an "IDE1" to name). Adding a second physical drive
 *   later is meant to look like: block.c grows an ide1_block_device()
 *   accessor, and this function grows one more `else if
 *   (strcmp(path, "IDE1") == 0) dev = ide1_block_device();` branch —
 *   no restructuring of vfs_mount()'s shape required, since the
 *   device-string-to-block_device_t mapping was designed to extend
 *   this way from the start.
 *
 *   A real path/mount-point resolver that turns e.g.
 *   "IDE0/System/Kernel" into a walked vfs_node_t chain is later work
 *   (Day 18's fat_open()-by-path is the first piece of that, Day
 *   68-69's `df`/`cd`/`pwd` the rest) — Day 13 only wires up the
 *   DEVICE-level mount, so vfs_mount() only ever looks at the
 *   top-level "IDE0" token, not a full path.
 *
 * ---- Resolving "IDE0" to an actual LBA ----
 *
 *   This project's real boot disk has no FAT partition at all: LBA 0
 *   is LumaOS's own custom mbr.asm bootloader (see kmain.c's DAY5/
 *   DAY9 header comments), not a partitioned disk with a FAT entry in
 *   its partition table — so on THIS project's stock image,
 *   mbr_find_first_active_fat() (Day 5) is expected to find nothing,
 *   same as it already does in kmain.c's own DAY5 test section.
 *
 *   Rather than inventing a new fixture just for Day 13, or leaving
 *   vfs_mount("IDE0") permanently unable to succeed on this project's
 *   own test image, vfs_mount() below does both:
 *
 *     1. Reads LBA 0 and runs the real Day 5 MBR-partition-scan path
 *        (mbr_parse() + mbr_find_first_active_fat()). This is the
 *        "correct" path for a real partitioned IDE disk, and costs
 *        nothing to keep working for when one exists.
 *     2. If (as expected today) no active FAT partition is found,
 *        falls back to VFS_IDE_FALLBACK_LBA — LBA 5000, the same
 *        already-embedded, already-cross-validated FAT12 test volume
 *        (tests/fat12-with-file.img, containing BIGFILE.BIN/
 *        SMALL.BIN) that kmain.c's DAY11/DAY12 sections already mount
 *        and readdir directly. This is NOT a new fixture — it is
 *        literally the same bytes on the same disk image build.sh has
 *        embedded since Day 11, just now also reachable through
 *        vfs_mount("IDE0") instead of only through kmain.c calling
 *        fat12_mount(disk, 5000, ...) by hand.
 *
 *   Once a real FAT-partitioned image exists (Day 16+'s "mkfs.fat -F
 *   16"/"-F 32" test images, or an eventual real install), step 1
 *   above starts actually finding something and step 2 stops being
 *   exercised — nothing about this function needs to change for that
 *   day to arrive; the fallback simply stops firing.
 */
#include "vfs.h"
#include "../fat/fat_vfs.h"
#include "../../drivers/block/block.h"
#include "../../drivers/block/mbr.h"
#include "../../lib/string.h"          /* strcmp */
#include "../../core/kernel.h"         /* console_write_color, printf —
                                           matches mbr.c/ide.c's own
                                           convention of a driver-level
                                           file printing its own
                                           "[tag] ..." status lines,
                                           not leaving all output to
                                           kmain.c's test sections */

/* DAY16 (Week 3, Day 16 of the 5.0 filesystem roadmap) — REFACTOR NOTE:
 *   Step 2 below used to call fat12_vfs_mount(dev, mount_lba) directly
 *   (Day 13's original code, when FAT12 was the only variant that
 *   existed). Now that fat16.h/fat16_vfs.h exist too, this file
 *   includes fat_vfs.h instead of fat12_vfs.h and calls the new
 *   fat_vfs_mount(dev, mount_lba) dispatcher (kernel/fs/fat/fat_vfs.c),
 *   which runs Day 15's fat_mount() to figure out which FAT variant is
 *   actually at `mount_lba` and routes to fat12_vfs_mount() or
 *   fat16_vfs_mount() accordingly — see fat_vfs.h's own header comment
 *   for the full rationale. Nothing about VFS_IDE_FALLBACK_LBA, the
 *   MBR-scan step, or vfs_root's contract changes: LBA 5000 is still
 *   the Day 11 FAT12 fixture and is still correctly detected as such,
 *   so this refactor is behavior-preserving for every existing caller
 *   — it only ADDS the ability for "IDE0" to resolve to a FAT16 volume
 *   on some future disk image, without requiring this file to change
 *   again when that happens (same "grows one more case, not a
 *   restructuring" property fat_vfs_mount() itself documents).
 */

/* See this file's header comment above for why LBA 5000 specifically:
 * it is Day 11's own already-embedded tests/fat12-with-file.img
 * fixture, not a new one invented for Day 13. */
#define VFS_IDE_FALLBACK_LBA  5000

/* The one supported device string through Day 13/14 — LumaOS's own
 * namespace document's top-level name for the primary IDE disk,
 * NUMBERED to match block.c's ide0_block_device() accessor (see this
 * file's header comment, part 2, for why plain "IDE" wasn't enough).
 * NOT a Unix /dev path. */
#define VFS_DEV_IDE0  "IDE0"

vfs_node_t *vfs_root = (vfs_node_t *)0;

int vfs_mount(const char *path)
{
    /* Only "IDE0" resolves to anything today — see this file's header
     * comment for why: block.c only implements ide0_block_device()
     * so far, no ide1_block_device() exists yet for an "IDE1" to name.
     * Adding one later is meant to be a single extra `else if` branch
     * here, not a restructuring of this function. */
    if (!path || strcmp(path, VFS_DEV_IDE0) != 0)
        return -1;

    block_device_t *dev = ide0_block_device();

    /* ---- Step 1: try the real MBR-partition path (Day 5) ---- */
    uint32_t mount_lba = VFS_IDE_FALLBACK_LBA;
    static uint8_t mbr_sector[512];

    if (dev->read(dev, 0, 1, mbr_sector) == 0) {
        mbr_partition_entry_t parts[MBR_PARTITION_COUNT];
        mbr_parse(mbr_sector, parts);   /* entries usable even if sig check fails */

        int active_idx;
        const mbr_partition_entry_t *active =
            mbr_find_first_active_fat(parts, &active_idx);
        if (active) {
            mount_lba = active->lba_start;
        }
        /* else: expected on this project's stock image today — see
         * this file's header comment. mount_lba stays the DAY11
         * fixture fallback set above. */
    }
    /* else: LBA 0 read itself failed — still try the fallback LBA
     * rather than giving up outright, same "degrade, don't just fail"
     * spirit as fat12_parse_bpb()'s "fill *out_geom anyway" contract. */

    /* Say out loud which physical LBA "IDE0" actually resolves to
     * before mounting it — this is exactly the ambiguity 计划.md's own
     * Day12 discussion notes flag ("为什么现在 ls 看不到 kernel.bin"):
     * "IDE0" today is NOT the same bytes as mbr.bin/loader.bin/
     * kernel.bin (those live at LBA 0-261, dd'd on raw, outside any
     * filesystem). Printing the LBA plainly means nobody has to guess
     * or go read this file's comments to know which volume "IDE0"
     * just became — the console output is self-explanatory either
     * way. Plain ASCII only in this string (matching every other
     * printf status line in the project) — the VGA text-mode font has
     * no glyph for an em dash, and an earlier draft of this line
     * learned that the hard way. */
    if (mount_lba == VFS_IDE_FALLBACK_LBA) {
        printf("[vfs] Reading LBA %u as IDE0 (DAY11 test fixture, "
               "no active FAT partition found in the MBR)\n", mount_lba);
    } else {
        printf("[vfs] Reading LBA %u as IDE0 (active FAT partition "
               "found in the MBR)\n", mount_lba);
    }

    /* ---- Step 2: auto-detect the FAT variant and wire it into the VFS ---- */
    filesystem_t *fs = fat_vfs_mount(dev, mount_lba);
    if (!fs)
        return -1;   /* vfs_root intentionally left untouched on failure */

    vfs_root = fs->root;
    return 0;
}