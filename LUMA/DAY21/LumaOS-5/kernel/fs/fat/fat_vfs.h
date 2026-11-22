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

/* ===== DAY21 (Week 3, Day 21 of the 5.0 filesystem roadmap — "周复盘") =====
 *
 *   "添加错误处理（文件不存在、簇链断裂）。关闭文件功能（fat_close）。"
 *
 *   Through Day 20, every failure case fat_open()/fat_read() could hit
 *   collapsed onto the same generic -1 ("failed", per vfs.h's own
 *   "Week 10 (Day 70) is where the roadmap formalizes named error
 *   codes... until then -1 is the generic failed value" design note).
 *   That was fine while nothing needed to tell failures apart, but
 *   fat_vfs.c's own DAY19 comment on fat_read_chain() already flagged
 *   the gap this day closes: "Day 21's '簇链断裂' error-handling
 *   week-recap item is where a friendlier diagnostic for this case is
 *   planned; for now it's folded into the same -1 read-didn't-fully-
 *   succeed return."
 *
 *   fat_error_t below is a SMALL, EARLY step toward Day 70's eventual
 *   named-error-code work — not that work itself. It only distinguishes
 *   the handful of cases THIS day's roadmap text names by name ("文件
 *   不存在", "簇链断裂") plus the couple of pre-existing generic buckets
 *   they were carved out of (bad arguments, a lower-level I/O failure).
 *   Every value is still <= 0 (0 reserved for "not an error", matching
 *   `vfs_read_fn`'s own "0 == EOF, not failure" convention already
 *   documented in vfs.h) — so every existing caller that only ever
 *   checked `fat_open(...) != 0` or `n < 0` keeps working unmodified;
 *   this is purely an ADDITIVE refinement of what the negative return
 *   value now specifically means, not a signature or contract change.
 *   fat_read()'s vfs_read_fn contract in particular already promised
 *   "returns... a negative value on error" (not literally -1), so
 *   returning one of these specific codes instead was always within
 *   that contract.
 */
typedef enum {
    FAT_OK               =  0,  /* not an error */
    FAT_ERR_INVALID_ARGS = -1,  /* NULL pointer, malformed path, node not
                                    a file, unrecognized fs->name, etc. —
                                    a caller-side misuse, not a disk-
                                    content problem */
    FAT_ERR_NOT_FOUND    = -2,  /* DAY21 "文件不存在": fat_open()'s mount-
                                    slot segment names no registered
                                    mount, or no root-directory entry's
                                    name matches the requested filename */
    FAT_ERR_IO           = -3,  /* the underlying dev->read() reported
                                    failure while walking the FAT table
                                    or reading a data cluster — a real
                                    hardware/media read error, not a
                                    logically-inconsistent chain */
    FAT_ERR_CHAIN_BROKEN = -4,  /* DAY21 "簇链断裂": the cluster chain hit
                                    its own legitimate end-of-chain (EOC)
                                    marker before reaching the cluster
                                    file_pos should have landed in, i.e.
                                    the chain is genuinely shorter than
                                    what the directory entry's file size
                                    claims — a corrupt/inconsistent
                                    filesystem, not an I/O failure */
} fat_error_t;

/* fat_strerror(err)
 *
 * Turns one of the fat_error_t values above (or FAT_OK) into a short,
 * human-readable, NUL-terminated string — e.g. for cmd_show
 * (kernel/shell/cmd/show.c, DAY21's renamed cmd_cat) to print something
 * more useful than a bare negative number when fat_open()/fat_read()
 * fail. Always returns a valid pointer, even for a value outside
 * fat_error_t's own range (a defensive "unknown error" fallback string)
 * — callers never need to NULL-check the result.
 */
const char *fat_strerror(int err);

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
 * Returns 0 (FAT_OK) and fills `*out_node` (a full copy of the matching
 * child vfs_node_t — name/type/size/fs/fs_private, exactly what
 * fs->readdir() itself would have produced for that entry) if the path
 * resolves to an existing root-directory entry on a registered mount.
 *
 * DAY21: on failure, returns one of fat_error_t's negative values
 * instead of a flat -1 — still always < 0, so every pre-Day21 caller
 * that only checked `fat_open(...) != 0` keeps working unchanged:
 *
 *   FAT_ERR_INVALID_ARGS - `path`/`out_node` is NULL; `path` has no
 *     '/' at all, or the mount-slot segment before it is empty or too
 *     long to be a valid slot name (see VFS_MAX_MOUNTS's `name[8]`);
 *     the remainder after the '/' is empty (naming the mount's root
 *     itself, not a file) or contains a second '/' (unsupported
 *     subdirectory path, see above) — all caller-side path-shape
 *     mistakes, not "the file doesn't exist".
 *   FAT_ERR_NOT_FOUND - DAY21's "文件不存在": the mount-slot segment
 *     names no currently-registered mount (vfs_lookup_mount() found
 *     nothing), OR the mount-slot is valid but no root-directory
 *     entry's name matches the requested filename (fs->readdir() ran
 *     out of entries before a match was seen). Both collapse to the
 *     same code because from a `show <path>` caller's point of view
 *     they mean the same thing: "nothing exists at that path" — see
 *     show.c's DAY21 comment for how it's reported either way.
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
 * documented vfs_read_fn "0 == EOF" contract (see vfs.h).
 *
 * DAY21: on failure, returns one of fat_error_t's negative values
 * instead of a flat -1 (still always < 0, so every pre-Day21 caller
 * that only checked `n < 0` for "read failed" keeps working
 * unchanged):
 *
 *   FAT_ERR_INVALID_ARGS - `file`/`buf`/`file->node` is NULL,
 *     `file->node->type` is not VFS_TYPE_FILE, `file->node->fs->name`
 *     doesn't match a known FAT variant, or the starting cluster is 0
 *     (a 0-length file has nothing to chain-walk — this project's own
 *     fixtures never produce this, but a 0-byte file is spec-legal) —
 *     all caller/data-shape problems distinct from either error below.
 *   FAT_ERR_IO - an underlying dev->read() call failed partway through
 *     (whatever was already copied into `buf` before the failing read
 *     stays valid, but the return value is negative so the caller
 *     knows the transfer was cut short — mirrors fat12_readdir()'s own
 *     "partial progress on a later error" contract). This is a real
 *     hardware/media read failure, not a logically-broken chain.
 *   FAT_ERR_CHAIN_BROKEN - DAY21's "簇链断裂": the cluster chain hit
 *     its own legitimate FAT_CHAIN_EOC marker before reaching the
 *     cluster `file_pos`'s target byte offset should have landed in —
 *     i.e. the directory entry's file size claims more data than the
 *     chain actually contains. This is a genuinely different situation
 *     from FAT_ERR_IO (the disk read itself succeeded every time; the
 *     FAT metadata is just internally inconsistent) and is exactly the
 *     case fat_vfs.c's fat_read_chain() DAY19 comment predicted this
 *     day would give "a friendlier diagnostic" for.
 *
 * DAY20 UPDATE (Week 3, Day 20 of the 5.0 filesystem roadmap):
 *   "增加文件偏移指针 file->pos，支持多次读取。连续调用两次 read，第
 *    二次能接着上次位置读。" — this function's BODY did not change for
 *   Day 20 (see the DAY19 paragraph above: it was already written to
 *   respect whatever `file->pos` holds). What changed is who actually
 *   CALLS it more than once with a nonzero, cluster-spanning
 *   file->pos: kernel/shell/cmd/cat.c's cmd_cat now loops fs->read()
 *   until EOF instead of one capped call, and kmain.c's DAY20
 *   boot-time section calls this repeatedly with a chunk size that
 *   deliberately does not divide the 512-byte cluster size evenly, so
 *   file->pos regularly resumes mid-cluster across calls — see both
 *   files' DAY20 comments for the full rationale and exactly which
 *   fat_read_chain() code path (kernel/fs/fat/fat_vfs.c) that newly
 *   exercises.
 */
int fat_read(file_t *file, void *buf, uint32_t size);

/* fat_close(file)
 *
 * DAY21 (Week 3, Day 21 of the 5.0 filesystem roadmap — "周复盘"):
 *   "关闭文件功能（fat_close）。"
 *
 *   The shared implementation every variant's `fs->close` (see
 *   fat12_vfs.c/fat16_vfs.c/fat32_vfs.c's `fatNN_vfs_close()`
 *   trampolines, mirroring how `fatNN_vfs_read()` already just
 *   forwards to fat_read()) funnels into — see vfs_close_fn's doc
 *   comment (kernel/fs/vfs/vfs.h) for the full contract this
 *   implements. Unlike fat_read(), this needs NO per-variant branch at
 *   all: closing doesn't touch cluster chains, `*_next_cluster()`, or
 *   any variant-specific `fs_private` shape — it only ever touches the
 *   filesystem-agnostic `file_t` fields `vfs.h` itself defines
 *   (`node`, `pos`). That is why this lives directly in fat_vfs.c as
 *   ONE shared function (not three per-variant helpers behind a
 *   dispatch, the way fat_read_chain() needs), with each variant's
 *   adapter file only contributing a one-line trampoline so `fs->close`
 *   has a variant-tagged function pointer to point at, matching the
 *   shape `fs->open`/`fs->read`/`fs->readdir` already have.
 *
 * Returns FAT_OK (0) on success, having set `file->node = NULL` and
 * `file->pos = 0` (see vfs_close_fn's doc comment for why: a later
 * accidental `fs->read()` on the same `file_t` will then see a NULL
 * `file->node` and fail, per fat_read()'s own NULL-check, instead of
 * silently continuing to read a "closed" file). Returns
 * FAT_ERR_INVALID_ARGS if `file` is NULL or `file->node` is already
 * NULL (double-close, or a `file_t` that was never successfully
 * opened) — closing something that isn't open is a caller mistake
 * worth reporting, not a silent no-op.
 */
int fat_close(file_t *file);

#endif /* LUMA_FAT_VFS_H */