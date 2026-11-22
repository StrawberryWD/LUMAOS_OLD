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
#include "../../lib/string.h"   /* DAY18: strchr(), strcmp() */

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