/* kernel/fs/fat/fat_vfs.c
 * LumaOS 5.0 - FAT-Family Auto-Detecting VFS Mount Dispatcher
 *
 * DAY16 (Week 3, Day 16 of the 5.0 filesystem roadmap):
 *   See fat_vfs.h's header comment for the full rationale. This file
 *   is intentionally tiny: fat_mount() (Day 15) already does all the
 *   real work of figuring out which FAT variant is at `lba`; this
 *   function's only job is picking which variant-specific
 *   *_vfs_mount() to hand that answer to.
 */
#include "fat_vfs.h"
#include "fat_common.h"    /* fat_mount(), fat_type_t */
#include "fat12_vfs.h"     /* fat12_vfs_mount() */
#include "fat16_vfs.h"     /* fat16_vfs_mount() */
#include "fat32_vfs.h"     /* DAY17: fat32_vfs_mount() */
#include "fat12.h"         /* DAY19: fat12_fs_t, fat12_next_cluster(), FAT12_CLUSTER_* */
#include "fat16.h"         /* DAY19: fat16_fs_t, fat16_next_cluster(), FAT16_CLUSTER_* */
#include "fat32.h"         /* DAY19: fat32_fs_t, fat32_next_cluster(), FAT32_CLUSTER_* */
#include "../../lib/string.h"   /* DAY18: strchr(), strcmp(); DAY19: memcpy() */

filesystem_t *fat_vfs_mount(block_device_t *dev, uint32_t lba)
{
    /* fat_mount() re-reads and re-parses the boot sector purely to
     * classify it; fat12_vfs_mount()/fat16_vfs_mount() below will each
     * read and parse it again themselves via fat12_mount()/
     * fat16_mount(). That's one extra sector read on the detection
     * path — the same "an extra read for simplicity, not worth
     * threading a pre-parsed BPB through two more layers of API"
     * trade-off fat12_next_cluster() already makes (see its DAY9
     * comment), just applied one layer up. */
    fat_type_t type = fat_mount(dev, lba, (fat_bpb_common_t *)0,
                                 (fat_geometry_t *)0);

    switch (type) {
    case FAT_TYPE_FAT12:
        return fat12_vfs_mount(dev, lba);
    case FAT_TYPE_FAT16:
        return fat16_vfs_mount(dev, lba);
    case FAT_TYPE_FAT32:
        /* DAY17: fat32_vfs_mount() now exists — this is exactly the
         * "grows one more case, not a restructuring" extension
         * fat_vfs.h's header comment predicted on Day 16. */
        return fat32_vfs_mount(dev, lba);
    default:
        /* FAT_TYPE_UNKNOWN: not a FAT volume this project recognizes
         * at all. Nothing this function can mount. */
        return (filesystem_t *)0;
    }
}

/* DAY18 (Week 3, Day 18 of the 5.0 filesystem roadmap):
 *   "实现 fat_open(path)：根据路径（如 /KERNEL.BIN）遍历目录项，返回文件
 *    的起始簇号。对根目录文件能正确返回簇号。"
 *
 * DAY18-FOLLOWUP: now resolves "<mount-slot>/<filename>" against the
 * vfs.h mount table instead of taking an explicit `filesystem_t *fs`
 * parameter. See fat_vfs.h's doc comment above the declaration for
 * the full contract, path syntax, and design rationale (why this
 * still needs no per-variant FAT branch, why only single-level
 * root-directory paths are supported today, why fs_private already
 * holds the cluster number). This is just the parse + walk + compare.
 */
/* fat_name_matches_ci(decoded, name)
 *
 * DAY26 (Week 4, Day 26 of the 5.0 filesystem roadmap) BUGFIX —
 * "大小写文件名问题": `decoded` (fat_decode_short_name()'s output) is
 * always upper-case, since fat_encode_short_name() upper-cases every
 * short name it writes (kernel/fs/fat/fat_common.c, strupper() call).
 * `name` is whatever case a caller/Shell-user actually typed. A plain
 * strcmp() between the two — what fat_open() used through Day 25 —
 * only matches when the user happens to type the exact upper-case
 * on-disk form, so e.g. `write test.txt` run a SECOND time against a
 * file that same command had just created (as "TEST.TXT") never found
 * its own earlier entry and silently created a duplicate second one
 * instead of overwriting it — exactly the duplicate-`ls`-listing
 * symptom this day's roadmap text calls out. This compares
 * case-insensitively instead, one character at a time, upper-casing
 * only `name`'s side (see fat_common.c's identically-named/-shaped
 * helper for the fixed-root side of this same fix, and fat32.c's
 * fat32_name_matches_ci() for the FAT32 side — three small per-TU
 * copies rather than a shared one, the same "static, so file-scope
 * only" pattern this project already relies on for e.g. every *_next_
 * cluster_adapter trampoline living once per file, not exported). */
static int fat_name_matches_ci(const char *decoded, const char *name)
{
    while (*decoded != '\0' && *name != '\0') {
        char a = *decoded;
        char b = *name;
        if (b >= 'a' && b <= 'z')
            b = (char)(b - 'a' + 'A');
        if (a != b)
            return 0;
        decoded++;
        name++;
    }
    return (*decoded == '\0' && *name == '\0');
}

int fat_open(const char *path, vfs_node_t *out_node)
{
    if (!path || !out_node)
        return FAT_ERR_INVALID_ARGS;

    /* Split "<mount-slot>/<filename>" at the FIRST '/'. Unlike the
     * original single-fs design, there's no optional leading '/' to
     * strip here — the '/' is mandatory, since it's the boundary
     * between the mount-slot segment and the filename, not path
     * syntax sugar. No '/' at all means no mount-slot was named. */
    const char *sep = strchr(path, '/');
    if (sep == (char *)0)
        return FAT_ERR_INVALID_ARGS;

    /* Copy the mount-slot segment (path[0..sep)) into a local buffer
     * sized to match vfs_mount_entry_t.name[8] (see vfs.h) — reject
     * anything that couldn't possibly be a valid slot name rather than
     * silently truncating it into matching the wrong slot. */
    char mount_name[8];
    {
        uint32_t len = (uint32_t)(sep - path);
        if (len == 0 || len >= sizeof(mount_name))
            return FAT_ERR_INVALID_ARGS;
        memcpy(mount_name, path, len);
        mount_name[len] = '\0';
    }

    /* DAY21 "文件不存在": an unrecognized mount-slot segment means
     * "nothing exists at that path" from the caller's point of view,
     * exactly like a filename that doesn't match anything below — see
     * fat_vfs.h's doc comment on why both collapse to FAT_ERR_NOT_FOUND
     * rather than being told apart. */
    filesystem_t *fs = vfs_lookup_mount(mount_name);
    if (!fs || !fs->readdir || !fs->root)
        return FAT_ERR_NOT_FOUND;

    /* Everything after the separator is the (single-level) filename.
     * A second '/' here means a subdirectory path ("IDE0/SYS/FOO.BIN")
     * — not supported until Day 42's multi-level path walking exists
     * (see fat_vfs.h's doc comment). Fail cleanly now rather than
     * silently treating "SYS/FOO.BIN" as one (wrong, unmatchable)
     * literal name. Both are caller-side path-SHAPE mistakes (not "the
     * file doesn't exist"), so FAT_ERR_INVALID_ARGS, not
     * FAT_ERR_NOT_FOUND. */
    const char *name = sep + 1;
    if (name[0] == '\0')
        return FAT_ERR_INVALID_ARGS;
    if (strchr(name, '/') != (char *)0)
        return FAT_ERR_INVALID_ARGS;

    /* Walk the root directory exactly the way kmain.c's DAY13/16/17
     * test sections already do (fs->readdir(fs->root, idx, &child) in
     * a loop until it returns nonzero) — the one loop shape that
     * already works identically across FAT12/16/32, since Day 13/16/17
     * gave all three variants byte-for-byte the same vfs_readdir_fn
     * signature and "fs_private == the entry's start_cluster" output
     * convention. */
    vfs_node_t child;
    uint32_t idx = 0;
    while (fs->readdir(fs->root, idx, &child) == 0) {
        /* DAY26 bugfix: case-insensitive match — see fat_name_matches_
         * ci()'s doc comment above. */
        if (fat_name_matches_ci(child.name, name)) {
            *out_node = child;
            return FAT_OK;
        }
        idx++;
    }

    /* fs->readdir() ran out of entries (returned -1) before a match
     * was seen — per vfs_readdir_fn's own contract this means "index
     * past the last entry", i.e. the whole directory was scanned and
     * nothing named `name` exists there. DAY21 "文件不存在" proper. */
    return FAT_ERR_NOT_FOUND;
}
/* ===================================================================
 * DAY19 (Week 3, Day 19 of the 5.0 filesystem roadmap):
 *   "实现 fat_read(file, buf, size)：根据起始簇，依次读取簇链，将数据
 *    拷贝到 buf。实现 cmd_cat /KERNEL.BIN，能在终端打印出内核二进制的
 *    开头部分（乱码也行，证明读到了数据）。"
 *
 * See fat_vfs.h's doc comment above fat_read()'s declaration for the
 * full contract and the "why one shared loop + three tiny per-variant
 * adapters, not three triplicated loops" rationale. This section is
 * the implementation of that split:
 *
 *   fat_chain_ctx_t        - the handful of variant-agnostic values
 *                             the shared copy loop actually needs
 *                             (which block_device_t, where the data
 *                             region starts, how many bytes a cluster
 *                             holds) — filled in by one of the three
 *                             *_chain_ctx_init() adapters below.
 *   fat_next_cluster_fn    - a function-pointer type every variant's
 *                             *_next_cluster() is wrapped to match
 *                             (each real *_next_cluster() takes its
 *                             own `fat12_fs_t *`/`fat16_fs_t *`/
 *                             `fat32_fs_t *` first argument and a
 *                             uint16_t or uint32_t cluster number —
 *                             not literally the same C type, so a
 *                             thin per-variant trampoline is what
 *                             makes them callable through one pointer
 *                             type from the shared loop).
 *   fat12_next_cluster_adapter/fat16_.../fat32_...
 *                           - the three trampolines above, each just
 *                             casting `ctx->fs_private` back to its
 *                             real type and forwarding to the real
 *                             *_next_cluster(), widening its 12/16/28-
 *                             bit-range return into one common
 *                             convention: >= 0 real value, negative on
 *                             I/O error, and FAT_CHAIN_EOC as the one
 *                             shared "end of chain" sentinel (each
 *                             variant's own EOC range starts at a
 *                             different raw value — 0xFF8/0xFFF8/
 *                             0x0FFFFFF8 — so the trampoline is also
 *                             where that per-variant range check
 *                             happens, once, rather than inside the
 *                             shared loop).
 *   fat_read_chain()        - the ONE cluster-chain copy loop, written
 *                             once against fat_chain_ctx_t + a
 *                             fat_next_cluster_fn, shared by all three
 *                             variants.
 *   fat_read()               - the public entry point: figures out
 *                             which variant `file->node->fs` is (by
 *                             `fs->name`), fills in a fat_chain_ctx_t
 *                             via the matching adapter, and calls
 *                             fat_read_chain().
 * =================================================================== */

/* FAT_CHAIN_EOC — the one shared "no next cluster, chain legitimately
 * ends here" sentinel the three trampolines below normalize their own
 * variant's EOC range down to, so fat_read_chain() itself only ever
 * needs to compare against ONE value instead of knowing FAT12/16/32's
 * three different raw EOC-range-start constants. Deliberately a
 * negative-looking-but-not-error value distinct from every legal
 * cluster number (2..0x0FFFFFFE) and from the trampolines' own -1
 * ("dev->read() failed") — chosen as -2 so a caller that ever printed
 * "next_cluster() returned %d" could tell EOC and I/O failure apart
 * at a glance while debugging, same spirit as this project's existing
 * "-1 means error, small non-negative means a real value" convention
 * just extended by one more distinguishable sentinel. */
#define FAT_CHAIN_EOC   (-2)

/* fat_chain_ctx_t — see this section's header comment above. Exactly
 * the fields fat_read_chain()'s shared loop needs to turn "cluster
 * number N" into "disk-absolute LBA of cluster N's first sector" and
 * "how many bytes am I copying per cluster", regardless of which FAT
 * variant is underneath. `fs_private` is the ONLY variant-specific
 * escape hatch — it's what each trampoline (below) casts back to its
 * real fat12_fs_t / fat16_fs_t / fat32_fs_t pointer to call the real
 * *_next_cluster(); fat_read_chain() itself never touches it directly,
 * only ever hands it back to next_cluster(). */
typedef struct {
    block_device_t *dev;               /* underlying block device */
    uint32_t         mount_lba;        /* disk-absolute LBA of this volume's boot sector */
    uint32_t         first_data_sector;/* volume-relative first sector of cluster 2 */
    uint32_t         sectors_per_cluster;
    uint32_t         bytes_per_cluster;
    void            *fs_private;       /* real fat12_fs_t / fat16_fs_t / fat32_fs_t
                                           pointer, opaque to fat_read_chain() itself */
} fat_chain_ctx_t;

typedef int32_t (*fat_next_cluster_fn)(fat_chain_ctx_t *ctx, uint32_t cluster);

/* ---- FAT12 trampoline ------------------------------------------- */
static int32_t fat12_next_cluster_adapter(fat_chain_ctx_t *ctx, uint32_t cluster)
{
    fat12_fs_t *fs = (fat12_fs_t *)ctx->fs_private;
    int32_t v = fat12_next_cluster(fs, (uint16_t)cluster);
    if (v < 0)
        return -1;                              /* dev->read() failure */
    if ((uint32_t)v >= FAT12_CLUSTER_EOC_MIN)
        return FAT_CHAIN_EOC;
    return v;
}

/* ---- FAT16 trampoline ------------------------------------------- */
static int32_t fat16_next_cluster_adapter(fat_chain_ctx_t *ctx, uint32_t cluster)
{
    fat16_fs_t *fs = (fat16_fs_t *)ctx->fs_private;
    int32_t v = fat16_next_cluster(fs, (uint16_t)cluster);
    if (v < 0)
        return -1;
    if ((uint32_t)v >= FAT16_CLUSTER_EOC_MIN)
        return FAT_CHAIN_EOC;
    return v;
}

/* ---- FAT32 trampoline ------------------------------------------- */
static int32_t fat32_next_cluster_adapter(fat_chain_ctx_t *ctx, uint32_t cluster)
{
    fat32_fs_t *fs = (fat32_fs_t *)ctx->fs_private;
    int64_t v = fat32_next_cluster(fs, cluster);
    if (v < 0)
        return -1;
    if ((uint32_t)v >= FAT32_CLUSTER_EOC_MIN)
        return FAT_CHAIN_EOC;
    return (int32_t)v;
}

/* fat_read_chain(ctx, next_cluster, start_cluster, file_pos,
 *                 file_size, buf, size)
 *
 * The one shared cluster-chain copy loop every variant's fat_read()
 * call funnels into (see this section's header comment). Walks the
 * chain starting at `start_cluster`, first SKIPPING whole clusters
 * until it reaches the one containing byte offset `file_pos` (Day 20
 * is what will actually exercise `file_pos != 0` from a real caller;
 * this loop already handles it correctly today rather than assuming
 * 0, so Day 20 needs no change here), then copies up to `size` bytes
 * — clamped to both `size` and however many bytes remain before
 * `file_size` — into `buf`, cluster by cluster.
 *
 * A single `static uint8_t` cluster-sized scratch buffer is used
 * (this project has no kernel heap yet — see fat12_vfs.h's DAY13
 * design notes — and the 4KB kernel stack, kernel/arch/entry.asm,
 * rules out a stack-allocated one for any cluster bigger than a few
 * hundred bytes) sized FAT_READ_MAX_CLUSTER_BYTES: large enough for
 * every fixture this project mounts today (512-byte sectors *
 * BPB_SecPerClus, and this project's own FAT12/16/32 fixtures all use
 * SecPerClus=1, i.e. 512-byte clusters — see build.sh's
 * gen_fat12_fixtures()/gen_fat16_fixtures()/gen_fat32_fixtures()) with
 * generous headroom for a more aggressively-clustered real-world
 * volume (SecPerClus up to 64, the largest a spec-legal FAT32 volume
 * can use with 512-byte sectors) without the buffer becoming another
 * multi-tens-of-KB static consumer. A `bytes_per_cluster` larger than
 * this is treated as a read error rather than silently truncated —
 * see the check below.
 *
 * Returns the number of bytes copied (>= 0) on success (including 0
 * if `file_pos >= file_size`, the documented EOF case).
 *
 * DAY21: on failure, returns fat_error_t's FAT_ERR_IO or
 * FAT_ERR_CHAIN_BROKEN (both < 0) instead of a flat -1 for every
 * failure case — see fat_vfs.h's DAY21 doc comment on fat_read() for
 * the full rationale of why these are told apart now (this is exactly
 * the "friendlier diagnostic" this function's own comment predicted
 * Day 21 would add):
 *
 *   FAT_ERR_IO - `next_cluster()` reported a genuine I/O failure
 *     (`dev->read()` returned nonzero) while walking the FAT table or
 *     reading a data cluster.
 *   FAT_ERR_CHAIN_BROKEN - the chain reached FAT_CHAIN_EOC before
 *     `file_pos`'s target cluster was reached — the chain is
 *     genuinely shorter than `file_size` claims (a corrupt/inconsistent
 *     filesystem), which is a different situation from an I/O failure:
 *     every disk read along the way succeeded, the DATA itself just
 *     doesn't agree with the directory entry.
 *   FAT_ERR_INVALID_ARGS - `bytes_per_cluster` is 0 or exceeds
 *     FAT_READ_MAX_CLUSTER_BYTES (an unsupported/malformed volume
 *     geometry, not a data-integrity problem).
 *
 * A read that fails PARTWAY through the copy loop (below) after
 * already copying some bytes still returns that partial byte count
 * (unchanged from Day 19/20 behavior) rather than one of the codes
 * above — see the copy loop's own comments for exactly which of the
 * two failure kinds can happen there and why partial progress still
 * wins.
 */
#define FAT_READ_MAX_CLUSTER_BYTES  (64u * 512u)   /* 64 sectors/cluster * 512 bytes */

static int32_t fat_read_chain(fat_chain_ctx_t *ctx, fat_next_cluster_fn next_cluster,
                               uint32_t start_cluster, uint32_t file_pos,
                               uint32_t file_size, void *buf, uint32_t size)
{
    static uint8_t cluster_buf[FAT_READ_MAX_CLUSTER_BYTES];

    if (ctx->bytes_per_cluster == 0 ||
        ctx->bytes_per_cluster > FAT_READ_MAX_CLUSTER_BYTES)
        return FAT_ERR_INVALID_ARGS;

    /* Clamp the request to what's actually left in the file — reading
     * past file_size would hand the caller stale/unrelated cluster
     * bytes past the file's real end, not a legal read. */
    if (file_pos >= file_size)
        return 0;                       /* EOF, per vfs_read_fn's "0 == EOF" contract */
    if (size > file_size - file_pos)
        size = file_size - file_pos;
    if (size == 0)
        return 0;

    uint32_t cluster = start_cluster;
    uint32_t cluster_index = file_pos / ctx->bytes_per_cluster;
    uint32_t offset_in_cluster = file_pos % ctx->bytes_per_cluster;

    /* Skip whole clusters until we reach the one file_pos points into.
     * On every call through Day 19 this loop body runs zero times
     * (file_pos is always 0 until Day 20's file->pos-carrying callers
     * exist), but it's written now so Day 20 is a caller-side change
     * only — see fat_vfs.h's doc comment. DAY21: the two ways this loop
     * can fail are now told apart — see this function's own doc
     * comment above. */
    for (uint32_t i = 0; i < cluster_index; i++) {
        int32_t next = next_cluster(ctx, cluster);
        /* DAY21 BUGFIX: FAT_CHAIN_EOC is itself a negative sentinel
         * (-2, see this file's FAT_CHAIN_EOC comment above) — a plain
         * `next < 0` check would catch it FIRST and misreport a
         * legitimate end-of-chain as FAT_ERR_IO. Harmless pre-Day21
         * (both branches returned the same flat -1), but now that the
         * two cases return DIFFERENT codes, EOC must be checked before
         * the generic "negative == I/O error" test, not after. */
        if (next == FAT_CHAIN_EOC)
            return FAT_ERR_CHAIN_BROKEN;   /* chain too short for file_pos — corrupt/short chain */
        if (next < 0)
            return FAT_ERR_IO;             /* I/O error walking the chain */
        cluster = (uint32_t)next;
    }

    uint32_t copied = 0;
    while (copied < size) {
        uint32_t lba = ctx->mount_lba + ctx->first_data_sector +
                        (cluster - 2) * ctx->sectors_per_cluster;

        if (ctx->dev->read(ctx->dev, lba, (uint8_t)ctx->sectors_per_cluster,
                            cluster_buf) != 0)
            return (copied > 0) ? (int32_t)copied : FAT_ERR_IO;

        uint32_t avail_in_cluster = ctx->bytes_per_cluster - offset_in_cluster;
        uint32_t remaining        = size - copied;
        uint32_t chunk            = (avail_in_cluster < remaining)
                                       ? avail_in_cluster : remaining;

        memcpy((uint8_t *)buf + copied, cluster_buf + offset_in_cluster, chunk);
        copied += chunk;
        offset_in_cluster = 0;       /* only the FIRST cluster can have a nonzero start */

        if (copied >= size)
            break;

        int32_t next = next_cluster(ctx, cluster);
        /* DAY21 BUGFIX: same EOC-vs-negative ordering fix as the skip
         * loop above — check FAT_CHAIN_EOC (-2) before the generic
         * `next < 0` test, so a legitimate end-of-chain here still
         * takes the "return what we have" path rather than being
         * misreported as an I/O error. (In practice `copied > 0`
         * always holds by this point in the loop, so the old ordering
         * happened to still return the right byte count here — unlike
         * the skip loop above, which had no such accidental cover —
         * but the ordering was still backwards on its own terms and is
         * fixed here for the same reason.) */
        if (next == FAT_CHAIN_EOC)
            break;                   /* legitimate end of chain — return what we have */
        if (next < 0)
            return (copied > 0) ? (int32_t)copied : FAT_ERR_IO;
        cluster = (uint32_t)next;
    }

    return (int32_t)copied;
}

int fat_read(file_t *file, void *buf, uint32_t size)
{
    if (!file || !buf || !file->node)
        return FAT_ERR_INVALID_ARGS;

    vfs_node_t *node = file->node;
    if (node->type != VFS_TYPE_FILE)
        return FAT_ERR_INVALID_ARGS;

    uint32_t start_cluster = (uint32_t)(uintptr_t)node->fs_private;
    if (start_cluster == 0)
        return FAT_ERR_INVALID_ARGS;   /* no data cluster to chain-walk (see doc comment) */

    if (!node->fs || !node->fs->name)
        return FAT_ERR_INVALID_ARGS;

    fat_chain_ctx_t ctx;
    fat_next_cluster_fn next_cluster;

    if (strcmp(node->fs->name, "fat12") == 0) {
        fat12_fs_t *fs = (fat12_fs_t *)node->fs->fs_private;
        if (!fs) return -1;
        ctx.dev                = fs->dev;
        ctx.mount_lba           = fs->mount_lba;
        ctx.first_data_sector   = fs->geom.first_data_sector;
        ctx.sectors_per_cluster = fs->bpb.BPB_SecPerClus;
        ctx.bytes_per_cluster   = fs->geom.bytes_per_cluster;
        ctx.fs_private          = fs;
        next_cluster            = fat12_next_cluster_adapter;
    } else if (strcmp(node->fs->name, "fat16") == 0) {
        fat16_fs_t *fs = (fat16_fs_t *)node->fs->fs_private;
        if (!fs) return -1;
        ctx.dev                = fs->dev;
        ctx.mount_lba           = fs->mount_lba;
        ctx.first_data_sector   = fs->geom.first_data_sector;
        ctx.sectors_per_cluster = fs->bpb.BPB_SecPerClus;
        ctx.bytes_per_cluster   = fs->geom.bytes_per_cluster;
        ctx.fs_private          = fs;
        next_cluster            = fat16_next_cluster_adapter;
    } else if (strcmp(node->fs->name, "fat32") == 0) {
        fat32_fs_t *fs = (fat32_fs_t *)node->fs->fs_private;
        if (!fs) return -1;
        ctx.dev                = fs->dev;
        ctx.mount_lba           = fs->mount_lba;
        ctx.first_data_sector   = fs->first_data_sector;
        ctx.sectors_per_cluster = fs->bpb.BPB_SecPerClus;
        ctx.bytes_per_cluster   = fs->bytes_per_cluster;
        ctx.fs_private          = fs;
        next_cluster            = fat32_next_cluster_adapter;
    } else {
        return FAT_ERR_INVALID_ARGS;   /* unknown filesystem name — not a FAT12/16/32 mount */
    }

    /* DAY21: forward fat_read_chain()'s specific negative code
     * (FAT_ERR_IO / FAT_ERR_CHAIN_BROKEN / FAT_ERR_INVALID_ARGS)
     * unchanged rather than flattening it back to -1 — see this
     * function's DAY21 doc comment in fat_vfs.h. */
    int32_t n = fat_read_chain(&ctx, next_cluster, start_cluster,
                                file->pos, node->size, buf, size);
    if (n < 0)
        return n;

    file->pos += (uint32_t)n;
    return n;
}

/* ===================================================================
 * DAY21 (Week 3, Day 21 of the 5.0 filesystem roadmap — "周复盘"):
 *   "添加错误处理（文件不存在、簇链断裂）。关闭文件功能（fat_close）。"
 *
 * fat_strerror() and fat_close() — see fat_vfs.h's doc comments above
 * each declaration for the full contract and rationale. Both are
 * implemented here (fat_vfs.c) rather than per-variant, for the same
 * reason: neither needs any FAT12/16/32-specific knowledge, unlike
 * fat_read()'s cluster-chain walk.
 * =================================================================== */

const char *fat_strerror(int err)
{
    switch (err) {
    case FAT_OK:               return "success";
    case FAT_ERR_INVALID_ARGS: return "invalid arguments";
    case FAT_ERR_NOT_FOUND:    return "not found";
    case FAT_ERR_IO:           return "I/O error";
    case FAT_ERR_CHAIN_BROKEN: return "cluster chain broken";
    default:                   return "unknown error";
    }
}

int fat_close(file_t *file)
{
    if (!file || !file->node)
        return FAT_ERR_INVALID_ARGS;   /* NULL file, or double-close */

    file->node = (vfs_node_t *)0;
    file->pos  = 0;
    return FAT_OK;
}

/* ===================================================================
 * DAY22 (Week 4, Day 22 of the 5.0 filesystem roadmap — start of
 *   "第 4 周：FAT 写入支持（创建、删除、截断）"):
 *   "实现 fat_find_free_cluster()：扫描 FAT 表，返回第一个值为 0 的簇。
 *    测试打印空闲簇号。"
 *
 * See fat_vfs.h's doc comment above the declaration for the full
 * contract and the "why this reuses DAY19's next_cluster trampolines
 * completely unchanged" rationale. This is just the per-variant
 * `total_clusters` lookup (the one thing fat_read()'s own three-way
 * dispatch above never needed) plus the scan loop itself.
 * =================================================================== */
int fat_find_free_cluster(filesystem_t *fs, uint32_t *out_cluster)
{
    if (!fs || !out_cluster || !fs->name || !fs->fs_private)
        return FAT_ERR_INVALID_ARGS;

    fat_chain_ctx_t ctx;
    fat_next_cluster_fn next_cluster;
    uint32_t total_clusters;

    /* Same three-way "which variant is this fs?" dispatch fat_read()
     * performs above — only `ctx.fs_private` needs filling in here
     * (the only field the trampolines themselves ever read; see
     * fat_chain_ctx_t's own header comment), plus each variant's own
     * total_clusters count, which no function before today has needed. */
    if (strcmp(fs->name, "fat12") == 0) {
        fat12_fs_t *f = (fat12_fs_t *)fs->fs_private;
        ctx.fs_private = f;
        next_cluster   = fat12_next_cluster_adapter;
        total_clusters = f->geom.total_clusters;
    } else if (strcmp(fs->name, "fat16") == 0) {
        fat16_fs_t *f = (fat16_fs_t *)fs->fs_private;
        ctx.fs_private = f;
        next_cluster   = fat16_next_cluster_adapter;
        total_clusters = f->geom.total_clusters;
    } else if (strcmp(fs->name, "fat32") == 0) {
        fat32_fs_t *f = (fat32_fs_t *)fs->fs_private;
        ctx.fs_private = f;
        next_cluster   = fat32_next_cluster_adapter;
        total_clusters = f->total_clusters;
    } else {
        return FAT_ERR_INVALID_ARGS;   /* unknown filesystem name — not a FAT12/16/32 mount */
    }

    /* Valid data-cluster numbers run 2 .. total_clusters+1 inclusive —
     * see this function's own doc comment in fat_vfs.h for why the
     * scan starts at 2, not 0. */
    uint32_t last_cluster = total_clusters + 1;

    for (uint32_t cluster = 2; cluster <= last_cluster; cluster++) {
        int32_t v = next_cluster(&ctx, cluster);

        /* Same EOC-before-negative ordering DAY21's fat_read_chain()
         * bugfix established above — FAT_CHAIN_EOC is itself a
         * negative sentinel, so it must be checked before the generic
         * "negative == I/O error" test. An EOC entry marks the LAST
         * cluster of some already-allocated file's chain — still in
         * use, not free. */
        if (v == FAT_CHAIN_EOC)
            continue;
        if (v < 0)
            return FAT_ERR_IO;
        if (v == 0) {
            /* FATxx_CLUSTER_FREE == 0 on every variant — the first
             * unallocated cluster this scan has found. */
            *out_cluster = cluster;
            return FAT_OK;
        }
        /* v > 0 and not EOC: a real next-cluster link somewhere in an
         * allocated chain, or a RESERVED/BAD sentinel (also > 0) —
         * either way this cluster is not free. Keep scanning. */
    }

    /* Scanned every data cluster on the volume without finding one
     * whose value is 0 — the volume is full. */
    return FAT_ERR_NOT_FOUND;
}

/* ===================================================================
 * DAY23 (Week 4, Day 23 of the 5.0 filesystem roadmap):
 *   "实现 fat_alloc_cluster()：分配一个新簇，并更新 FAT 表（写回磁盘）。
 *    分配成功后，再次读取 FAT 对应位置值变为 0xFFF（FAT12）或 0xFFFF
 *    （FAT16）。"
 *
 * See fat_vfs.h's doc comment above the declaration for the full
 * contract and the "why an EOC marker, not a real link" rationale.
 * This is the write-side counterpart of DAY19's fat_next_cluster_fn
 * trampoline pattern: fat_set_cluster_fn/fat12_set_cluster_adapter/
 * fat16_.../fat32_... wrap each variant's new Day 23
 * *_set_cluster_value() the exact same way the Day 19 trampolines
 * above wrap *_next_cluster() — same `ctx->fs_private` cast-back-and-
 * forward shape, just one direction reversed (writing instead of
 * reading).
 * =================================================================== */
typedef int32_t (*fat_set_cluster_fn)(fat_chain_ctx_t *ctx, uint32_t cluster,
                                       uint32_t value);

/* ---- FAT12 trampoline ------------------------------------------- */
static int32_t fat12_set_cluster_adapter(fat_chain_ctx_t *ctx, uint32_t cluster,
                                          uint32_t value)
{
    fat12_fs_t *fs = (fat12_fs_t *)ctx->fs_private;
    return (int32_t)fat12_set_cluster_value(fs, (uint16_t)cluster, (uint16_t)value);
}

/* ---- FAT16 trampoline ------------------------------------------- */
static int32_t fat16_set_cluster_adapter(fat_chain_ctx_t *ctx, uint32_t cluster,
                                          uint32_t value)
{
    fat16_fs_t *fs = (fat16_fs_t *)ctx->fs_private;
    return (int32_t)fat16_set_cluster_value(fs, (uint16_t)cluster, (uint16_t)value);
}

/* ---- FAT32 trampoline ------------------------------------------- */
static int32_t fat32_set_cluster_adapter(fat_chain_ctx_t *ctx, uint32_t cluster,
                                          uint32_t value)
{
    fat32_fs_t *fs = (fat32_fs_t *)ctx->fs_private;
    return (int32_t)fat32_set_cluster_value(fs, cluster, value);
}

int fat_alloc_cluster(filesystem_t *fs, uint32_t *out_cluster)
{
    if (!fs || !out_cluster || !fs->name || !fs->fs_private)
        return FAT_ERR_INVALID_ARGS;

    /* ---- Find half: reuse DAY22 completely unchanged ---- */
    uint32_t free_cluster;
    int rc = fat_find_free_cluster(fs, &free_cluster);
    if (rc != FAT_OK)
        return rc;   /* FAT_ERR_NOT_FOUND (volume full) or FAT_ERR_IO,
                       * forwarded as-is — nothing to claim either way. */

    /* ---- Claim half: same three-way "which variant is this fs?"
     * dispatch fat_find_free_cluster() performs above, this time
     * picking a *_set_cluster_adapter() and this variant's own
     * EOC-max sentinel instead of a next_cluster adapter. */
    fat_chain_ctx_t ctx;
    fat_set_cluster_fn set_cluster;
    block_device_t *dev;
    uint32_t eoc_value;

    if (strcmp(fs->name, "fat12") == 0) {
        fat12_fs_t *f = (fat12_fs_t *)fs->fs_private;
        ctx.fs_private = f;
        dev            = f->dev;
        set_cluster    = fat12_set_cluster_adapter;
        eoc_value      = FAT12_CLUSTER_EOC_MAX;
    } else if (strcmp(fs->name, "fat16") == 0) {
        fat16_fs_t *f = (fat16_fs_t *)fs->fs_private;
        ctx.fs_private = f;
        dev            = f->dev;
        set_cluster    = fat16_set_cluster_adapter;
        eoc_value      = FAT16_CLUSTER_EOC_MAX;
    } else if (strcmp(fs->name, "fat32") == 0) {
        fat32_fs_t *f = (fat32_fs_t *)fs->fs_private;
        ctx.fs_private = f;
        dev            = f->dev;
        set_cluster    = fat32_set_cluster_adapter;
        eoc_value      = FAT32_CLUSTER_EOC_MAX;
    } else {
        return FAT_ERR_INVALID_ARGS;   /* unknown filesystem name — not a FAT12/16/32 mount */
    }

    /* A read-only backend (dev->write == NULL, see block.h's doc
     * comment on block_write_fn) can never claim a cluster — this is
     * a permanent property of the mount, not a transient I/O hiccup,
     * but FAT_ERR_IO is still the closest existing fat_error_t value
     * (see fat_vfs.h's doc comment: no dedicated "read-only" code
     * exists yet). */
    if (!dev || !dev->write)
        return FAT_ERR_IO;

    if (set_cluster(&ctx, free_cluster, eoc_value) != 0)
        return FAT_ERR_IO;   /* dev->read()/dev->write() failed persisting the EOC marker */

    *out_cluster = free_cluster;
    return FAT_OK;
}

/* ===================================================================
 * DAY24 (Week 4, Day 24 of the 5.0 filesystem roadmap):
 *   "实现 fat_create_file(name)：在根目录区找一个空条目（首字节 0x00
 *    或 0xE5），填入文件名、起始簇 0、大小 0。"
 *
 * See fat_vfs.h's doc comment above the declaration for the full
 * contract (path syntax, duplicate-name check, per-variant dispatch
 * rationale). This is just the implementation: parse the path exactly
 * like fat_open() does, reject a duplicate via fat_open() itself, then
 * dispatch to the matching variant's create call.
 * =================================================================== */
int fat_create_file(const char *path)
{
    if (!path)
        return FAT_ERR_INVALID_ARGS;

    /* ---- Same "<mount-slot>/<filename>" split fat_open() uses ---- */
    const char *sep = strchr(path, '/');
    if (sep == (char *)0)
        return FAT_ERR_INVALID_ARGS;

    char mount_name[8];
    {
        uint32_t len = (uint32_t)(sep - path);
        if (len == 0 || len >= sizeof(mount_name))
            return FAT_ERR_INVALID_ARGS;
        memcpy(mount_name, path, len);
        mount_name[len] = '\0';
    }

    filesystem_t *fs = vfs_lookup_mount(mount_name);
    if (!fs || !fs->name || !fs->fs_private)
        return FAT_ERR_NOT_FOUND;

    const char *name = sep + 1;
    if (name[0] == '\0')
        return FAT_ERR_INVALID_ARGS;
    if (strchr(name, '/') != (char *)0)
        return FAT_ERR_INVALID_ARGS;

    /* ---- Duplicate-name check: reuse fat_open() itself, same path,
     * unchanged — see this function's own doc comment in fat_vfs.h for
     * why this lives here rather than inside the variant-specific
     * create calls. A SUCCESSFUL fat_open() here means `name` already
     * exists; anything else (FAT_ERR_NOT_FOUND in particular) means
     * the coast is clear to create it. ---- */
    {
        vfs_node_t existing;
        if (fat_open(path, &existing) == FAT_OK)
            return FAT_ERR_INVALID_ARGS;   /* already exists — refuse the duplicate */
    }

    /* ---- Per-variant dispatch — same three-way "which variant is
     * this fs?" split fat_read()/fat_find_free_cluster()/
     * fat_alloc_cluster() all perform above. ---- */
    int rc;

    if (strcmp(fs->name, "fat12") == 0) {
        fat12_fs_t *f = (fat12_fs_t *)fs->fs_private;
        rc = fat_create_entry_fixed_root(f->dev, f->mount_lba, &f->geom, name);
    } else if (strcmp(fs->name, "fat16") == 0) {
        fat16_fs_t *f = (fat16_fs_t *)fs->fs_private;
        rc = fat_create_entry_fixed_root(f->dev, f->mount_lba, &f->geom, name);
    } else if (strcmp(fs->name, "fat32") == 0) {
        fat32_fs_t *f = (fat32_fs_t *)fs->fs_private;
        rc = fat32_create_entry_root(f, name);
    } else {
        return FAT_ERR_INVALID_ARGS;   /* unknown filesystem name — not a FAT12/16/32 mount */
    }

    /* fat_create_entry_fixed_root()/fat32_create_entry_root() only
     * ever return 0/-1 (see each of their own doc comments — neither
     * distinguishes "bad name"/"read-only"/"I/O failure"/"region full"
     * any more finely than a flat failure) — normalize that single -1
     * into FAT_ERR_IO here, the closest existing fat_error_t bucket,
     * the same "no dedicated code exists yet" choice fat_alloc_
     * cluster() already makes for its own read-only-backend case. */
    return (rc == 0) ? FAT_OK : FAT_ERR_IO;
}

/* ===================================================================
 * DAY25 (Week 4, Day 25 of the 5.0 filesystem roadmap):
 *   "实现 fat_write(file, buf, size)：处理簇链扩展（如果当前簇链不够，
 *    追加新簇）。在 Shell 中输入 write test.txt，写入内容，show 能看到。"
 *
 * See fat_vfs.h's doc comment above the declaration for the full
 * contract (overwrite-from-0 semantics, why a path not a file_t, the
 * "chain extension reuses fat_alloc_cluster() + *_set_cluster_value()"
 * rationale fat_alloc_cluster()'s own DAY23 doc comment predicted).
 * This is just the implementation: same "<mount-slot>/<filename>"
 * parse + fat_open() existence check fat_create_file() itself uses,
 * then the same three-way "which variant is this fs?" dispatch every
 * other per-variant fat_vfs.c function performs — this time filling in
 * a fat_chain_ctx_t (reused unchanged from the DAY19/22/23 read-side
 * machinery above: it already carries every geometry field a cluster-
 * by-cluster byte copy needs) plus this variant's fat_set_cluster_fn
 * (DAY23) and EOC-max sentinel, then the write copy loop itself.
 * =================================================================== */

/* fat_write_max_cluster_bytes — same FAT_READ_MAX_CLUSTER_BYTES ceiling
 * fat_read_chain() uses for its own static scratch buffer, reused here
 * unchanged: this project's own fixtures never exceed 512-byte
 * clusters (SecPerClus=1), and the same "generous headroom, still
 * bounded" rationale applies to the write side. A single shared
 * static buffer (not two) keeps this function from doubling the
 * kernel's .bss usage for what is, in practice, the exact same 512-
 * byte scratch region fat_read_chain() already reserves — but it is a
 * SEPARATE buffer (not the same symbol), since fat_read_chain()'s own
 * cluster_buf is `static` to its own function scope and not reachable
 * from here. */
#define FAT_WRITE_MAX_CLUSTER_BYTES  (64u * 512u)

int fat_write(const char *path, const void *buf, uint32_t size)
{
    if (!path)
        return FAT_ERR_INVALID_ARGS;
    if (!buf && size != 0)
        return FAT_ERR_INVALID_ARGS;

    /* ---- Must already exist: fat_open() forwards its own specific
     * fat_error_t code unchanged (see fat_vfs.h's doc comment: "the
     * file named by `path` must already exist... fat_open() failing on
     * `path` is a plain error here"). This also gives us the file's
     * CURRENT directory-entry state (start cluster, old size) without
     * a second lookup. ---- */
    vfs_node_t node;
    int open_err = fat_open(path, &node);
    if (open_err != FAT_OK)
        return open_err;

    if (node.type != VFS_TYPE_FILE)
        return FAT_ERR_INVALID_ARGS;   /* can't write to a directory */

    if (!node.fs || !node.fs->name || !node.fs->fs_private)
        return FAT_ERR_INVALID_ARGS;

    /* ---- Same "<mount-slot>/<filename>" split fat_open()/
     * fat_create_file() use, needed again here only to recover the
     * bare `name` (fat_update_entry_fixed_root()/fat32_update_entry_
     * root() below match by decoded NAME, not by fs_private cluster
     * number — see each of their own doc comments). fat_open() having
     * already succeeded above guarantees this split can't fail. ---- */
    const char *sep = strchr(path, '/');
    const char *name = sep + 1;   /* fat_open() already validated this shape */

    /* ---- Per-variant dispatch: fill in a fat_chain_ctx_t (same shape
     * fat_read()/fat_find_free_cluster()/fat_alloc_cluster() above
     * already fill in) plus this variant's fat_set_cluster_fn (DAY23)
     * and EOC-max sentinel — everything the write copy loop below
     * needs, regardless of which FAT variant `path` resolves to. ---- */
    fat_chain_ctx_t ctx;
    fat_set_cluster_fn set_cluster;
    block_device_t *dev;

    /* Per-variant "update this file's directory entry" call — cannot
     * be unified into fat_set_cluster_fn's shape (different argument
     * shapes: fat_update_entry_fixed_root() needs dev/mount_lba/geom,
     * fat32_update_entry_root() needs the whole fat32_fs_t*), so this
     * is a small local closure-by-hand: a variant tag plus the two
     * fs_private pointers, resolved in the one place below that
     * actually calls one or the other. */
    int is_fat32 = 0;
    fat12_fs_t  *f12 = (fat12_fs_t  *)0;
    fat16_fs_t  *f16 = (fat16_fs_t  *)0;
    fat32_fs_t  *f32 = (fat32_fs_t  *)0;

    if (strcmp(node.fs->name, "fat12") == 0) {
        fat12_fs_t *f = (fat12_fs_t *)node.fs->fs_private;
        ctx.dev                = f->dev;
        ctx.mount_lba          = f->mount_lba;
        ctx.first_data_sector  = f->geom.first_data_sector;
        ctx.sectors_per_cluster = f->bpb.BPB_SecPerClus;
        ctx.bytes_per_cluster  = f->geom.bytes_per_cluster;
        ctx.fs_private         = f;
        set_cluster            = fat12_set_cluster_adapter;
        dev                     = f->dev;
        f12                     = f;
    } else if (strcmp(node.fs->name, "fat16") == 0) {
        fat16_fs_t *f = (fat16_fs_t *)node.fs->fs_private;
        ctx.dev                = f->dev;
        ctx.mount_lba          = f->mount_lba;
        ctx.first_data_sector  = f->geom.first_data_sector;
        ctx.sectors_per_cluster = f->bpb.BPB_SecPerClus;
        ctx.bytes_per_cluster  = f->geom.bytes_per_cluster;
        ctx.fs_private         = f;
        set_cluster            = fat16_set_cluster_adapter;
        dev                     = f->dev;
        f16                     = f;
    } else if (strcmp(node.fs->name, "fat32") == 0) {
        fat32_fs_t *f = (fat32_fs_t *)node.fs->fs_private;
        ctx.dev                = f->dev;
        ctx.mount_lba          = f->mount_lba;
        ctx.first_data_sector  = f->first_data_sector;
        ctx.sectors_per_cluster = f->bpb.BPB_SecPerClus;
        ctx.bytes_per_cluster  = f->bytes_per_cluster;
        ctx.fs_private         = f;
        set_cluster            = fat32_set_cluster_adapter;
        dev                     = f->dev;
        f32                     = f;
        is_fat32                = 1;
    } else {
        return FAT_ERR_INVALID_ARGS;   /* unknown filesystem name */
    }

    if (!dev || !dev->write)
        return FAT_ERR_IO;   /* read-only backend — write is impossible */

    if (ctx.bytes_per_cluster == 0 ||
        ctx.bytes_per_cluster > FAT_WRITE_MAX_CLUSTER_BYTES)
        return FAT_ERR_INVALID_ARGS;

    /* ---- Claim the file's FIRST cluster if it doesn't have one yet
     * (a freshly fat_create_file()'d entry — "起始簇 0" per Day 24's
     * own contract). See fat_vfs.h's doc comment's "CLUSTER-CHAIN
     * HANDLING" section for the full rationale. ---- */
    uint32_t start_cluster = (uint32_t)(uintptr_t)node.fs_private;

    if (size == 0) {
        /* Writing an empty file: no cluster needed at all — mirrors
         * fat_create_file()'s own "起始簇 0，大小 0" starting state.
         * Still update the directory entry (in case this call is
         * shrinking a previously-nonzero file back to empty — the old
         * chain is left allocated/orphaned, see fat_vfs.h's doc
         * comment on why that's Day 27's job, not this function's). */
        int rc0;
        if (is_fat32) {
            rc0 = fat32_update_entry_root(f32, name, 0, 0);
        } else {
            const fat_geometry_t *geom = f12 ? &f12->geom : &f16->geom;
            rc0 = fat_update_entry_fixed_root(dev, ctx.mount_lba, geom,
                                               name, 0, 0);
        }
        if (rc0 != 0)
            return FAT_ERR_IO;
        return 0;
    }

    if (start_cluster == 0) {
        uint32_t new_cluster;
        int rc = fat_alloc_cluster(node.fs, &new_cluster);
        if (rc != FAT_OK)
            return FAT_ERR_IO;   /* volume full or I/O failure — see
                                   * fat_vfs.h's doc comment: folded into
                                   * FAT_ERR_IO from fat_write()'s own
                                   * caller's point of view */
        start_cluster = new_cluster;
    }

    /* ---- The write copy loop itself: walk/extend the chain starting
     * at `start_cluster`, copying `size` bytes from `buf` cluster by
     * cluster, allocating + linking a new cluster whenever the current
     * one fills up and bytes remain. ---- */
    static uint8_t cluster_buf[FAT_WRITE_MAX_CLUSTER_BYTES];

    uint32_t cluster  = start_cluster;
    uint32_t written  = 0;

    while (written < size) {
        uint32_t chunk = size - written;
        if (chunk > ctx.bytes_per_cluster)
            chunk = ctx.bytes_per_cluster;

        /* Zero-pad the scratch buffer first so a chunk smaller than a
         * full cluster (typically the LAST chunk of the file) doesn't
         * leave stale bytes from a previous write sitting past the
         * file's own new size within that final cluster — harmless
         * (fat_read_chain() never reads past `file_size`) but cleaner
         * than leaving whatever this static buffer last held. */
        memset(cluster_buf, 0, ctx.bytes_per_cluster);
        memcpy(cluster_buf, (const uint8_t *)buf + written, chunk);

        uint32_t lba = ctx.mount_lba + ctx.first_data_sector +
                        (cluster - 2) * ctx.sectors_per_cluster;

        if (dev->write(dev, lba, (uint8_t)ctx.sectors_per_cluster,
                        cluster_buf) != 0)
            return FAT_ERR_IO;

        written += chunk;
        if (written >= size)
            break;

        /* More bytes remain and this cluster is now full — extend the
         * chain: allocate a new cluster, then re-point the OLD last
         * cluster's own FAT entry at it (overwriting the EOC marker
         * fat_alloc_cluster() gave the old cluster when IT was
         * claimed) — exactly the operation fat_alloc_cluster()'s own
         * DAY23 doc comment predicted this day would need. */
        uint32_t new_cluster;
        int rc = fat_alloc_cluster(node.fs, &new_cluster);
        if (rc != FAT_OK)
            return FAT_ERR_IO;

        if (set_cluster(&ctx, cluster, new_cluster) < 0)
            return FAT_ERR_IO;

        cluster = new_cluster;
    }

    /* ---- Persist the file's new start cluster + size to its
     * directory entry, so a subsequent fat_open()/fat_read() (`show`)
     * sees exactly what was just written. ---- */
    int rc;
    if (is_fat32) {
        rc = fat32_update_entry_root(f32, name, start_cluster, size);
    } else {
        const fat_geometry_t *geom = f12 ? &f12->geom : &f16->geom;
        rc = fat_update_entry_fixed_root(dev, ctx.mount_lba, geom,
                                          name, start_cluster, size);
    }
    if (rc != 0)
        return FAT_ERR_IO;

    return (int)written;
}

/* ===================================================================
 * DAY26 (Week 4, Day 26 of the 5.0 filesystem roadmap):
 *   "实现 fat_delete_file(name)：找到目录项，标记为已删除（首字节设为
 *    0xE5），释放其占用的簇链（在 FAT 表中标记为空闲）。Shell 中
 *    rm test.txt 后 ls 消失，且空闲簇数增加。"
 *
 * See fat_vfs.h's doc comment above the declaration for the full
 * contract. Two steps, done in this specific ORDER:
 *
 *   1. Free the FAT-table cluster chain (if the file has one at all —
 *      an empty, never-written `create`d file has start_cluster == 0
 *      and nothing to free). Reuses DAY19's fat_next_cluster_fn
 *      trampolines to WALK the chain and DAY23's fat_set_cluster_fn
 *      trampolines to zero (FATxx_CLUSTER_FREE == 0 on every variant)
 *      each cluster visited — the exact same two trampoline families
 *      fat_read()/fat_alloc_cluster()/fat_write() above already use,
 *      just walking-and-clearing instead of walking-and-copying or
 *      claiming-one-fresh-cluster. The chain is walked BEFORE it's
 *      cleared (`next_cluster()` is called on cluster N before
 *      `set_cluster(N, 0)` runs) since zeroing a cluster's FAT entry
 *      destroys the very link needed to find the NEXT cluster in the
 *      chain — clearing first would strand every cluster after the
 *      first one, still marked allocated forever.
 *
 *   2. Mark the directory entry itself deleted
 *      (fat_delete_entry_fixed_root()/fat32_delete_entry_root(), DAY26,
 *      kernel/fs/fat/fat_common.c / fat32.c).
 *
 *   WHY THIS ORDER, NOT THE REVERSE: step 1 needs the directory entry's
 *   own DIR_FstClusHI/LO fields (surfaced here as `node.fs_private`,
 *   fat_open()'s own decode) to know WHERE the chain even starts. Step
 *   2 doesn't touch those fields — see fat_delete_entry_fixed_root()'s
 *   own doc comment on why the "rest of the entry" is deliberately left
 *   stale — but if step 2 ran first there would be nothing left
 *   stopping a CONCURRENT fat_create_file()/fat_write() from reusing
 *   that now-free directory slot (and its now-freshly-claimed cluster
 *   numbers) while step 1 was still using the OLD start_cluster value
 *   to walk and free clusters — a race this single-threaded, no-
 *   interrupt-reentrancy kernel doesn't actually have today, but
 *   "free the data before you erase the only record of where it was"
 *   is the correct order regardless, and costs nothing to get right
 *   now rather than needing to be revisited once concurrency exists.
 *
 * Returns FAT_OK (0) on success. Returns fat_open()'s own error as-is
 * if `path` doesn't resolve to an existing file (FAT_ERR_NOT_FOUND /
 * FAT_ERR_INVALID_ARGS — same "don't reinvent a diagnostic fat_open()
 * already produces" choice fat_write()'s own error handling makes
 * above). Returns FAT_ERR_INVALID_ARGS if the resolved node is not a
 * plain file (nothing else exists to delete yet in this project) or
 * `node.fs->name` is unrecognized. Returns FAT_ERR_IO if any
 * dev->read()/dev->write() call fails while freeing the chain or
 * deleting the entry.
 * =================================================================== */
int fat_delete_file(const char *path)
{
    if (!path)
        return FAT_ERR_INVALID_ARGS;

    vfs_node_t node;
    int open_err = fat_open(path, &node);
    if (open_err != FAT_OK)
        return open_err;   /* forward fat_open()'s own diagnostic as-is */

    if (node.type != VFS_TYPE_FILE)
        return FAT_ERR_INVALID_ARGS;   /* nothing else exists yet to delete */

    if (!node.fs || !node.fs->name || !node.fs->fs_private)
        return FAT_ERR_INVALID_ARGS;

    const char *sep = strchr(path, '/');
    const char *name = sep + 1;   /* fat_open() already validated this shape */

    /* ---- Per-variant dispatch: same three-way "which variant is this
     * fs?" split fat_write() above already performs, capturing each
     * variant's own fs_private pointer (needed again in step 2) plus
     * this function's own trampoline pair and total_clusters (the
     * corruption-guard hop limit, same fat32_update_entry_root()'s own
     * max_hops convention). ---- */
    int is_fat32 = 0;
    fat12_fs_t *f12 = (fat12_fs_t *)0;
    fat16_fs_t *f16 = (fat16_fs_t *)0;
    fat32_fs_t *f32 = (fat32_fs_t *)0;

    fat_chain_ctx_t ctx;
    fat_next_cluster_fn next_cluster;
    fat_set_cluster_fn  set_cluster;
    uint32_t total_clusters;

    if (strcmp(node.fs->name, "fat12") == 0) {
        f12 = (fat12_fs_t *)node.fs->fs_private;
        ctx.fs_private = f12;
        next_cluster   = fat12_next_cluster_adapter;
        set_cluster    = fat12_set_cluster_adapter;
        total_clusters = f12->geom.total_clusters;
    } else if (strcmp(node.fs->name, "fat16") == 0) {
        f16 = (fat16_fs_t *)node.fs->fs_private;
        ctx.fs_private = f16;
        next_cluster   = fat16_next_cluster_adapter;
        set_cluster    = fat16_set_cluster_adapter;
        total_clusters = f16->geom.total_clusters;
    } else if (strcmp(node.fs->name, "fat32") == 0) {
        f32 = (fat32_fs_t *)node.fs->fs_private;
        ctx.fs_private = f32;
        next_cluster   = fat32_next_cluster_adapter;
        set_cluster    = fat32_set_cluster_adapter;
        total_clusters = f32->total_clusters;
        is_fat32        = 1;
    } else {
        return FAT_ERR_INVALID_ARGS;   /* unknown filesystem name */
    }

    /* ---- Step 1: free the cluster chain, if any — see this function's
     * header comment above for the full "walk before you clear, and
     * why" rationale. `node.fs_private` is fat_open()'s own decoded
     * start_cluster, the exact same `fs_private`-holds-start-cluster
     * convention fat_write() already relies on above. ---- */
    uint32_t cluster  = (uint32_t)(uintptr_t)node.fs_private;
    uint32_t hops      = 0;
    uint32_t max_hops  = total_clusters + 1;

    while (cluster >= 2 && hops < max_hops) {
        int32_t next = next_cluster(&ctx, cluster);

        /* BUGFIX (caught by tests/day26_test_rm.py's scenario B, not by
         * inspection — see that script's own comment): FAT_CHAIN_EOC
         * (-2) IS negative, so a naive `if (next < 0)` here treats the
         * single most common case — a one-cluster file's LAST cluster,
         * which is exactly what "no next cluster" (EOC) means — as an
         * I/O failure and aborts before ever freeing anything. This is
         * the identical "EOC is a negative sentinel, so it must be
         * checked before the generic negative-means-error test" trap
         * fat_find_free_cluster()'s own DAY22 doc comment already
         * warns about above — this loop just failed to apply that same
         * rule when it was first written. Checking `next ==
         * FAT_CHAIN_EOC` explicitly first (excluding it from the error
         * test) is the fix. */
        if (next != FAT_CHAIN_EOC && next < 0)
            return FAT_ERR_IO;   /* genuine dev->read() failure walking the chain */

        if (set_cluster(&ctx, cluster, 0) < 0)   /* FATxx_CLUSTER_FREE == 0 on every variant */
            return FAT_ERR_IO;   /* dev->read()/dev->write() failure freeing it */

        if (next == FAT_CHAIN_EOC)
            break;   /* just freed the last cluster in the chain */

        cluster = (uint32_t)next;
        hops++;
    }

    /* ---- Step 2: mark the directory entry itself deleted. ---- */
    int rc;
    if (is_fat32) {
        rc = fat32_delete_entry_root(f32, name);
    } else {
        /* FAT12/FAT16 share the exact same fixed-root geometry-field
         * shape fat_create_file()/fat_write() above already exploit —
         * only which fat_private struct the geom/dev/mount_lba come
         * from differs. */
        const fat_geometry_t *geom = f12 ? &f12->geom      : &f16->geom;
        block_device_t       *dev  = f12 ? f12->dev        : f16->dev;
        uint32_t         mount_lba = f12 ? f12->mount_lba  : f16->mount_lba;
        rc = fat_delete_entry_fixed_root(dev, mount_lba, geom, name);
    }

    return (rc == 0) ? FAT_OK : FAT_ERR_IO;
}