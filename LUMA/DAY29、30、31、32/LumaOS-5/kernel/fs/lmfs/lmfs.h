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
 *   lmfs_inode_t 在这里先把结构体定义出来（后面 lmfs_read_inode()
 *   要用），但读取 inode 的函数本身是 Day 33 的任务，这里只放形状，
 *   不放实现——跟 fat12.h 当年先定义 fat_dirent_t（Day 9）、几天后
 *   才有 fat12_readdir() 真正去遍历它（Day 12）是同一个"先定形状、
 *   后写函数"的顺序。
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

/* doc 第八节：inode type 字段取值约定 */
#define LMFS_TYPE_FREE   0
#define LMFS_TYPE_FILE   1
#define LMFS_TYPE_DIR    2

/* doc 第九节末尾：根目录固定是 0 号 inode */
#define LMFS_ROOT_INODE_NUM  0

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

/* ===== 挂载状态（Day 32 的实际交付物） ===== */
typedef struct {
    block_device_t     *dev;         /* 这个卷所在的块设备 */
    uint32_t             mount_lba;  /* 卷首（卷内 LBA 0）在磁盘上的绝对 LBA，
                                       * 跟 fat12_fs_t::mount_lba 是同一个概念：
                                       * Day 33 起读 inode 表/位图/数据块时，
                                       * 都要用 mount_lba + 卷内相对 LBA 才是
                                       * 真正要传给 dev->read() 的磁盘绝对 LBA。 */
    lmfs_superblock_t    sb;          /* 已解析、已验证过的超级块 */
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

#endif /* LUMA_LMFS_H */