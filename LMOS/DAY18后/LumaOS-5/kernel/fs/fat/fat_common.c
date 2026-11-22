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
#include "../../core/kernel.h"   /* memcpy, memset, strlen */

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

/* DAY16 (Week 3, Day 16 of the 5.0 filesystem roadmap):
 *   "实现 FAT16 解析（FAT16 的根目录在数据区之前，簇号 16 位）。挂载一个
 *    FAT16 镜像，ls 能正确列出文件。"
 *
 *   fat_decode_short_name() and fat_readdir_fixed_root() below are the
 *   promoted, FAT12/16-shared versions of fat12.c's original DAY12
 *   fat12_decode_short_name() (was `static`) and fat12_readdir()'s
 *   decode loop. The logic is UNCHANGED from Day 12 — this is a pure
 *   relocation/generalization (fat12_fs_t's `dev`/`mount_lba`/`geom`
 *   fields become explicit parameters instead of struct members) so
 *   fat16_readdir() (kernel/fs/fat/fat16.c) can call the exact same
 *   code fat12_readdir() (now a thin wrapper, see fat12.c's own DAY16
 *   note) already relies on. See fat_common.h's DAY16 header note for
 *   the full "why this is safe to share" rationale (FAT12 and FAT16
 *   have byte-for-byte identical fixed-root-region layouts).
 */
void fat_decode_short_name(const uint8_t raw11[11], char out13[13])
{
    /* DIR_Name[0] == 0x05 is a documented special case in the FAT
     * spec: byte 0x05 stands in for a *literal* leading 0xE5 byte in
     * a filename (0xE5 itself is reserved for FAT_DIRENT_FREE, so a
     * real filename that happens to start with 0xE5 is stored with
     * this substitution to avoid being misread as a deleted entry).
     * Not expected to be exercised by this project's own fixtures
     * (mtools never generates it for ordinary ASCII names like
     * BIGFILE.BIN/SMALL.BIN), but handled here rather than silently
     * mis-decoding a real-world volume that does use it. */
    uint8_t base[9];
    uint8_t ext[4];
    int i;

    for (i = 0; i < 8; i++)
        base[i] = raw11[i];
    base[0] = (raw11[0] == 0x05) ? 0xE5 : raw11[0];
    base[8] = '\0';
    for (i = 7; i >= 0 && base[i] == ' '; i--)
        base[i] = '\0';

    for (i = 0; i < 3; i++)
        ext[i] = raw11[8 + i];
    ext[3] = '\0';
    for (i = 2; i >= 0 && ext[i] == ' '; i--)
        ext[i] = '\0';

    size_t base_len = strlen((const char *)base);
    size_t ext_len  = strlen((const char *)ext);

    memcpy(out13, base, base_len);
    if (ext_len > 0) {
        out13[base_len] = '.';
        memcpy(out13 + base_len + 1, ext, ext_len);
        out13[base_len + 1 + ext_len] = '\0';
    } else {
        out13[base_len] = '\0';
    }
}

int fat_readdir_fixed_root(block_device_t *dev, uint32_t mount_lba,
                            const fat_geometry_t *geom,
                            fat_dirlist_entry_t *out_entries,
                            uint32_t max_entries, uint32_t *out_count)
{
    uint32_t count = 0;
    uint32_t sec;

    /* Root directory region is BLOCK_SECTOR_SIZE(512)-byte sectors,
     * same fixed-512 assumption every other disk-facing routine in
     * this project already makes (see fat12.c's original DAY11
     * comment). 16 entries of 32 bytes each fit in one 512-byte
     * sector. */
    static uint8_t sector_buf[512];
    const uint32_t entries_per_sector = 512 / sizeof(fat_dirent_t);

    for (sec = 0; sec < geom->root_dir_sectors && count < max_entries; sec++) {
        /* Same volume-relative -> disk-absolute LBA fix fat12.h's
         * DAY11 note documents for first_fat_sector, applied here to
         * first_root_dir_sec: it is relative to this volume's own
         * boot sector, so `mount_lba` must be added back in. */
        uint32_t lba = mount_lba + geom->first_root_dir_sec + sec;

        if (dev->read(dev, lba, 1, sector_buf) != 0) {
            if (out_count)
                *out_count = count;
            return -1;
        }

        fat_dirent_t *dirents = (fat_dirent_t *)sector_buf;
        uint32_t i;

        for (i = 0; i < entries_per_sector && count < max_entries; i++) {
            fat_dirent_t *de = &dirents[i];

            if (de->DIR_Name[0] == FAT_DIRENT_END) {
                /* Spec-defined "no more entries past this point" —
                 * stop the whole walk, not just this sector. */
                if (out_count)
                    *out_count = count;
                return 0;
            }

            if (de->DIR_Name[0] == FAT_DIRENT_FREE)
                continue;   /* deleted entry — skip, keep scanning */

            if (de->DIR_Attr == FAT_ATTR_LONG_NAME)
                continue;   /* VFAT long-name fragment — 8.3-only for now */

            if (de->DIR_Attr & FAT_ATTR_VOLUME_ID)
                continue;   /* root directory's volume-label entry */

            fat_dirlist_entry_t *out = &out_entries[count];
            fat_decode_short_name(de->DIR_Name, out->name);
            out->attr          = de->DIR_Attr;
            /* DAY17: combine DIR_FstClusHI:DIR_FstClusLO — a no-op for
             * FAT12/16 (FstClusHI is always 0 there per spec), but this
             * is what lets the same fat_dirlist_entry_t/this same
             * function's shape stay meaningful if a future caller ever
             * points it at a region containing FAT32-sourced entries
             * (it doesn't today — fat32_readdir(), kernel/fs/fat/
             * fat32.c, has its own cluster-chain walk and does this
             * same combine itself, see its DAY17 comment). */
            out->start_cluster = ((uint32_t)de->DIR_FstClusHI << 16) |
                                  (uint32_t)de->DIR_FstClusLO;
            out->size          = de->DIR_FileSize;
            count++;
        }
    }

    if (out_count)
        *out_count = count;
    return 0;
}