/* kernel/fs/lmfs/lmfs.c
 * LumaOS 5.0 - LMFS Mount, Inode Read & Directory Listing
 *
 * DAY32 (第二月 · 第5周 · Day 32 of the 5.0 filesystem roadmap):
 *   "C 端实现 lmfs_mount(dev)。"
 *   验收："能读取超级块并验证版本/容量。"
 *
 * DAY33 (Day 33 of the same roadmap):
 *   "实现 lmfs_read_inode()。" 验收："能读取 inode 0。"
 *
 * DAY34 (Day 34 of the same roadmap):
 *   "实现 lmfs_readdir()。" 验收："能列出根目录。"
 *
 * 见 lmfs.h 顶部注释：这是 fat12_mount()（kernel/fs/fat/fat12.c，
 * Day 10）在 LMFS 这边的对应实现，读盘方式、成功/失败的填充/清零
 * 约定都直接照搬那份已经跑通的代码，只是超级块的位置、校验的字段
 * 不同——具体校验哪些字段、为什么，见 lmfs.h 里 lmfs_mount() 声明
 * 上方那段详细注释，这里的实现严格照那段注释的四条校验顺序写。
 *
 * lmfs_read_inode() 的定位算法、为什么 inode 表起始 LBA 要在
 * lmfs_mount() 里现算而不是存进超级块，同样见 lmfs.h 里对应声明
 * 上方的详细注释——这里的实现只是照那段注释翻译成代码。
 *
 * lmfs_readdir() 的扫描算法、块号->LBA 换算、v1.0 只走 direct[] 的
 * 范围限定，同样见 lmfs.h 里对应声明上方的详细注释。
 */
#include "lmfs.h"
#include "../../lib/string.h"   /* memset()/memcpy() */

/* compute_inode_table_lba(sb)
 *
 * doc/lmfs.txt 第九节：inode 表紧接在数据块位图之后，起始位置 =
 * data_bitmap_lba + 数据块位图占用的扇区数。数据块位图大小 =
 * ceil(data_block_count / 8) 字节（1 个 bit 对应 1 个数据块），
 * 再 ceil 到整数个 512 字节扇区——跟 tools/mkfs.lmfs 里
 * build_layout() 算 data_bitmap_sectors 的公式完全一样，两边算出
 * 来的值必须一致，mkfs 写下的超级块字段（data_bitmap_lba、
 * data_block_count）才能被这里正确复算回同一个 inode 表位置。
 */
static uint32_t compute_inode_table_lba(const lmfs_superblock_t *sb)
{
    uint32_t bitmap_bytes = (sb->data_block_count + 7) / 8;
    uint32_t bitmap_sectors = (bitmap_bytes + LMFS_SECTOR_SIZE - 1)
                              / LMFS_SECTOR_SIZE;
    return sb->data_bitmap_lba + bitmap_sectors;
}

/* compute_data_area_lba(sb, inode_table_lba)
 *
 * DAY34: doc/lmfs.txt 第九节：数据区紧接在 inode 表之后，且起点必须
 * 是偶数 LBA（块号 -> LBA 的换算是 LBA = 块号*2，只有起点是偶数，
 * 块号 0 才会精确落在起点上）。这里的取整方式必须跟 tools/mkfs.lmfs
 * 的 build_layout() / round_up_even() 完全一致——两边独立算出同一个
 * 数据区起点，是 lmfs_readdir() 能读对 mkfs.lmfs 写下的目录块的前提。 */
static uint32_t compute_data_area_lba(const lmfs_superblock_t *sb,
                                       uint32_t inode_table_lba)
{
    uint32_t inode_table_bytes = sb->inode_count * LMFS_INODE_SIZE;
    uint32_t inode_table_sectors = (inode_table_bytes + LMFS_SECTOR_SIZE - 1)
                                   / LMFS_SECTOR_SIZE;
    uint32_t raw = inode_table_lba + inode_table_sectors;
    return (raw % 2 == 0) ? raw : raw + 1;   /* round_up_even() */
}

int lmfs_mount(block_device_t *dev, uint32_t lba, lmfs_fs_t *out_fs)
{
    /* 失败路径一律清零 out_fs，不留半成品状态——跟 fat12_mount()
     * 开头的同一句 memset() 是同一个理由（见 fat12.c 对应位置的
     * 注释）。 */
    memset(out_fs, 0, sizeof(lmfs_fs_t));

    /* 超级块整块占满 1 个 512 字节扇区（doc/lmfs.txt 第二节），
     * 用一个跟 lmfs_superblock_t 同样大小、同样 packed 布局的缓冲区
     * 接住 dev->read() 读回来的原始字节，读完直接把缓冲区内容整体
     * 拷进 lmfs_superblock_t——跟 fat12_mount() 用 `static uint8_t
     * boot_sector[512]` 再喂给 fat12_parse_bpb() 是同一个"先读原始
     * 扇区、再按结构体解释"的两步走，只是 LMFS 的超级块本来就是
     * packed struct，可以省掉 FAT12 那边单独的 parse 函数，直接把
     * 缓冲区整体当 lmfs_superblock_t 使用。 */
    static uint8_t sb_buf[LMFS_SECTOR_SIZE];
    _Static_assert(sizeof(sb_buf) == sizeof(lmfs_superblock_t),
                   "超级块读取缓冲区大小应该跟 lmfs_superblock_t 一致");

    uint32_t sb_lba = lba + LMFS_SUPERBLOCK_LBA;  /* 卷内 LBA1 -> 磁盘绝对 LBA，
                                                     * 见 lmfs.h Design notes
                                                     * 最后一条 */
    if (dev->read(dev, sb_lba, 1, sb_buf) != 0)
        return -1;

    lmfs_superblock_t sb;
    memcpy(&sb, sb_buf, sizeof(sb));

    /* ---- 校验 1/4：魔数 ---- */
    if (sb.magic != LMFS_MAGIC)
        return -1;

    /* ---- 校验 2/4：版本（主版本必须精确匹配；次版本暂不检查——
     * doc/lmfs.txt 第八节 version_major/minor 详解：次版本升级通常
     * 只是加可选特性，老驱动大多还能正常读写，只有主版本不同才代表
     * 磁盘布局可能已经根本性改变） ---- */
    if (sb.version_major != LMFS_VERSION_MAJOR)
        return -1;

    /* ---- 校验 2/4（续）：不兼容特性标志。v1.0 的 mkfs.lmfs 产出的
     * 卷这个字段恒为 0；非 0 说明这是本驱动看不懂的更新格式，必须
     * 拒绝挂载而不是硬挂载后读出一堆解释错误的数据——doc/lmfs.txt
     * 第三节 feature_incompat 字段详解的直接应用。 ---- */
    if (sb.feature_incompat != 0)
        return -1;

    /* ---- 校验 3/4：容量非零 ---- */
    if (sb.inode_count == 0 || sb.data_block_count == 0)
        return -1;

    /* ---- 校验 4/4：两个位图的相对顺序必须符合 doc 第九节定死的卷
     * 布局（超级块 -> inode 位图 -> 数据块位图 -> ...）。这不是在
     * 重新计算精确的区域大小，只是一个"数值顺序合理"的粗粒度自检
     * ——真损坏的超级块（比如被写坏、或者压根不是 mkfs.lmfs 格式化
     * 出来的卷）大概率会在这一步就露出破绽，而不用等到 lmfs_read_
     * inode() 读 inode 表时才用一堆垃圾地址崩掉。 ---- */
    if (sb.inode_bitmap_lba <= LMFS_SUPERBLOCK_LBA)
        return -1;
    if (sb.data_bitmap_lba <= sb.inode_bitmap_lba)
        return -1;

    /* 全部通过：填好 out_fs 并返回成功。inode_table_lba 在这里现算
     * 一次、缓存下来——DAY33: lmfs_read_inode() 每次调用直接读这个
     * 缓存值，不会每次都重新算一遍（见 lmfs.h::lmfs_fs_t 的注释）。 */
    out_fs->dev             = dev;
    out_fs->mount_lba       = lba;
    out_fs->sb              = sb;
    out_fs->inode_table_lba = compute_inode_table_lba(&sb);
    out_fs->data_area_lba   = compute_data_area_lba(&sb, out_fs->inode_table_lba);

    return 0;
}

int lmfs_read_inode(lmfs_fs_t *fs, uint32_t inode_num,
                     lmfs_inode_t *out_inode)
{
    memset(out_inode, 0, sizeof(lmfs_inode_t));

    /* fs 必须是已经 lmfs_mount() 成功过的状态——跟真正 NULL 检查
     * 一样便宜，但顺带也挡住了"调用者传了一个从没 mount 成功、
     * dev 还是清零状态的 fs"这种用法错误。 */
    if (fs->dev == 0)
        return -1;

    /* 越界检查：doc/lmfs.txt 第三节，inode_count 就是这个卷的
     * inode 总数，>= 这个数的 inode 号在这个卷上根本不存在。 */
    if (inode_num >= fs->sb.inode_count)
        return -1;

    /* 定位：见 lmfs.h 里 lmfs_read_inode() 声明上方注释的公式，
     * 8 个 inode 一个扇区（LMFS_INODES_PER_SECTOR = 512/64）。 */
    uint32_t sector_in_table = inode_num / LMFS_INODES_PER_SECTOR;
    uint32_t offset_in_sector = (inode_num % LMFS_INODES_PER_SECTOR)
                                * LMFS_INODE_SIZE;
    uint32_t sector_lba = fs->mount_lba + fs->inode_table_lba
                          + sector_in_table;

    static uint8_t sector_buf[LMFS_SECTOR_SIZE];
    if (fs->dev->read(fs->dev, sector_lba, 1, sector_buf) != 0)
        return -1;

    memcpy(out_inode, sector_buf + offset_in_sector, sizeof(lmfs_inode_t));
    return 0;
}

int lmfs_readdir(lmfs_fs_t *fs, uint32_t dir_inode_num,
                  lmfs_dirlist_entry_t *out_entries,
                  uint32_t max_entries, uint32_t *out_count)
{
    *out_count = 0;

    lmfs_inode_t dir_inode;
    if (lmfs_read_inode(fs, dir_inode_num, &dir_inode) != 0)
        return -1;

    /* 对一个不是目录的 inode 调用 lmfs_readdir() 直接拒绝——见
     * lmfs.h 声明上方注释：不会把普通文件的内容误当目录项解析。 */
    if (dir_inode.type != LMFS_TYPE_DIR)
        return -1;

    static uint8_t block_buf[LMFS_BLOCK_SIZE];
    uint32_t count = 0;

    /* v1.0 范围：只走 8 个直接块，不处理 indirect1/indirect2——见
     * lmfs.h 声明上方注释里"日常目录用不到间接块"的说明。 */
    for (int i = 0; i < 8 && count < max_entries; i++) {
        uint32_t block_num = dir_inode.direct[i];
        if (block_num == LMFS_BLOCK_NONE)
            break;   /* 没有更多块了，正常结束，不是错误 */

        uint32_t block_lba = fs->mount_lba + fs->data_area_lba
                             + block_num * LMFS_SECTORS_PER_BLOCK;
        if (fs->dev->read(fs->dev, block_lba, LMFS_SECTORS_PER_BLOCK,
                           block_buf) != 0)
            return -1;

        for (uint32_t slot = 0;
             slot < LMFS_DIRENTS_PER_BLOCK && count < max_entries;
             slot++) {
            lmfs_dirent_t de;
            memcpy(&de, block_buf + slot * LMFS_DIRENT_SIZE,
                   sizeof(lmfs_dirent_t));

            if (!de.used)
                continue;   /* 空闲/已删除槽位，跳过——不是结束标志 */

            lmfs_inode_t target_inode;
            if (lmfs_read_inode(fs, de.inode_num, &target_inode) != 0)
                return -1;

            lmfs_dirlist_entry_t *out = &out_entries[count];
            memcpy(out->name, de.name, sizeof(out->name));
            out->name[LMFS_NAME_MAX] = '\0';  /* 保底：即便磁盘数据损坏
                                                  缺了结尾 NUL 也不会
                                                  越界读 */
            out->type      = target_inode.type;
            out->inode_num = de.inode_num;
            out->size      = target_inode.file_size;

            count++;
        }
    }

    *out_count = count;
    return 0;
}