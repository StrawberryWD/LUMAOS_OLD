/* kernel/fs/lmfs/lmfs.c
 * LumaOS 5.0 - LMFS Mount
 *
 * DAY32 (第二月 · 第5周 · Day 32 of the 5.0 filesystem roadmap):
 *   "C 端实现 lmfs_mount(dev)。"
 *   验收："能读取超级块并验证版本/容量。"
 *
 * 见 lmfs.h 顶部注释：这是 fat12_mount()（kernel/fs/fat/fat12.c，
 * Day 10）在 LMFS 这边的对应实现，读盘方式、成功/失败的填充/清零
 * 约定都直接照搬那份已经跑通的代码，只是超级块的位置、校验的字段
 * 不同——具体校验哪些字段、为什么，见 lmfs.h 里 lmfs_mount() 声明
 * 上方那段详细注释，这里的实现严格照那段注释的四条校验顺序写。
 */
#include "lmfs.h"
#include "../../lib/string.h"   /* memset() */

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
     * 重新计算精确的区域大小（那需要 Day 30 的 inode 大小信息，
     * lmfs_mount() 本身不关心），只是一个"数值顺序合理"的粗粒度
     * 自检——真损坏的超级块（比如被写坏、或者压根不是 mkfs.lmfs
     * 格式化出来的卷）大概率会在这一步就露出破绽，而不用等到 Day 33
     * 读 inode 表时才用一堆垃圾地址崩掉。 ---- */
    if (sb.inode_bitmap_lba <= LMFS_SUPERBLOCK_LBA)
        return -1;
    if (sb.data_bitmap_lba <= sb.inode_bitmap_lba)
        return -1;

    /* 全部通过：填好 out_fs 并返回成功。 */
    out_fs->dev        = dev;
    out_fs->mount_lba  = lba;
    out_fs->sb         = sb;

    return 0;
}