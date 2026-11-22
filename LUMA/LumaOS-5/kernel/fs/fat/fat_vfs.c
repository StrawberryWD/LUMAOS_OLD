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