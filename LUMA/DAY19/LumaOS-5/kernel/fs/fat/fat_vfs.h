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

/* fat_read(file, buf, size)
 *
 * DAY19 (Week 3, Day 19 of the 5.0 filesystem roadmap):
 *   "实现 fat_read(file, buf, size)：根据起始簇，依次读取簇链，将数据
 *    拷贝到 buf。实现 cmd_cat /KERNEL.BIN，能在终端打印出内核二进制的
 *    开头部分（乱码也行，证明读到了数据）。"
 *
 *   `file` is a Day 8 `file_t` — the same shape `fat_open()`+
 *   fs->open() (fat12_vfs_open()/fat16_vfs_open()/fat32_vfs_open())
 *   already produce: `file->node` is a VFS_TYPE_FILE vfs_node_t whose
 *   `fs_private` holds the file's STARTING CLUSTER NUMBER (see
 *   fat_open()'s doc comment above, and each variant's *_vfs_readdir()
 *   — the convention is identical across FAT12/16/32) and whose `size`
 *   is the file's byte length from its directory entry. `file->pos` is
 *   the byte offset to resume reading from — 0 on every call through
 *   Day 19 (nothing sets it to anything else yet), but this function
 *   is written to respect whatever it holds so Day 20 ("增加文件偏移
 *   指针 file->pos，支持多次读取") is a caller-side change only: it
 *   only needs to update `file->pos` after a successful fat_open()+
 *   fs->open() and call fat_read() again, no change to this function's
 *   body.
 *
 *   THIS FUNCTION NEEDS ONE PER-VARIANT BRANCH, UNLIKE fat_open():
 *   fat_open()'s loop only ever calls fs->readdir(), already identical
 *   across FAT12/16/32 since Day 13/16/17. Walking a FILE's cluster
 *   chain has no such shared vfs_readdir_fn-shaped hook — each variant
 *   has its own *_next_cluster() (fat12_next_cluster() 12-bit-packed,
 *   fat16_next_cluster() plain 16-bit, fat32_next_cluster() masked
 *   28-bit — see fat12.h/fat16.h/fat32.h's own doc comments) over its
 *   own *_fs_t mount-state shape (fat12_fs_t/fat16_fs_t/fat32_fs_t —
 *   NOT byte-compatible past their shared `dev`/`mount_lba`/`bpb`
 *   prefix: fat32_fs_t replaces fat_geometry_t's `geom` field with its
 *   own flat first_data_sector/bytes_per_cluster fields plus a
 *   fat32_bpb_ext_t `ext` the other two don't have — see fat32.h's
 *   `fat32_fs_t` doc comment). So fat_read() dispatches on
 *   `file->node->fs->name` ("fat12"/"fat16"/"fat32", set by each
 *   variant's `*_filesystem_ops` table — see fat12_vfs.c/fat16_vfs.c/
 *   fat32_vfs.c) to a small per-variant helper in this file
 *   (fat12_read_chain()/fat16_read_chain()/fat32_read_chain(), static
 *   to fat_vfs.c) that knows its own fs_t shape and *_next_cluster()
 *   signature; the byte-copy loop shape itself (walk clusters, skip
 *   whole clusters until file->pos's target cluster, copy min(size,
 *   bytes-left-in-cluster, bytes-left-in-file) per cluster, advance)
 *   is identical across all three and is NOT triplicated — see
 *   fat_vfs.c's DAY19 comment for exactly how the shared shape and the
 *   per-variant cluster-table lookup are split.
 *
 * Returns the number of bytes actually copied into `buf` (0 <= n <=
 * size) and advances `file->pos` by that amount. Returns 0 (not an
 * error) if `file->pos` is already at or past `file->node->size` — the
 * documented vfs_read_fn "0 == EOF" contract (see vfs.h). Returns -1
 * if `file`/`buf`/`file->node` is NULL, `file->node->type` is not
 * VFS_TYPE_FILE, `file->node->fs->name` doesn't match a known FAT
 * variant, the starting cluster is 0 (a 0-length file has nothing to
 * chain-walk — this project's own fixtures never produce this, but a
 * 0-byte file is spec-legal), or an underlying dev->read()/
 * *_next_cluster() call fails partway through (whatever was already
 * copied into `buf` before the failing read stays valid, but the
 * return value is -1 so the caller knows the transfer was cut short —
 * mirrors fat12_readdir()'s own "partial progress on a later error"
 * contract).
 */
int fat_read(file_t *file, void *buf, uint32_t size);

#endif /* LUMA_FAT_VFS_H */