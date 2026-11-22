/* kernel/fs/fat/fat12.c
 * LumaOS 5.0 - FAT12 BPB Parser & Mount
 *
 * DAY9 (Week 2, Day 9 of the 5.0 filesystem roadmap):
 *   "创建 kernel/fs/fat/fat12.h。定义 BPB 结构体（bpb_12_t）和目录项
 *    结构（fat_dirent_t）。写一个 fat12_parse_bpb(buf)，能算出 RootDir
 *    扇区数、Data 起始扇区。"
 *
 *   See fat12.h for the full struct-layout rationale and this
 *   function's contract. This file is just the arithmetic: copy the
 *   raw sector into a bpb_12_t (same packed-struct-over-bytes
 *   technique kernel/drivers/block/mbr.c uses for
 *   mbr_partition_entry_t), sanity-check it, and apply the standard
 *   Microsoft FAT spec geometry formulas to produce a
 *   fat12_geometry_t. No disk I/O happens in fat12_parse_bpb() itself —
 *   same separation of concerns mbr.c keeps from the block-device
 *   layer below it: the caller (kmain.c's Day 9 test, or Day 10's
 *   fat12_mount() below) is responsible for actually reading the boot
 *   sector via disk->read(...) first.
 *
 * DAY10 (Week 2, Day 10 of the 5.0 filesystem roadmap):
 *   "实现 fat12_mount(block_dev)：读取引导扇区，检查 BS_FileSysType
 *    是否为 FAT12。挂载成功返回 0，失败返回 -1，在终端打印
 *    "FAT12 mounted"。"
 *
 *   Adds fat12_mount(): the first function in this file that actually
 *   touches disk I/O (via dev->read(...), the Day 6 block_device_t
 *   interface — same call kmain.c already made directly for the
 *   Day 9 tests). It's a thin wrapper around Day 9's
 *   fat12_parse_bpb(): read one sector, parse it, and — on success —
 *   package the result into a fat12_fs_t the caller can hold onto.
 *
 *   NOTE ON "在终端打印 FAT12 mounted": that print happens in kmain.c,
 *   not here. This file's job is the mount *logic* (0/-1 return,
 *   populated fat12_fs_t); kmain.c's Day 10 section is what actually
 *   writes "FAT12 mounted" to the console on a successful call,
 *   consistent with how every prior driver in this codebase (ide,
 *   mbr, block) keeps console/printf output in kmain.c's test-harness
 *   code rather than inside the driver functions themselves.
 *
 * DAY15 (Week 3, Day 15 of the 5.0 filesystem roadmap):
 *   "重构 FAT 通用层：分离 FAT12/16/32 的共性（BPB 解析、目录项结构），
 *    创建 fat_common.c。"
 *
 *   fat12_parse_bpb() below is rewritten to delegate its shared BPB
 *   sanity-check and geometry-formula work to the new kernel/fs/fat/
 *   fat_common.{h,c} (fat_parse_bpb_common()/fat_compute_geometry()),
 *   keeping only the one genuinely FAT12-specific check (BS_FilSysType
 *   == "FAT12   ") in this file. fat12_mount(), fat12_next_cluster(),
 *   and fat12_readdir() below are UNCHANGED by this refactor — their
 *   12-bit cluster-chain format and fixed-region directory walk are
 *   FAT12-specific behavior, not shared BPB/geometry parsing, so they
 *   have nothing to delegate. See fat_common.h's header comment for
 *   the complete "what moved, what stayed, and why" rationale.
 */
#include "fat12.h"
#include "../../core/kernel.h"   /* memcpy, memset, strlen */

/* DAY15 REFACTOR: fat12_parse_bpb() used to do all of this inline —
 * copy the raw sector into a bpb_12_t, sanity-check the signature and
 * BytesPerSec/SecPerClus/NumFATs/BS_FilSysType fields, then apply the
 * Microsoft FAT spec's RootDirSectors/FirstDataSector/... formulas —
 * see fat_common.h's own header comment for the full "what moved and
 * why" rationale. That inline logic has been split into two shared,
 * variant-agnostic pieces now living in fat_common.c:
 *
 *   - fat_parse_bpb_common(): the signature + BytesPerSec/SecPerClus/
 *     NumFATs sanity check (everything that's true of ANY FAT
 *     variant, not just FAT12).
 *   - fat_compute_geometry(): the RootDirSectors/FirstDataSector/...
 *     arithmetic (also variant-agnostic — the formulas themselves
 *     don't care whether the volume turns out to be FAT12/16/32).
 *
 * This function is now a thin FAT12-specific wrapper around both:
 * it adds back the ONE check that's genuinely FAT12-specific
 * (BS_FilSysType == "FAT12   ") on top of fat_parse_bpb_common()'s
 * shared sanity check, and simply forwards fat_compute_geometry()'s
 * result — bpb_12_t/fat12_geometry_t are type-identical to
 * fat_bpb_common_t/fat_geometry_t (see fat12.h's DAY15 alias note),
 * so no conversion is needed between the two.
 */
int fat12_parse_bpb(const uint8_t buf[512], bpb_12_t *out_bpb,
                     fat12_geometry_t *out_geom)
{
    fat_bpb_common_t bpb;
    int common_ok = (fat_parse_bpb_common(buf, &bpb) == 0);

    if (out_bpb)
        *out_bpb = bpb;

    /* The one check fat_parse_bpb_common() deliberately leaves out —
     * see fat_common.h's doc comment on that function for why
     * BS_FilSysType is FAT12-specific rather than shared. This is
     * Day 10's own acceptance-criteria check ("检查 BS_FileSysType
     * 是否为 FAT12"), unchanged from before the Day 15 refactor. */
    int fstype_ok = (memcmp(bpb.BS_FilSysType, "FAT12", 5) == 0);

    fat_geometry_t geom;
    fat_compute_geometry(&bpb, &geom);   /* always 0 — pure arithmetic */
    if (out_geom)
        *out_geom = geom;

    return (common_ok && fstype_ok) ? 0 : -1;
}

int fat12_mount(block_device_t *dev, uint32_t lba, fat12_fs_t *out_fs)
{
    /* Zero the whole handle up front so a failed mount never hands
     * the caller a partially-populated fat12_fs_t (e.g. a stale
     * dev/bpb/geom left over from a previous successful mount call
     * reusing the same out_fs storage). Mirrors fat12_parse_bpb()'s
     * "caller must check the return value before trusting output"
     * discipline, just stricter: unlike fat12_parse_bpb(), a failed
     * mount() zeroes rather than "fills anyway", since there is no
     * meaningful geometry to report when the read itself failed
     * (fat12_parse_bpb() is never even reached in that case). */
    memset(out_fs, 0, sizeof(fat12_fs_t));

    static uint8_t boot_sector[512];
    if (dev->read(dev, lba, 1, boot_sector) != 0)
        return -1;

    bpb_12_t bpb;
    fat12_geometry_t geom;

    /* fat12_parse_bpb()'s sanity check already includes the
     * BS_FilSysType == "FAT12" comparison (see its DAY9 comment
     * above), which is exactly Day 10's own acceptance-criteria
     * check ("检查 BS_FileSysType 是否为 FAT12") — so a single call
     * here satisfies both Day 9's general "is this a plausible FAT12
     * BPB" question and Day 10's specific "is BS_FileSysType FAT12"
     * question without duplicating the field comparison. */
    if (fat12_parse_bpb(boot_sector, &bpb, &geom) != 0)
        return -1;

    out_fs->dev        = dev;
    out_fs->mount_lba  = lba;   /* DAY11: remember the disk-absolute LBA this
                                  * volume's boot sector was read from — see
                                  * fat12.h's DAY11 note on fat12_geometry_t
                                  * for why fat12_next_cluster() below needs
                                  * this in addition to the volume-relative
                                  * geom.first_fat_sector. */
    out_fs->bpb  = bpb;
    out_fs->geom = geom;

    return 0;
}

/* DAY11 (Week 2, Day 11 of the 5.0 filesystem roadmap):
 *   "实现 FAT12 簇链遍历函数 fat12_next_cluster(cluster)（处理 1.5
 *    字节对齐，注意奇偶簇）。给定根目录的起始簇（通常为 0），能遍历
 *    打印出前 10 个簇号。"
 *
 *   See fat12.h's DAY11 doc comment for the full contract, the
 *   even/odd nibble-packing rationale, and the note on why "root
 *   directory's starting cluster" doesn't literally apply to FAT12.
 *   This is just the arithmetic + I/O.
 */
int32_t fat12_next_cluster(fat12_fs_t *fs, uint16_t cluster)
{
    /* Byte offset of this cluster's 12-bit entry within the FAT
     * region (FAT #1 — this project, like most FAT12 readers, never
     * consults the second/backup FAT copy; see fat12.h's DAY11
     * comment for why `cluster + cluster/2` is written this way
     * instead of `cluster * 3 / 2`). */
    uint32_t fat_byte_offset = cluster + (cluster / 2);

    /* This project assumes BLOCK_SECTOR_SIZE (512)-byte sectors
     * everywhere else (see block.h's header comment), so the two
     * static sector buffers below are sized for that rather than
     * fs->bpb.BPB_BytesPerSec generically — consistent with how
     * fat12_mount() above and every IDE PIO call in this codebase
     * already hardcode 512. bytes_per_sec is still read from the BPB
     * (not hardcoded 512 directly) for the offset/index arithmetic,
     * so this only breaks if a future non-512-byte-sector volume is
     * mounted, at which point the buffer sizes below would need to
     * grow too — noted here rather than silently assumed. */
    uint32_t bytes_per_sec = fs->bpb.BPB_BytesPerSec;
    if (bytes_per_sec == 0)
        bytes_per_sec = 512;

    uint32_t sector_index_in_fat = fat_byte_offset / bytes_per_sec;
    uint32_t offset_in_sector    = fat_byte_offset % bytes_per_sec;

    /* DAY11 BUGFIX: geom.first_fat_sector is volume-relative (straight
     * from the BPB — see fat12.h's DAY11 note above fat12_fs_t), so it
     * must be added to fs->mount_lba (the disk-absolute LBA this
     * volume's boot sector actually lives at) to get a real
     * dev->read() LBA. Using geom.first_fat_sector alone here is
     * exactly the bug that note describes — it would silently read
     * the wrong sector for any volume not mounted at disk LBA 0. */
    uint32_t lba = fs->mount_lba + fs->geom.first_fat_sector + sector_index_in_fat;

    /* Always read the FAT sector containing offset_in_sector, PLUS
     * the sector right after it, into one contiguous 2-sector buffer
     * (up to 1024 bytes for the 512-byte-sector case this project
     * assumes), then index into that combined buffer by
     * offset_in_sector. This unconditionally covers the "12-bit
     * value straddles a sector boundary" case described in fat12.h's
     * DAY11 comment (when offset_in_sector == bytes_per_sec - 1, b1
     * lives in the second sector) WITHOUT a separate boundary-only
     * code path: the non-boundary case just happens to only ever
     * touch buffer bytes that came from the first of the two sectors
     * read. One extra sector read per call is a deliberate simplicity
     * trade-off — this function isn't on a hot path yet (Day 65's
     * "预读缓存" roadmap step is where FAT-read performance work
     * happens), and a single always-correct code path is worth more
     * right now than avoiding one extra 512-byte read in the common
     * case.
     */
    static uint8_t fat_buf[1024];
    if (fs->dev->read(fs->dev, lba, 2, fat_buf) != 0)
        return -1;

    uint8_t b0 = fat_buf[offset_in_sector];
    uint8_t b1 = fat_buf[offset_in_sector + 1];

    uint16_t value;
    if ((cluster % 2) == 0) {
        /* Even cluster: owns the FIRST (low) 12 bits of the 3-byte
         * group -> all 8 bits of b0, plus the low nibble of b1 as
         * the top 4 bits. See fat12.h's DAY11 comment for why this
         * branch (and not the odd one) is the low-12-bits owner. */
        value = (uint16_t)b0 | (uint16_t)((b1 & 0x0F) << 8);
    } else {
        /* Odd cluster: owns the LAST (high) 12 bits of the 3-byte
         * group -> the high nibble of b0 as the bottom 4 bits, plus
         * all 8 bits of b1 as the top 8 bits. */
        value = (uint16_t)(b0 >> 4) | (uint16_t)((uint16_t)b1 << 4);
    }

    return (int32_t)value;
}
/* DAY12 (Week 2, Day 12 of the 5.0 filesystem roadmap):
 *   "实现 fat12_readdir(root_node)：读取根目录区（Root Directory
 *    Region），遍历每 32 字节条目。提取文件名（去掉空格）、属性、起始
 *    簇、文件大小。"
 *
 *   See fat12.h's DAY12 doc comment for the full contract: which
 *   entries are skipped (0x00 end-marker, 0xE5 deleted, LFN
 *   fragments, volume-label entries) and exactly how an 11-byte
 *   DIR_Name is decoded into a printable "8.3" string. This is just
 *   the I/O + the per-entry decode loop.
 */

/* fat12_decode_short_name(raw11, out13)
 *
 * Decodes one raw 11-byte DIR_Name (8-byte base + 3-byte extension,
 * space-padded) into a NUL-terminated "BASE.EXT" string in out13
 * (must have room for 13 bytes: 8 + '.' + 3 + '\0'). Trailing spaces
 * are stripped from the base and extension *separately* (see
 * fat12.h's DAY12 NAME DECODING note for why a naive single trailing-
 * space-strip over the whole 11 bytes is wrong), and the '.' is only
 * written if the extension is non-empty after stripping.
 */
static void fat12_decode_short_name(const uint8_t raw11[11], char out13[13])
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

int fat12_readdir(fat12_fs_t *fs, fat12_dirlist_entry_t *out_entries,
                   uint32_t max_entries, uint32_t *out_count)
{
    uint32_t count = 0;
    uint32_t sec;

    /* Root directory region is BLOCK_SECTOR_SIZE(512)-byte sectors,
     * same fixed-512 assumption fat12_next_cluster() already
     * documents and relies on (see its DAY11 comment). 16 entries of
     * 32 bytes each fit in one 512-byte sector. */
    static uint8_t sector_buf[512];
    const uint32_t entries_per_sector = 512 / sizeof(fat_dirent_t);

    for (sec = 0; sec < fs->geom.root_dir_sectors && count < max_entries; sec++) {
        /* DAY12: same volume-relative -> disk-absolute LBA fix Day 11
         * had to apply to first_fat_sector — geom.first_root_dir_sec
         * is relative to this volume's own boot sector, so
         * fs->mount_lba must be added back in. Using
         * geom.first_root_dir_sec alone here would be exactly the
         * Day 11 bug (silently reads the wrong sectors on any volume
         * not sitting at disk LBA 0), just against the root directory
         * region instead of the FAT region. */
        uint32_t lba = fs->mount_lba + fs->geom.first_root_dir_sec + sec;

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
                /* Spec-defined "no more entries past this point" —
                 * stop the whole walk, not just this sector (mirrors
                 * fat12_next_cluster()'s "legitimate end, not an
                 * error" treatment of the FAT EOC range). */
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

            fat12_dirlist_entry_t *out = &out_entries[count];
            fat12_decode_short_name(de->DIR_Name, out->name);
            out->attr          = de->DIR_Attr;
            out->start_cluster = de->DIR_FstClusLO;
            out->size          = de->DIR_FileSize;
            count++;
        }
    }

    if (out_count)
        *out_count = count;
    return 0;
}