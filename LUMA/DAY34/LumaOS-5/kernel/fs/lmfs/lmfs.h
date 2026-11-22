/* kernel/fs/lmfs/lmfs.h
 * LumaOS 5.0 - LMFS On-Disk Structures & Mount
 *
 * DAY32 (第二月 · 第5周 · Day 32 of the 5.0 filesystem roadmap):
 *   "C 端实现 lmfs_mount(dev)。"
 *   验收："能读取超级块并验证版本/容量。"
 *
 *   Day 29/30 只产出了 doc/lmfs.txt 这份设计文档；今天是 LMFS 第一次
 *   在内核里落成真正的 C 代码。跟 kernel/fs/fat/fat12.h（Day 9）的
 *   角色完全对应：那份头文件把 FAT12 的磁盘结构（BPB、目录项）定义
 *   成 C 结构体，这份头文件把 doc/lmfs.txt 第二/七节已经设计好的
 *   lmfs_superblock_t / lmfs_inode_t 同样定义成 C 结构体——字段名、
 *   字节偏移、大小全部照抄文档，任何一处不一致都以 doc/lmfs.txt 为准
 *   去改这里，而不是反过来。
 *
 *   lmfs_inode_t 在 Day 32 先把结构体定义出来，Day 33 的
 *   lmfs_read_inode()（本文件下方）才真正把它读出来用——跟 fat12.h
 *   当年先定义 fat_dirent_t（Day 9）、几天后才有 fat12_readdir() 真正
 *   去遍历它（Day 12）是同一个"先定形状、后写函数"的顺序。
 *
 * Design notes:
 *   - 两个结构体都 __attribute__((packed))，原因跟 fat_common.h 的
 *     bpb_12_t/fat_dirent_t 完全一样：需要直接把从磁盘读到的原始
 *     字节缓冲区指针强转成这个类型使用（或者反过来，把结构体内容
 *     原样当字节写回磁盘——Day 31 的 tools/mkfs.lmfs 用 Python 的
 *     struct.pack 做的事，lmfs_mount() 这里用 C 的 packed struct
 *     覆盖到已读入的缓冲区上做等价的事），x86 上非对齐的多字节字段
 *     访问是硬件原生支持的，不需要额外处理。
 *   - LMFS_SUPERBLOCK_LBA(=1) 是"卷内相对 LBA"，不是磁盘绝对 LBA
 *     ——doc/lmfs.txt 第一节专门解释过这一点：磁盘绝对 LBA 1~5 早被
 *     loader.bin 占用了，LMFS 卷自己的编号从它自己的 LBA 0（卷首）
 *     开始算。所以 lmfs_mount() 的 `lba` 参数意思跟 fat12_mount()
 *     的 `lba` 参数完全一样：调用者传入的是"这个卷的卷首在磁盘上的
 *     绝对 LBA"，函数内部自己再加上 LMFS_SUPERBLOCK_LBA 算出超级块
 *     真正所在的磁盘绝对 LBA。
 */
#ifndef LUMA_LMFS_H
#define LUMA_LMFS_H

#include <stdint.h>
#include "../../drivers/block/block.h"   /* block_device_t，lmfs_mount() 用 */

/* ===== 常量（全部照抄 doc/lmfs.txt，两边必须保持一致） ===== */

#define LMFS_MAGIC            0x4C4D4653u   /* 'L''M''F''S'，doc 第二/三节 */
#define LMFS_VERSION_MAJOR    1              /* doc 第七节：当前主版本 */
#define LMFS_VERSION_MINOR    0              /* 当前次版本 */

#define LMFS_BLOCK_SIZE        1024   /* doc 第六节：LMFS 块大小（字节） */
#define LMFS_SECTOR_SIZE       BLOCK_SECTOR_SIZE          /* 512，来自 block.h */
#define LMFS_SECTORS_PER_BLOCK (LMFS_BLOCK_SIZE / LMFS_SECTOR_SIZE)  /* = 2 */

/* doc 第一节：卷内 LBA 0 = 保留扇区，卷内 LBA 1 = 超级块。这是"卷内
 * 相对"地址，lmfs_mount() 会加上调用者传入的卷首绝对 LBA 才是真正
 * 要读的磁盘扇区——见本文件顶部 Design notes 最后一条。 */
#define LMFS_SUPERBLOCK_LBA     1

#define LMFS_INODE_SIZE         64    /* doc 第七节 */
#define LMFS_INODES_PER_BLOCK   (LMFS_BLOCK_SIZE / LMFS_INODE_SIZE)  /* = 16 */
#define LMFS_INODES_PER_SECTOR  (LMFS_SECTOR_SIZE / LMFS_INODE_SIZE) /* = 8,
                                   DAY33: lmfs_read_inode() 按扇区读，不是按
                                   块读——见该函数声明上方注释 */

/* DAY34: 目录项大小——doc/lmfs.txt 第十二节：跟 inode 同样选 64 字节，
 * 图的是同一个"能整除块大小"的道理（1024/64=16，一个块正好装 16 个
 * 目录项），并不是巧合跟 LMFS_INODE_SIZE 数值相同。 */
#define LMFS_DIRENT_SIZE         64
#define LMFS_DIRENTS_PER_BLOCK   (LMFS_BLOCK_SIZE / LMFS_DIRENT_SIZE)  /* = 16 */

/* DAY34: 目录项里文件名字段的最大可用长度（不含结尾 NUL）。选 55 而
 * 不是 FAT 的 8.3（11 字符）——doc/lmfs.txt 第十二节：计划.md 文末
 * 「LumaOS 9.0 目录架构规范」里出现的目录/文件名（"Administrator"
 * 13 字符、"TaskManager.sw"/"FileManager.sw" 14 字符等）已经超出
 * 8.3 的容量，LMFS 作为 LumaOS 自己的原生文件系统，没有理由继续
 * 束缚在 FAT 的历史包袱里。 */
#define LMFS_NAME_MAX            55

/* DAY34: 一次 lmfs_readdir() 调用最多解码/返回多少个目录项——跟
 * FAT_DIRLIST_MAX_ENTRIES（fat_common.h）同样的" caller 提供多大的
 * 数组，就还给它多少"上限，数值也保持一致，方便未来 VFS 层用同一个
 * 固定大小的缓冲区应付 FAT 和 LMFS 两种文件系统。 */
#define LMFS_DIRLIST_MAX_ENTRIES  32

/* doc 第八节：inode type 字段取值约定 */
#define LMFS_TYPE_FREE   0
#define LMFS_TYPE_FILE   1
#define LMFS_TYPE_DIR    2

/* doc 第九节末尾：根目录固定是 0 号 inode */
#define LMFS_ROOT_INODE_NUM  0

/* DAY34: 数据块号 0 是保留哨兵值，永远不会被分配给任何文件/目录
 * ——doc/lmfs.txt 第十二节：这样 inode 里 direct[i]==0 / indirect1==0
 * / indirect2==0 就能干净地表示"这个指针槽位没有指向任何块"，不需要
 * 另外找一个字段记录"这个 inode 实际用了几个直接块"。跟根目录固定用
 * 0 号 inode（"inode 0 永远有意义，代表根目录"）是同一枚硬币的另一
 * 面："数据块 0 永远没有意义，代表指针为空"——真正能分配给文件内容
 * 的数据块号从 1 开始。mkfs.lmfs 格式化时会把数据块位图的第 0 位
 * 直接标记为已占用（永久保留，不属于任何 inode），Day 36/37 的位图
 * 分配算法也要遵守"永远跳过块号 0"这条规则。 */
#define LMFS_BLOCK_NONE  0

/* ===== 超级块（doc/lmfs.txt 第二节，512 字节，卷内 LBA 1） ===== */
typedef struct __attribute__((packed)) {
    uint32_t magic;              /* 必须等于 LMFS_MAGIC，否则不是 LMFS 卷 */
    uint16_t version_major;      /* 主版本：跟本文件 LMFS_VERSION_MAJOR 不同
                                   * 就代表磁盘布局可能已经变了，必须拒绝挂载 */
    uint16_t version_minor;      /* 次版本：通常只是新增可选特性，暂不检查 */
    uint32_t inode_count;        /* 本卷 inode 总数 */
    uint32_t data_block_count;   /* 本卷数据块总数 */
    uint32_t inode_bitmap_lba;   /* inode 位图起始 LBA（卷内相对） */
    uint32_t data_bitmap_lba;    /* 数据块位图起始 LBA（卷内相对） */
    uint32_t feature_compat;     /* 可选特性标志，v1.0 恒为 0 */
    uint32_t feature_incompat;   /* 不兼容特性标志：非 0 且驱动不认识 -> 拒绝挂载 */
    uint32_t feature_ro_compat;  /* 只读兼容特性标志，v1.0 恒为 0 */
    uint8_t  reserved[476];      /* 预留，补齐到 512 字节 */
} lmfs_superblock_t;

/* ===== inode（doc/lmfs.txt 第七节，64 字节；Day 33 起真正被读取/使用） ===== */
typedef struct __attribute__((packed)) {
    uint32_t file_size;       /* 文件实际大小，单位：字节 */
    uint32_t created_time;    /* 创建时间戳；本项目暂无 RTC 驱动，固定 0 */
    uint8_t  type;            /* LMFS_TYPE_FREE / _FILE / _DIR */
    uint8_t  reserved0[3];    /* 对齐填充，补到 4 字节边界 */
    uint32_t direct[8];       /* 8 个直接块指针（块号，非 LBA） */
    uint32_t indirect1;       /* 一级间接块指针 */
    uint32_t indirect2;       /* 二级间接块指针 */
    uint8_t  reserved1[12];   /* 预留，补齐到 64 字节 */
} lmfs_inode_t;

/* 两个结构体的大小必须跟 doc/lmfs.txt 完全一致——用 _Static_assert
 * 在编译期就把偏差挡住，不用等到运行时用十六进制查看器才发现
 * "怎么读出来的字段全是错位的"。跟 tools/mkfs.lmfs 里
 * `assert struct.calcsize(...) == ...` 是同一个把关思路，只是这里
 * 是 C 编译期版本。 */
_Static_assert(sizeof(lmfs_superblock_t) == 512,
               "lmfs_superblock_t 必须是 512 字节，跟 doc/lmfs.txt 第二节不符");
_Static_assert(sizeof(lmfs_inode_t) == LMFS_INODE_SIZE,
               "lmfs_inode_t 必须是 64 字节，跟 doc/lmfs.txt 第七节不符");

/* ===== 目录项（doc/lmfs.txt 第十二节，64 字节；Day 34 起被读取/使用）
 *
 * 一个目录的数据（inode.direct[]/indirect1/indirect2 指向的那些块）
 * 里存的就是一连串这种定长记录，逐个槽位扫描，遇到 used==0 就跳过
 * （空闲/已删除槽位），直到扫完 inode.file_size 覆盖的全部块。跟
 * FAT 目录项固定 32 字节、按槽位线性扫描是同一个思路，只是 FAT 用
 * DIR_Name[0] 的哨兵字节（0x00/0xE5）表示空闲/结束，这里改用一个
 * 独立的 used 字节——因为 LMFS 的文件名是变长且可能真的以 NUL 开头
 * 都合法（虽然目前 mkfs.lmfs 从不生成这样的名字），不能像 FAT 那样
 * 把"名字第一个字节"兼职当空闲标记用。
 * ===== */
typedef struct __attribute__((packed)) {
    uint32_t inode_num;    /* 这个目录项指向的 inode 号 */
    uint8_t  used;         /* 0=空闲（未使用或已删除）1=已使用 */
    uint8_t  name_len;     /* name 里实际字符数，不含结尾 NUL */
    uint8_t  reserved[2];  /* 对齐/预留 */
    char     name[LMFS_NAME_MAX + 1]; /* NUL 结尾，最长 LMFS_NAME_MAX 个字符 */
} lmfs_dirent_t;            /* sizeof == 64 == LMFS_DIRENT_SIZE */

_Static_assert(sizeof(lmfs_dirent_t) == LMFS_DIRENT_SIZE,
               "lmfs_dirent_t 必须是 64 字节，跟 doc/lmfs.txt 第十二节不符");

/* lmfs_readdir() 解码后交给调用者的形式——跟 fat_dirlist_entry_t
 * （fat_common.h）是同一个角色：调用者不需要关心磁盘上原始的
 * lmfs_dirent_t/used/reserved 这些实现细节，只关心"这一项叫什么
 * 名字、是文件还是目录、指向哪个 inode、（如果是文件）多大"。 */
typedef struct {
    char     name[LMFS_NAME_MAX + 1];  /* NUL 结尾 */
    uint8_t  type;         /* LMFS_TYPE_FILE 或 LMFS_TYPE_DIR（FREE 槽位
                               在 lmfs_readdir() 内部就跳过，不会出现在
                               这里） */
    uint32_t inode_num;
    uint32_t size;         /* 对应 inode 的 file_size */
} lmfs_dirlist_entry_t;

/* ===== 挂载状态（Day 32 的实际交付物） ===== */
typedef struct {
    block_device_t     *dev;         /* 这个卷所在的块设备 */
    uint32_t             mount_lba;  /* 卷首（卷内 LBA 0）在磁盘上的绝对 LBA，
                                       * 跟 fat12_fs_t::mount_lba 是同一个概念：
                                       * Day 33 起读 inode 表/位图/数据块时，
                                       * 都要用 mount_lba + 卷内相对 LBA 才是
                                       * 真正要传给 dev->read() 的磁盘绝对 LBA。 */
    lmfs_superblock_t    sb;          /* 已解析、已验证过的超级块 */
    uint32_t             inode_table_lba;  /* DAY33: inode 表起始 LBA（卷内
                                       相对），挂载时算好缓存在这里，见
                                       lmfs_read_inode() 声明上方注释——
                                       doc/lmfs.txt 第九节故意没有把这个
                                       位置存进超级块字段本身，是靠这里
                                       "挂载时算一次、之后重复用"，而不是
                                       "存进超级块、靠 mkfs 和挂载两处代码
                                       各自保持一致"。 */
    uint32_t             data_area_lba;    /* DAY34: 数据区起始 LBA（卷内
                                       相对），跟 inode_table_lba 同一个
                                       "挂载时现算一次、缓存复用"的理由，
                                       算法见 lmfs_readdir() 声明上方
                                       注释——lmfs_readdir() 靠这个字段
                                       把 inode 里的"块号"换算成磁盘绝对
                                       LBA。 */
} lmfs_fs_t;

/* lmfs_mount(dev, lba, out_fs)
 *
 * 从 `dev` 读取卷首在磁盘绝对 LBA `lba` 处的 LMFS 卷：读取卷内 LBA
 * LMFS_SUPERBLOCK_LBA（即磁盘绝对 LBA = lba + 1）处的 512 字节超级块，
 * 做 Day 32 验收标准要求的两件事：
 *
 *   1. "验证版本"：magic 必须等于 LMFS_MAGIC；version_major 必须等于
 *      本文件的 LMFS_VERSION_MAJOR（主版本不同 = 磁盘布局可能不兼容，
 *      doc/lmfs.txt 第八节 version_major/minor 字段详解里说明过这个
 *      区分的必要性）；feature_incompat 必须是 0（v1.0 的镜像本来就
 *      不会置这个字段的任何一位，非 0 说明这是本驱动读不懂的更新
 *      版本卷，doc 第三节 feature_incompat 字段详解里"必须拒绝挂载"
 *      的约定）。
 *   2. "验证容量"：inode_count、data_block_count 都不能是 0（否则这个
 *      卷里连一个 inode / 一个数据块都没有，不是一个合法能用的卷）；
 *      inode_bitmap_lba 必须落在超级块之后，data_bitmap_lba 必须落在
 *      inode 位图之后——两个位图的先后顺序是 doc 第九节定死的卷布局，
 *      顺序不对说明超级块内容已经损坏或者根本不是 mkfs.lmfs 格式化出
 *      来的卷。
 *
 * 全部通过 -> 填好 *out_fs（dev、mount_lba、已验证的 sb），返回 0。
 * 任意一项不通过，或者 dev->read() 本身失败 -> *out_fs 清零，返回 -1
 * ——跟 fat12_mount() 的"失败就清零、成功才填"约定完全一致。
 *
 * `out_fs` 不能为 NULL。
 */
int lmfs_mount(block_device_t *dev, uint32_t lba, lmfs_fs_t *out_fs);

/* lmfs_read_inode(fs, inode_num, out_inode)
 *
 * DAY33 (第二月 · 第5周 · Day 33 of the 5.0 filesystem roadmap):
 *   "实现 lmfs_read_inode()。" 验收："能读取 inode 0。"
 *
 * 按 inode 号从 `fs` 已挂载的卷里读出对应的 lmfs_inode_t，写进
 * *out_inode。inode 表的位置本身不是超级块里的一个字段（doc/lmfs.txt
 * 第九节的设计决定：省掉一个"两处数据要保持一致"的冗余字段），是
 * lmfs_mount() 挂载时用 "data_bitmap_lba + 数据块位图占用的扇区数"
 * 现算出来、缓存进 fs->inode_table_lba 的（见 lmfs_fs_t 定义处的
 * 注释）——lmfs_read_inode() 直接用这个缓存值，不会每次调用都重新
 * 算一遍。
 *
 * 定位方式：inode 表里每个 inode 固定 64 字节（LMFS_INODE_SIZE），
 * 一个 512 字节扇区正好装 8 个（LMFS_INODES_PER_SECTOR）——这跟
 * doc/lmfs.txt 第七节"inode 大小选 64 字节，为的是能整除块大小"是
 * 同一个思路在扇区这一级的自然结果（64 同样整除 512）。所以：
 *
 *     扇区号（inode 表内，从 0 开始） = inode_num / 8
 *     扇区内偏移                     = (inode_num % 8) * 64
 *
 * 读那一个扇区（fs->mount_lba + fs->inode_table_lba + 扇区号），
 * 从读回来的 512 字节缓冲区里把对应的 64 字节切出来，就是这个 inode
 * ——不需要把整个 inode 表都读进内存，一次 dev->read() 只读 1 个
 * 扇区，跟 fat12_readdir() 逐簇读目录区、不一次性整卷读入是同一个
 * "按需读、不贪多"的原则。
 *
 * 校验：inode_num 必须 < fs->sb.inode_count（doc 第三节"验证容量"
 * 里 inode_count 本来就是这个卷的 inode 总数，超出这个数的 inode 号
 * 在这个卷上根本不存在，读了也是垃圾数据）；fs 必须是已经
 * lmfs_mount() 成功过的状态（fs->dev 非 NULL）。
 *
 * 成功返回 0 并填好 *out_inode；越界、fs 未挂载、或者 dev->read()
 * 本身失败，返回 -1，*out_inode 清零——跟 lmfs_mount() 的"失败就
 * 清零"约定一致。
 *
 * `fs` 和 `out_inode` 都不能为 NULL。
 */
int lmfs_read_inode(lmfs_fs_t *fs, uint32_t inode_num,
                     lmfs_inode_t *out_inode);

/* lmfs_readdir(fs, dir_inode_num, out_entries, max_entries, out_count)
 *
 * DAY34 (第二月 · 第5周 · Day 34 of the 5.0 filesystem roadmap):
 *   "实现 lmfs_readdir()。" 验收："能列出根目录。"
 *
 * 读 `dir_inode_num` 这个目录 inode 的数据，把里面每一个"已使用"的
 * lmfs_dirent_t 解码成 lmfs_dirlist_entry_t，写进 out_entries[]。
 *
 * 定位数据块：inode.direct[0..7] 是这个目录数据所在的块号（块号，不
 * 是 LBA）；块号 -> 磁盘绝对 LBA 的换算是
 * `fs->mount_lba + fs->data_area_lba + 块号 * LMFS_SECTORS_PER_BLOCK`
 * ——fs->data_area_lba 是 lmfs_mount() 挂载时用 doc/lmfs.txt 第九节
 * 的公式（inode 表起始 LBA + inode 表占用的扇区数，再向上取整成偶数）
 * 现算出来、缓存进 fs->data_area_lba 的，算法必须跟 tools/mkfs.lmfs
 * 的 build_layout() 完全一致，两边独立算出同一个位置。
 *
 * v1.0 范围：本次实现只走 direct[8]（8 个直接块 = 最多 8*16=128 个
 * 目录项，8KB 目录内容），不处理 indirect1/indirect2——doc/lmfs.txt
 * 第七节设计 inode 时，间接块本来就是给大文件预留的，日常使用的
 * 目录（哪怕未来 IDE/System/SysApp/ 这种目录，里面文件也就几个到
 * 几十个）远用不到需要间接块才能装下的规模；真正哪天有目录大到需要
 * 间接块，再来扩展这个函数，不提前做这个复杂度。
 * direct[i] == LMFS_BLOCK_NONE(0) 表示"没有更多块了"，遇到就停止
 * 扫描（不是错误——见 LMFS_BLOCK_NONE 的注释：块号 0 是保留哨兵值，
 * 真正的数据块从块号 1 开始，所以 0 能安全地当"空指针"用）。
 *
 * 每个块内部：LMFS_DIRENTS_PER_BLOCK(=16) 个 lmfs_dirent_t 逐一检查
 * used 字段，used==0 的槽位跳过（空闲/已删除），used==1 的槽位解码
 * 成一条 out_entries[]。跟 fat12_readdir() 的"线性扫描全部槽位，
 * 跳过空闲的"是同一个思路，只是空闲标记字段不同（见 lmfs_dirent_t
 * 定义处的注释）。
 *
 * 写满 max_entries 个或者扫描完这个目录全部数据（含遇到
 * LMFS_BLOCK_NONE）后停止，把实际解码出的条目数写进 *out_count（0
 * 是合法结果：一个空目录）。
 *
 * 校验：dir_inode_num 对应的 inode 必须 type==LMFS_TYPE_DIR（对一个
 * 普通文件调用 lmfs_readdir() 没有意义，直接拒绝，不会把文件内容
 * 错当目录项解析）；其余校验（inode 号越界、fs 未挂载）复用
 * lmfs_read_inode() 自己的检查。
 *
 * 成功返回 0；dir_inode_num 不是目录、lmfs_read_inode() 失败、或者
 * 扫描途中 dev->read() 失败，返回 -1（已解码的条目和 *out_count 保留
 * 失败前的状态，不回滚——跟 fat12_readdir() "读到一半失败，之前解码
 * 的条目仍然有效"是同一个约定）。
 *
 * `fs`、`out_entries`、`out_count` 都不能为 NULL。`out_entries` 至少
 * 要有 `max_entries` 个 lmfs_dirlist_entry_t 的空间。
 */
int lmfs_readdir(lmfs_fs_t *fs, uint32_t dir_inode_num,
                  lmfs_dirlist_entry_t *out_entries,
                  uint32_t max_entries, uint32_t *out_count);

#endif /* LUMA_LMFS_H */