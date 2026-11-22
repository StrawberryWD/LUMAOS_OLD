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
int fat_open(const char *path, vfs_node_t *out_node)
{
    if (!path || !out_node)
        return -1;

    /* Split "<mount-slot>/<filename>" at the FIRST '/'. Unlike the
     * original single-fs design, there's no optional leading '/' to
     * strip here — the '/' is mandatory, since it's the boundary
     * between the mount-slot segment and the filename, not path
     * syntax sugar. No '/' at all means no mount-slot was named. */
    const char *sep = strchr(path, '/');
    if (sep == (char *)0)
        return -1;

    /* Copy the mount-slot segment (path[0..sep)) into a local buffer
     * sized to match vfs_mount_entry_t.name[8] (see vfs.h) — reject
     * anything that couldn't possibly be a valid slot name rather than
     * silently truncating it into matching the wrong slot. */
    char mount_name[8];
    {
        uint32_t len = (uint32_t)(sep - path);
        if (len == 0 || len >= sizeof(mount_name))
            return -1;
        memcpy(mount_name, path, len);
        mount_name[len] = '\0';
    }

    filesystem_t *fs = vfs_lookup_mount(mount_name);
    if (!fs || !fs->readdir || !fs->root)
        return -1;

    /* Everything after the separator is the (single-level) filename.
     * A second '/' here means a subdirectory path ("IDE0/SYS/FOO.BIN")
     * — not supported until Day 42's multi-level path walking exists
     * (see fat_vfs.h's doc comment). Fail cleanly now rather than
     * silently treating "SYS/FOO.BIN" as one (wrong, unmatchable)
     * literal name. */
    const char *name = sep + 1;
    if (name[0] == '\0')
        return -1;
    if (strchr(name, '/') != (char *)0)
        return -1;

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
        if (strcmp(child.name, name) == 0) {
            *out_node = child;
            return 0;
        }
        idx++;
    }

    /* fs->readdir() ran out of entries (returned -1) before a match
     * was seen — per vfs_readdir_fn's own contract this means "index
     * past the last entry", i.e. the whole directory was scanned and
     * nothing named `name` exists there. */
    return -1;
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
 * if `file_pos >= file_size`, the documented EOF case), or -1 if
 * `next_cluster()` reports an I/O error, or the chain reaches
 * FAT_CHAIN_EOC before `file_pos`'s target cluster is reached (a
 * corrupt/short chain — Day 21's "簇链断裂" error-handling week-recap
 * item is where a friendlier diagnostic for this case is planned; for
 * now it's folded into the same -1 "read didn't fully succeed"
 * return), or `bytes_per_cluster` exceeds FAT_READ_MAX_CLUSTER_BYTES.
 */
#define FAT_READ_MAX_CLUSTER_BYTES  (64u * 512u)   /* 64 sectors/cluster * 512 bytes */

static int32_t fat_read_chain(fat_chain_ctx_t *ctx, fat_next_cluster_fn next_cluster,
                               uint32_t start_cluster, uint32_t file_pos,
                               uint32_t file_size, void *buf, uint32_t size)
{
    static uint8_t cluster_buf[FAT_READ_MAX_CLUSTER_BYTES];

    if (ctx->bytes_per_cluster == 0 ||
        ctx->bytes_per_cluster > FAT_READ_MAX_CLUSTER_BYTES)
        return -1;

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
     * only — see fat_vfs.h's doc comment. */
    for (uint32_t i = 0; i < cluster_index; i++) {
        int32_t next = next_cluster(ctx, cluster);
        if (next < 0)
            return -1;               /* I/O error walking the chain */
        if (next == FAT_CHAIN_EOC)
            return -1;               /* chain too short for file_pos — corrupt/short chain */
        cluster = (uint32_t)next;
    }

    uint32_t copied = 0;
    while (copied < size) {
        uint32_t lba = ctx->mount_lba + ctx->first_data_sector +
                        (cluster - 2) * ctx->sectors_per_cluster;

        if (ctx->dev->read(ctx->dev, lba, (uint8_t)ctx->sectors_per_cluster,
                            cluster_buf) != 0)
            return (copied > 0) ? (int32_t)copied : -1;

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
        if (next < 0)
            return (copied > 0) ? (int32_t)copied : -1;
        if (next == FAT_CHAIN_EOC)
            break;                   /* legitimate end of chain — return what we have */
        cluster = (uint32_t)next;
    }

    return (int32_t)copied;
}

int fat_read(file_t *file, void *buf, uint32_t size)
{
    if (!file || !buf || !file->node)
        return -1;

    vfs_node_t *node = file->node;
    if (node->type != VFS_TYPE_FILE)
        return -1;

    uint32_t start_cluster = (uint32_t)(uintptr_t)node->fs_private;
    if (start_cluster == 0)
        return -1;   /* no data cluster to chain-walk (see doc comment) */

    if (!node->fs || !node->fs->name)
        return -1;

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
        return -1;   /* unknown filesystem name — not a FAT12/16/32 mount */
    }

    int32_t n = fat_read_chain(&ctx, next_cluster, start_cluster,
                                file->pos, node->size, buf, size);
    if (n < 0)
        return -1;

    file->pos += (uint32_t)n;
    return n;
}