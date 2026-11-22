/* kernel/fs/fat/fat_common.c
 * LumaOS 5.0 - FAT12/16/32 Shared BPB Parser, Geometry & Type Detection
 *
 * DAY15 (Week 3, Day 15 of the 5.0 filesystem roadmap):
 *   "重构 FAT 通用层：分离 FAT12/16/32 的共性（BPB 解析、目录项结构），
 *    创建 fat_common.c。一个 fat_mount() 根据 BPB 的 BytesPerSector 和
 *    TotalSectors 自动判断类型。"
 *
 *   See fat_common.h for the full design rationale: which structs/
 *   macros moved here from fat12.h, which stayed FAT12-specific, and
 *   why. This file is the arithmetic + I/O: fat_parse_bpb_common() is
 *   fat12_parse_bpb()'s old sanity-check logic minus the FAT12-only
 *   BS_FilSysType string match; fat_compute_geometry() is its old
 *   geometry-formula block, generalized with the new TotalClusters/
 *   type fields; fat_classify_type() is genuinely new (Day 9-14 never
 *   needed to tell FAT variants apart, since only FAT12 existed);
 *   fat_mount() is the new top-level entry point that ties all three
 *   together against a real block_device_t, mirroring fat12_mount()'s
 *   old read-then-parse shape but without assuming FAT12 specifically.
 */
#include "fat_common.h"
#include "../../core/kernel.h"   /* memcpy */

int fat_parse_bpb_common(const uint8_t buf[512], fat_bpb_common_t *out_bpb)
{
    fat_bpb_common_t bpb;
    memcpy(&bpb, buf, sizeof(fat_bpb_common_t));

    if (out_bpb)
        *out_bpb = bpb;

    /* Same three-field-plus-signature check fat12_parse_bpb() used to
     * do inline through Day 14 (see this file's header comment and
     * fat_common.h's doc comment on fat_parse_bpb_common() for why
     * these specific fields/values are the ones the FAT spec actually
     * constrains identically across FAT12/16/32) — just without the
     * FAT12-only BS_FilSysType == "FAT12" comparison, which stays in
     * fat12.c since it's a FAT12-specific check, not a shared one. */
    int sig_ok = (buf[FAT_BOOT_SIG_OFFSET]     == FAT_BOOT_SIG_BYTE0 &&
                  buf[FAT_BOOT_SIG_OFFSET + 1] == FAT_BOOT_SIG_BYTE1);

    int bytes_per_sec_ok = (bpb.BPB_BytesPerSec == 512  ||
                             bpb.BPB_BytesPerSec == 1024 ||
                             bpb.BPB_BytesPerSec == 2048 ||
                             bpb.BPB_BytesPerSec == 4096);

    int sec_per_clus_ok = (bpb.BPB_SecPerClus == 1  || bpb.BPB_SecPerClus == 2  ||
                            bpb.BPB_SecPerClus == 4  || bpb.BPB_SecPerClus == 8  ||
                            bpb.BPB_SecPerClus == 16 || bpb.BPB_SecPerClus == 32 ||
                            bpb.BPB_SecPerClus == 64 || bpb.BPB_SecPerClus == 128);

    int num_fats_ok = (bpb.BPB_NumFATs == 1 || bpb.BPB_NumFATs == 2);

    return (sig_ok && bytes_per_sec_ok && sec_per_clus_ok && num_fats_ok) ? 0 : -1;
}

fat_type_t fat_classify_type(uint32_t total_clusters)
{
    /* Microsoft FAT spec (fatgen103, "FAT Type Determination") — see
     * fat_common.h's doc comment on this function for the full
     * rationale, including why this is NOT simply "compare
     * BytesPerSec/TotalSectors directly". Order matters: check the
     * smallest boundary first. */
    if (total_clusters < 4085)
        return FAT_TYPE_FAT12;
    if (total_clusters < 65525)
        return FAT_TYPE_FAT16;
    return FAT_TYPE_FAT32;
}

int fat_compute_geometry(const fat_bpb_common_t *bpb, fat_geometry_t *out_geom)
{
    /* ---- Standard Microsoft FAT spec geometry formulas ----
     * Identical to fat12_parse_bpb()'s original inline computation
     * through Day 14 — see fat12.c's original DAY9 comment for the
     * formula-by-formula rationale, reproduced here since this is now
     * where the arithmetic actually lives. */

    uint32_t bytes_per_sec = (bpb->BPB_BytesPerSec != 0) ? bpb->BPB_BytesPerSec : 512;
    uint32_t sec_per_clus  = (bpb->BPB_SecPerClus  != 0) ? bpb->BPB_SecPerClus  : 1;

    uint32_t root_dir_sectors =
        ((uint32_t)bpb->BPB_RootEntCnt * 32 + (bytes_per_sec - 1)) / bytes_per_sec;

    uint32_t first_fat_sector = bpb->BPB_RsvdSecCnt;
    uint32_t first_root_dir_sec =
        bpb->BPB_RsvdSecCnt + ((uint32_t)bpb->BPB_NumFATs * bpb->BPB_FATSz16);

    uint32_t first_data_sector = first_root_dir_sec + root_dir_sectors;

    uint32_t total_sectors =
        bpb->BPB_TotSec16 != 0 ? (uint32_t)bpb->BPB_TotSec16 : bpb->BPB_TotSec32;

    /* DAY15: the one genuinely new number — TotalClusters, the value
     * fat_classify_type() actually keys off of. Guarded against
     * total_sectors < first_data_sector (a bogus/tiny BPB, e.g. the
     * all-zero or garbage buffers fat_parse_bpb_common() already
     * rejected on sanity grounds, but this function computes geometry
     * "anyway" per its documented contract) so the subtraction never
     * wraps around a uint32_t into a huge bogus cluster count. */
    uint32_t data_sectors =
        (total_sectors > first_data_sector) ? (total_sectors - first_data_sector) : 0;
    uint32_t total_clusters = data_sectors / sec_per_clus;

    if (out_geom) {
        out_geom->root_dir_sectors   = root_dir_sectors;
        out_geom->first_root_dir_sec = first_root_dir_sec;
        out_geom->first_data_sector  = first_data_sector;
        out_geom->first_fat_sector   = first_fat_sector;
        out_geom->total_sectors      = total_sectors;
        out_geom->bytes_per_cluster  = bytes_per_sec * sec_per_clus;
        out_geom->total_clusters     = total_clusters;
        out_geom->type               = fat_classify_type(total_clusters);
    }

    return 0;
}

fat_type_t fat_mount(block_device_t *dev, uint32_t lba,
                      fat_bpb_common_t *out_bpb, fat_geometry_t *out_geom)
{
    if (!dev || !dev->read)
        return FAT_TYPE_UNKNOWN;

    /* static scratch buffer — same convention fat12_mount() already
     * uses for its own boot-sector read (see fat12.c's DAY10
     * comment): avoids putting 512 bytes on what may be a small
     * kernel stack. */
    static uint8_t boot_sector[512];
    if (dev->read(dev, lba, 1, boot_sector) != 0)
        return FAT_TYPE_UNKNOWN;

    fat_bpb_common_t bpb;
    int bpb_ok = (fat_parse_bpb_common(boot_sector, &bpb) == 0);
    if (out_bpb)
        *out_bpb = bpb;

    fat_geometry_t geom;
    fat_compute_geometry(&bpb, &geom);   /* always 0 — pure arithmetic, see its own doc comment */
    if (out_geom)
        *out_geom = geom;

    if (!bpb_ok)
        return FAT_TYPE_UNKNOWN;

    return geom.type;
}