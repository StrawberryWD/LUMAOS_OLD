/* kernel/fs/fat/fat32.c
 * LumaOS 5.0 - FAT32 Mount, FSInfo, Cluster-Chain Walk & Root Directory Listing
 *
 * DAY17 (Week 3, Day 17 of the 5.0 filesystem roadmap):
 *   "实现 FAT32 解析（注意 BPB_RootClus，根目录是一个普通簇链，且存在
 *    FSInfo 结构）。挂载 FAT32 镜像（mkfs.fat -F 32），ls 能工作。"
 *
 *   See fat32.h for the full contract of every function here.
 */
#include "fat32.h"
#include "../../core/kernel.h"   /* memcpy, memset, memcmp */

int fat32_mount(block_device_t *dev, uint32_t lba, fat32_fs_t *out_fs)
{
    memset(out_fs, 0, sizeof(fat32_fs_t));

    if (!dev || !dev->read)
        return -1;

    static uint8_t boot_sector[512];
    if (dev->read(dev, lba, 1, boot_sector) != 0)
        return -1;

    fat_bpb_common_t bpb;
    if (fat_parse_bpb_common(boot_sector, &bpb) != 0)
        return -1;   /* not a recognizable FAT BPB at all */

    /* FAT32's own tail, offset 0x24 onward — NOT the same bytes
     * fat_bpb_common_t's BS_DrvNum..BS_FilSysType would parse them
     * as (see fat32.h's header comment). Read directly off the raw
     * sector instead of overlaying fat_bpb_common_t's tail fields. */
    fat32_bpb_ext_t ext;
    memcpy(&ext, boot_sector + 0x24, sizeof(fat32_bpb_ext_t));

    if (memcmp(ext.BS_FilSysType, "FAT32", 5) != 0)
        return -1;

    /* ---- Geometry, computed with the REAL per-FAT sector count ----
     * See fat32.h's header comment for exactly why
     * fat_common.c's shared fat_compute_geometry() is not reused: it
     * assumes BPB_FATSz16, which is always 0 on FAT32. */
    uint32_t bytes_per_sec = (bpb.BPB_BytesPerSec != 0) ? bpb.BPB_BytesPerSec : 512;
    uint32_t sec_per_clus  = (bpb.BPB_SecPerClus  != 0) ? bpb.BPB_SecPerClus  : 1;

    uint32_t first_fat_sector  = bpb.BPB_RsvdSecCnt;
    /* FAT32 has no fixed root-directory region at all (BPB_RootEntCnt
     * is always 0) — the data region starts immediately after the
     * NumFATs copies of the (real, FATSz32-sized) FAT. */
    uint32_t first_data_sector = first_fat_sector +
                                  ((uint32_t)bpb.BPB_NumFATs * ext.BPB_FATSz32);

    uint32_t total_sectors =
        bpb.BPB_TotSec16 != 0 ? (uint32_t)bpb.BPB_TotSec16 : bpb.BPB_TotSec32;

    uint32_t data_sectors =
        (total_sectors > first_data_sector) ? (total_sectors - first_data_sector) : 0;
    uint32_t total_clusters = data_sectors / sec_per_clus;

    out_fs->dev               = dev;
    out_fs->mount_lba         = lba;
    out_fs->bpb                = bpb;
    out_fs->ext                = ext;
    out_fs->first_fat_sector   = first_fat_sector;
    out_fs->first_data_sector  = first_data_sector;
    out_fs->total_sectors      = total_sectors;
    out_fs->total_clusters     = total_clusters;
    out_fs->bytes_per_cluster  = bytes_per_sec * sec_per_clus;

    return 0;
}

int fat32_read_fsinfo(fat32_fs_t *fs, fat32_fsinfo_t *out_fsinfo)
{
    memset(out_fsinfo, 0, sizeof(fat32_fsinfo_t));

    uint32_t lba = fs->mount_lba + fs->ext.BPB_FSInfo;

    static uint8_t sector_buf[512];
    if (fs->dev->read(fs->dev, lba, 1, sector_buf) != 0)
        return -1;

    fat32_fsinfo_t fsinfo;
    memcpy(&fsinfo, sector_buf, sizeof(fat32_fsinfo_t));

    if (fsinfo.FSI_LeadSig  != FAT32_FSINFO_LEAD_SIG ||
        fsinfo.FSI_StrucSig != FAT32_FSINFO_STRUC_SIG ||
        fsinfo.FSI_TrailSig != FAT32_FSINFO_TRAIL_SIG)
        return -1;   /* BPB_FSInfo didn't point at a real FSInfo sector */

    *out_fsinfo = fsinfo;
    return 0;
}

int64_t fat32_next_cluster(fat32_fs_t *fs, uint32_t cluster)
{
    uint32_t fat_byte_offset = cluster * 4u;

    uint32_t bytes_per_sec = fs->bpb.BPB_BytesPerSec;
    if (bytes_per_sec == 0)
        bytes_per_sec = 512;

    uint32_t sector_index_in_fat = fat_byte_offset / bytes_per_sec;
    uint32_t offset_in_sector    = fat_byte_offset % bytes_per_sec;

    uint32_t lba = fs->mount_lba + fs->first_fat_sector + sector_index_in_fat;

    /* 4-byte entry, even offset (4 divides 512 evenly) — never
     * straddles a sector boundary, same reasoning as
     * fat16_next_cluster(). */
    static uint8_t fat_buf[512];
    if (fs->dev->read(fs->dev, lba, 1, fat_buf) != 0)
        return -1;

    uint32_t raw = (uint32_t)fat_buf[offset_in_sector]
                 | ((uint32_t)fat_buf[offset_in_sector + 1] << 8)
                 | ((uint32_t)fat_buf[offset_in_sector + 2] << 16)
                 | ((uint32_t)fat_buf[offset_in_sector + 3] << 24);

    return (int64_t)(raw & FAT32_CLUSTER_MASK);
}

int fat32_readdir(fat32_fs_t *fs, fat32_dirlist_entry_t *out_entries,
                   uint32_t max_entries, uint32_t *out_count)
{
    uint32_t count = 0;
    uint32_t cluster = fs->ext.BPB_RootClus;

    /* Corruption guard: a well-formed chain can't legitimately need
     * more hops than the volume has clusters — without this, a
     * corrupt FAT that cycles back on itself would hang the walk
     * forever instead of just returning a truncated (or empty)
     * listing. +1 for slack: an all-cluster chain still needs exactly
     * total_clusters hops, not total_clusters - 1. */
    uint32_t max_hops = fs->total_clusters + 1;
    uint32_t hops = 0;

    static uint8_t sector_buf[512];
    const uint32_t entries_per_sector = 512 / sizeof(fat_dirent_t);

    while (cluster >= 2 && cluster < FAT32_CLUSTER_EOC_MIN &&
           count < max_entries && hops < max_hops) {
        uint32_t bytes_per_sec = (fs->bpb.BPB_BytesPerSec != 0) ? fs->bpb.BPB_BytesPerSec : 512;
        uint32_t sec_per_clus  = (fs->bpb.BPB_SecPerClus  != 0) ? fs->bpb.BPB_SecPerClus  : 1;
        (void)bytes_per_sec;

        uint32_t cluster_first_lba = fs->mount_lba + fs->first_data_sector +
                                      (cluster - 2) * sec_per_clus;

        uint32_t s;
        int stop_whole_walk = 0;

        for (s = 0; s < sec_per_clus && count < max_entries; s++) {
            uint32_t lba = cluster_first_lba + s;

            if (fs->dev->read(fs->dev, lba, 1, sector_buf) != 0) {
                if (out_count)
                    *out_count = count;
                return -1;
            }

            fat_dirent_t *dirents = (fat_dirent_t *)sector_buf;
            uint32_t i;

            for (i = 0; i < entries_per_sector && count < max_entries; i++) {
                fat_dirent_t *de = &dirents[i];

                if (de->DIR_Name[0] == FAT_DIRENT_END) {
                    stop_whole_walk = 1;
                    break;
                }
                if (de->DIR_Name[0] == FAT_DIRENT_FREE)
                    continue;
                if (de->DIR_Attr == FAT_ATTR_LONG_NAME)
                    continue;
                if (de->DIR_Attr & FAT_ATTR_VOLUME_ID)
                    continue;

                fat32_dirlist_entry_t *out = &out_entries[count];
                fat_decode_short_name(de->DIR_Name, out->name);
                out->attr          = de->DIR_Attr;
                out->start_cluster = ((uint32_t)de->DIR_FstClusHI << 16) |
                                      (uint32_t)de->DIR_FstClusLO;
                out->size          = de->DIR_FileSize;
                count++;
            }

            if (stop_whole_walk)
                break;
        }

        if (stop_whole_walk)
            break;

        int64_t next = fat32_next_cluster(fs, cluster);
        if (next < 0) {
            if (out_count)
                *out_count = count;
            return -1;
        }
        cluster = (uint32_t)next;
        hops++;
    }

    if (out_count)
        *out_count = count;
    return 0;
}