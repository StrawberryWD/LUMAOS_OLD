/* kernel/fs/lmfs/lmfs_vfs.h
 * LumaOS 5.0 - LMFS <-> VFS Adapter
 *
 * DAY35 (第二月 · 第6周 · Day 35 of the 5.0 filesystem roadmap):
 *   "LMFS 注册进 VFS。" 验收："FAT 与 LMFS 可以同时存在。"
 *
 *   这是 fat12_vfs.h/fat12_vfs.c（Day 13）在 LMFS 这边的对应实现：
 *   Day 32~34 建好的 lmfs_mount()/lmfs_read_inode()/lmfs_readdir() 都
 *   只认 lmfs_fs_t* / inode 号这些 LMFS 自己的形状，VFS 层的 Shell
 *   命令（`ls` 等）只认 vfs_node_t / struct filesystem 的函数指针表
 *   ——这个文件就是翻译层，把 lmfs.h 的磁盘专属 API 适配成 vfs.h 的
 *   通用 vfs_open_fn/vfs_read_fn/vfs_readdir_fn 形状，跟
 *   kernel/drivers/block/block.c 的 ide0_block_device() 把 IDE 驱动
 *   适配成通用 block_device_t 接口是同一个道理。
 *
 *   "FAT 与 LMFS 可以同时存在"这条验收标准，机制上早就有了：
 *   kernel/fs/vfs/vfs.h 的 vfs_mounts[]/vfs_register_mount() 是
 *   DAY18-FOLLOWUP 时"提前做"的一张多挂载点命名表，FAT12/16/32 三个
 *   卷已经在同时挂载、各自占一个 "IDE<N>" 槽位（见 kmain.c 的
 *   DAY13/16/17 段落）。Day 35 要做的实际上只是：让 LMFS 也能通过
 *   同一张表登记一个槽位——这个适配器文件就是让这件事成立所缺的
 *   最后一块拼图，不需要改 vfs.h/vfs.c 本身一个字。
 *
 * Design notes（跟 fat12_vfs.h 完全对应，逐条照抄其理由）：
 *   - lmfs_vfs_mount(dev, lba) 是这个文件唯一的入口：调用
 *     lmfs_mount()（Day 32）做真正的挂载，再包一层 filesystem_t
 *     （ops=lmfs_filesystem_ops，fs_private=lmfs_fs_t 挂载状态）和
 *     一个根 vfs_node_t（type=VFS_TYPE_DIR）。成功返回填好的
 *     filesystem_t*，lmfs_mount() 本身失败则返回 NULL。
 *   - 挂载状态（lmfs_fs_t、filesystem_t、根 vfs_node_t）都是
 *     lmfs_vfs.c 内部的 static 变量，不是堆分配——这个项目到现在还
 *     没有内核堆（Day 73 才有 malloc/free），跟 fat12_vfs.c 的理由
 *     完全一样。Day 35 的范围是"能同时挂载几个卷"，不要求"能挂载
 *     任意多个动态数量的卷"，一个 static 实例够用。
 *   - 目录节点的 fs_private 存的是 inode 号（不是 FAT12 那种"根目录
 *     用哨兵值、子目录用起始簇号"的两套值）——LMFS 的每一个目录，
 *     包括根目录本身，都有一个真正的 inode 号（根目录固定是
 *     LMFS_ROOT_INODE_NUM=0），不需要 FAT12 那种"根目录是固定区域、
 *     没有簇号"的特殊情况处理，直接把 inode 号转成 (void*) 存进
 *     fs_private，比 FAT12 那边还简单一些。
 *   - open()/readdir() 是真正能用的实现，read() 是文档化的占位桩
 *     （永远返回 0，即"读到 0 字节，不是错误"，符合 vfs_read_fn 自己
 *     文档里"0 == EOF"的约定）——原因跟 fat12_vfs_read() 在 Day 13
 *     时的桩函数完全一样：真正按字节读文件内容需要 lmfs_read_
 *     inode() 之外还要走 direct[]/indirect1/indirect2 的完整数据
 *     读取逻辑，那是 Day 40（"实现 read()"）的任务，不是今天。
 *   - `lmfs_filesystem_ops` 不加 static，跟 fat12_filesystem_ops 一样
 *     的理由：给未来可能的直接调用者留出可见性，即便目前只有
 *     lmfs_vfs_mount() 自己用到它。
 */
#ifndef LUMA_LMFS_VFS_H
#define LUMA_LMFS_VFS_H

#include "../../drivers/block/block.h"   /* block_device_t */
#include "../vfs/vfs.h"                  /* filesystem_t, vfs_node_t */

/* lmfs_filesystem_ops
 *
 * LMFS 驱动的 VFS 操作表：.open/.read/.readdir/.close 指向本文件的
 * 适配函数（lmfs_vfs.c）。.name 是 "lmfs"，供诊断输出使用。
 * .root/.fs_private 留 NULL——那是每次挂载各自的状态，由下面的
 * lmfs_vfs_mount() 按实例填入，不属于这张共享的操作表本身。
 */
extern const filesystem_t lmfs_filesystem_ops;

/* lmfs_vfs_mount(dev, lba)
 *
 * 在 `dev` 的磁盘绝对 LBA `lba` 处挂载一个 LMFS 卷（通过
 * lmfs_mount()，Day 32），包装成一个 `.root` 是 VFS_TYPE_DIR 节点
 * （代表 LMFS 根目录，inode 号固定是 LMFS_ROOT_INODE_NUM）的
 * filesystem_t。
 *
 * 成功时返回填好的、静态分配的 filesystem_t*。lmfs_mount() 本身失败
 * （不是合法的 LMFS 卷、或者底层 dev->read() 失败）时返回 NULL——
 * 跟 lmfs_mount() 自己"失败用 -1"的约定一致，只是这里换成了指针
 * 形式的失败值，因为 vfs_mount()/kmain.c 的调用方要的是"挂载出来的
 * 东西能不能用"而不是原始的错误码。
 */
filesystem_t *lmfs_vfs_mount(block_device_t *dev, uint32_t lba);

#endif /* LUMA_LMFS_VFS_H */