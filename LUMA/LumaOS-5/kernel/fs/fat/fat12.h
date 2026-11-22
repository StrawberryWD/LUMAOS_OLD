/* kernel/fs/fat/fat12.h
 * LumaOS 5.0 - FAT12 On-Disk Structures & BPB Parser
 *
 * DAY9 (Week 2, Day 9 of the 5.0 filesystem roadmap):
 *   "创建 kernel/fs/fat/fat12.h。定义 BPB 结构体（bpb_12_t）和目录项
 *    结构（fat_dirent_t）。写一个 fat12_parse_bpb(buf)，能算出 RootDir
 *    扇区数、Data 起始扇区。"
 *
 *   This is the FAT12-specific counterpart to Day 5's mbr.h/mbr.c: it
 *   defines the raw on-disk layout of a FAT12 boot sector (the BIOS
 *   Parameter Block, BPB) and a 32-byte root/subdirectory entry, then
 *   provides fat12_parse_bpb() to turn a raw 512-byte sector buffer
 *   (as already read by disk->read(dev, lba, 1, buf) — the Day 6
 *   block_device_t interface) into a populated bpb_12_t plus the two
 *   derived geometry values Day 9's acceptance check asks for:
 *   RootDirSectors (how many sectors the root directory region
 *   occupies) and the first Data Region sector (where cluster 2, the
 *   first real data cluster, begins).
 *
 *   Nothing in this file mounts a filesystem, walks a directory, or
 *   touches the VFS layer from Day 8 (kernel/fs/vfs/vfs.h) yet — that
 *   starts Day 10 ("fat12_mount(block_dev)... 挂载成功返回 0") and
 *   Day 13 ("将 FAT12 挂接到 VFS"). Day 9's scope is strictly the
 *   struct shapes plus the arithmetic that every later FAT12 step
 *   (mount, cluster-chain walk, readdir, open, read) depends on, so
 *   getting the RootDirSectors/Data-region math right here first
 *   means Day 10+ can build on it without re-deriving it.
 *
 * Design notes:
 *   - bpb_12_t is laid out exactly as it appears in a FAT12 boot
 *     sector (offset 0, 512-byte sector 0 of a FAT12-formatted
 *     volume/partition), `packed` for the same reason
 *     mbr_partition_entry_t is in kernel/drivers/block/mbr.h: so it
 *     can be pointer-cast directly over bytes already read into a
 *     sector buffer, with x86 tolerating the resulting unaligned
 *     multi-byte field accesses natively. Field names follow the
 *     conventional Microsoft FAT spec naming (BS_jmpBoot, BPB_
 *     BytesPerSec, BPB_RootEntCnt, ...) so anyone cross-referencing
 *     the spec (or Day 16/17's FAT16/FAT32 work, which extends this
 *     same boot-sector prefix) can match fields by name.
 *   - Only the fields Day 9-14's roadmap steps actually need are
 *     included: enough to compute FAT/root-dir/data-region geometry
 *     (Day 9), verify the filesystem-type string (Day 10: "检查
 *     BS_FileSysType 是否为 FAT12"), and locate the root directory
 *     region for a later readdir (Day 12). Fields specific to
 *     FAT16/32 boot sectors (e.g. FAT32's BPB_FATSz32/BPB_RootClus)
 *     are deliberately left out of *this* struct — Day 15's roadmap
 *     step ("重构 FAT 通用层...创建 fat_common.c") is where the
 *     common vs. FAT12-only fields get properly separated; adding
 *     them here now would just be dead weight this struct doesn't
 *     use.
 *   - fat_dirent_t is the classic 32-byte 8.3 directory entry, exact
 *     on-disk layout, `packed` for the same pointer-cast reason as
 *     above. Day 12 ("遍历每 32 字节条目...提取文件名、属性、起始簇、
 *     文件大小") is what will actually walk an array of these; this
 *     header just defines the shape so that step doesn't also need to
 *     work out the byte layout from scratch.
 *   - fat12_parse_bpb() returns a small result struct (fat12_geometry_t)
 *     rather than several out-parameters, both because Day 9 asks for
 *     two related numbers (RootDirSectors and the Data region's first
 *     sector) that are logically one "layout" result, and because
 *     mbr_parse()'s int-return/out-array split isn't a good fit here:
 *     unlike the 4 fixed MBR entries, there's exactly one BPB per
 *     call, so a single struct return is simpler than an out-param.
 */
#ifndef LUMA_FAT12_H
#define LUMA_FAT12_H

#include <stdint.h>
#include "../../drivers/block/block.h"   /* block_device_t, used by fat12_mount() (DAY10) */

/* ===== BIOS Parameter Block (FAT12 boot sector, offset 0) =====
 *
 * Only the fields needed through Day 14 (mount, cluster-chain walk,
 * root-dir readdir, VFS registration) are included — see the
 * file-header design note above for why FAT16/32-only fields are
 * deliberately absent. Reserved/unused BPB bytes between named
 * fields are *not* padded out explicitly; the fields below are laid
 * out in on-disk order so the packed struct's implicit layout matches
 * the spec without gaps.
 */
typedef struct __attribute__((packed)) {
    uint8_t  BS_jmpBoot[3];      /* 0x00: x86 jump to boot code (ignored — not our loader) */
    uint8_t  BS_OEMName[8];      /* 0x03: OEM name string, not NUL-terminated */
    uint16_t BPB_BytesPerSec;    /* 0x0B: bytes per sector, almost always 512 */
    uint8_t  BPB_SecPerClus;     /* 0x0D: sectors per cluster (power of 2, >=1) */
    uint16_t BPB_RsvdSecCnt;     /* 0x0E: reserved sectors before the first FAT (incl. boot sector) */
    uint8_t  BPB_NumFATs;        /* 0x10: number of FAT copies, almost always 2 */
    uint16_t BPB_RootEntCnt;     /* 0x11: max root-directory entries (FAT12/16 only) */
    uint16_t BPB_TotSec16;       /* 0x13: total sectors, if it fits in 16 bits (else 0, use TotSec32) */
    uint8_t  BPB_Media;          /* 0x15: media descriptor byte (0xF8 = fixed disk) */
    uint16_t BPB_FATSz16;        /* 0x16: sectors per FAT (FAT12/16; FAT32 uses FATSz32 instead) */
    uint16_t BPB_SecPerTrk;      /* 0x18: sectors per track (CHS geometry, legacy) */
    uint16_t BPB_NumHeads;       /* 0x1A: number of heads (CHS geometry, legacy) */
    uint32_t BPB_HiddSec;        /* 0x1C: hidden sectors preceding this volume */
    uint32_t BPB_TotSec32;       /* 0x20: total sectors, if TotSec16 was 0 */
    uint8_t  BS_DrvNum;          /* 0x24: INT 13h drive number (0x80 = first fixed disk) */
    uint8_t  BS_Reserved1;       /* 0x25: reserved, must be 0 */
    uint8_t  BS_BootSig;         /* 0x26: extended boot signature, 0x29 if the next 3 fields are valid */
    uint32_t BS_VolID;           /* 0x27: volume serial number */
    uint8_t  BS_VolLab[11];      /* 0x2B: volume label, space-padded, not NUL-terminated */
    uint8_t  BS_FilSysType[8];   /* 0x36: e.g. "FAT12   " — advisory only per spec, but Day 10 checks it */
} bpb_12_t;

/* Boot sector signature bytes (same 0x55AA convention as MBR — see
 * MBR_SIGNATURE_OFFSET/BYTE0/BYTE1 in kernel/drivers/block/mbr.h),
 * present at the very end of every FAT boot sector regardless of
 * FAT12/16/32 variant. */
#define FAT12_BOOT_SIG_OFFSET  0x1FE  /* 510: 2-byte 0x55AA signature */
#define FAT12_BOOT_SIG_BYTE0   0x55
#define FAT12_BOOT_SIG_BYTE1   0xAA

/* ===== Directory entry (root directory & subdirectories, 32 bytes) =====
 *
 * The classic short (8.3) FAT directory entry. Day 12 will walk an
 * array of these (one per 32-byte slot in the root directory region,
 * or in a subdirectory's data clusters once subdirectories are
 * supported); this header only defines the on-disk shape.
 */
typedef struct __attribute__((packed)) {
    uint8_t  DIR_Name[11];        /* 0x00: 8.3 name, space-padded, no dot stored */
    uint8_t  DIR_Attr;            /* 0x0B: attribute bits, see FAT_ATTR_* below */
    uint8_t  DIR_NTRes;           /* 0x0C: reserved for Windows NT, must be 0 */
    uint8_t  DIR_CrtTimeTenth;    /* 0x0D: creation time, tenths of a second */
    uint16_t DIR_CrtTime;         /* 0x0E: creation time */
    uint16_t DIR_CrtDate;         /* 0x10: creation date */
    uint16_t DIR_LstAccDate;      /* 0x12: last access date */
    uint16_t DIR_FstClusHI;       /* 0x14: high 16 bits of first cluster (0 on FAT12/16) */
    uint16_t DIR_WrtTime;         /* 0x16: last write time */
    uint16_t DIR_WrtDate;         /* 0x18: last write date */
    uint16_t DIR_FstClusLO;       /* 0x1A: low 16 bits of first cluster (the whole cluster # on FAT12) */
    uint32_t DIR_FileSize;        /* 0x1C: file size in bytes, 0 for directories */
} fat_dirent_t;

/* DIR_Name[0] sentinel values (Day 12/24/26 all need these). */
#define FAT_DIRENT_FREE       0xE5  /* entry is free (deleted); rest of entry is stale */
#define FAT_DIRENT_END        0x00  /* entry is free AND no further entries exist in this directory */
#define FAT_DIRENT_DOT_FLAG   0x2E  /* '.' — this or the following byte starts "." / ".." */

/* DIR_Attr bits (standard FAT attribute byte). */
#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
/* Long-file-name entries mark all four of these bits — not otherwise
 * used by this roadmap (8.3 names only through Day 14), but recorded
 * here so a raw dump of a real-world FAT image doesn't misdecode LFN
 * slots as ordinary garbage entries. */
#define FAT_ATTR_LONG_NAME  (FAT_ATTR_READ_ONLY | FAT_ATTR_HIDDEN | \
                              FAT_ATTR_SYSTEM | FAT_ATTR_VOLUME_ID)

/* ===== Derived geometry (Day 9's actual deliverable) =====
 *
 * The two numbers Day 9 asks fat12_parse_bpb() to compute, plus the
 * handful of intermediate values later steps (Day 10 mount, Day 11
 * cluster-chain walk, Day 12 readdir) will also need and would
 * otherwise have to re-derive from the raw bpb_12_t by hand.
 */
typedef struct {
    uint32_t root_dir_sectors;   /* sectors occupied by the root directory region */
    uint32_t first_root_dir_sec; /* first sector (volume-relative) of the root directory region */
    uint32_t first_data_sector;  /* first sector (volume-relative) of the data region (cluster 2) */
    uint32_t first_fat_sector;   /* first sector (volume-relative) of FAT #1 */
    uint32_t total_sectors;      /* volume's total sector count (TotSec16 or TotSec32, whichever is set) */
    uint32_t bytes_per_cluster;  /* BPB_BytesPerSec * BPB_SecPerClus, for convenience */
} fat12_geometry_t;

/* fat12_parse_bpb(buf, out_bpb, out_geom)
 *
 * Reads a raw 512-byte FAT12 boot-sector buffer (as already read via
 * block_device_t's disk->read(dev, lba, 1, buf) — same pattern Day 3/
 * 5 use for LBA 0) and:
 *
 *   1. Copies it into *out_bpb (a straight struct-sized memcpy, since
 *      bpb_12_t is packed to match the on-disk layout exactly — same
 *      technique mbr_parse() uses for mbr_partition_entry_t).
 *   2. Computes the Day 9 acceptance-criteria values into *out_geom:
 *        RootDirSectors  = ((BPB_RootEntCnt * 32) + (BPB_BytesPerSec - 1))
 *                           / BPB_BytesPerSec
 *        FirstRootDirSec = BPB_RsvdSecCnt + (BPB_NumFATs * BPB_FATSz16)
 *        FirstDataSector = FirstRootDirSec + RootDirSectors
 *      (the standard Microsoft FAT spec formulas), plus the
 *      first-FAT-sector, total-sector-count, and bytes-per-cluster
 *      values later steps will also need.
 *
 * `out_bpb` and/or `out_geom` may be NULL if the caller only wants
 * the other one populated.
 *
 * Returns 0 if the buffer looks like a genuine FAT12 boot sector: the
 * 0x55AA signature at FAT12_BOOT_SIG_OFFSET is present AND
 * BPB_BytesPerSec/BPB_SecPerClus/BPB_NumFATs all hold spec-legal
 * values AND BS_FilSysType reads "FAT12" (see fat12.c's DAY9 comment
 * for why 0x55AA alone isn't sufficient — every bootable x86 sector,
 * FAT or not, ends in 0x55AA). Returns -1 otherwise; *out_geom is
 * still filled in on a -1 return (mirroring mbr_parse()'s "entries
 * still copied out, treat with suspicion" contract) so a caller doing
 * diagnostic dumping can inspect what was actually on the sector.
 */
int fat12_parse_bpb(const uint8_t buf[512], bpb_12_t *out_bpb,
                     fat12_geometry_t *out_geom);

/* ===== Mount state (Day 10's actual deliverable) =====
 *
 * DAY10 (Week 2, Day 10 of the 5.0 filesystem roadmap):
 *   "实现 fat12_mount(block_dev)：读取引导扇区，检查 BS_FileSysType
 *    是否为 FAT12。挂载成功返回 0，失败返回 -1，在终端打印
 *    "FAT12 mounted"。"
 *
 *   fat12_fs_t is the small piece of mount state a successful
 *   fat12_mount() call produces: which block_device_t the volume
 *   lives on, plus the BPB and derived geometry fat12_parse_bpb()
 *   already knows how to compute (Day 9). This is deliberately NOT
 *   wired into kernel/fs/vfs/vfs.h's `struct filesystem` yet — that
 *   plug-in ("注册 fat12_filesystem_ops") is Day 13's job. Until
 *   then, fat12_fs_t is a standalone handle a caller (today: kmain.c;
 *   Day 11+: the cluster-chain walk and readdir code) can hold and
 *   pass around on its own.
 */
typedef struct {
    block_device_t    *dev;        /* the volume's underlying block device */
    uint32_t           mount_lba;  /* DAY11: disk-absolute LBA of this volume's boot
                                     * sector (the `lba` fat12_mount() was called
                                     * with) — see DAY11 note on fat12_geometry_t
                                     * below for why this has to be remembered. */
    bpb_12_t           bpb;        /* parsed boot sector, see fat12_parse_bpb() */
    fat12_geometry_t   geom;       /* derived geometry, see fat12_parse_bpb() */
} fat12_fs_t;

/* DAY11 NOTE ON fat12_geometry_t's *_sector FIELDS BEING VOLUME-RELATIVE:
 * fat12_parse_bpb() (Day 9) computes first_fat_sector/first_root_dir_sec/
 * first_data_sector purely from BPB fields — they are sector offsets
 * *within the volume* (sector 0 = this volume's own boot sector),
 * exactly matching the FAT spec's own formulas. They are NOT
 * disk-absolute LBAs whenever the volume doesn't start at LBA 0 of
 * the underlying disk (e.g. this project's Day 9-followup/Day 10 test
 * fixture at disk LBA 2500, or Day 11's own fixture at disk LBA 5000).
 *
 * Through Day 10 this distinction never mattered in practice: no code
 * in this codebase ever issued a dev->read() using one of these
 * fields as if it were disk-absolute — fat12_mount() only reads the
 * single boot-sector LBA it's explicitly given, and Day 9/10's kmain.c
 * tests only ever print geom's fields, never dereference them. Day 11's
 * fat12_next_cluster() is the FIRST code path to actually turn
 * first_fat_sector into a dev->read() LBA — and doing that with just
 * first_fat_sector alone (as an early version of this function did,
 * before this was caught by a live QEMU run showing garbage cluster
 * values at LBA 5000 despite a passing host cross-validation test that
 * happened to mount at LBA 0) silently reads the wrong sector on any
 * volume not sitting at LBA 0 of the disk. Fixed by having
 * fat12_mount() remember mount_lba above, and fat12_next_cluster()
 * add it back in: the correct disk-absolute LBA is always
 * `fs->mount_lba + fs->geom.first_fat_sector + <sectors into the FAT>`.
 */

/* fat12_mount(dev, lba, out_fs)
 *
 * Reads the boot sector at `lba` (volume-relative — normally 0 for a
 * whole-disk FAT12 volume, or an MBR partition's start_lba per Day 5's
 * mbr_find_first_active_fat() for a partitioned one) off `dev` via
 * dev->read(dev, lba, 1, buf) — the Day 6 block_device_t interface,
 * same call kmain.c already makes directly — then hands the buffer to
 * fat12_parse_bpb() (Day 9) and additionally checks BS_FilSysType
 * reads exactly "FAT12   " (8 bytes, space-padded), the Day 10
 * acceptance criterion's specific check on top of Day 9's broader
 * sanity check (which already includes this same comparison — see
 * fat12_parse_bpb()'s contract above — so this is a second look at
 * the same field for the sake of Day 10's own explicit "check
 * BS_FileSysType" requirement, not a materially different test).
 *
 * On success, populates *out_fs (dev pointer, parsed bpb, derived
 * geometry) and returns 0. On failure — dev->read() itself fails, OR
 * the sector doesn't parse as a valid FAT12 BPB per
 * fat12_parse_bpb()'s contract — returns -1; *out_fs is left
 * zero-initialized in that case (unlike fat12_parse_bpb()'s "fill
 * anyway" contract: a failed mount has no sensible fs handle to hand
 * back, so callers must check the return value before using *out_fs).
 *
 * `out_fs` must not be NULL.
 */
int fat12_mount(block_device_t *dev, uint32_t lba, fat12_fs_t *out_fs);

/* ===== Cluster-chain values (Day 11's actual deliverable) =====
 *
 * FAT12 reserves the low end of the cluster-number space for
 * bookkeeping rather than real data clusters — a FAT[cluster] table
 * entry doesn't always hold "the next cluster in a chain", it can
 * also hold one of these sentinel meanings. Every later step that
 * walks a chain (Day 11 here; Day 12's readdir indirectly, via the
 * DIR_FstClusLO it hands to Day 11's walker; Day 19's file read) has
 * to recognize these before treating a FAT entry as "keep going".
 *
 *   0x000            - free cluster (not allocated to any file)
 *   0x001             - reserved, never used by real data (mirrors
 *                        clusters 0/1 both being reserved FAT
 *                        entries per the spec — see fat12.c's DAY9
 *                        FirstDataSector comment)
 *   0xFF7             - marks a cluster the OS has flagged "bad
 *                        sector, do not allocate"
 *   0xFF8 - 0xFFF      - end-of-chain. IMPORTANT: this is a *range*,
 *                        not a single value — a real mkfs.fat/format
 *                        tool is free to write any value in
 *                        0xFF8..0xFFF to terminate a chain (0xFFF is
 *                        the most common in practice, but treating
 *                        ONLY 0xFFF as "end" and anything else in
 *                        that range as "a real next-cluster number"
 *                        is a bug — this project's own test fixture,
 *                        built with `mkfs.fat -F 12`, terminates its
 *                        test file's chain at 0xFFF, but code that
 *                        hardcodes that single value instead of the
 *                        >= 0xFF8 range would silently break against
 *                        other formatters).
 */
#define FAT12_CLUSTER_FREE        0x000
#define FAT12_CLUSTER_RESERVED    0x001  /* clusters 0 and 1 are both reserved */
#define FAT12_CLUSTER_BAD         0xFF7
#define FAT12_CLUSTER_EOC_MIN     0xFF8  /* >= this value means "end of chain" */
#define FAT12_CLUSTER_EOC_MAX     0xFFF  /* largest legal 12-bit value */

/* fat12_next_cluster(fs, cluster)
 *
 * DAY11 (Week 2, Day 11 of the 5.0 filesystem roadmap):
 *   "实现 FAT12 簇链遍历函数 fat12_next_cluster(cluster)（处理 1.5
 *    字节对齐，注意奇偶簇）。给定根目录的起始簇（通常为 0），能遍历
 *    打印出前 10 个簇号。"
 *
 *   NOTE ON THE ROADMAP TEXT: strictly speaking FAT12/16's root
 *   directory is NOT a cluster chain — it's a fixed-size region
 *   (BPB_RootEntCnt entries) located by fat12_geometry_t's
 *   first_root_dir_sec, described here in fat12.h's Day 9 doc
 *   comment. There is no "root directory's starting cluster" to feed
 *   this function on FAT12/16 (that concept only exists for FAT32's
 *   BPB_RootClus, a Day 17 concern). The roadmap's acceptance text is
 *   read here as shorthand for "exercise the cluster-chain walker
 *   end-to-end, printing 10 cluster numbers" rather than a literal
 *   instruction to chain-walk the root directory — see kmain.c's
 *   DAY11 section for how the test harness resolves this (it walks
 *   cluster 2 onward, the first real data cluster, using a test file
 *   built specifically to have a chain >= 10 clusters long, plus a
 *   sanity call on cluster 0/1 to confirm the reserved values print
 *   without the walker crashing or looping).
 *
 *   `cluster` is the CURRENT cluster number; this function looks up
 *   FAT[cluster] — the table entry describing what comes after it —
 *   and returns that raw 12-bit value: either a real next-cluster
 *   number (2..0xFEF), FAT12_CLUSTER_FREE/RESERVED/BAD, or a value
 *   >= FAT12_CLUSTER_EOC_MIN meaning "chain ends here". Callers walk
 *   a whole chain by repeatedly feeding each return value back in as
 *   the next `cluster` argument, stopping once the return is negative
 *   (I/O error) or >= FAT12_CLUSTER_EOC_MIN (legitimate end).
 *
 *   ---- The 1.5-byte-per-entry packing, and why it must be handled
 *   this way (this is the step the roadmap calls out by name as the
 *   easiest one to get backwards) ----
 *
 *   A FAT12 table entry is only 12 bits, not a clean 8 or 16 — so two
 *   consecutive cluster numbers (one even-numbered, one odd-numbered)
 *   are packed together into 3 bytes (24 bits = 2*12 bits) instead of
 *   each getting its own byte-aligned slot. The byte offset of
 *   cluster N's entry within the FAT is `N + N/2` (equivalently
 *   `N*3/2`, but `N + N/2` is used here specifically because it never
 *   needs a 3x multiply and, more importantly, makes it visually
 *   obvious this is "N whole bytes, plus a half-byte-pair correction"
 *   rather than looking like an arbitrary scaling factor — the
 *   integer-division truncation of N/2 is exactly what's wanted here,
 *   since a pair of clusters (2*(N/2), 2*(N/2)+1) always shares one
 *   fixed 3-byte group starting at byte 3*(N/2)).
 *
 *   Once you have the 2 bytes at that offset (call them b0 = low
 *   byte, b1 = high byte of the 16-bit little-endian read), which 12
 *   bits belong to cluster N depends on whether N is even or odd,
 *   because the packing alternates which half of the 3-byte group
 *   each cluster owns:
 *
 *     - N even:  N's 12 bits are the FIRST (low) 12 bits of the
 *                3-byte group -> all of b0, plus the low nibble of
 *                b1: value = b0 | ((b1 & 0x0F) << 8)
 *     - N odd:   N's 12 bits are the LAST (high) 12 bits of the
 *                3-byte group -> the high nibble of b0, plus all of
 *                b1: value = (b0 >> 4) | (b1 << 4)
 *
 *   Concretely: bytes [B0 B1 B2] hold clusters N (even) and N+1 (odd)
 *   packed as nibbles [b0_lo b0_hi | b1_lo b1_hi | b2_lo b2_hi] =
 *   [N_0 N_1 | N_2 N0+1_0 | N+1_1 N+1_2] — cluster N owns B0's full
 *   byte plus B1's low nibble; cluster N+1 owns B1's high nibble plus
 *   B2's full byte. Getting the even/odd branches swapped is the
 *   classic FAT12 bug this doc comment (and the roadmap text) warns
 *   about: it silently produces a plausible-looking but wrong cluster
 *   number instead of an obvious crash, since both branches produce
 *   values in the same 0-4095 range.
 *
 *   ---- Sector-boundary crossing ----
 *
 *   The 2 bytes an entry needs can straddle a sector boundary: if
 *   `N + N/2` mod BPB_BytesPerSec == BPB_BytesPerSec - 1, byte b0 is
 *   the last byte of one FAT sector and b1 is the first byte of the
 *   next one. fat12.c's implementation always reads two consecutive
 *   FAT sectors into one buffer before indexing into it, specifically
 *   so this case never needs special-casing separately from the
 *   common case — see fat12.c's DAY11 comment for why that's the
 *   chosen approach over a boundary-only fallback path.
 *
 * Returns the raw 12-bit FAT entry value (0..0xFFF, always
 * non-negative) on success. Returns -1 if the underlying
 * dev->read(...) call fails (mirrors this project's "negative means
 * failure" convention used throughout — see fat12_mount()'s
 * comment) — this is unambiguous because a real 12-bit entry can
 * never itself be negative.
 */
int32_t fat12_next_cluster(fat12_fs_t *fs, uint16_t cluster);

/* ===== Root directory listing (Day 12's actual deliverable) =====
 *
 * DAY12 (Week 2, Day 12 of the 5.0 filesystem roadmap):
 *   "实现 fat12_readdir(root_node)：读取根目录区（Root Directory
 *    Region），遍历每 32 字节条目。提取文件名（去掉空格）、属性、起始
 *    簇、文件大小。在终端列出根目录下所有文件名（至少能看到 KERNEL
 *    BIN 和 LOADER BIN）。"
 *
 *   NOTE ON "root_node" AND "KERNEL BIN"/"LOADER BIN": the roadmap's
 *   acceptance text is phrased as if this project's own lumaos-5.0.img
 *   were itself a FAT12 volume whose root directory holds KERNEL.BIN
 *   and LOADER.BIN — but LBA 0 of this image is LumaOS's own custom
 *   mbr.asm bootloader, not a FAT12 boot sector (same point already
 *   made in fat12.h's DAY9 comment and kmain.c's DAY9 comment), and
 *   kernel.bin/loader.bin are dd'd onto raw disk-absolute sectors by
 *   build.sh, never stored as named files inside a filesystem. There
 *   is no FAT12 volume anywhere in this project that literally
 *   contains a "KERNEL.BIN" or "LOADER.BIN" directory entry, and
 *   Day 8/13's `struct vfs_node` (kernel/fs/vfs/vfs.h) isn't wired to
 *   FAT12 until Day 13 ("将 FAT12 挂接到 VFS"), so `fat12_readdir()`
 *   below intentionally takes a `fat12_fs_t *` (the same standalone
 *   mount handle fat12_mount()/fat12_next_cluster() already use), not
 *   a `vfs_node_t *`. This is read the same way Day 11's header
 *   comment reads "根目录的起始簇（通常为 0）": as shorthand for
 *   "list whatever real, verifiable files the current fixture
 *   actually has", not a literal KERNEL.BIN/LOADER.BIN requirement.
 *   kmain.c's DAY12 section lists tests/fat12-with-file.img's root
 *   directory (Day 11's own fixture, already embedded at LBA 5000),
 *   which for exactly this reason contains BIGFILE.BIN and SMALL.BIN
 *   instead — two real, independently-verifiable files this project
 *   already cross-validated in Day 11's header comment — rather than
 *   inventing a new fixture just to match the roadmap text's literal
 *   two example filenames.
 *
 *   `fat12_dirlist_entry_t` is the small, already-decoded result of
 *   walking one 32-byte fat_dirent_t: a NUL-terminated display name
 *   with trailing 8.3 padding spaces stripped (Day 12's "去掉空格"),
 *   plus the attribute byte, starting cluster, and file size the
 *   roadmap explicitly asks to extract. This is deliberately a
 *   separate, smaller struct from fat_dirent_t itself (which stays
 *   the raw on-disk layout, Day 9's struct) rather than reusing
 *   fat_dirent_t directly for the decoded result — the space-padded
 *   11-byte DIR_Name isn't a C string, and callers (kmain.c's test
 *   harness today; Day 14's `cmd_ls` eventually) want a printable
 *   name, not raw on-disk bytes.
 *
 *   NAME DECODING ("去掉空格" — strip the spaces): a raw DIR_Name[11]
 *   is 8 bytes of base name + 3 bytes of extension, space-padded on
 *   the right within each part (e.g. "BIGFILE BIN" as the literal 11
 *   bytes for "BIGFILE.BIN", "SMALL   BIN" for "SMALL.BIN"). Decoding
 *   trims trailing spaces from the 8-byte base separately from the
 *   3-byte extension (an entry with a short base name but a full
 *   3-char extension, e.g. "A       TXT", must not have the base's
 *   trailing spaces bleed into where the dot belongs), then joins
 *   them with "." only if the extension part is non-empty (many real
 *   FAT volumes have extension-less entries, e.g. a subdirectory or a
 *   file literally named "README" with no extension — an unconditional
 *   dot would wrongly render those as "README."). A directory entry's
 *   FAT_ATTR_DIRECTORY-flagged name (e.g. "." or ".." synthetic
 *   entries once subdirectories exist, not exercised by this project
 *   until Day 42) follows this same 8+3 decode; nothing here special-
 *   cases "." / ".." differently.
 *
 *   ENTRIES SKIPPED BY THE WALK (none of these count toward
 *   `out_count`/get returned to the caller):
 *     - DIR_Name[0] == FAT_DIRENT_END (0x00): per the FAT spec this
 *       marks not just "this entry is free" but "every entry from
 *       here to the end of the region is also free" — so encountering
 *       it is the walk's normal, expected stop condition, not an
 *       error (mirrors fat12_next_cluster()'s treatment of
 *       FAT12_CLUSTER_EOC_MIN..MAX as "legitimate end", not failure).
 *     - DIR_Name[0] == FAT_DIRENT_FREE (0xE5): a deleted entry — the
 *       rest of the 32 bytes are stale leftovers from whatever file
 *       used to be there, so it's skipped but the walk continues
 *       (unlike 0x00, a deleted slot doesn't imply every entry after
 *       it is also free — a later create could have reused a
 *       different, non-adjacent free slot).
 *     - DIR_Attr == FAT_ATTR_LONG_NAME (the four bits all set): a
 *       VFAT long-filename fragment, not a real 8.3 entry. This
 *       roadmap is 8.3-only through Day 14 (see fat12.h's DAY9 file
 *       comment), so these are skipped rather than decoded — the
 *       fixture this project mounts (tests/fat12-with-file.img, an
 *       mtools `mcopy` of two files whose names are already legal
 *       8.3 short names) never actually produces one, but real-world
 *       FAT volumes commonly do, and skipping instead of misreading
 *       a fragment as a garbled short name is the only safe option.
 *     - DIR_Attr & FAT_ATTR_VOLUME_ID: the root directory's volume
 *       label entry (if present) isn't a file or a directory; skipped
 *       for the same reason.
 *
 * Implementation note for fat12.c: this walk needs to read the root
 * directory region's `geom.root_dir_sectors` sectors starting at
 * disk-absolute LBA `fs->mount_lba + fs->geom.first_root_dir_sec` —
 * same volume-relative-vs-disk-absolute distinction fat12_next_
 * cluster()'s Day 11 bugfix already documents for first_fat_sector
 * (see fat12_fs_t's DAY11 note above); getting this wrong would
 * silently read the wrong sectors for the Day 11/12 fixture mounted
 * at LBA 5000, the same way the pre-bugfix fat12_next_cluster() did
 * for the FAT region.
 */
#define FAT12_DIRLIST_MAX_ENTRIES  32  /* enough for this roadmap's
                                          fixtures (2 real files); a
                                          bigger root dir just gets
                                          truncated to this many, not
                                          a crash — see fat12.c */

typedef struct {
    char      name[13];   /* "8.3" decoded, NUL-terminated, spaces
                              stripped, at most "12345678.123\0" */
    uint8_t   attr;        /* raw DIR_Attr byte, see FAT_ATTR_* */
    uint16_t  start_cluster; /* DIR_FstClusLO (FAT12: the whole cluster #) */
    uint32_t  size;        /* DIR_FileSize, 0 for directories */
} fat12_dirlist_entry_t;

/* fat12_readdir(fs, out_entries, max_entries, out_count)
 *
 * Reads `fs`'s root directory region (fs->geom.root_dir_sectors
 * sectors at disk-absolute LBA fs->mount_lba + fs->geom.
 * first_root_dir_sec) and decodes each 32-byte fat_dirent_t slot into
 * `out_entries[]`, per the DAY12 doc comment above for exactly which
 * entries are skipped and how names are decoded. Stops early, without
 * error, at the first FAT_DIRENT_END (0x00) entry (spec-defined "no
 * more entries past this point") or once `max_entries` decoded
 * entries have been written, whichever comes first.
 *
 * On success, writes the number of entries actually decoded into
 * `*out_count` (0 is a legal result: an empty root directory) and
 * returns 0. Returns -1 if the underlying dev->read(...) call fails
 * partway through the region — mirrors fat12_next_cluster()'s
 * "negative means I/O failure" convention; *out_count is left
 * whatever it was last set to (entries already decoded before the
 * failing read are still valid, same "don't discard partial progress
 * on a later error" spirit as fat12_mount() zeroing *out_fs only on
 * its *own* first read failing, not a later one).
 *
 * `fs`, `out_entries`, and `out_count` must not be NULL. `out_entries`
 * must have room for at least `max_entries` fat12_dirlist_entry_t
 * slots.
 */
int fat12_readdir(fat12_fs_t *fs, fat12_dirlist_entry_t *out_entries,
                   uint32_t max_entries, uint32_t *out_count);

#endif /* LUMA_FAT12_H */