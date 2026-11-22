\# 📅 LumaOS 5 - 文件系统与 ISO 构建 三个月总纲



\## 🏛️ 核心架构变更（前三天必须定好）



| 新增目录 | 职责 |

| :--- | :--- |

| `kernel/drivers/block/` | IDE 硬盘驱动、分区解析（MBR） |

| `kernel/fs/vfs/` | 虚拟文件系统开关（挂载、打开、读、读目录） |

| `kernel/fs/fat/` | FAT12/16/32 具体实现 |

| `kernel/fs/lmfs/` | 你的专属 LMFS 实现 |

| `tools/` | `mkfs.lmfs`（用户态格式化工具，用 Python 写） |

| `iso/` | ISO 构建临时目录和脚本 |



\*\*内核加载限制紧急调整（第 1 天必须做）\*\*：

\- `loader.asm`：`mov al, 128` → `mov al, 255`（读 255 扇区 ≈ 127.5 KB）

\- `build.sh`：`MAX\_LOAD=65536` → `MAX\_LOAD=131072`

\- 这样你加完所有 FS 代码（约 40\~50 KB）后，总大小仍能控制在 100 KB 以内。



\---



\## 📆 第一月：地基（磁盘驱动 + VFS + FAT12）—— 第 1\~4 周



\### 第 1 周：脱离 BIOS，自己掌控硬盘（IDE PIO 驱动）



| 日期 | 任务 | 产出/验收标准 |

| :--- | :--- | :--- |

| \*\*Day 1\*\* | 修改 `loader.asm` 和 `build.sh` 解除 64KB 限制；修改 `run.sh` 将 `if=floppy` 改为 `if=ide`（让软盘镜像被识别为 IDE 主盘）。 | 重新编译后镜像体积允许超过 64KB，QEMU 启动如常。 | <已经完成！>

| \*\*Day 2\*\* | 创建 `kernel/drivers/block/ide.h` 和 `ide.c`。实现 `ide\_wait()`（轮询 0x1F7 状态寄存器）。 | 编写一个 `ide\_test()` 在 `kmain` 中调用，能读到状态寄存器非 0xFF。 | <已经完成！>

| \*\*Day 3\*\* | 实现 `ide\_read\_sectors(lba, count, buf)`（使用 PIO 模式，`insw` 指令）。 | 在 `kmain` 中读取 LBA 0（MBR），用 `console\_write\_hex()` 打印前 64 字节，屏幕上应看到 `EB 3C 90`（FAT 引导跳转）。 | <已经完成！>

| \*\*Day 4\*\* | 实现 `ide\_write\_sectors(lba, count, buf)`（只读模式先通过，写入可先置空）。 | 能向 LBA 1 写入一个测试字符串，再读回来对比一致。 | <已经完成！>

| \*\*Day 5\*\* | 解析 MBR（LBA 0），扫描分区表（偏移 0x1BE）。识别第一个活动分区（Type 0x0C = FAT32 LBA，0x06 = FAT16，0x01 = FAT12）。 | 在终端打印出分区起始 LBA 和大小。 | <已经完成！>

| \*\*Day 6\*\* | 封装块设备层：`struct block\_device` 包含 `read(lba, count, buf)` 函数指针。 | 上层代码不再直接调用 ide 函数，统一走设备结构体。 | <已经完成！>

| \*\*Day 7\*\* | \*\*周复盘\*\*：整理 IDE 驱动代码，消除硬编码，增加超时保护（防止死循环）。 | 写一个 `ide\_dump\_info()` 打印硬盘型号（IDENTIFY 命令 0xEC）。 | <已经完成！>



\---



\### 第 2 周：VFS 抽象与 FAT12 只读挂载



| 日期 | 任务 | 产出/验收标准 |

| :--- | :--- | :--- |

| \*\*Day 8\*\* | 创建 `kernel/fs/vfs/vfs.h`。定义 `struct file`、`struct vfs\_node`、`struct filesystem`。定义函数指针：`open`、`read`、`readdir`。 | 头文件编译通过。 | <已经完成！>

| \*\*Day 9\*\* | 创建 `kernel/fs/fat/fat12.h`。定义 BPB 结构体（`bpb\_12\_t`）和目录项结构（`fat\_dirent\_t`）。 | 写一个 `fat12\_parse\_bpb(buf)`，能算出 RootDir 扇区数、Data 起始扇区。 | <已经完成！>

| \*\*Day 10\*\* | 实现 `fat12\_mount(block\_dev)`：读取引导扇区，检查 `BS\_FileSysType` 是否为 "FAT12"。 | 挂载成功返回 0，失败返回 -1，在终端打印 "FAT12 mounted"。 |

| \*\*Day 11\*\* | 实现 FAT12 簇链遍历函数 `fat12\_next\_cluster(cluster)`（处理 1.5 字节对齐，注意奇偶簇）。 | 给定根目录的起始簇（通常为 0），能遍历打印出前 10 个簇号。 |

| \*\*Day 12\*\* | 实现 `fat12\_readdir(root\_node)`：读取根目录区（Root Directory Region），遍历每 32 字节条目。提取文件名（去掉空格）、属性、起始簇、文件大小。 | 在终端列出根目录下所有文件名（至少能看到 `KERNEL  BIN` 和 `LOADER  BIN`）。 |

| \*\*Day 13\*\* | 将 FAT12 挂接到 VFS：注册 `fat12\_filesystem\_ops`。实现 `vfs\_mount("/dev/hda")`。 | 调用 `vfs\_mount` 后，`vfs\_root` 指针指向 FAT 根目录。 |

| \*\*Day 14\*\* | \*\*整合 Shell\*\*：新增命令 `cmd\_ls`（调用 `vfs\_readdir` 列出文件）。 | 在终端输入 `ls`，能看到 FAT 镜像里的所有文件。 |



\---



\### 第 3 周：FAT16 / FAT32 支持 + 文件读取（cat）



| 日期 | 任务 | 产出/验收标准 |

| :--- | :--- | :--- |

| \*\*Day 15\*\* | 重构 FAT 通用层：分离 FAT12/16/32 的共性（BPB 解析、目录项结构），创建 `fat\_common.c`。 | 一个 `fat\_mount()` 根据 BPB 的 `BytesPerSector` 和 `TotalSectors` 自动判断类型。 |

| \*\*Day 16\*\* | 实现 FAT16 解析（`FAT16` 的根目录在数据区之前，簇号 16 位）。 | 挂载一个 FAT16 镜像（可以用 `mkfs.fat -F 16` 制作），`ls` 能正确列出文件。 |

| \*\*Day 17\*\* | 实现 FAT32 解析（注意 `BPB\_RootClus`，根目录是一个普通簇链，且存在 `FSInfo` 结构）。 | 挂载 FAT32 镜像（`mkfs.fat -F 32`），`ls` 能工作。 |

| \*\*Day 18\*\* | 实现 `fat\_open(path)`：根据路径（如 `/KERNEL.BIN`）遍历目录项，返回文件的起始簇号。 | 对根目录文件能正确返回簇号。 |

| \*\*Day 19\*\* | 实现 `fat\_read(file, buf, size)`：根据起始簇，依次读取簇链，将数据拷贝到 buf。 | 实现 `cmd\_cat /KERNEL.BIN`，能在终端打印出内核二进制的开头部分（乱码也行，证明读到了数据）。 |

| \*\*Day 20\*\* | 增加文件偏移指针 `file->pos`，支持多次读取。 | 连续调用两次 `read`，第二次能接着上次位置读。 |

| \*\*Day 21\*\* | \*\*周复盘\*\*：添加错误处理（文件不存在、簇链断裂）。关闭文件功能（`fat\_close`）。 | 编写测试用例：`cat` 一个根目录下的文本文件（用 Python 提前写入）。 |



\---



\### 第 4 周：FAT 写入支持（创建、删除、截断）



| 日期 | 任务 | 产出/验收标准 |

| :--- | :--- | :--- |

| \*\*Day 22\*\* | 实现 `fat\_find\_free\_cluster()`：扫描 FAT 表，返回第一个值为 0 的簇。 | 测试打印空闲簇号。 |

| \*\*Day 23\*\* | 实现 `fat\_alloc\_cluster()`：分配一个新簇，并更新 FAT 表（写回磁盘）。 | 分配成功后，再次读取 FAT 对应位置值变为 0xFFF（FAT12）或 0xFFFF。 |

| \*\*Day 24\*\* | 实现 `fat\_create\_file(name)`：在根目录区找一个空条目（首字节 0x00 或 0xE5），填入文件名、起始簇 0、大小 0。 | 在 Shell 中输入 `touch test.txt`，重新 `ls` 能看到它。 |

| \*\*Day 25\*\* | 实现 `fat\_write(file, buf, len)`：处理簇链扩展（如果当前簇链不够，追加新簇）。 | 向 `test.txt` 写入 "Hello LMFS"，再读出来验证。 |

| \*\*Day 26\*\* | 实现 `fat\_delete\_file(name)`：将目录项首字节置为 0xE5，并释放 FAT 簇链（置 0）。 | `rm test.txt` 后 `ls` 消失，且空闲簇数增加。 |

| \*\*Day 27\*\* | 实现文件截断（`fat\_truncate`），支持清空文件内容。 | 只保留目录项，释放所有数据簇。 |

| \*\*Day 28\*\* | \*\*月总结\*\*：编写 `cmd\_echo > file.txt`（重定向输出）。 | 能通过 Shell 创建并写入文件。 |



\---



\## 📆 第二月：专属 LMFS + 用户态程序加载 —— 第 5\~8 周



\### 第 5 周：LMFS 磁盘格式设计 + mkfs 工具



| 日期 | 任务 | 产出/验收标准 |

| :--- | :--- | :--- |

| \*\*Day 29\*\* | 设计 LMFS 超级块布局（位于 LBA 1）：魔数 `0x4C4D4653`、版本号、inode 数量、数据块数量、inode 位图起始 LBA、数据块位图起始 LBA。 | 输出设计文档到 `doc/lmfs.txt`。 |

| \*\*Day 30\*\* | 设计 inode 结构（`lmfs\_inode\_t`）：包含文件大小、创建时间、直接块指针 8 个 + 间接块指针 1 个 + 二级间接块 1 个（支持大文件）。 | 定义块大小 1024 字节（适合软盘/小硬盘）。 |

| \*\*Day 31\*\* | 用 Python 编写 `tools/mkfs.lmfs`：创建空白镜像，写入超级块，初始化所有位图为全 0，创建根目录 inode（inode 0）。 | 生成 `lmfs.img`，用十六进制查看器检查魔数。 |

| \*\*Day 32\*\* | 在 C 端实现 `lmfs\_mount(dev)`：读取超级块，检查魔数，初始化位图缓存（读入内存）。 | 挂载成功，打印 "LMFS 1.0 mounted, 1024 inodes, 2048 blocks"。 |

| \*\*Day 33\*\* | 实现 `lmfs\_read\_inode(inum)`：根据超级块中的 inode 表起始 LBA，计算偏移读取 inode。 | 成功读取根目录 inode（inum=0）。 |

| \*\*Day 34\*\* | 实现 `lmfs\_readdir(root\_inode)`：遍历根目录的数据块（每个目录项 32 字节：文件名 24 字节 + 文件类型 1 字节 + inode 号 4 字节 + 保留）。 | 终端能列出根目录下的文件（目前只有 `.` 和 `..`）。 |

| \*\*Day 35\*\* | \*\*周复盘\*\*：将 LMFS 注册到 VFS，支持 `mount -t lmfs /dev/hdb`。 | 实现多文件系统同时挂载（FAT 和 LMFS 共存）。 |



\---



\### 第 6 周：LMFS 文件读写 + 目录操作



| 日期 | 任务 | 产出/验收标准 |

| :--- | :--- | :--- |

| \*\*Day 36\*\* | 实现块分配器：`lmfs\_alloc\_block()`，扫数据块位图找 0 置 1，返回 LBA。 | 分配 10 个连续块，位图对应位全变 1。 |

| \*\*Day 37\*\* | 实现 `lmfs\_create\_inode(type)`：扫 inode 位图分配新 inode，初始化指针全 0。 | 返回新的 inode 号。 |

| \*\*Day 38\*\* | 实现 `lmfs\_create(name, type)`：在父目录中创建目录项，分配 inode。 | `touch l.txt` 能创建空文件。 |

| \*\*Day 39\*\* | 实现 `lmfs\_write(inode, offset, buf, len)`：处理直接块和间接块（如果 offset > 8\*1024，跳转间接块）。 | 写入 10KB 数据，验证间接块正确分配。 |

| \*\*Day 40\*\* | 实现 `lmfs\_read(inode, offset, buf, len)`：支持从任意偏移读取。 | `cat bigfile.txt` 能完整输出。 |

| \*\*Day 41\*\* | 实现 `lmfs\_delete(inode)`：释放所有数据块，释放 inode 位图，清空父目录项。 | `rm` 后空间回收。 |

| \*\*Day 42\*\* | \*\*周复盘\*\*：添加目录操作（`mkdir`），支持多级路径（如 `/home/user/a.txt`）。 | 能在 LMFS 分区创建多级目录。 |



\---



\### 第 7 周：ELF 加载器（用户态程序）



| 日期 | 任务 | 产出/验收标准 |

| :--- | :--- | :--- |

| \*\*Day 43\*\* | 学习 ELF 32-bit 格式（`e\_ident`、`e\_entry`、程序头表）。 | 写一个最简单的 C 程序 `hello.c`，用 `i386-elf-gcc` 编译成 ELF。 |

| \*\*Day 44\*\* | 实现 `elf\_load(path)`：读取文件头部，检查 `e\_magic` = 0x464C457F。 | 能打印出入口点地址。 |

| \*\*Day 45\*\* | 解析程序头（`PT\_LOAD` 段），根据 `p\_vaddr` 和 `p\_offset` 将段加载到内存。 | 用 `memcpy` 将 ELF 文件中的代码段拷贝到目标虚拟地址（目前仍为物理地址 0x800000）。 |

| \*\*Day 46\*\* | 实现 `elf\_run(entry)`：设置 `eip` 跳转到入口点（不返回）。 | 执行 `hello` 程序，在 VGA 上看到 "Hello from user!"（注意：此时仍在内核态）。 |

| \*\*Day 47\*\* | 增加用户态栈（`esp` 指向 0x900000）和简单的返回方式（`int 0x80` 系统调用退出）。 | 程序运行后能返回 Shell。 |

| \*\*Day 48\*\* | \*\*Shell 集成\*\*：`cmd\_run /hello.elf`，加载并执行。 | 用户在终端输入 `run hello` 即可运行程序。 |

| \*\*Day 49\*\* | \*\*周复盘\*\*：增加参数传递（`argc` 和 `argv`）。 | `run hello luma` 能在程序内打印参数。 |



\---



\### 第 8 周：系统调用（syscall）与用户态内存保护（分页）准备



| 日期 | 任务 | 产出/验收标准 |

| :--- | :--- | :--- |

| \*\*Day 50\*\* | 定义系统调用号（`SYS\_WRITE`、`SYS\_EXIT`）。在 `interrupts.c` 中增加 `int 0x80` 处理。 | 用户程序调用 `int 0x80` 能陷入内核。 |

| \*\*Day 51\*\* | 实现 `sys\_write`：将用户态缓冲区内容输出到终端（仅做演示）。 | `hello` 程序通过系统调用输出 "Hello"。 |

| \*\*Day 52\*\* | 实现 `sys\_exit`：终止当前任务并返回 Shell（清理现场）。 | 程序运行结束不崩溃。 |

| \*\*Day 53\*\* | \*\*分页入门\*\*：启用 CR0.PG，创建内核页表（恒等映射 0-16MB）。 | 启用分页后系统依然正常运行（这是最难的一步，可能需要回退调试）。 |

| \*\*Day 54\*\* | 实现用户态页表：为 ELF 程序创建独立的地址空间（将物理地址 0x800000 映射到虚拟地址 0x400000）。 | 程序在虚拟地址空间运行。 |

| \*\*Day 55\*\* | 使用 `iret` 切换至用户态（设置 CS=0x1B, SS=0x23, EFLAGS=0x202）。 | 用户程序运行在低特权级，无法随意修改内核。 |

| \*\*Day 56\*\* | \*\*周复盘\*\*：修复用户态程序运行后的键盘中断返回。 | 系统完全稳定，用户程序可多次运行。 |



\---



\## 📆 第三月：ISO 构建、测试、优化、文档 —— 第 9\~12 周



\### 第 9 周：构建 El Torito 可启动 ISO



| 日期 | 任务 | 产出/验收标准 |

| :--- | :--- | :--- |

| \*\*Day 57\*\* | 研究 ISO 9660 和 El Torito 规范。 | 理解启动目录（Boot Catalog）结构。 |

| \*\*Day 58\*\* | 写一个 Python 脚本 `tools/mkiso.py`：将 `lumaos-4.1.img`（包含 MBR+Loader+Kernel）作为“软盘模拟”映像嵌入 ISO。 | 使用 `xorriso -as mkisofs -b lumaos.img -no-emul-boot ...` 生成 ISO。 |

| \*\*Day 59\*\* | \*\*改用“无模拟”（No Emulation）模式\*\*：将 `loader.bin` 作为启动映像（`-no-emul-boot`），跳过软盘模拟，直接从 CD 加载。 | 修改 `loader.asm` 支持从 CD 读取（使用 BIOS `int 13h` 的扩展读功能 `ah=42h` 读取 LBA）。 |

| \*\*Day 60\*\* | 修改 `build.sh`：编译完成后自动调用 `mkiso.py`，生成 `lumaos.iso`。 | 执行 `./build.sh` 后直接出现 `.iso` 文件。 |

| \*\*Day 61\*\* | 测试 ISO 启动：`qemu-system-i386 -cdrom lumaos.iso -boot d`。 | 成功进入 GUI 桌面，`ls` 能列出光盘上的文件（如果有）。 |

| \*\*Day 62\*\* | 将 LMFS 镜像作为第二个文件放入 ISO 的 `/boot/lmfs.img`。 | 内核启动后挂载这个文件（通过 loop 设备抽象）。 |

| \*\*Day 63\*\* | \*\*周复盘\*\*：实现从 CD-ROM 加载内核模块（例如字体文件、配置文件）。 | 系统完全摆脱软盘镜像限制。 |



\---



\### 第 10 周：压力测试与性能优化



| 日期 | 任务 | 产出/验收标准 |

| :--- | :--- | :--- |

| \*\*Day 64\*\* | 写一个测试程序 `fs\_test.elf`：创建 100 个文件，写入随机数据，再读取校验。 | 所有文件校验通过，无内存泄漏。 |

| \*\*Day 65\*\* | 优化 FAT 簇链读取（预读缓存），提高 `cat` 大文件的速度。 | 读取 1MB 文件的时间从 2 秒降至 0.5 秒。 |

| \*\*Day 66\*\* | 优化 LMFS 位图查找（使用 `bsf` 指令快速找空闲位）。 | 分配速度提升。 |

| \*\*Day 67\*\* | 修复所有已知 bug（特别是 FAT32 长文件名解析导致的崩溃）。 | 使用 Valgrind（或手动检查）确保无内存越界。 |

| \*\*Day 68\*\* | 增加 `df` 命令：显示挂载分区的总块数、已用块数、可用块数。 | 终端输出准确。 |

| \*\*Day 69\*\* | 增加 `chdir` / `pwd` 支持，让 Shell 支持相对路径。 | `cd /home` 切换目录，`pwd` 显示当前路径。 |

| \*\*Day 70\*\* | \*\*周复盘\*\*：统一错误码（`-ENOENT`，`-EIO`）。 | 用户程序能通过返回值判断错误类型。 |



\---



\### 第 11 周：用户态库（libc 基础）



| 日期 | 任务 | 产出/验收标准 |

| :--- | :--- | :--- |

| \*\*Day 71\*\* | 创建 `user/lib/stdio.c`：实现 `putchar`（通过 `int 0x80`）。 | 用户程序能调用 `putchar('A')`。 |

| \*\*Day 72\*\* | 实现 `printf`（格式化输出，调用 `sys\_write`）。 | `printf("Number: %d", 42)` 输出正确。 |

| \*\*Day 73\*\* | 实现 `malloc` / `free`（基于内核的 `sbrk` 系统调用）。 | 用户程序能动态分配内存。 |

| \*\*Day 74\*\* | 实现 `fopen` / `fread` / `fclose`（调用 VFS 系统调用）。 | 用户程序能读写文件。 |

| \*\*Day 75\*\* | 编写 `user/programs/` 目录，创建 `cat.c`、`ls.c` 作为用户态程序。 | 内核 Shell 的内置命令逐渐迁移为用户程序。 |

| \*\*Day 76\*\* | 实现 `execve` 系统调用（加载新程序替换当前进程）。 | 运行 `ls` 时不再返回 Shell，而是直接运行新程序。 |

| \*\*Day 77\*\* | \*\*周复盘\*\*：实现 `fork` 的雏形（复制当前进程上下文）。 | 为多任务打基础。 |



\---



\### 第 12 周：最终打磨、文档、发布 ISO



| 日期 | 任务 | 产出/验收标准 |

| :--- | :--- | :--- |

| \*\*Day 78\*\* | 编写 `README.md` 和 `BUILD.md`：详细说明编译、运行、挂载 LMFS 的步骤。 | 其他人可以照着文档复现。 |

| \*\*Day 79\*\* | 整理代码风格，删除无用注释和调试输出。 | 无编译警告。 |

| \*\*Day 80\*\* | 将根文件系统（包含 `/bin`、`/etc`、`/home`）打包进 ISO。 | 启动后自动进入 Shell，`ls /bin` 能看到所有用户程序。 |

| \*\*Day 81\*\* | \*\*最终 ISO 构建\*\*：使用 `grub-mkrescue` 将你的内核打包成混合 ISO（同时支持 BIOS 和 UEFI 启动，仅限 BIOS 的话保持原样）。 | 在真实硬件（或 VirtualBox）上启动成功。 |

| \*\*Day 82\*\* | 录制一个 5 分钟的演示视频：从开机到运行 GUI，执行 `ls`、`cat`、`run hello.elf`。 | 作为项目展示。 |

| \*\*Day 83\*\* | 编写 `doc/lmfs-spec.md`，详细描述 LMFS 的数据结构。 | 为开源做准备。 |

| \*\*Day 84\*\* | \*\*发布日\*\*：将 `lumaos.iso` 和源码打包成 `lumaos-5-final.tar.gz`。 | 在 GitHub 上创建 Release。 |



\---



\## 🛠️ ISO 构建核心命令（第 9 周直接可用）



\*\*build.sh 末尾新增：\*\*

```bash

\# 生成 El Torito 软盘模拟 ISO（最简单、兼容性最好）

xorriso -as mkisofs \\

&#x20;   -b lumaos-4.1.img \\

&#x20;   -no-emul-boot \\

&#x20;   -boot-load-size 4 \\

&#x20;   -boot-info-table \\

&#x20;   -o lumaos-5.iso \\

&#x20;   ./build/

```



\*\*如果要制作“混合 ISO”（可写入 U 盘）：\*\*

```bash

xorriso -as mkisofs ... -isohybrid-mbr /usr/lib/ISOHybrid/isohdpfx.bin ...

```



\---



\## ⚠️ 给你的 3 个关键建议



1\. \*\*第 1 个月千万别碰 GUI\*\*：文件系统调试期间，只使用文本终端（`console\_write`）。GUI 会拖慢你的调试速度。你可以暂时注释掉 `gui\_init()`，只保留 Shell 文本界面。

2\. \*\*多准备测试镜像\*\*：用 `dd` 和 `mkfs.fat` 制作不同尺寸的 FAT12/16/32 镜像（10MB、100MB、1GB），放在 `tests/` 目录下，用脚本自动化挂载测试。

3\. \*\*善用 QEMU 调试\*\*：

&#x20;  - 编译时加 `-g`，运行 `qemu -s -S`，用 GDB 单步调试 IDE 驱动。

&#x20;  - 用 `-trace events=ide\_\*` 查看 IDE 命令发送情况。



