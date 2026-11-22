/* kernel/fs/lmfs/lmfs_vfs.c
 * LumaOS 5.0 - LMFS <-> VFS Adapter
 *
 * DAY35 (第二月 · 第6周 · Day 35 of the 5.0 filesystem roadmap):
 *   "LMFS 注册进 VFS。" 验收："FAT 与 LMFS 可以同时存在。"
 *
 * 见 lmfs_vfs.h 的文件头注释：这是 fat12_vfs.c 在 LMFS 这边的对应
 * 实现，逐条设计理由都照抄那边、只把 FAT12 专属的部分换成 LMFS 的。
 */
#include "lmfs.h"
#include "lmfs_vfs.h"
#include "../../lib/string.h"   /* memset */

/* ===== Static mount state =====
 *
 * 一次 LMFS 挂载所需的全部状态：lmfs_mount()（Day 32）填好的
 * lmfs_fs_t、Day 35 加的 filesystem_t 包装、以及每个 VFS 层调用方都
 * 会经由 fs->root / 全局 vfs_root 触达的那一个根 vfs_node_t。跟
 * fat12_vfs.c 一样用 static 而不是堆分配——理由见 lmfs_vfs.h 的设计
 * 说明。
 */
static lmfs_fs_t    g_lmfs_mount;
static filesystem_t g_lmfs_fs;
static vfs_node_t   g_lmfs_root;

/* ---------------------------------------------------------------------
 * lmfs_vfs_open(node, out_file)
 *
 * 准备 *out_file 从 node 开始读：存好节点指针，偏移量清零——符合
 * vfs_open_fn 的文档契约（kernel/fs/vfs/vfs.h）。拒绝把目录当文件
 * 打开，跟 fat12_vfs_open() 完全一样的检查。
 * --------------------------------------------------------------------- */
static int lmfs_vfs_open(vfs_node_t *node, file_t *out_file)
{
    if (!node || !out_file || node->type != VFS_TYPE_FILE)
        return -1;

    out_file->node = node;
    out_file->pos  = 0;
    return 0;
}

/* ---------------------------------------------------------------------
 * lmfs_vfs_read(file, buf, size)
 *
 * DAY35 占位桩：真正按字节读取文件内容需要走 inode 的
 * direct[]/indirect1/indirect2，那是 Day 40（"实现 read()"）的任务。
 * 这里始终返回 0——按 vfs_read_fn 自己文档里"0 == EOF，不是错误"的
 * 约定，而不是返回 -1（"出错"），跟 fat12_vfs_read() 在 Day 13 时的
 * 占位桩是同一个理由、同一个返回值选择。
 * --------------------------------------------------------------------- */
static int lmfs_vfs_read(file_t *file, void *buf, uint32_t size)
{
    (void)file;
    (void)buf;
    (void)size;
    return 0;
}

/* ---------------------------------------------------------------------
 * lmfs_vfs_close(file)
 *
 * 跟 vfs_close_fn 自己文档里的默认契约完全一致：把 file->node 清成
 * NULL，让任何后续不小心对同一个（已经"关闭"的）file_t 再调用
 * fs->read() 会因为 file->node 是 NULL 而明显失败，而不是悄悄继续
 * 读一个调用者以为已经关掉的文件。LMFS 目前没有需要释放的每次打开
 * 状态（没有内核堆，也没有额外分配的资源），所以这里不需要像
 * fat_close() 那样转发到一个共享实现，直接原地做这件事就够。
 * --------------------------------------------------------------------- */
static int lmfs_vfs_close(file_t *file)
{
    if (!file || !file->node)
        return -1;

    file->node = 0;
    return 0;
}

/* ---------------------------------------------------------------------
 * lmfs_vfs_readdir(dir_node, index, out_node)
 *
 * 把 lmfs_readdir()（Day 34，"一次性给整个数组"的形状）适配成
 * vfs_readdir_fn（"按 index 一次给一条"的形状）——跟
 * fat12_vfs_readdir() 对 fat12_readdir() 做的事完全一样的适配方向。
 *
 * dir_node 必须是 VFS_TYPE_DIR，其 fs_private 是这个目录的 inode 号
 * （lmfs_vfs_mount() 给根目录填的是 LMFS_ROOT_INODE_NUM；子目录还
 * 没有任何代码产出，Day 42 的多级目录会用到这条路径，读法完全一样，
 * 不需要改这个函数）。
 *
 * 关于按 index 单条调用却每次都重新跑一遍 lmfs_readdir() 全量扫描：
 * 跟 fat12_vfs_readdir() 的 NOTE ON RE-WALKING PER CALL 是同一个
 * 权衡——LMFS_DIRLIST_MAX_ENTRIES(32) 把这个代价限制得足够小，现在
 * 优化是在解决一个还不存在的性能问题。
 * --------------------------------------------------------------------- */
static int lmfs_vfs_readdir(vfs_node_t *dir_node, uint32_t index,
                             vfs_node_t *out_node)
{
    if (!dir_node || !out_node || dir_node->type != VFS_TYPE_DIR)
        return -1;

    lmfs_fs_t *fs = (lmfs_fs_t *)dir_node->fs->fs_private;
    if (!fs)
        return -1;

    uint32_t dir_inode_num = (uint32_t)(uintptr_t)dir_node->fs_private;

    static lmfs_dirlist_entry_t entries[LMFS_DIRLIST_MAX_ENTRIES];
    uint32_t count = 0;
    if (lmfs_readdir(fs, dir_inode_num, entries, LMFS_DIRLIST_MAX_ENTRIES,
                      &count) != 0)
        return -1;
    if (index >= count)
        return -1;   /* 按 vfs_readdir_fn 的约定："index 超出末尾"就停 */

    lmfs_dirlist_entry_t *e = &entries[index];

    /* lmfs_dirlist_entry_t 的 name[56]（LMFS_NAME_MAX+1）比
     * vfs_node_t 的 name[32] 还大——见 vfs.h 里 name[32] 的注释：
     * 这是 Day 8 定的、原本按 FAT 8.3 场景设计的大小，LMFS 的长
     * 文件名可能超出它。这里按 vfs_node_t 的容量截断拷贝（宁可显示
     * 被截断的名字，也不要越界写），这个截断本身是 vfs_node_t 这个
     * 通用结构体的固有限制，不是 lmfs_readdir() 的问题——e->name
     * 在 entries[] 里本身是完整、未截断的。 */
    {
        uint32_t i = 0;
        while (e->name[i] != '\0' && i < sizeof(out_node->name) - 1) {
            out_node->name[i] = e->name[i];
            i++;
        }
        out_node->name[i] = '\0';
    }

    out_node->type = (e->type == LMFS_TYPE_DIR) ? VFS_TYPE_DIR
                                                  : VFS_TYPE_FILE;
    out_node->size = e->size;
    out_node->fs   = dir_node->fs;
    out_node->fs_private = (void *)(uintptr_t)e->inode_num;

    return 0;
}

/* lmfs_filesystem_ops — 见 lmfs_vfs.h 的文档注释。`.root`/`.fs_private`
 * 是每次挂载各自的状态（由 lmfs_vfs_mount() 填入），这张表本身是
 * 共享的，留 NULL/0。 */
const filesystem_t lmfs_filesystem_ops = {
    .name       = "lmfs",
    .open       = lmfs_vfs_open,
    .read       = lmfs_vfs_read,
    .readdir    = lmfs_vfs_readdir,
    .close      = lmfs_vfs_close,
    .root       = (vfs_node_t *)0,
    .fs_private = (void *)0,
};

filesystem_t *lmfs_vfs_mount(block_device_t *dev, uint32_t lba)
{
    if (lmfs_mount(dev, lba, &g_lmfs_mount) != 0)
        return (filesystem_t *)0;

    /* 从共享操作表出发，再填这一次挂载自己的 root/fs_private——
     * g_lmfs_fs 是"实例"，lmfs_filesystem_ops 是它复制函数指针的
     * "模板"，跟 fat12_vfs_mount() 的 g_fat12_fs 是同一个套路。 */
    g_lmfs_fs = lmfs_filesystem_ops;
    g_lmfs_fs.fs_private = &g_lmfs_mount;

    memset(&g_lmfs_root, 0, sizeof(g_lmfs_root));

    /* 根节点自己的 .name 字段：跟 fat12/16/32_vfs_mount() 一样先写
     * 死成 "IDE0"——这个字段本来就不是显示用的权威来源（见 vfs.c
     * 里 vfs_mount_name_of() 那段"BUGFIX / WHY THIS EXISTS"的完整
     * 说明：多次挂载会共享同一个 static 根节点，后写的会覆盖先写的
     * .name，所以显示层一律改查 vfs_mounts[] 表而不是这个字段），
     * 这里写 "IDE0"只是跟其他三个适配器保持字面一致，不代表这个卷
     * 真的会被分到 "IDE0" 这个槽位——真正的槽位名是
     * vfs_register_mount() 分配、调用方从它的 out_name 参数里拿到的
     * 那个。 */
    {
        const char *root_name = "IDE0";
        uint32_t i = 0;
        while (root_name[i] != '\0' && i < sizeof(g_lmfs_root.name) - 1) {
            g_lmfs_root.name[i] = root_name[i];
            i++;
        }
        g_lmfs_root.name[i] = '\0';
    }
    g_lmfs_root.type = VFS_TYPE_DIR;
    g_lmfs_root.size = 0;
    g_lmfs_root.fs   = &g_lmfs_fs;
    g_lmfs_root.fs_private = (void *)(uintptr_t)LMFS_ROOT_INODE_NUM;

    g_lmfs_fs.root = &g_lmfs_root;

    return &g_lmfs_fs;
}