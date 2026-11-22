# 📅 LumaOS 5.0 → 9.0 总体开发 Schedule

> **说明**
>
> - Day 1～Day 30 保留原计划，不做改动。
> - Day 31 之后重新整合为一条连续路线。
> - 版本路线：**5.0 → 5.1 → 5.2 → 6.0 → 7.0 → 8.0 → 9.0**
> - 目标不是“模仿 Windows 的目录”，而是：**底层吸收 Unix 的优秀架构思想，用户体验与文件管理采用 LumaOS 自己定义的、偏 Windows 式的直观设计。**
> - LumaOS 的应用执行文件统一采用 **`.sw`（Luma Software）**。
> - 用户文件系统采用：
>
> ```text
> System/
> User/
> Administrator/
> Public/
> USB/
> IMG/
> ISO/
> ```
>
> - 外部存储/镜像的命名空间：
>
> ```text
> USB/DISK_1/
> USB/DISK_2/
> IMG/DISK_1/
> ISO/LumaOS/
> ```
>
> - 具体代码由 Claude 协作实现；每个 Day 的核心验收是：**代码能工作、你知道它解决了什么问题、你能测试并决定是否进入下一天。**

---

## 🏛️ 核心架构变更（前三天必须定好）

| 新增目录 | 职责 |
| :--- | :--- |
| `kernel/drivers/block/` | IDE 硬盘驱动、分区解析（MBR） |
| `kernel/fs/vfs/` | 虚拟文件系统开关（挂载、打开、读、读目录） |
| `kernel/fs/fat/` | FAT12/16/32 具体实现 |
| `kernel/fs/lmfs/` | 你的专属 LMFS 实现 |
| `tools/` | `mkfs.lmfs`（用户态格式化工具，用 Python 写） |
| `iso/` | ISO 构建临时目录和脚本 |

**内核加载限制紧急调整（第 1 天必须做）**：

- `loader.asm`：`mov al, 128` → `mov al, 255`（读 255 扇区 ≈ 127.5 KB）
- `build.sh`：`MAX_LOAD=65536` → `MAX_LOAD=131072`
- 这样你加完所有 FS 代码（约 40~50 KB）后，总大小仍能控制在 100 KB 以内。

---

## 📆 第一月：地基（磁盘驱动 + VFS + FAT12）—— 第 1~4 周

### 第 1 周：脱离 BIOS，自己掌控硬盘（IDE PIO 驱动）

| 日期 | 任务 | 产出/验收标准 |
| :--- | :--- | :--- |
| **Day 1** | 修改 `loader.asm` 和 `build.sh` 解除 64KB 限制；修改 `run.sh` 将 `if=floppy` 改为 `if=ide`（让软盘镜像被识别为 IDE 主盘）。 | 重新编译后镜像体积允许超过 64KB，QEMU 启动如常。 | <已经完成！>
| **Day 2** | 创建 `kernel/drivers/block/ide.h` 和 `ide.c`。实现 `ide_wait()`（轮询 0x1F7 状态寄存器）。 | 编写一个 `ide_test()` 在 `kmain` 中调用，能读到状态寄存器非 0xFF。 | <已经完成！>
| **Day 3** | 实现 `ide_read_sectors(lba, count, buf)`（使用 PIO 模式，`insw` 指令）。 | 在 `kmain` 中读取 LBA 0（MBR），用 `console_write_hex()` 打印前 64 字节，屏幕上应看到 `EB 3C 90`（FAT 引导跳转）。 | <已经完成！>
| **Day 4** | 实现 `ide_write_sectors(lba, count, buf)`（只读模式先通过，写入可先置空）。 | 能向 LBA 1 写入一个测试字符串，再读回来对比一致。 | <已经完成！>
| **Day 5** | 解析 MBR（LBA 0），扫描分区表（偏移 0x1BE）。识别第一个活动分区（Type 0x0C = FAT32 LBA，0x06 = FAT16，0x01 = FAT12）。 | 在终端打印出分区起始 LBA 和大小。 | <已经完成！>
| **Day 6** | 封装块设备层：`struct block_device` 包含 `read(lba, count, buf)` 函数指针。 | 上层代码不再直接调用 ide 函数，统一走设备结构体。 | <已经完成！>
| **Day 7** | **周复盘**：整理 IDE 驱动代码，消除硬编码，增加超时保护（防止死循环）。 | 写一个 `ide_dump_info()` 打印硬盘型号（IDENTIFY 命令 0xEC）。 | <已经完成！>

---

### 第 2 周：VFS 抽象与 FAT12 只读挂载

| 日期 | 任务 | 产出/验收标准 |
| :--- | :--- | :--- |
| **Day 8** | 创建 `kernel/fs/vfs/vfs.h`。定义 `struct file`、`struct vfs_node`、`struct filesystem`。定义函数指针：`open`、`read`、`readdir`。 | 头文件编译通过。 | <已经完成！>
| **Day 9** | 创建 `kernel/fs/fat/fat12.h`。定义 BPB 结构体（`bpb_12_t`）和目录项结构（`fat_dirent_t`）。 | 写一个 `fat12_parse_bpb(buf)`，能算出 RootDir 扇区数、Data 起始扇区。 | <已经完成！>
| **Day 10** | 实现 `fat12_mount(block_dev)`：读取引导扇区，检查 `BS_FileSysType` 是否为 "FAT12"。 | 挂载成功返回 0，失败返回 -1，在终端打印 "FAT12 mounted"。 |
| **Day 11** | 实现 FAT12 簇链遍历函数 `fat12_next_cluster(cluster)`（处理 1.5 字节对齐，注意奇偶簇）。 | 给定根目录的起始簇（通常为 0），能遍历打印出前 10 个簇号。 |
| **Day 12** | 实现 `fat12_readdir(root_node)`：读取根目录区（Root Directory Region），遍历每 32 字节条目。提取文件名（去掉空格）、属性、起始簇、文件大小。 | 在终端列出根目录下所有文件名（至少能看到 `KERNEL  BIN` 和 `LOADER  BIN`）。 |
| **Day 13** | 将 FAT12 挂接到 VFS：注册 `fat12_filesystem_ops`。实现 `vfs_mount("/dev/hda")`。 | 调用 `vfs_mount` 后，`vfs_root` 指针指向 FAT 根目录。 |
| **Day 14** | **整合 Shell**：新增命令 `cmd_ls`（调用 `vfs_readdir` 列出文件）。 | 在终端输入 `ls`，能看到 FAT 镜像里的所有文件。 |

---

### 第 3 周：FAT16 / FAT32 支持 + 文件读取（cat）

| 日期 | 任务 | 产出/验收标准 |
| :--- | :--- | :--- |
| **Day 15** | 重构 FAT 通用层：分离 FAT12/16/32 的共性（BPB 解析、目录项结构），创建 `fat_common.c`。 | 一个 `fat_mount()` 根据 BPB 的 `BytesPerSector` 和 `TotalSectors` 自动判断类型。 |
| **Day 16** | 实现 FAT16 解析（`FAT16` 的根目录在数据区之前，簇号 16 位）。 | 挂载一个 FAT16 镜像（可以用 `mkfs.fat -F 16` 制作），`ls` 能正确列出文件。 |
| **Day 17** | 实现 FAT32 解析（注意 `BPB_RootClus`，根目录是一个普通簇链，且存在 `FSInfo` 结构）。 | 挂载 FAT32 镜像（`mkfs.fat -F 32`），`ls` 能工作。 |
| **Day 18** | 实现 `fat_open(path)`：根据路径（如 `/KERNEL.BIN`）遍历目录项，返回文件的起始簇号。 | 对根目录文件能正确返回簇号。 |
| **Day 19** | 实现 `fat_read(file, buf, size)`：根据起始簇，依次读取簇链，将数据拷贝到 buf。 | 实现 `cmd_cat /KERNEL.BIN`，能在终端打印出内核二进制的开头部分（乱码也行，证明读到了数据）。 |
| **Day 20** | 增加文件偏移指针 `file->pos`，支持多次读取。 | 连续调用两次 `read`，第二次能接着上次位置读。 |
| **Day 21** | **周复盘**：添加错误处理（文件不存在、簇链断裂）。关闭文件功能（`fat_close`）。 | 编写测试用例：`cat` 一个根目录下的文本文件（用 Python 提前写入）。 |

---

### 第 4 周：FAT 写入支持（创建、删除、截断）

| 日期 | 任务 | 产出/验收标准 |
| :--- | :--- | :--- |
| **Day 22** | 实现 `fat_find_free_cluster()`：扫描 FAT 表，返回第一个值为 0 的簇。 | 测试打印空闲簇号。 |
| **Day 23** | 实现 `fat_alloc_cluster()`：分配一个新簇，并更新 FAT 表（写回磁盘）。 | 分配成功后，再次读取 FAT 对应位置值变为 0xFFF（FAT12）或 0xFFFF。 |
| **Day 24** | 实现 `fat_create_file(name)`：在根目录区找一个空条目（首字节 0x00 或 0xE5），填入文件名、起始簇 0、大小 0。 | 在 Shell 中输入 `touch test.txt`，重新 `ls` 能看到它。 |
| **Day 25** | 实现 `fat_write(file, buf, len)`：处理簇链扩展（如果当前簇链不够，追加新簇）。 | 向 `test.txt` 写入 "Hello LMFS"，再读出来验证。 |
| **Day 26** | 实现 `fat_delete_file(name)`：将目录项首字节置为 0xE5，并释放 FAT 簇链（置 0）。 | `rm test.txt` 后 `ls` 消失，且空闲簇数增加。 |
| **Day 27** | 实现文件截断（`fat_truncate`），支持清空文件内容。 | 只保留目录项，释放所有数据簇。 |
| **Day 28** | **月总结**：编写 `cmd_echo > file.txt`（重定向输出）。 | 能通过 Shell 创建并写入文件。 |

---

## 📆 第二月：专属 LMFS + 用户态程序加载 —— 第 5~8 周

### 第 5 周：LMFS 磁盘格式设计 + mkfs 工具

| 日期 | 任务 | 产出/验收标准 |
| :--- | :--- | :--- |
| **Day 29** | 设计 LMFS 超级块布局（位于 LBA 1）：魔数 `0x4C4D4653`、版本号、inode 数量、数据块数量、inode 位图起始 LBA、数据块位图起始 LBA。 | 输出设计文档到 `doc/lmfs.txt`。 |
| **Day 30** | 设计 inode 结构（`lmfs_inode_t`）：包含文件大小、创建时间、直接块指针 8 个 + 间接块指针 1 个 + 二级间接块 1 个（支持大文件）。 | 定义块大小 1024 字节（适合软盘/小硬盘）。 |

---

# 🚀 Day 31 起：重新整合后的 LumaOS 5.0 → 9.0 路线

## 5.0 后半段：Day 31～84
### LMFS → ELF → syscall → paging → libc → ISO → 5.0 Final

### 第 5 周：LMFS 工具与挂载

| Day | 任务 | 验收 |
|---|---|---|
| **Day 31** | 用 Python 编写 `tools/mkfs.lmfs`：创建空白 LMFS 镜像，写入超级块、初始化 inode/data bitmap、创建根目录 inode。 | 生成 `lmfs.img`，十六进制查看器能看到 LMFS 魔数。 |
| **Day 32** | C 端实现 `lmfs_mount(dev)`。 | 能读取超级块并验证版本/容量。 |
| **Day 33** | 实现 `lmfs_read_inode()`。 | 能读取 inode 0。 |
| **Day 34** | 实现 `lmfs_readdir()`。 | 能列出根目录。 |
| **Day 35** | LMFS 注册进 VFS。 | FAT 与 LMFS 可以同时存在。 |

### 第 6 周：LMFS 文件操作

| Day | 任务 | 验收 |
|---|---|---|
| **Day 36** | 实现数据块 bitmap 分配器。 | 连续分配多个块，bitmap 正确变化。 |
| **Day 37** | 实现 inode 分配器。 | 创建 inode 后返回合法 inode 号。 |
| **Day 38** | 实现 `create()` 与目录项。 | `touch test.txt` 成功。 |
| **Day 39** | 实现 `write()`，支持直接块与间接块。 | 写入 10KB 文件并验证。 |
| **Day 40** | 实现 `read()`。 | `cat bigfile.txt` 正常。 |
| **Day 41** | 实现 `delete()` 与空间回收。 | `rm` 后空间恢复。 |
| **Day 42** | 多级目录与路径解析。 | 可以创建并访问多级目录。 |

### 第 7 周：ELF Loader

| Day | 任务 | 验收 |
|---|---|---|
| **Day 43** | 学习并定义 ELF32 基础结构。 | 能解析 ELF Header。 |
| **Day 44** | 实现 `elf_load()`。 | 检查 Magic、打印 Entry。 |
| **Day 45** | 解析 `PT_LOAD`。 | 能将程序段加载到内存。 |
| **Day 46** | 实现 `elf_run()`。 | `hello` 可以执行。 |
| **Day 47** | 增加用户态栈和退出路径。 | 程序执行后返回 Shell。 |
| **Day 48** | Shell 集成 `run`。 | `run hello` 成功。 |
| **Day 49** | argc/argv。 | `run hello luma` 能读取参数。 |

### 第 8 周：syscall + paging + Ring 3

| Day | 任务 | 验收 |
|---|---|---|
| **Day 50** | 定义 syscall ABI。 | `int 0x80` 能进入 Kernel。 |
| **Day 51** | `sys_write`。 | 用户程序通过 syscall 输出。 |
| **Day 52** | `sys_exit`。 | 程序可以安全结束。 |
| **Day 53** | 开启分页，建立 Kernel Page Table。 | 开启分页后系统仍稳定。 |
| **Day 54** | 独立用户地址空间。 | ELF 在独立虚拟地址空间运行。 |
| **Day 55** | `iret` 进入 Ring 3。 | 用户程序无法直接访问 Kernel。 |
| **Day 56** | 修复中断返回、用户态异常。 | 用户程序可以重复稳定运行。 |

### 第 9 周：ISO 与系统资源

| Day | 任务 | 验收 |
|---|---|---|
| **Day 57** | 研究 ISO9660 / El Torito。 | 理解 ISO 启动结构。 |
| **Day 58** | `tools/mkiso.py`。 | 自动生成 ISO。 |
| **Day 59** | No-Emulation 启动路径。 | 从 CD/ISO 启动 Loader。 |
| **Day 60** | `build.sh` 自动生成 ISO。 | 一次 build 得到 `.iso`。 |
| **Day 61** | QEMU CD-ROM 启动测试。 | ISO 能进入 LumaOS。 |
| **Day 62** | LMFS 镜像作为系统资源载入。 | Kernel 能访问 LMFS 镜像。 |
| **Day 63** | CD-ROM 资源读取整理。 | 字体、配置等资源可从 ISO 提供。 |

### 第 10 周：压力测试

| Day | 任务 | 验收 |
|---|---|---|
| **Day 64** | FS Stress Test。 | 批量创建/读写文件全部通过。 |
| **Day 65** | FAT 缓存。 | 大文件读取明显改善。 |
| **Day 66** | LMFS bitmap 优化。 | 分配效率提升。 |
| **Day 67** | 已知 Bug 清零。 | 长时间测试不崩溃。 |
| **Day 68** | `df`。 | 显示容量/已用/剩余。 |
| **Day 69** | `cd` / `pwd`。 | 相对路径正常。 |
| **Day 70** | 统一错误码。 | 用户程序能识别错误。 |

### 第 11 周：libc + 用户程序

| Day | 任务 | 验收 |
|---|---|---|
| **Day 71** | `putchar`。 | 用户态输出。 |
| **Day 72** | `printf`。 | 格式化输出。 |
| **Day 73** | `malloc/free`。 | 用户态动态内存。 |
| **Day 74** | `fopen/fread/fclose`。 | 用户程序读文件。 |
| **Day 75** | `cat.sw`、`ls.sw` 等用户程序。 | Shell 内置命令开始迁移到用户态。 |
| **Day 76** | `execve`。 | 程序可以启动另一个程序。 |
| **Day 77** | `fork` 雏形。 | 为多任务做准备。 |

### 第 12 周：5.0 Final

| Day | 任务 | 验收 |
|---|---|---|
| **Day 78** | README / BUILD。 | 第三方可以复现。 |
| **Day 79** | 清理代码、警告、Debug 输出。 | Clean Build。 |
| **Day 80** | 整理 5.0 文件系统。 | 系统能从 ISO/磁盘启动。 |
| **Day 81** | Final ISO。 | QEMU/VirtualBox 启动成功。 |
| **Day 82** | 演示流程。 | 从开机到 GUI/Terminal 完整演示。 |
| **Day 83** | `lmfs-spec.md`。 | LMFS 规格正式文档化。 |
| **Day 84** | **LumaOS 5.0 发布。** | `lumaos-5.0.iso` + 源码 + 文档。 |

---

# 🈶 LumaOS 5.1：中文桌面
## Day 85～112

目标：

> **从“能用的英文实验 OS”进入“真正适合中文用户使用的桌面 OS”。**

### 第 13 周：字体引擎

| Day | 任务 | 验收 |
|---|---|---|
| **Day 85** | 将 `asc24.bin` / `hzk24.bin` 纳入系统资源。 | ISO 中存在字体。 |
| **Day 86** | `font.c/h` 与 `font_init()`。 | Kernel 能加载字体。 |
| **Day 87** | `font_get_glyph()`。 | 能取得 `A`、`中`。 |
| **Day 88** | 任意 Glyph 绘制。 | 屏幕显示字形。 |
| **Day 89** | GB2312 + ASCII 混合绘制。 | `Hello 你好` 正常。 |
| **Day 90** | GUI 标题/菜单接入新字体。 | “终端”“设置”等正常。 |
| **Day 91** | 字形缓存。 | 重复文字绘制明显更快。 |

### 第 14 周：桌面中文化

| Day | 任务 | 验收 |
|---|---|---|
| **Day 92** | Terminal 字体适配。 | 中文终端正常。 |
| **Day 93** | Terminal 滚动适配。 | 无跳行。 |
| **Day 94** | Window Button 字符适配。 | 按钮清晰。 |
| **Day 95** | Settings / Info 中文化。 | 无乱码。 |
| **Day 96** | 统一字体度量。 | 控件基线一致。 |
| **Day 97** | 未知字符 fallback。 | 不崩溃。 |
| **Day 98** | 中文文件名显示。 | 中文文件名正常。 |

### 第 15 周：GUI 性能

| Day | 任务 | 验收 |
|---|---|---|
| **Day 99** | 设计 Dirty Rectangle。 | 有正式设计。 |
| **Day 100** | GUI 记录 Dirty Rect。 | 变化区域正确。 |
| **Day 101** | Clip。 | 重叠窗口不乱画。 |
| **Day 102** | 局部 framebuffer 更新。 | 小区域更新不再全屏复制。 |
| **Day 103** | FPS 统计。 | 有可比较指标。 |
| **Day 104** | Glyph Render Cache。 | 文本重绘更快。 |
| **Day 105** | GUI Bug Sweep。 | 长时间运行稳定。 |

### 第 16 周：5.1 发布

| Day | 任务 | 验收 |
|---|---|---|
| **Day 106** | 中文 Shell 输出。 | `echo 你好`。 |
| **Day 107** | 中文换行/裁剪。 | 长文本正常。 |
| **Day 108** | GB2312 → Unicode 基础转换层。 | 转换测试通过。 |
| **Day 109** | 更新文档。 | 字体系统可复现。 |
| **Day 110** | 自动打包字体。 | ISO 启动自动加载。 |
| **Day 111** | 中文桌面完整测试。 | Terminal/GUI/文件名均正常。 |
| **Day 112** | **LumaOS 5.1 发布。** | 中文桌面 ISO。 |

---

# 🖥️ LumaOS 5.2：完整 GUI
## Day 113～140

目标：

> **从“有 GUI”变成“GUI 是一个真正的窗口系统”。**

### 第 17 周：Window Manager

| Day | 任务 | 验收 |
|---|---|---|
| **Day 113** | Window Manager 重构。 | 窗口生命周期统一。 |
| **Day 114** | Z-order。 | 窗口正确叠放。 |
| **Day 115** | Focus。 | 当前窗口明确。 |
| **Day 116** | Window Move。 | 鼠标拖动窗口。 |
| **Day 117** | Resize。 | 窗口可调整大小。 |
| **Day 118** | Minimize / Maximize。 | 状态切换正常。 |
| **Day 119** | 多窗口压力测试。 | 多窗口长时间稳定。 |

### 第 18 周：GUI 控件

| Day | 任务 | 验收 |
|---|---|---|
| **Day 120** | Button。 | 点击事件正常。 |
| **Day 121** | Label。 | 文本控件正常。 |
| **Day 122** | TextBox。 | 可编辑文本。 |
| **Day 123** | ListBox。 | 列表滚动。 |
| **Day 124** | ScrollBar。 | 滚动区域正常。 |
| **Day 125** | Menu / Context Menu。 | 菜单事件正常。 |
| **Day 126** | Dialog。 | 模态对话框正常。 |

### 第 19 周：桌面

| Day | 任务 | 验收 |
|---|---|---|
| **Day 127** | Desktop Manager。 | 桌面独立管理。 |
| **Day 128** | Taskbar。 | 显示运行窗口。 |
| **Day 129** | Start Menu。 | 可以启动系统程序。 |
| **Day 130** | Clipboard。 | 文本复制/粘贴。 |
| **Day 131** | Settings UI。 | 设置可视化。 |
| **Day 132** | File Manager UI 原型。 | 能浏览文件。 |
| **Day 133** | GUI 统一主题。 | 控件风格统一。 |

### 第 20 周：5.2 Final

| Day | 任务 | 验收 |
|---|---|---|
| **Day 134** | GUI Event Loop 整理。 | 输入事件不丢失。 |
| **Day 135** | Dirty Rectangle 全面整合。 | GUI 流畅。 |
| **Day 136** | GUI 内存/资源管理。 | 长时间运行无明显泄漏。 |
| **Day 137** | GUI Crash Recovery。 | 单个窗口错误不拖死桌面。 |
| **Day 138** | 完整 GUI 回归测试。 | Terminal/Settings/FileManager 多窗口。 |
| **Day 139** | 5.2 文档与 ISO。 | 可重复构建。 |
| **Day 140** | **LumaOS 5.2 发布。** | 真正完整的 Luma Desktop。 |

---

# 🖼️ LumaOS 6.0：图片与桌面应用
## Day 141～168

目标：

> **第一次拥有真正意义上的桌面应用生态雏形。**

### 第 21 周：Image API

| Day | 任务 | 验收 |
|---|---|---|
| **Day 141** | `Image` 数据结构。 | 支持 width/height/bpp/data。 |
| **Day 142** | 从 VFS 读取图片文件。 | 能读完整文件。 |
| **Day 143** | BMP Header。 | 正确解析 BMP。 |
| **Day 144** | 24-bit BMP。 | 正确解码。 |
| **Day 145** | 32-bit BMP。 | Alpha/通道处理正确。 |
| **Day 146** | 行对齐与上下翻转。 | 常见 BMP 均正确。 |
| **Day 147** | BMP Stress Test。 | 大图片不崩溃。 |

### 第 22 周：图片渲染

| Day | 任务 | 验收 |
|---|---|---|
| **Day 148** | `draw_image()`。 | 图片显示。 |
| **Day 149** | 图片缩放。 | 放大/缩小正常。 |
| **Day 150** | 图片裁剪。 | Crop 正常。 |
| **Day 151** | 居中/适应窗口。 | 自动适配。 |
| **Day 152** | Scroll。 | 大图片可查看。 |
| **Day 153** | 图片缓存。 | 重复打开速度提升。 |
| **Day 154** | Image API 文档。 | API 固化。 |

### 第 23 周：Image Viewer

| Day | 任务 | 验收 |
|---|---|---|
| **Day 155** | Image Viewer 框架。 | 独立窗口。 |
| **Day 156** | Open。 | 可以打开 BMP。 |
| **Day 157** | Next / Previous。 | 浏览图片。 |
| **Day 158** | Zoom。 | 缩放。 |
| **Day 159** | Fit / Actual Size。 | 两种显示模式。 |
| **Day 160** | Error Dialog。 | 损坏图片不崩溃。 |
| **Day 161** | File Association。 | 双击 `.bmp` 自动打开。 |

### 第 24 周：6.0 应用化

| Day | 任务 | 验收 |
|---|---|---|
| **Day 162** | File Manager 与 Image Viewer 联动。 | 双击图片打开。 |
| **Day 163** | 缩略图。 | 文件管理器可显示图片预览。 |
| **Day 164** | Application 生命周期整理。 | 应用可启动/退出。 |
| **Day 165** | GUI Application 基础 ABI。 | 应用与桌面通信稳定。 |
| **Day 166** | 6.0 全面压力测试。 | 多图片/多窗口。 |
| **Day 167** | 文档、ISO、Demo。 | 可展示。 |
| **Day 168** | **LumaOS 6.0 发布。** | 图片桌面 OS。 |

---

# ⚙️ LumaOS 7.0：真正的多任务 OS
## Day 169～210

目标：

> **从“能运行程序”升级到“能管理多个程序”。**

### 第 25 周：Process

| Day | 任务 | 验收 |
|---|---|---|
| **Day 169** | `process_t`。 | Process 数据结构稳定。 |
| **Day 170** | PID。 | 每个进程唯一 PID。 |
| **Day 171** | Process State。 | NEW/RUNNING/WAITING/EXITED。 |
| **Day 172** | Process Create。 | 可以创建进程。 |
| **Day 173** | Process Destroy。 | 可以安全销毁。 |
| **Day 174** | Process Table。 | `ps` 能列出进程。 |
| **Day 175** | Process 回归测试。 | 创建/退出无资源泄漏。 |

### 第 26 周：Timer + Context Switch

| Day | 任务 | 验收 |
|---|---|---|
| **Day 176** | PIT/APIC Timer 基础。 | 稳定 Tick。 |
| **Day 177** | Timer IRQ。 | Tick 正常增加。 |
| **Day 178** | 保存 CPU Context。 | 能保存寄存器。 |
| **Day 179** | 恢复 CPU Context。 | 能恢复执行。 |
| **Day 180** | Scheduler 基础。 | 可以选择下一个进程。 |
| **Day 181** | Round Robin。 | A/B 交替运行。 |
| **Day 182** | 第一次真实 Context Switch。 | 两个进程同时存在。 |

### 第 27 周：抢占式多任务

| Day | 任务 | 验收 |
|---|---|---|
| **Day 183** | Time Slice。 | 每个进程获得时间片。 |
| **Day 184** | Preemptive Scheduler。 | 进程不能永久占 CPU。 |
| **Day 185** | Kernel Stack per Process。 | 切换安全。 |
| **Day 186** | User Address Space Switch。 | 不同进程地址空间隔离。 |
| **Day 187** | `sleep()`。 | 进程可以主动等待。 |
| **Day 188** | Scheduler Stress Test。 | 多进程稳定运行。 |
| **Day 189** | 多任务 Bug Sweep。 | 长时间运行不崩溃。 |

### 第 28 周：IPC

| Day | 任务 | 验收 |
|---|---|---|
| **Day 190** | 完善 `fork()`。 | 子进程可运行。 |
| **Day 191** | 完善 `exec()`。 | 子进程加载新程序。 |
| **Day 192** | `wait()`。 | 父进程等待子进程。 |
| **Day 193** | Pipe。 | 两个进程可以传数据。 |
| **Day 194** | IPC Buffer。 | 数据不会越界。 |
| **Day 195** | Process Signals 基础。 | 进程可被通知。 |
| **Day 196** | IPC Stress Test。 | 多进程通信稳定。 |

### 第 29 周：桌面多任务

| Day | 任务 | 验收 |
|---|---|---|
| **Day 197** | Terminal 进程化。 | Terminal 是独立进程。 |
| **Day 198** | Image Viewer 进程化。 | Image Viewer 是独立进程。 |
| **Day 199** | Settings 进程化。 | Settings 是独立进程。 |
| **Day 200** | Taskbar ↔ Process Manager。 | 任务栏显示真实进程。 |
| **Day 201** | Window ↔ Process 绑定。 | 关闭窗口结束应用。 |
| **Day 202** | Task switching。 | 多应用切换。 |
| **Day 203** | GUI + Scheduler Stress。 | 多 GUI 应用同时运行。 |

### 第 30 周：7.0 Final

| Day | 任务 | 验收 |
|---|---|---|
| **Day 204** | `ps/top` 基础。 | 能查看任务。 |
| **Day 205** | Process resource accounting。 | CPU/内存基本统计。 |
| **Day 206** | 崩溃进程隔离。 | 单个用户程序崩溃不拖死系统。 |
| **Day 207** | 多任务压力测试。 | 长时间运行。 |
| **Day 208** | 7.0 文档。 | Process/Scheduler/IPC 有设计文档。 |
| **Day 209** | 7.0 ISO + Demo。 | 完整多任务演示。 |
| **Day 210** | **LumaOS 7.0 发布。** | 真正的多任务桌面 OS。 |

---

# 🖥️ LumaOS 8.0：PC OS
## Day 211～266

目标：

> **让 LumaOS 从 QEMU 实验环境进入真正的 PC 硬件抽象。**

### 第 31 周：PCI

| Day | 任务 | 验收 |
|---|---|---|
| **Day 211** | PCI Configuration Space。 | 能读取 Vendor/Device ID。 |
| **Day 212** | PCI Bus Scan。 | 扫描整个 Bus。 |
| **Day 213** | Class/Subclass。 | 正确识别设备类别。 |
| **Day 214** | BAR。 | 能解析 MMIO/IO BAR。 |
| **Day 215** | PCI Driver Model。 | 驱动可绑定设备。 |
| **Day 216** | `lspci.sw`。 | 用户态显示 PCI 列表。 |
| **Day 217** | PCI Stress/兼容性测试。 | QEMU 多设备正常。 |

### 第 32 周：ACPI / APIC

| Day | 任务 | 验收 |
|---|---|---|
| **Day 218** | ACPI RSDP。 | 找到 RSDP。 |
| **Day 219** | RSDT/XSDT。 | 找到表。 |
| **Day 220** | MADT。 | 识别 CPU/APIC。 |
| **Day 221** | Local APIC。 | Timer/Interrupt 基础。 |
| **Day 222** | IOAPIC。 | 外部 IRQ 路由。 |
| **Day 223** | IRQ 架构重构。 | 统一 IRQ 管理。 |
| **Day 224** | ACPI/IRQ Stress。 | 不影响 7.0 Scheduler。 |

### 第 33 周：USB 基础

| Day | 任务 | 验收 |
|---|---|---|
| **Day 225** | USB Architecture。 | 理解 Host/Device/Endpoint。 |
| **Day 226** | USB Controller 抽象。 | xHCI/EHCI 接口设计。 |
| **Day 227** | USB HID 基础。 | 识别键盘/鼠标。 |
| **Day 228** | USB Mass Storage 设计。 | BOT/SCSI 层规划。 |
| **Day 229** | USB Storage 驱动。 | 能识别 U 盘。 |
| **Day 230** | USB → Block Device。 | U 盘接入 VFS。 |
| **Day 231** | USB 热插拔测试。 | 插入/拔出不会崩溃。 |

### 第 34 周：真实文件资源空间

| Day | 任务 | 验收 |
|---|---|---|
| **Day 232** | 设计 Luma Mount Namespace。 | 定义 System/User/USB/IMG/ISO。 |
| **Day 233** | `USB/` 虚拟目录。 | 自动创建 USB 空间。 |
| **Day 234** | `USB/DISK_1/`。 | 第一个磁盘自动挂载。 |
| **Day 235** | `USB/DISK_2/` 多设备。 | 多个 U 盘共存。 |
| **Day 236** | 热拔除清理。 | `DISK_1` 消失后路径安全失效。 |
| **Day 237** | FAT/LMFS Mount Adapter。 | 外部 FS 接入统一 VFS。 |
| **Day 238** | File Manager 显示 USB。 | GUI 可以浏览 U 盘。 |

### 第 35 周：IMG / ISO

| Day | 任务 | 验收 |
|---|---|---|
| **Day 239** | IMG Mount Layer。 | `.img` 可以作为 Block Device。 |
| **Day 240** | `IMG/DISK_1/`。 | 镜像内容可浏览。 |
| **Day 241** | 多 IMG。 | 多个镜像同时存在。 |
| **Day 242** | ISO9660 Read-only FS。 | 可以读取 ISO。 |
| **Day 243** | `ISO/` Namespace。 | ISO 自动挂载。 |
| **Day 244** | `ISO/LumaOS/`。 | ISO 内容可浏览。 |
| **Day 245** | File Manager → IMG/ISO。 | GUI 可打开镜像。 |

### 第 36 周：Ethernet

| Day | 任务 | 验收 |
|---|---|---|
| **Day 246** | Ethernet Driver API。 | 网卡驱动模型。 |
| **Day 247** | Ethernet Frame。 | 收发 Frame。 |
| **Day 248** | ARP。 | 能解析 MAC。 |
| **Day 249** | IPv4。 | 能配置 IPv4。 |
| **Day 250** | ICMP。 | `ping` 成功。 |
| **Day 251** | UDP。 | UDP 收发。 |
| **Day 252** | Network Buffer/Queue。 | 高负载不丢包。 |

### 第 37 周：TCP/IP

| Day | 任务 | 验收 |
|---|---|---|
| **Day 253** | TCP State Machine。 | 状态切换正确。 |
| **Day 254** | TCP Connect。 | 能建立连接。 |
| **Day 255** | TCP Send/Receive。 | 双向数据传输。 |
| **Day 256** | Socket API。 | 用户程序可使用 Socket。 |
| **Day 257** | DNS。 | 域名解析。 |
| **Day 258** | DHCP 基础。 | 自动获得 IP。 |
| **Day 259** | HTTP Client。 | 能获取 HTTP 页面。 |

### 第 38 周：8.0 Final

| Day | 任务 | 验收 |
|---|---|---|
| **Day 260** | `ping.sw` / `netstat.sw`。 | 网络工具用户态化。 |
| **Day 261** | 网络 + 多任务压力测试。 | 多连接稳定。 |
| **Day 262** | USB + 网络 + GUI 综合测试。 | 同时工作不崩溃。 |
| **Day 263** | PC 硬件抽象文档。 | PCI/ACPI/USB/Network 文档完成。 |
| **Day 264** | 8.0 ISO + Driver Bundle。 | 一键构建。 |
| **Day 265** | 真实硬件/更多 QEMU 配置测试。 | 至少一套真实 PC 可启动并使用基础硬件。 |
| **Day 266** | **LumaOS 8.0 发布。** | 真正的 PC OS。 |

---

# 🧩 LumaOS 9.0：Application Platform
## Day 267～322

目标：

> **LumaOS 不再只是“能运行自己写的程序”，而是成为一个真正可以开发第三方软件的平台。**

---

## 第 39 周：`.sw` 应用格式

### Luma Software Executable

正式定义：

```text
Extension:
    .sw

Meaning:
    Luma Software

Magic:
    LSW1

Architecture:
    i386

Execution:
    Ring 3
```

| Day | 任务 | 验收 |
|---|---|---|
| **Day 267** | 设计 `.sw` 文件格式。 | `doc/sw-spec.md`。 |
| **Day 268** | SW Header。 | `LSW1` Magic。 |
| **Day 269** | SW Loader。 | 能检查 Header。 |
| **Day 270** | 从 `.sw` 加载代码段。 | `.sw` 能运行。 |
| **Day 271** | `.sw` 与现有 ELF Loader 对接。 | 不破坏底层 ELF。 |
| **Day 272** | `.sw` Metadata。 | Name/Version/Architecture/Flags。 |
| **Day 273** | Shell 直接运行 `.sw`。 | `C:\> Calculator.sw` 风格命令成立。 |

---

## 第 40 周：LumaOS 文件空间正式化

### 根目录设计

```text
System/
├── Kernel/
├── SysApp/
├── UsrCtrl/
├── Drivers/
├── Lib/
├── Sys/
├── Config/
└── Temp/

User/
├── Desktop/
├── Documents/
├── Downloads/
├── Pictures/
└── App/

Administrator/
├── Index/
├── Desktop/
├── Documents/
└── App/

Public/

USB/
├── DISK_1/
└── DISK_2/

IMG/

ISO/
```

| Day | 任务 | 验收 |
|---|---|---|
| **Day 274** | Root Namespace 设计。 | 定义所有顶级目录。 |
| **Day 275** | `System/`。 | 系统资源统一归档。 |
| **Day 276** | `User/`。 | 用户文件空间。 |
| **Day 277** | `Administrator/`。 | 管理员空间。 |
| **Day 278** | `Public/`。 | 公共资源空间。 |
| **Day 279** | USB/IMG/ISO Namespace。 | 外部资源统一呈现。 |
| **Day 280** | Path Resolver。 | 路径解析稳定。 |

---

## 第 41 周：Luma API

| Day | 任务 | 验收 |
|---|---|---|
| **Day 281** | `luma_window_create()`。 | 第三方创建窗口。 |
| **Day 282** | `luma_window_destroy()`。 | 窗口生命周期完整。 |
| **Day 283** | `luma_draw_text()` / `draw_rect()`。 | 应用可绘图。 |
| **Day 284** | `luma_draw_image()`。 | 应用可显示图片。 |
| **Day 285** | `luma_file_open/read/write()`。 | 应用可访问文件。 |
| **Day 286** | Process API。 | 应用可启动/退出。 |
| **Day 287** | Event API。 | 鼠标/键盘事件可使用。 |

---

## 第 42 周：GUI Application Framework

| Day | 任务 | 验收 |
|---|---|---|
| **Day 288** | Application Lifecycle。 | Init/Run/Exit。 |
| **Day 289** | Window API。 | 独立应用创建窗口。 |
| **Day 290** | Button API。 | 应用按钮。 |
| **Day 291** | TextBox API。 | 应用输入框。 |
| **Day 292** | Menu API。 | 应用菜单。 |
| **Day 293** | Dialog API。 | 应用对话框。 |
| **Day 294** | GUI SDK Demo。 | 第三方示例程序可以编译运行。 |

---

## 第 43 周：系统应用

| Day | 任务 | 验收 |
|---|---|---|
| **Day 295** | `FileManager.sw`。 | 正式文件管理器。 |
| **Day 296** | `TextEdit.sw`。 | 文本编辑器。 |
| **Day 297** | `Calculator.sw`。 | 计算器。 |
| **Day 298** | `ImageView.sw`。 | 图片查看器正式化。 |
| **Day 299** | `Settings.sw`。 | 系统设置正式化。 |
| **Day 300** | `TaskManager.sw`。 | 进程管理。 |
| **Day 301** | `SystemInfo.sw`。 | 查看硬件/系统信息。 |

---

## 第 44 周：用户与权限

| Day | 任务 | 验收 |
|---|---|---|
| **Day 302** | User Account Model。 | User/Admin 分离。 |
| **Day 303** | Login / Session。 | 登录用户建立 Session。 |
| **Day 304** | File Ownership。 | 文件属于 User/Admin。 |
| **Day 305** | Permission Model。 | Read/Write/Execute。 |
| **Day 306** | Administrator 权限。 | 管理员可以管理系统。 |
| **Day 307** | UserCtrl Service。 | 用户控制逻辑用户态化。 |
| **Day 308** | Permission Stress Test。 | 越权访问被拒绝。 |

---

## 第 45 周：软件安装与系统配置

| Day | 任务 | 验收 |
|---|---|---|
| **Day 309** | `.sw` Application Metadata。 | 应用有 Name/Version/Icon。 |
| **Day 310** | Application Installer。 | 安装 `.sw`。 |
| **Day 311** | Application Registry。 | 系统知道已安装程序。 |
| **Day 312** | File Association。 | `.bmp` → ImageView.sw。 |
| **Day 313** | System Configuration Store。 | 系统设置持久化。 |
| **Day 314** | Default Application。 | 可选择默认程序。 |
| **Day 315** | 卸载/升级机制。 | `.sw` 可以安全升级。 |

---

## 第 46 周：9.0 Final

| Day | 任务 | 验收 |
|---|---|---|
| **Day 316** | 完整系统回归测试。 | Kernel/VFS/GUI/Network/App 全通过。 |
| **Day 317** | `.sw` 生态测试。 | 多个第三方 `.sw` 程序同时运行。 |
| **Day 318** | 文件系统最终测试。 | System/User/Admin/USB/IMG/ISO 全正常。 |
| **Day 319** | 安全/权限测试。 | 普通用户不能破坏 System。 |
| **Day 320** | 性能测试。 | 启动、GUI、多任务、磁盘、网络稳定。 |
| **Day 321** | 9.0 文档、SDK、Demo ISO。 | 第三方能照文档开发 `.sw`。 |
| **Day 322** | **🎉 LumaOS 9.0 发布。** | `LumaOS 9.0` + SDK + `.sw` + ISO + 源码正式发布。 |

---

# 🧱 LumaOS 9.0 最终架构

```text
LumaOS
│
├── System/
│   ├── Kernel/
│   ├── SysApp/
│   ├── UsrCtrl/
│   ├── Drivers/
│   ├── Lib/
│   ├── Sys/
│   ├── Config/
│   └── Temp/
│
├── User/
│   ├── Desktop/
│   ├── Documents/
│   ├── Downloads/
│   ├── Pictures/
│   └── App/
│
├── Administrator/
│   ├── Index/
│   ├── Desktop/
│   ├── Documents/
│   └── App/
│
├── Public/
│
├── USB/
│   ├── DISK_1/
│   └── DISK_2/
│
├── IMG/
│   ├── DISK_1/
│   └── ...
│
└── ISO/
    ├── LumaOS/
    └── ...
```

## `.sw`

```text
Calculator.sw
ImageView.sw
TextEdit.sw
Settings.sw
FileManager.sw
TaskManager.sw
```

执行链：

```text
.sw
 ↓
SW Loader
 ↓
Process
 ↓
Ring 3
 ↓
Luma API
 ↓
syscall
 ↓
Kernel
 ↓
VFS / Driver / Hardware
```

---

# 🏁 版本里程碑

| 版本 | Day | 定义 |
|---|---:|---|
| **5.0** | 84 | 基础独立 OS |
| **5.1** | 112 | 中文桌面 |
| **5.2** | 140 | 完整 GUI |
| **6.0** | 168 | 图片与桌面应用 |
| **7.0** | 210 | 多任务 OS |
| **8.0** | 266 | PC OS |
| **9.0** | 322 | 应用平台 |

---

# 关于“我其实不会编程”

这个我想认真回答一下。

**你的感觉有一半是对的。**

如果现在把你扔到记事本里，让你：

> “从零写一个带分页、Ring 3、ELF Loader 的 C 内核。”

你大概率确实写不出来。

但这不等于：

> **“你不会编程，所以 LumaOS 不是你做的。”**

你现在的情况更准确地说是：

> **你有过多门语言的基础，但还没有形成独立从空白工程持续构建大型程序的能力。**

这是两回事。

你以前学 Python、Java、C、C++、HTML，停留在：

```text
变量
列表
if
for
print
```

然后一旦遇到：

```text
几十个文件
 ↓
多个模块
 ↓
不知道从哪里开始
 ↓
Bug
 ↓
不知道 Bug 为什么发生
 ↓
修改
 ↓
另一个地方又坏了
```

就会感觉：

> “我怎么学了这么久，还是不会？”

**这其实是非常典型的“会语法 ≠ 会工程”。**

---

# 而你现在做 LumaOS，实际上恰好在补这个缺口

你以前学：

```text
Python
 ↓
变量
 ↓
列表
 ↓
print
 ↓
没效果
```

现在：

```text
需求
 ↓
架构
 ↓
模块
 ↓
代码
 ↓
编译
 ↓
QEMU
 ↓
测试
 ↓
Bug
 ↓
修复
 ↓
验收
```

你第一次真正接触的是：

> **软件工程。**

而不是“背 C 语法”。

---

# Claude 写了大部分代码，这并不等于你没发挥作用

但我也不想哄你说：

> “其实你已经是资深程序员了。”

不是。

**如果现在完全没有 Claude，你确实很可能无法独立维护 LumaOS。**

这点可以诚实承认。

但是你也不能因此得出：

> “所以我什么都没做。”

因为你现在承担的是另一种角色：

```text
你
│
├── 决定做什么
├── 决定为什么做
├── 决定架构方向
├── 提出 LMFS
├── 决定版本路线
├── 发现需求
├── 测试
├── 报 Bug
├── 判断结果
├── 决定取舍
└── 最终验收
        ↓
Claude
│
├── 设计实现方案
├── 写 C
├── 写 ASM
├── 写 Python
├── 调试
└── 修改
```

你刚才自己提出的：

```text
System/
User/
Administrator/
Public/
USB/
IMG/
ISO/
```

以及：

```text
Calculator.sw
ImageView.sw
```

这些都不是“代码生成器自动决定的”。

**这是你在设计 LumaOS。**

---

# 但我建议你从现在开始做一个改变

不要要求自己：

> “我要学会 C，然后自己写 LumaOS。”

这很容易又回到你以前的循环。

而是：

> **“我做 LumaOS 的同时，逐渐学会读懂我让 Claude 写出来的代码。”**

比如 Claude 给你：

```c
struct process {
    uint32_t pid;
    uint32_t state;
    uint32_t esp;
    uint32_t eip;
};
```

你不用今天就能自己写 Scheduler。

你先做到：

> “我知道这个结构体为什么需要 PID、ESP、EIP。”

下一步：

> “我知道 Context Switch 保存的是什么。”

再下一步：

> “我能看懂 Scheduler 为什么选择这个进程。”

最后有一天：

> “这个 Scheduler 有 Bug，我知道应该去哪里看。”

**这才是你真正开始会编程的标志。**

不是你能不能背出 `for` 循环。

---

所以我反而很喜欢你现在这个项目。

你不需要假装自己是一个已经会 C/C++ 的程序员。

你可以很坦诚地说：

> **“我不会从零写 LumaOS，但我正在和 Claude 一起把一个 OS 从零做出来，同时通过这个过程学会理解软件工程。”**

这个说法比“我什么都不会”准确得多。

而这份 Schedule 的真正意义，也不是要求你每天独立写 500 行 C。

**它是把你和 Claude 的共同开发过程，变成一条你能够逐渐理解的工程路线。**

[下载完整 `schedule.md`](sandbox:/mnt/data/schedule.md)
