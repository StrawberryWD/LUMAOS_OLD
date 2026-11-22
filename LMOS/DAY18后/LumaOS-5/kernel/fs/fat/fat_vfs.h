/* kernel/fs/fat/fat_vfs.h
 * LumaOS 5.0 - FAT-Family Auto-Detecting VFS Mount Dispatcher
 *
 * DAY16 (Week 3, Day 16 of the 5.0 filesystem roadmap):
 *   "实现 FAT16 解析（FAT16 的根目录在数据区之前，簇号 16 位）。挂载一个
 *    FAT16 镜像，ls 能正确列出文件。"
 *
 *   This is the dispatcher fat_common.h's own DAY15 header comment
 *   foretold: "Once FAT16/32 drivers exist, the natural place for a
 *   caller to use fat_mount()'s result is a small dispatcher (e.g.
 *   inside vfs.c, or a future generic fat_vfs_mount())... not built
 *   today since only fat12_mount() exists, but this function's shape
 *   is exactly what that dispatch will need." Today FAT16's driver
 *   (fat16.h/fat16.c) and its VFS adapter (fat16_vfs.h/fat16_vfs.c)
 *   both exist alongside FAT12's, so this file ties them together:
 *   fat_vfs_mount(dev, lba) runs Day 15's fat_mount() to find out
 *   WHICH FAT variant lives at `lba`, then routes to whichever
 *   variant-specific *_vfs_mount() actually knows how to mount it.
 *
 *   This file is deliberately separate from BOTH kernel/fs/vfs/vfs.c
 *   (which stays the ONE place that resolves a device STRING like
 *   "IDE0" to a physical dev+lba — see vfs.c's own header comment) and
 *   fat_common.c (which stays variant-agnostic on principle — it has
 *   no #include of fat12.h/fat16.h and this file needs both, so this
 *   logic can't live there without breaking that layering). Instead,
 *   fat_vfs_mount() sits one layer above fat12_vfs_mount()/
 *   fat16_vfs_mount(), and is itself called by vfs_mount() (which
 *   supplies the resolved dev+lba) AND directly by kmain.c's DAY16
 *   test section (to mount the new FAT16 fixture and make it live for
 *   the shell's `ls`, alongside vfs_mount("IDE0")'s existing FAT12
 *   regression test — see kmain.c's own DAY16 comment).
 *
 * Design notes:
 *   - Returns a `filesystem_t *`, exactly what fat12_vfs_mount()/
 *     fat16_vfs_mount() themselves return — this function adds no new
 *     return shape, it just picks which of the two to call. Callers
 *     that want vfs_root updated do that themselves (`vfs_root =
 *     fs->root`), the same way vfs_mount() already does after calling
 *     fat12_vfs_mount() directly today.
 *   - FAT32 (Day 17) is a natural future `case FAT_TYPE_FAT32:` branch
 *     here once fat32_vfs_mount() exists — this function's shape does
 *     not need to change shape for that, only grow one more case.
 */
#ifndef LUMA_FAT_VFS_H
#define LUMA_FAT_VFS_H

#include "../../drivers/block/block.h"   /* block_device_t */
#include "../vfs/vfs.h"                  /* filesystem_t */

/* fat_vfs_mount(dev, lba)
 *
 * Detects which FAT variant (if any) is mounted at `lba` on `dev` via
 * fat_mount() (Day 15, kernel/fs/fat/fat_common.c), then dispatches to
 * the matching variant-specific adapter:
 *
 *   FAT_TYPE_FAT12 -> fat12_vfs_mount(dev, lba)
 *   FAT_TYPE_FAT16 -> fat16_vfs_mount(dev, lba)
 *   FAT_TYPE_FAT32 -> fat32_vfs_mount(dev, lba)  (DAY17)
 *   FAT_TYPE_UNKNOWN -> not mounted, returns NULL
 *
 * Returns the populated `filesystem_t *` on success (whichever
 * variant's *_vfs_mount() produced it), or NULL if `lba` doesn't hold
 * a FAT12/16/32 volume this project can currently mount, or if the
 * variant-specific mount call itself fails.
 */
filesystem_t *fat_vfs_mount(block_device_t *dev, uint32_t lba);

/* fat_open(path, out_node)
 *
 * DAY18 (Week 3, Day 18 of the 5.0 filesystem roadmap):
 *   "实现 fat_open(path)：根据路径（如 /KERNEL.BIN）遍历目录项，返回文件
 *    的起始簇号。对根目录文件能正确返回簇号。"
 *
 * DAY18-FOLLOWUP (user-requested change, ahead of Day 35's own "mount
 * table" plan — see vfs.h's doc comment on vfs_register_mount()/
 * vfs_lookup_mount() for the full rationale):
 *   The roadmap's own example path, "/KERNEL.BIN", assumed a single
 *   global `vfs_root` (the ORIGINAL Day 13-18 design: "the" mounted
 *   volume, whichever one mounted last). That stopped being enough
 *   once more than one volume could be mounted at once and needed to
 *   stay independently reachable — so `fat_open()` now takes a path
 *   whose FIRST segment is a mount-table slot name ("IDE0", "IDE1",
 *   "IDE2", ... — see vfs.h) instead of a leading '/', and no longer
 *   takes an explicit `filesystem_t *fs` parameter at all: which
 *   filesystem to search is now entirely determined by parsing that
 *   leading segment out of `path` itself and looking it up via
 *   vfs_lookup_mount(). Concretely: `fat_open("IDE0/BIGFILE.BIN",
 *   &node)` searches whatever volume registered as "IDE0" (in this
 *   project's kmain.c, that's the FAT12 fixture, mounted first via
 *   vfs_mount("IDE0") in the DAY13 section); `fat_open("IDE1/SMALL.
 *   BIN", &node)` searches the second-mounted volume (FAT16, DAY16
 *   section); `fat_open("IDE2/SMALLFIL.BIN", &node)` the third
 *   (FAT32, DAY17 section) — see kmain.c's DAY18 test section for all
 *   three exercised together.
 *
 *   This function still needs no per-variant FAT12/16/32 branch at
 *   all, for the same reason the original Day 18 design didn't: once
 *   vfs_lookup_mount() has produced a `filesystem_t *`, resolving the
 *   remainder of the path is just `fs->readdir(fs->root, idx, &child)`
 *   in a loop — identical across all three variants since Day 13/16/
 *   17. fat12_vfs_readdir()/fat16_vfs_readdir()/fat32_vfs_readdir()
 *   still don't change for this either.
 *
 *   PATH SYNTAX: "<mount-slot>/<filename>" — exactly one '/' expected,
 *   separating a mount-table slot name (registered by an earlier
 *   vfs_register_mount() call — see vfs.h) from one 8.3 filename
 *   naming a ROOT DIRECTORY entry on that volume. This still only
 *   covers root-directory files, per the roadmap's own explicit scope
 *   ("对根目录文件能正确返回簇号") — a path with a SECOND '/' after
 *   the mount-slot segment (e.g. "IDE0/SYS/FOO.BIN") names a file
 *   inside a subdirectory, which this function still deliberately
 *   REJECTS (-1) rather than mis-resolving, for the same reason as
 *   before: subdirectories don't exist in this project's VFS yet
 *   (real multi-level path walking is Day 42's "多级目录与路径解析"
 *   job). A path with NO '/' at all (no mount-slot segment given) is
 *   also rejected (-1) — there is deliberately no "assume the last-
 *   mounted volume" fallback anymore now that a mount table exists;
 *   every caller must name which volume it means.
 *
 *   NAME COMPARISON: exact `strcmp()` against the already-decoded,
 *   dot-joined 8.3 name fat_decode_short_name() produces (e.g.
 *   "BIGFILE.BIN") — the same string readdir() callers already print.
 *   Case-sensitive; FAT8.3 names are conventionally stored/decoded
 *   upper-case, so callers should pass the filename upper-case too.
 *
 * Returns 0 and fills `*out_node` (a full copy of the matching child
 * vfs_node_t — name/type/size/fs/fs_private, exactly what fs->readdir()
 * itself would have produced for that entry) if the path resolves to
 * an existing root-directory entry on a registered mount. Returns -1
 * if: `path`/`out_node` is NULL; `path` has no '/' at all, or the
 * mount-slot segment before it is empty or too long to be a valid slot
 * name (see VFS_MAX_MOUNTS's `name[8]`); the mount-slot segment names
 * no currently-registered mount (vfs_lookup_mount() found nothing);
 * the remainder after the '/' is empty (naming the mount's root
 * itself, not a file) or contains a second '/' (unsupported
 * subdirectory path, see above); or no root-directory entry's name
 * matches (not found — fs->readdir() runs out of entries, i.e. returns
 * nonzero, before a match is seen).
 */
int fat_open(const char *path, vfs_node_t *out_node);

#endif /* LUMA_FAT_VFS_H */