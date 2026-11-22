/* kernel/fs/lmfs/lmfs.c
 * LumaOS 5.0 - LMFS Mount, Inode Read, Directory Listing & Allocators
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
 * DAY36 (第二月 · 第6周 · Day 36 of the same roadmap):
 *   "实现数据块 bitmap 分配器。" 验收："连续分配多个块，bitmap 正确变化。"
 *
 * DAY37 (Day 37 of the same roadmap):
 *   "实现 inode 分配器。" 验收："创建 inode 后返回合法 inode 号。"
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
 *
 * lmfs_alloc_block() 的位图扫描算法、为什么块号 0 不需要特殊判断也
 * 天然不会被分配出去，同样见 lmfs.h 里对应声明上方的详细注释。
 *
 * lmfs_alloc_inode() 跟 lmfs_alloc_block() 几乎是同一份扫描代码换了
 * 一套字段名，唯一的实质差异（为什么要多传一个 type 参数、为什么要
 * 顺带初始化 inode 表项而不是像数据块那样放着不管）见 lmfs.h 里
 * 对应声明上方的详细注释。
 */
#include "lmfs.h"
#include "../../lib/string.h"   /* memset()/memcpy() */

/* compute_data_bitmap_sectors(sb)
 *
 * 数据块位图大小 = ceil(data_block_count / 8) 字节（1 个 bit 对应
 * 1 个数据块），再 ceil 到整数个 512 字节扇区——跟 tools/mkfs.lmfs
 * 的 build_layout() 算 data_bitmap_sectors 用的是同一条公式，两边
 * 独立算出同一个值。DAY32 起 compute_inode_table_lba() 就在用这个
 * 公式（当时是内联算的），DAY36 的 lmfs_alloc_block() 也要用同一个
 * 数字来限定自己扫描位图的范围，所以把公式提出来共用一份，不再各自
 * 抄一遍——两处算的如果哪天悄悄改岔了，是那种只在边界情况才会露头
 * 的隐蔽 bug。
 */
static uint32_t compute_data_bitmap_sectors(const lmfs_superblock_t *sb)
{
    uint32_t bitmap_bytes = (sb->data_block_count + 7) / 8;
    return (bitmap_bytes + LMFS_SECTOR_SIZE - 1) / LMFS_SECTOR_SIZE;
}

/* compute_inode_bitmap_sectors(sb)
 *
 * 跟 compute_data_bitmap_sectors() 同一条公式，换成 inode_count——
 * DAY37 的 lmfs_alloc_inode() 要用它来限定自己扫描 inode 位图的
 * 范围，见该函数实现处的注释。 */
static uint32_t compute_inode_bitmap_sectors(const lmfs_superblock_t *sb)
{
    uint32_t bitmap_bytes = (sb->inode_count + 7) / 8;
    return (bitmap_bytes + LMFS_SECTOR_SIZE - 1) / LMFS_SECTOR_SIZE;
}

/* compute_inode_table_lba(sb)
 *
 * doc/lmfs.txt 第九节：inode 表紧接在数据块位图之后，起始位置 =
 * data_bitmap_lba + 数据块位图占用的扇区数。
 */
static uint32_t compute_inode_table_lba(const lmfs_superblock_t *sb)
{
    return sb->data_bitmap_lba + compute_data_bitmap_sectors(sb);
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

int lmfs_alloc_block(lmfs_fs_t *fs, uint32_t *out_block_num)
{
    if (!fs || !fs->dev || !out_block_num)
        return -1;

    /* 只读后端（dev->write == NULL，见 block.h 的文档注释）没法持久化
     * 分配结果——跟 fat_alloc_cluster() 开头的同一条检查是同一个
     * 理由：内存里标一下但写不回磁盘，等于分配了个假的，下次重新
     * 挂载又会被当成空闲，不如现在就直接拒绝。 */
    if (!fs->dev->write)
        return -1;

    uint32_t bitmap_sectors = compute_data_bitmap_sectors(&fs->sb);
    uint32_t data_block_count = fs->sb.data_block_count;

    static uint8_t sector_buf[LMFS_SECTOR_SIZE];

    for (uint32_t sector_idx = 0; sector_idx < bitmap_sectors; sector_idx++) {
        uint32_t sector_lba = fs->mount_lba + fs->sb.data_bitmap_lba
                              + sector_idx;
        if (fs->dev->read(fs->dev, sector_lba, 1, sector_buf) != 0)
            return -1;

        for (uint32_t byte_idx = 0; byte_idx < LMFS_SECTOR_SIZE; byte_idx++) {
            uint32_t byte_base = (sector_idx * LMFS_SECTOR_SIZE + byte_idx)
                                 * 8;
            if (byte_base >= data_block_count)
                break;   /* 位图这一字节之后全是补齐用的填充，不代表
                            任何真实块号，见 lmfs.h 声明上方的注释 */

            uint8_t byte = sector_buf[byte_idx];
            if (byte == 0xFF)
                continue;   /* 这 8 个块全部已占用，跳过整字节，不用
                               逐 bit 检查——位图通常大片连续已分配，
                               这一步能省下不少逐 bit 判断 */

            for (uint32_t bit = 0; bit < 8; bit++) {
                uint32_t block_num = byte_base + bit;
                if (block_num >= data_block_count)
                    break;

                /* 块号 0 永远不分配（doc/lmfs.txt 第十二节）——正常
                 * 情况下 mkfs.lmfs 早就把这一位标记成已占用，这里
                 * 这条判断走不到；保留它是防御性的最后一道防线，
                 * 万一 Day 41 的空间回收哪天有 bug 意外清掉了这一位，
                 * 分配器自己也不会把块号 0 发出去。 */
                if (block_num == LMFS_BLOCK_NONE)
                    continue;

                if (byte & (1u << bit))
                    continue;   /* 已占用 */

                /* 找到空闲块：置位，只写回这一个扇区（不是整张位图
                 * ——位图可能跨好几个扇区，改动只落在其中一个字节
                 * 时没必要把其余扇区也重写一遍），写盘失败就整体
                 * 判失败，不留"内存里标记了、磁盘上没写"的半成品
                 * 状态。 */
                sector_buf[byte_idx] = (uint8_t)(byte | (1u << bit));
                if (fs->dev->write(fs->dev, sector_lba, 1, sector_buf) != 0)
                    return -1;

                *out_block_num = block_num;
                return 0;
            }
        }
    }

    return -1;   /* 位图扫描完，一个空闲块都没有——卷已经写满 */
}

int lmfs_alloc_inode(lmfs_fs_t *fs, uint8_t type, uint32_t *out_inode_num)
{
    if (!fs || !fs->dev || !out_inode_num)
        return -1;
    if (type != LMFS_TYPE_FILE && type != LMFS_TYPE_DIR)
        return -1;   /* 只能分配出"文件"或"目录"，FREE 不是一个合法的
                        调用者可以指定的目标状态——见 lmfs.h 声明上方
                        注释 */
    if (!fs->dev->write)
        return -1;   /* 只读后端，理由跟 lmfs_alloc_block() 开头那条
                        检查一样 */

    uint32_t bitmap_sectors = compute_inode_bitmap_sectors(&fs->sb);
    uint32_t inode_count = fs->sb.inode_count;

    static uint8_t bitmap_sector_buf[LMFS_SECTOR_SIZE];

    for (uint32_t sector_idx = 0; sector_idx < bitmap_sectors; sector_idx++) {
        uint32_t bitmap_sector_lba = fs->mount_lba + fs->sb.inode_bitmap_lba
                                     + sector_idx;
        if (fs->dev->read(fs->dev, bitmap_sector_lba, 1,
                           bitmap_sector_buf) != 0)
            return -1;

        for (uint32_t byte_idx = 0; byte_idx < LMFS_SECTOR_SIZE; byte_idx++) {
            uint32_t byte_base = (sector_idx * LMFS_SECTOR_SIZE + byte_idx)
                                 * 8;
            if (byte_base >= inode_count)
                break;   /* 位图这一字节之后全是补齐用的填充——跟
                            lmfs_alloc_block() 同一条理由 */

            uint8_t byte = bitmap_sector_buf[byte_idx];
            if (byte == 0xFF)
                continue;   /* 这 8 个 inode 全部已分配，整字节跳过 */

            for (uint32_t bit = 0; bit < 8; bit++) {
                uint32_t inode_num = byte_base + bit;
                if (inode_num >= inode_count)
                    break;
                if (byte & (1u << bit))
                    continue;   /* 已分配（inode 0/根目录会在这里被
                                   跳过，mkfs.lmfs 早就把它的位标记
                                   成已占用了） */

                /* 找到空闲 inode 号。跟 lmfs_alloc_block() 的关键
                 * 差异就在这里：数据块分配完只置位、不碰块内容；
                 * inode 分配除了置位，还要顺带把 inode 表里这个槽位
                 * 的内容初始化成一个"刚分配、空的、给定 type"的合法
                 * inode——因为 Day 30 设计 type 字段时定下的约定是
                 * "type==FREE(0) 要跟位图里这一位是 0 保持一致"
                 * （doc/lmfs.txt、lmfs.h 里都写了这条不变式），如果
                 * 这里只置位、不写 inode 内容，位图说"已分配"而
                 * inode 自己的 type 还停在 FREE(0)，两处记录的"是否
                 * 已分配"就对不上了。见 lmfs.h 声明上方注释的完整
                 * 解释。 */
                uint32_t sector_in_table = inode_num
                                          / LMFS_INODES_PER_SECTOR;
                uint32_t offset_in_sector = (inode_num
                                             % LMFS_INODES_PER_SECTOR)
                                            * LMFS_INODE_SIZE;
                uint32_t inode_sector_lba = fs->mount_lba
                                            + fs->inode_table_lba
                                            + sector_in_table;

                static uint8_t inode_sector_buf[LMFS_SECTOR_SIZE];
                if (fs->dev->read(fs->dev, inode_sector_lba, 1,
                                   inode_sector_buf) != 0)
                    return -1;

                lmfs_inode_t fresh;
                memset(&fresh, 0, sizeof(fresh));
                fresh.type = type;   /* file_size/created_time/direct[]/
                                        indirect1/indirect2 全部保持
                                        memset 清零后的 0——"刚分配、
                                        还没有任何内容"的合法起始状态，
                                        跟 tools/mkfs.lmfs 里
                                        build_free_inode() 的空槽位、
                                        只是 type 不同 */
                memcpy(inode_sector_buf + offset_in_sector, &fresh,
                       sizeof(fresh));
                if (fs->dev->write(fs->dev, inode_sector_lba, 1,
                                    inode_sector_buf) != 0)
                    return -1;   /* 已知的未处理边界情况：位图这一位
                                    已经在上面读进 bitmap_sector_buf
                                    但还没写回磁盘（见下面的写回语句
                                    在这一步之后才执行），所以这里失败
                                    直接返回时磁盘上位图还是旧状态，
                                    不会出现"位图说已分配、inode 内容
                                    却没写成功"的半成品——但 inode 表
                                    这次写磁盘失败本身没有重试/回滚，
                                    下次挂载这个扇区的磁盘 I/O 问题
                                    (如果是持续性的，比如坏道）仍然
                                    会在，这个项目目前没有日志
                                    (journaling，doc/lmfs.txt 提到的
                                    v3.0 特性)，不打算在这里新增 */

                /* inode 内容写成功后，最后才把位图这一位置位并写回
                 * ——顺序是刻意的：先确保 inode 表的新内容已经落盘，
                 * 再宣布"这个号已经分配"，避免反过来的顺序下，万一
                 * inode 表那一步失败，却已经把位图标记成已分配，
                 * 制造出"位图说有、inode 内容其实是旧的/别的 inode
                 * 的"这种更难排查的不一致。 */
                bitmap_sector_buf[byte_idx] = (uint8_t)(byte | (1u << bit));
                if (fs->dev->write(fs->dev, bitmap_sector_lba, 1,
                                    bitmap_sector_buf) != 0)
                    return -1;

                *out_inode_num = inode_num;
                return 0;
            }
        }
    }

    return -1;   /* 位图扫描完，一个空闲 inode 都没有——inode 表已经用满 */
}