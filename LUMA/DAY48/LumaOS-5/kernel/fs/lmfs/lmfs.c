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
 * DAY42 (Day 42 of the same roadmap):
 *   "多级目录与路径解析。" 验收："可以创建并访问多级目录。"
 *   lmfs_create() 的实现被提成 create_node()（文件/目录共用），新增
 *   lmfs_mkdir()、lmfs_resolve_path()、lmfs_resolve_parent()——磁盘
 *   格式完全不变，设计说明见 lmfs.h 里 "DAY42" 那一大段注释和
 *   doc/lmfs.txt 第二十六节。
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

/* DAY41: forward declaration — free_inode_data() is defined further
 * down (near lmfs_free_block()/lmfs_free_inode()/lmfs_delete(), its
 * natural neighbors), but lmfs_write()'s DAY41 leak-fix (defined
 * earlier in this file, unchanged position from Day 39) needs to call
 * it. See free_inode_data()'s own definition for the full doc
 * comment. */
static int free_inode_data(lmfs_fs_t *fs, const lmfs_inode_t *inode);

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

/* write_inode_at(fs, inode_num, inode)
 *
 * 把 *inode 原样写回 inode 表里 inode_num 对应的那 64 字节槽位——
 * 先读出整个扇区、在内存里覆盖目标槽位、再把整个扇区写回去（LMFS_
 * INODES_PER_SECTOR=8 个 inode 共享一个扇区，不能只写这一个 inode
 * 的 64 字节，必须连同同一扇区里其余 7 个 inode 的内容一起原样
 * 写回）。DAY37 的 lmfs_alloc_inode() 最初是把这段逻辑内联写的，
 * DAY38 的 lmfs_create() 需要再写一次父目录 inode（更新它的
 * file_size/direct[]），所以提出来共用——避免两处定位算法
 * （LMFS_INODES_PER_SECTOR 的除法/取模）各抄一遍、以后改岔了却没
 * 发现。 */
static int write_inode_at(lmfs_fs_t *fs, uint32_t inode_num,
                           const lmfs_inode_t *inode)
{
    uint32_t sector_in_table = inode_num / LMFS_INODES_PER_SECTOR;
    uint32_t offset_in_sector = (inode_num % LMFS_INODES_PER_SECTOR)
                                * LMFS_INODE_SIZE;
    uint32_t sector_lba = fs->mount_lba + fs->inode_table_lba
                          + sector_in_table;

    static uint8_t sector_buf[LMFS_SECTOR_SIZE];
    if (fs->dev->read(fs->dev, sector_lba, 1, sector_buf) != 0)
        return -1;

    memcpy(sector_buf + offset_in_sector, inode, sizeof(*inode));

    return fs->dev->write(fs->dev, sector_lba, 1, sector_buf);
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
                lmfs_inode_t fresh;
                memset(&fresh, 0, sizeof(fresh));
                fresh.type = type;   /* file_size/created_time/direct[]/
                                        indirect1/indirect2 全部保持
                                        memset 清零后的 0——"刚分配、
                                        还没有任何内容"的合法起始状态，
                                        跟 tools/mkfs.lmfs 里
                                        build_free_inode() 的空槽位、
                                        只是 type 不同 */
                if (write_inode_at(fs, inode_num, &fresh) != 0)
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

int lmfs_lookup(lmfs_fs_t *fs, uint32_t parent_inode_num, const char *name,
                 uint32_t *out_inode_num)
{
    if (!fs || !fs->dev || !name || !out_inode_num)
        return -1;

    lmfs_inode_t parent;
    if (lmfs_read_inode(fs, parent_inode_num, &parent) != 0)
        return -1;
    if (parent.type != LMFS_TYPE_DIR)
        return -1;

    uint32_t name_len = strlen(name);

    static uint8_t block_buf[LMFS_BLOCK_SIZE];

    for (int i = 0; i < 8; i++) {
        uint32_t block_num = parent.direct[i];
        if (block_num == LMFS_BLOCK_NONE)
            break;

        uint32_t block_lba = fs->mount_lba + fs->data_area_lba
                             + block_num * LMFS_SECTORS_PER_BLOCK;
        if (fs->dev->read(fs->dev, block_lba, LMFS_SECTORS_PER_BLOCK,
                           block_buf) != 0)
            return -1;

        for (uint32_t slot = 0; slot < LMFS_DIRENTS_PER_BLOCK; slot++) {
            lmfs_dirent_t de;
            memcpy(&de, block_buf + slot * LMFS_DIRENT_SIZE, sizeof(de));

            if (de.used && de.name_len == name_len &&
                memcmp(de.name, name, name_len) == 0) {
                *out_inode_num = de.inode_num;
                return 0;
            }
        }
    }

    return -1;   /* 没有任何一条目录项匹配这个名字 */
}

/* create_node(fs, parent_inode_num, name, type, out_inode_num)
 *
 * DAY42: 原来的 lmfs_create() 整个函数体——"校验 -> 分配 inode ->
 * 把 (名字 -> inode) 挂进父目录"——被提成这个静态函数，多了一个
 * `type` 参数（LMFS_TYPE_FILE 或 LMFS_TYPE_DIR）。lmfs_create() 传
 * FILE，lmfs_mkdir() 传 DIR，两者的校验、重名检查、空槽位/新块两条
 * 挂接路径【完全共用】，不是复制粘贴出第二份；唯一的差别是 type==DIR
 * 时，在"分配 inode"之后、"挂进父目录"之前，多一步"给新目录写出
 * 它自己的 '.'/'..' 内容块"（见下方带 DAY42 标记的那一段）。
 * `out_inode_num` 可以为 NULL（调用者不关心新 inode 号）。 */
static int create_node(lmfs_fs_t *fs, uint32_t parent_inode_num,
                       const char *name, uint8_t type,
                       uint32_t *out_inode_num)
{
    if (!fs || !fs->dev || !name)
        return -1;
    if (type != LMFS_TYPE_FILE && type != LMFS_TYPE_DIR)
        return -1;
    if (!fs->dev->write)
        return -1;

    uint32_t name_len = strlen(name);
    if (name_len == 0 || name_len > LMFS_NAME_MAX)
        return -1;
    if (strchr(name, '/') != (char *)0)
        return -1;   /* 单一路径分量——多级路径由调用者先用
                        lmfs_resolve_parent() 拆成"父目录 + 最后一段" */
    /* DAY42: "." 和 ".." 是 mkdir 自动生成的保留名，不能被用户创建
     * ——否则同一个目录里会出现两条 "." 目录项，lmfs_lookup() 找到
     * 哪一条取决于槽位顺序。 */
    if ((name_len == 1 && name[0] == '.') ||
        (name_len == 2 && name[0] == '.' && name[1] == '.'))
        return -1;

    lmfs_inode_t parent;
    if (lmfs_read_inode(fs, parent_inode_num, &parent) != 0)
        return -1;
    if (parent.type != LMFS_TYPE_DIR)
        return -1;

    /* ---- 第一遍扫描：只读，找重名 + 记录第一个空闲槽位 ----
     * 必须扫完父目录现有的全部块（而不是找到第一个空闲槽位就提前
     * 退出），因为空闲槽位可能在前面的块、重名的条目却在后面的块
     * 里——两件事互不影响，缺一个都会让后面的判断出错。 */
    int free_slot_found = 0;
    uint32_t free_slot_block_num = 0;
    uint32_t free_slot_index_in_block = 0;   /* 0..15，块内第几个槽位 */
    int free_direct_idx = -1;   /* 第一个值是 LMFS_BLOCK_NONE 的
                                    direct[] 下标，没有新分配块的
                                    需求时不会用到 */

    static uint8_t block_buf[LMFS_BLOCK_SIZE];

    for (int i = 0; i < 8; i++) {
        uint32_t block_num = parent.direct[i];
        if (block_num == LMFS_BLOCK_NONE) {
            if (free_direct_idx < 0)
                free_direct_idx = i;   /* 记下第一个空指针槽位，万一
                                           后面需要新分配一个块时用 */
            break;   /* direct[] 里从第一个 NONE 开始往后必然全是
                        NONE——跟 lmfs_readdir() 的扫描约定一致 */
        }

        uint32_t block_lba = fs->mount_lba + fs->data_area_lba
                             + block_num * LMFS_SECTORS_PER_BLOCK;
        if (fs->dev->read(fs->dev, block_lba, LMFS_SECTORS_PER_BLOCK,
                           block_buf) != 0)
            return -1;

        for (uint32_t slot = 0; slot < LMFS_DIRENTS_PER_BLOCK; slot++) {
            lmfs_dirent_t de;
            memcpy(&de, block_buf + slot * LMFS_DIRENT_SIZE, sizeof(de));

            if (de.used) {
                if (de.name_len == name_len &&
                    memcmp(de.name, name, name_len) == 0)
                    return -1;   /* 重名，直接拒绝，不做任何分配 */
            } else if (!free_slot_found) {
                free_slot_found = 1;
                free_slot_block_num = block_num;
                free_slot_index_in_block = slot;
            }
        }
    }

    if (!free_slot_found && free_direct_idx < 0)
        return -1;   /* 父目录已经彻底满了：8 个块全部用满，没有
                        空闲槽位，也没有空的 direct[] 指针槽位可以
                        再分配新块——doc/lmfs.txt 第十二节提到的
                        128 个目录项上限 */

    /* ---- 校验全部通过，开始真正改动。先分配 inode——两条后续路径
     * （写进已有槽位 / 新分配一个块）都需要这个号码，所以提前拿到；
     * 这一步失败的话，前面纯只读的扫描还没产生任何副作用。 ---- */
    uint32_t new_inode_num;
    if (lmfs_alloc_inode(fs, type, &new_inode_num) != 0)
        return -1;

    /* ---- DAY42: 新建的是目录——先让它"有内容"，再让它"可见" ----
     * 一个空白的 DIR inode（file_size=0、没有数据块）不是合法目录：
     * lmfs_readdir()/lmfs_lookup() 看到 direct[0]==NONE 就直接结束
     * 扫描，连 "." 和 ".." 都找不到，`cd` 进去后 `cd ..` 无路可退。
     * 所以这里分配 1 个数据块，写入两条目录项：
     *     slot 0: "."  -> 新目录自己
     *     slot 1: ".." -> 父目录
     * 再把 inode 的 file_size=1024、direct[0]=该块 写回。
     * 写盘顺序沿用"内容先、可见性后"：这一整段全部成功之后，下面才
     * 把 (name -> new_inode_num) 登记进父目录。任何一步失败，尽力把
     * 刚分配的块/inode 还回去（best-effort——这个项目没有事务，释放
     * 本身再失败就只能泄漏，跟 Day 41 的已知限制同一档）。 */
    if (type == LMFS_TYPE_DIR) {
        uint32_t dir_block;
        if (lmfs_alloc_block(fs, &dir_block) != 0) {
            lmfs_free_inode(fs, new_inode_num);
            return -1;
        }

        static uint8_t dir_block_buf[LMFS_BLOCK_SIZE];
        memset(dir_block_buf, 0, sizeof(dir_block_buf));

        lmfs_dirent_t dot, dotdot;
        memset(&dot, 0, sizeof(dot));
        dot.inode_num = new_inode_num;
        dot.used = 1;
        dot.name_len = 1;
        dot.name[0] = '.';
        memset(&dotdot, 0, sizeof(dotdot));
        dotdot.inode_num = parent_inode_num;
        dotdot.used = 1;
        dotdot.name_len = 2;
        dotdot.name[0] = '.';
        dotdot.name[1] = '.';
        memcpy(dir_block_buf, &dot, sizeof(dot));
        memcpy(dir_block_buf + LMFS_DIRENT_SIZE, &dotdot, sizeof(dotdot));

        uint32_t dir_block_lba = fs->mount_lba + fs->data_area_lba
                                 + dir_block * LMFS_SECTORS_PER_BLOCK;
        lmfs_inode_t dir_inode;
        memset(&dir_inode, 0, sizeof(dir_inode));
        dir_inode.type = LMFS_TYPE_DIR;
        dir_inode.file_size = LMFS_BLOCK_SIZE;   /* 目录的 file_size =
                                                    已分配块数 × 块大小，
                                                    见 doc 第十二节 */
        dir_inode.direct[0] = dir_block;

        if (fs->dev->write(fs->dev, dir_block_lba, LMFS_SECTORS_PER_BLOCK,
                            dir_block_buf) != 0 ||
            write_inode_at(fs, new_inode_num, &dir_inode) != 0) {
            lmfs_free_block(fs, dir_block);
            lmfs_free_inode(fs, new_inode_num);
            return -1;
        }
    }

    lmfs_dirent_t new_dirent;
    memset(&new_dirent, 0, sizeof(new_dirent));
    new_dirent.inode_num = new_inode_num;
    new_dirent.used = 1;
    new_dirent.name_len = (uint8_t)name_len;
    memcpy(new_dirent.name, name, name_len);   /* 剩余字节已经被上面
                                                   的 memset 清零，天然
                                                   就是 NUL 结尾 */

    if (free_slot_found) {
        /* 路径 A：已有块里有空闲槽位——只需要读写槽位所在的那一个
         * 扇区（LMFS_DIRENTS_PER_SECTOR=8 个槽位共享 1 个扇区），
         * 不用碰整个块，更不用动父目录的 inode。 */
        uint32_t sector_in_block = free_slot_index_in_block
                                   / LMFS_DIRENTS_PER_SECTOR;
        uint32_t offset_in_sector = (free_slot_index_in_block
                                     % LMFS_DIRENTS_PER_SECTOR)
                                    * LMFS_DIRENT_SIZE;
        uint32_t sector_lba = fs->mount_lba + fs->data_area_lba
                              + free_slot_block_num * LMFS_SECTORS_PER_BLOCK
                              + sector_in_block;

        static uint8_t sector_buf[LMFS_SECTOR_SIZE];
        if (fs->dev->read(fs->dev, sector_lba, 1, sector_buf) != 0)
            return -1;
        memcpy(sector_buf + offset_in_sector, &new_dirent,
               sizeof(new_dirent));
        if (fs->dev->write(fs->dev, sector_lba, 1, sector_buf) != 0)
            return -1;
    } else {
        /* 路径 B：现有块全满，父目录自己也还有空的 direct[] 指针
         * 槽位——先分配一个新块，把新 dirent 放进这个新块的第 0 个
         * 槽位，其余 15 个槽位保持全零（used=0，空闲）；块内容先
         * 写成功，最后才更新父目录 inode 的 direct[]/file_size 并
         * 写回——这个顺序的理由跟 lmfs_alloc_inode() 的"先 inode
         * 内容、后位图"是同一个道理：避免父目录已经指向一个块、但
         * 那个块还没真正写出正确内容的半成品状态。 */
        uint32_t new_block_num;
        if (lmfs_alloc_block(fs, &new_block_num) != 0)
            return -1;

        memset(block_buf, 0, sizeof(block_buf));
        memcpy(block_buf, &new_dirent, sizeof(new_dirent));

        uint32_t new_block_lba = fs->mount_lba + fs->data_area_lba
                                 + new_block_num * LMFS_SECTORS_PER_BLOCK;
        if (fs->dev->write(fs->dev, new_block_lba, LMFS_SECTORS_PER_BLOCK,
                            block_buf) != 0)
            return -1;

        parent.direct[free_direct_idx] = new_block_num;
        parent.file_size += LMFS_BLOCK_SIZE;
        if (write_inode_at(fs, parent_inode_num, &parent) != 0)
            return -1;
    }

    if (out_inode_num)
        *out_inode_num = new_inode_num;
    return 0;
}

int lmfs_create(lmfs_fs_t *fs, uint32_t parent_inode_num, const char *name,
                 uint32_t *out_inode_num)
{
    if (!out_inode_num)
        return -1;
    return create_node(fs, parent_inode_num, name, LMFS_TYPE_FILE,
                       out_inode_num);
}

int lmfs_mkdir(lmfs_fs_t *fs, uint32_t parent_inode_num, const char *name,
               uint32_t *out_inode_num)
{
    return create_node(fs, parent_inode_num, name, LMFS_TYPE_DIR,
                       out_inode_num);
}

int lmfs_resolve_path(lmfs_fs_t *fs, uint32_t start_inode_num,
                      const char *path, uint32_t *out_inode_num)
{
    if (!fs || !fs->dev || !path || !out_inode_num)
        return -1;

    uint32_t cur = start_inode_num;
    if (path[0] == '/')
        cur = LMFS_ROOT_INODE_NUM;   /* 绝对路径：忽略起点，从根开始 */

    /* 起点本身必须是目录（空路径时也要校验——"返回 start 本身"并不
     * 意味着 start 一定存在/合法）。 */
    lmfs_inode_t ino;
    if (lmfs_read_inode(fs, cur, &ino) != 0)
        return -1;

    const char *p = path;
    while (*p != '\0') {
        while (*p == '/')
            p++;                      /* 连续 '/' 与结尾 '/' 一并吞掉 */
        if (*p == '\0')
            break;

        char comp[LMFS_NAME_MAX + 1];
        uint32_t n = 0;
        while (*p != '\0' && *p != '/') {
            if (n >= LMFS_NAME_MAX)
                return -1;            /* 单段超长，磁盘上不可能有这个名字 */
            comp[n++] = *p++;
        }
        comp[n] = '\0';

        /* "."/".." 就是两个普通名字——磁盘上目录里真有这两条目录项，
         * lmfs_lookup() 找得到；lookup 内部会拒绝"在非目录里找"，
         * 所以 "file.txt/x" 这种路径在这里自然失败。 */
        uint32_t next;
        if (lmfs_lookup(fs, cur, comp, &next) != 0)
            return -1;
        cur = next;
    }

    *out_inode_num = cur;
    return 0;
}

int lmfs_resolve_parent(lmfs_fs_t *fs, uint32_t start_inode_num,
                        const char *path, uint32_t *out_parent_inode,
                        char out_name[LMFS_NAME_MAX + 1])
{
    if (!fs || !fs->dev || !path || !out_parent_inode || !out_name)
        return -1;

    /* 拷进一个本地缓冲区，原地去掉结尾的 '/'，方便找"最后一段"。 */
    char buf[128];
    uint32_t len = strlen(path);
    if (len >= sizeof(buf))
        return -1;
    memcpy(buf, path, len + 1);
    while (len > 0 && buf[len - 1] == '/')
        buf[--len] = '\0';
    if (len == 0)
        return -1;                    /* "" 或 "/"：没有"最后一段" */

    int last_slash = -1;
    for (uint32_t i = 0; i < len; i++)
        if (buf[i] == '/')
            last_slash = (int)i;

    const char *name_part = buf + last_slash + 1;
    uint32_t name_len = strlen(name_part);
    if (name_len == 0 || name_len > LMFS_NAME_MAX)
        return -1;

    uint32_t parent;
    if (last_slash < 0) {
        parent = start_inode_num;     /* "c.txt"：父目录就是起点 */
    } else {
        /* 目录部分 = buf[0..last_slash)；"/c.txt" 时 last_slash==0，
         * 目录部分为空但原路径以 '/' 开头，应当解析成根目录。 */
        buf[last_slash] = '\0';
        const char *dir_part = (last_slash == 0) ? "/" : buf;
        if (lmfs_resolve_path(fs, start_inode_num, dir_part, &parent) != 0)
            return -1;
    }

    lmfs_inode_t pi;
    if (lmfs_read_inode(fs, parent, &pi) != 0 || pi.type != LMFS_TYPE_DIR)
        return -1;

    memcpy(out_name, name_part, name_len + 1);
    *out_parent_inode = parent;
    return 0;
}

int lmfs_write(lmfs_fs_t *fs, uint32_t inode_num, const void *buf,
               uint32_t size)
{
    if (!fs || !fs->dev || (size > 0 && !buf))
        return -1;
    if (!fs->dev->write)
        return -1;
    if (size > LMFS_MAX_FILE_SIZE)
        return -1;

    lmfs_inode_t inode;
    if (lmfs_read_inode(fs, inode_num, &inode) != 0)
        return -1;
    if (inode.type != LMFS_TYPE_FILE)
        return -1;

    /* DAY41 更新：开始分配任何新块之前，先把这个 inode 原来占用的
     * 全部数据块释放干净（如果本来就是空文件，direct[]/indirect1
     * 全部是 LMFS_BLOCK_NONE，free_inode_data() 是个no-op，不会
     * 白白报错）——Day 39 发布时这里没有这一步，旧内容会泄漏，见
     * lmfs.h 声明上方"DAY41 UPDATE"那段注释里补记的历史。 */
    if (free_inode_data(fs, &inode) != 0)
        return -1;

    /* 新内容从一张白纸开始累积——先在内存里攒好 direct[]/indirect1，
     * 等全部新内容都确认写盘成功，最后一步才整个覆盖过去。 */
    lmfs_inode_t new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.type = LMFS_TYPE_FILE;
    new_inode.file_size = size;

    uint32_t total_blocks = (size + LMFS_BLOCK_SIZE - 1) / LMFS_BLOCK_SIZE;

    static uint8_t data_block_buf[LMFS_BLOCK_SIZE];
    /* 一级间接块的索引表——256 个 uint32_t，正好是 1 个块的大小，
     * 写盘时直接把这块内存整体当数据块内容用，不需要额外转换。 */
    static uint32_t indirect_table[LMFS_INDIRECT1_PTRS];
    uint32_t indirect_block_num = LMFS_BLOCK_NONE;
    uint32_t indirect_used = 0;   /* 索引表里已经填了几个指针 */

    const uint8_t *src = (const uint8_t *)buf;

    for (uint32_t i = 0; i < total_blocks; i++) {
        uint32_t new_block_num;
        if (lmfs_alloc_block(fs, &new_block_num) != 0)
            return -1;

        uint32_t offset = i * LMFS_BLOCK_SIZE;
        uint32_t chunk = size - offset;
        if (chunk > LMFS_BLOCK_SIZE)
            chunk = LMFS_BLOCK_SIZE;

        memcpy(data_block_buf, src + offset, chunk);
        if (chunk < LMFS_BLOCK_SIZE)
            memset(data_block_buf + chunk, 0, LMFS_BLOCK_SIZE - chunk);
            /* 最后一个块，文件实际内容填不满整块——剩余部分清零，
               不留上一个使用者（或者 mkfs 时期）残留的垃圾字节，
               跟 lmfs_alloc_block() 自己"不保证新块内容是什么"的
               文档说明互相配合：这里由调用者（本函数）负责清零。 */

        uint32_t block_lba = fs->mount_lba + fs->data_area_lba
                             + new_block_num * LMFS_SECTORS_PER_BLOCK;
        if (fs->dev->write(fs->dev, block_lba, LMFS_SECTORS_PER_BLOCK,
                            data_block_buf) != 0)
            return -1;

        if (i < 8) {
            new_inode.direct[i] = new_block_num;
        } else {
            if (indirect_block_num == LMFS_BLOCK_NONE) {
                /* 第一次需要用到间接块——现在才分配索引表自己的块，
                   不提前分配（文件如果恰好 <= 8 个块，根本用不上
                   间接块，不该白白分配一个）。 */
                if (lmfs_alloc_block(fs, &indirect_block_num) != 0)
                    return -1;
                memset(indirect_table, 0, sizeof(indirect_table));
            }
            indirect_table[indirect_used] = new_block_num;
            indirect_used++;
        }
    }

    if (indirect_block_num != LMFS_BLOCK_NONE) {
        /* 索引表内容现在才真正写盘——必须等所有它指向的数据块都已经
           写成功之后才写索引表，不然会出现"索引表已经指向某个块、
           那个块其实还没写内容"的半成品状态，见 lmfs.h 声明上方
           注释的"块分配/写入顺序"说明。 */
        uint32_t indirect_lba = fs->mount_lba + fs->data_area_lba
                                + indirect_block_num * LMFS_SECTORS_PER_BLOCK;
        if (fs->dev->write(fs->dev, indirect_lba, LMFS_SECTORS_PER_BLOCK,
                            indirect_table) != 0)
            return -1;
        new_inode.indirect1 = indirect_block_num;
    }

    /* 全部数据块、索引块都已经确认写盘成功——最后一步才把 inode
       改成指向这些新内容，见 lmfs.h 声明上方注释。 */
    if (write_inode_at(fs, inode_num, &new_inode) != 0)
        return -1;

    return 0;
}

int lmfs_read(lmfs_fs_t *fs, uint32_t inode_num, void *buf, uint32_t size,
              uint32_t offset)
{
    if (!fs || !fs->dev || (size > 0 && !buf))
        return -1;

    lmfs_inode_t inode;
    if (lmfs_read_inode(fs, inode_num, &inode) != 0)
        return -1;
    if (inode.type != LMFS_TYPE_FILE)
        return -1;

    if (offset >= inode.file_size)
        return 0;   /* EOF，不是错误——见 lmfs.h 声明上方注释 */

    uint32_t to_read = inode.file_size - offset;
    if (to_read > size)
        to_read = size;
    if (to_read == 0)
        return 0;

    static uint8_t block_buf[LMFS_BLOCK_SIZE];
    static uint32_t indirect_table[LMFS_INDIRECT1_PTRS];
    int indirect_loaded = 0;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t remaining = to_read;
    uint32_t cur_offset = offset;

    while (remaining > 0) {
        uint32_t block_index = cur_offset / LMFS_BLOCK_SIZE;
        uint32_t offset_in_block = cur_offset % LMFS_BLOCK_SIZE;

        uint32_t block_num;
        if (block_index < 8) {
            block_num = inode.direct[block_index];
        } else {
            /* 第一次跨进间接块覆盖的范围才去读索引表，只读一次、
             * 缓存复用——不会每跨一个块就重新读一遍。 */
            if (!indirect_loaded) {
                if (inode.indirect1 == LMFS_BLOCK_NONE)
                    return -1;   /* file_size 声称有更多数据，但 inode
                                    没有间接块——数据不一致 */
                uint32_t indirect_lba = fs->mount_lba + fs->data_area_lba
                    + inode.indirect1 * LMFS_SECTORS_PER_BLOCK;
                if (fs->dev->read(fs->dev, indirect_lba,
                                   LMFS_SECTORS_PER_BLOCK,
                                   indirect_table) != 0)
                    return -1;
                indirect_loaded = 1;
            }
            block_num = indirect_table[block_index - 8];
        }
        if (block_num == LMFS_BLOCK_NONE)
            return -1;   /* 链条在不该断的地方断了 */

        uint32_t block_lba = fs->mount_lba + fs->data_area_lba
                             + block_num * LMFS_SECTORS_PER_BLOCK;
        if (fs->dev->read(fs->dev, block_lba, LMFS_SECTORS_PER_BLOCK,
                           block_buf) != 0)
            return -1;

        uint32_t chunk = LMFS_BLOCK_SIZE - offset_in_block;
        if (chunk > remaining)
            chunk = remaining;

        memcpy(dst, block_buf + offset_in_block, chunk);

        dst += chunk;
        cur_offset += chunk;
        remaining -= chunk;
    }

    return (int)to_read;
}

int lmfs_free_block(lmfs_fs_t *fs, uint32_t block_num)
{
    if (!fs || !fs->dev)
        return -1;
    if (!fs->dev->write)
        return -1;
    if (block_num == LMFS_BLOCK_NONE)
        return -1;   /* 永久保留的哨兵值，从来不是一次真正的分配 */
    if (block_num >= fs->sb.data_block_count)
        return -1;

    uint32_t global_byte = block_num / 8;
    uint32_t bit = block_num % 8;
    uint32_t sector_idx = global_byte / LMFS_SECTOR_SIZE;
    uint32_t byte_in_sector = global_byte % LMFS_SECTOR_SIZE;
    uint32_t sector_lba = fs->mount_lba + fs->sb.data_bitmap_lba
                          + sector_idx;

    static uint8_t sector_buf[LMFS_SECTOR_SIZE];
    if (fs->dev->read(fs->dev, sector_lba, 1, sector_buf) != 0)
        return -1;

    sector_buf[byte_in_sector] &= (uint8_t)~(1u << bit);

    return fs->dev->write(fs->dev, sector_lba, 1, sector_buf);
}

int lmfs_free_inode(lmfs_fs_t *fs, uint32_t inode_num)
{
    if (!fs || !fs->dev)
        return -1;
    if (!fs->dev->write)
        return -1;
    if (inode_num == LMFS_ROOT_INODE_NUM)
        return -1;   /* 根目录永远不会被释放 */
    if (inode_num >= fs->sb.inode_count)
        return -1;

    /* 内容先、位图后——跟 lmfs_alloc_inode() 同一条"永远先碰内容，
     * 再碰位图"的规则，分配、释放两个方向都遵守，不用记两条不同的
     * 顺序规则。 */
    lmfs_inode_t blank;
    memset(&blank, 0, sizeof(blank));   /* type 留 0 = LMFS_TYPE_FREE */
    if (write_inode_at(fs, inode_num, &blank) != 0)
        return -1;

    uint32_t global_byte = inode_num / 8;
    uint32_t bit = inode_num % 8;
    uint32_t sector_idx = global_byte / LMFS_SECTOR_SIZE;
    uint32_t byte_in_sector = global_byte % LMFS_SECTOR_SIZE;
    uint32_t sector_lba = fs->mount_lba + fs->sb.inode_bitmap_lba
                          + sector_idx;

    static uint8_t sector_buf[LMFS_SECTOR_SIZE];
    if (fs->dev->read(fs->dev, sector_lba, 1, sector_buf) != 0)
        return -1;

    sector_buf[byte_in_sector] &= (uint8_t)~(1u << bit);

    return fs->dev->write(fs->dev, sector_lba, 1, sector_buf);
}

/* free_inode_data(fs, inode)
 *
 * 共享助手：释放 `inode` 当前指向的全部数据块（direct[8] + indirect1
 * 索引表里的每一项 + indirect1 索引块自己），不释放 inode 本身——
 * lmfs_delete()（释放一个即将被删除的文件）和 lmfs_write()（Day 41
 * 更新：覆盖已有内容前先释放旧块，修掉 Day 39 文档里记录过的那个
 * 已知泄漏）两处都需要同一套"把这个 inode 现在指向的块全部还给
 * 位图"的逻辑，不该各写一份——跟 fat_free_cluster_chain_by_variant()
 * 被 fat_delete_file()/fat_truncate()/fat_write() 三处共用是同一个
 * "不重复撞车同一套释放循环"的理由。
 *
 * 必须在"清掉指向这个 inode 的目录项"之后才调用——见 lmfs_delete()
 * 声明上方注释里完整的顺序论证；本函数自己不关心调用者有没有先清
 * 目录项，这是调用者的责任，不是本函数的职责范围。
 */
static int free_inode_data(lmfs_fs_t *fs, const lmfs_inode_t *inode)
{
    for (int i = 0; i < 8; i++) {
        if (inode->direct[i] == LMFS_BLOCK_NONE)
            continue;
        if (lmfs_free_block(fs, inode->direct[i]) != 0)
            return -1;
    }

    if (inode->indirect1 != LMFS_BLOCK_NONE) {
        static uint32_t indirect_table[LMFS_INDIRECT1_PTRS];
        uint32_t indirect_lba = fs->mount_lba + fs->data_area_lba
            + inode->indirect1 * LMFS_SECTORS_PER_BLOCK;
        if (fs->dev->read(fs->dev, indirect_lba, LMFS_SECTORS_PER_BLOCK,
                           indirect_table) != 0)
            return -1;

        for (uint32_t i = 0; i < LMFS_INDIRECT1_PTRS; i++) {
            if (indirect_table[i] == LMFS_BLOCK_NONE)
                continue;
            if (lmfs_free_block(fs, indirect_table[i]) != 0)
                return -1;
        }

        /* 索引块自己也是一个数据块，释放数据之后，索引块本身也要
         * 还给位图——不然索引表占用的那个块会永久泄漏。 */
        if (lmfs_free_block(fs, inode->indirect1) != 0)
            return -1;
    }

    return 0;
}

int lmfs_delete(lmfs_fs_t *fs, uint32_t parent_inode_num, const char *name)
{
    if (!fs || !fs->dev || !name)
        return -1;
    if (!fs->dev->write)
        return -1;

    lmfs_inode_t parent;
    if (lmfs_read_inode(fs, parent_inode_num, &parent) != 0)
        return -1;
    if (parent.type != LMFS_TYPE_DIR)
        return -1;

    uint32_t name_len = strlen(name);

    static uint8_t block_buf[LMFS_BLOCK_SIZE];

    uint32_t found_block_num = 0;
    uint32_t found_slot = 0;
    uint32_t found_inode_num = 0;
    int found = 0;

    for (int i = 0; i < 8 && !found; i++) {
        uint32_t block_num = parent.direct[i];
        if (block_num == LMFS_BLOCK_NONE)
            break;

        uint32_t block_lba = fs->mount_lba + fs->data_area_lba
                             + block_num * LMFS_SECTORS_PER_BLOCK;
        if (fs->dev->read(fs->dev, block_lba, LMFS_SECTORS_PER_BLOCK,
                           block_buf) != 0)
            return -1;

        for (uint32_t slot = 0; slot < LMFS_DIRENTS_PER_BLOCK; slot++) {
            lmfs_dirent_t de;
            memcpy(&de, block_buf + slot * LMFS_DIRENT_SIZE, sizeof(de));

            if (de.used && de.name_len == name_len &&
                memcmp(de.name, name, name_len) == 0) {
                found_block_num = block_num;
                found_slot = slot;
                found_inode_num = de.inode_num;
                found = 1;
                break;
            }
        }
    }

    if (!found)
        return -1;   /* 没有这个名字的文件 */

    lmfs_inode_t target;
    if (lmfs_read_inode(fs, found_inode_num, &target) != 0)
        return -1;
    if (target.type != LMFS_TYPE_FILE)
        return -1;   /* 目录（比如根目录自己的 "."/".."）不允许删除，
                        见 lmfs_delete() 声明上方注释 */

    /* ---- 第 1 步：先清目录项，`rm` 的用户可见效果立刻生效 ----
     * 顺序的完整论证见 lmfs_delete() 声明上方注释。 */
    {
        uint32_t sector_in_block = found_slot / LMFS_DIRENTS_PER_SECTOR;
        uint32_t offset_in_sector = (found_slot % LMFS_DIRENTS_PER_SECTOR)
                                    * LMFS_DIRENT_SIZE;
        uint32_t sector_lba = fs->mount_lba + fs->data_area_lba
                              + found_block_num * LMFS_SECTORS_PER_BLOCK
                              + sector_in_block;

        static uint8_t sector_buf[LMFS_SECTOR_SIZE];
        if (fs->dev->read(fs->dev, sector_lba, 1, sector_buf) != 0)
            return -1;

        lmfs_dirent_t cleared;
        memset(&cleared, 0, sizeof(cleared));   /* used=0 */
        memcpy(sector_buf + offset_in_sector, &cleared, sizeof(cleared));

        if (fs->dev->write(fs->dev, sector_lba, 1, sector_buf) != 0)
            return -1;
    }

    /* ---- 第 2 步：释放数据块 ---- */
    if (free_inode_data(fs, &target) != 0)
        return -1;

    /* ---- 第 3 步：释放 inode 本身 ---- */
    if (lmfs_free_inode(fs, found_inode_num) != 0)
        return -1;

    return 0;
}