/* kernel/exec/elf.h
 * LumaOS 5.0 - ELF32 基础结构定义 + ELF Header 解析
 *
 * DAY43 (Week 7, Day 43 of the 5.0 roadmap):
 *   "学习并定义 ELF32 基础结构。"  验收："能解析 ELF Header。"
 *
 * ---------------------------------------------------------------------
 * 为什么要有 ELF？（今天的原理）
 * ---------------------------------------------------------------------
 *   Day 42 为止 LumaOS 只能“读写文件”；它不能“运行文件”。磁盘上的一个
 *   程序文件只是一串字节——内核必须回答三个问题才能运行它：
 *     1) 这串字节是不是一个可执行程序？目标 CPU 是不是我（i386）？
 *     2) 它的哪些字节要放进内存、放到哪个地址、放多大？（→ Program
 *        Header，Day 45 的 PT_LOAD）
 *     3) 放好之后 CPU 该从哪个地址开始执行？（→ e_entry，Day 46）
 *   ELF（Executable and Linkable Format）就是这三个问题的“标准答案
 *   格式”。LumaOS 的 `.sw`（Luma Software，计划.md “第 40 周”）在 Day
 *   278 之前就是“扩展名叫 .sw 的 ELF32 i386 可执行文件”，所以今天定义
 *   的结构就是整个 5.0 → 9.0 应用平台的地基。
 *
 *   一个 ELF 文件的总体布局（我们只关心“可执行视图”）：
 *
 *       偏移 0   +----------------------+
 *                | ELF Header   (52 B)  |  <- 今天解析它
 *     e_phoff    +----------------------+
 *                | Program Header 表    |  <- Day 45：每项 32 B，告诉内核
 *                |  (e_phnum 项)        |     “把文件的 [p_offset,+p_filesz)
 *                +----------------------+      拷到内存 p_vaddr 处”
 *                | 各段(segment)的内容  |
 *                |  .text .rodata .data |
 *     e_shoff    +----------------------+
 *                | Section Header 表    |  <- 链接器/调试器用，加载器可忽略
 *                +----------------------+
 *
 *   “Header 在文件开头、固定 52 字节、自己说明后面各表在哪”是这类格式
 *   的通用套路（和 FAT 的 BPB、LMFS 的超级块是同一个思想：先读一个固定
 *   大小、位置已知的“目录”结构，再按它的指示找别的东西）。
 *
 * ---------------------------------------------------------------------
 * 设计要点
 * ---------------------------------------------------------------------
 *   - 结构体定义与 ELF 规范（System V ABI, i386 supplement）逐字段一一
 *     对应，用 _Static_assert 锁死大小：Ehdr=52、Phdr=32、Shdr=40。
 *     以后任何人改错字段类型，编译期立刻报错，而不是运行期读到错位数据。
 *   - 解析（elf32_parse_header）与校验（elf32_check_header）分开：
 *       parse : “把字节按 ELF 规则翻译成 C 结构体”，只关心格式能不能读；
 *       check : “这个程序我能不能运行”，关心语义（CPU 架构、类型…）。
 *     elfinfo 命令需要“即使不能运行也把字段打印出来”，Day 44 的
 *     elf_load() 则会两步都做。
 *   - 多字节字段用显式小端逐字节组装（rd16/rd32），不靠“指针强转 +
 *     刚好 x86 是小端”。ELF 自己在 e_ident[EI_DATA] 里声明字节序，
 *     解析器必须先看那一位才有资格解释后面的整数——所以遇到大端文件
 *     我们返回 ELF_ERR_NOT_LE 而不是读出一堆错值。
 *   - 本文件不依赖 VFS / 磁盘 / 终端，只吃一块内存。因此可以直接在宿主
 *     机上编译做单元测试（tests/day43_elf_host_test.c）。
 *   - 不使用 64 位整数运算：32 位内核没有链接 libgcc，64 位乘除会变成
 *     对 __udivdi3 等符号的引用而链接失败。范围检查改用“先判上界再做
 *     减法”的写法避免溢出。
 *
 * ---------------------------------------------------------------------
 * DAY44 (Week 7, Day 44): "实现 elf_load()。" 验收："检查 Magic、打印
 * Entry，实现在 lmfs 上用 elf。"
 * ---------------------------------------------------------------------
 *   Day 43 只会“读 52 字节的头并翻译”。内核要真正运行一个程序，还得
 *   先回答一个更现实的问题：这个文件“值不值得被加载”——否则 Day 45 把
 *   段拷进内存时，一个手工拼出来的坏文件（段声称在文件之外、段的目标
 *   地址压在内核上、入口点不在任何代码段里……）就会让内核把垃圾写到
 *   不该写的地方。所以 elf_load() 是“加载的前半场：打开 → 读 → 验证 →
 *   产出一份已验证的 elf_image_t”；后半场（把 PT_LOAD 段拷进内存）是
 *   Day 45 的工作，Day 46 才会跳到入口。分三层，职责各自单一：
 *
 *     elf.c         纯内存、无 I/O：Header + 每个 Program Header 的解析与
 *                   校验（elf32_parse_phdr / elf32_check_phdrs），宿主机
 *                   可编译做单元测试（tests/day44_elf_host_test.c）。
 *     elf_loader.c  VFS 层：按路径打开文件（FAT 或 LMFS——靠 fs->name
 *                   分派，和 read/elfinfo 一致），把 file_t.pos 设到目标
 *                   偏移后读 Header 与 Program Header 表，再调用上面的
 *                   纯函数。它只依赖 VFS 的 open/read/close 这套通用接口，
 *                   所以同一份代码对 FAT12/16/32 与 LMFS 一视同仁——这就
 *                   是“实现在 lmfs 上用 elf”的含义。
 *     elfload 命令  把 elf_image_t 打印出来（Magic、Entry、段表…）。
 *
 *   “验证”具体检查什么（每一条都对应一种真实的损坏/攻击）：
 *     1. 不能有 PT_INTERP / PT_DYNAMIC —— 动态链接需要解释器与重定位，
 *        LumaOS 只做静态可执行文件。
 *     2. 每个非空 PT_LOAD：filesz <= memsz（差额 = .bss，加载时清零）；
 *        [p_offset, p_offset+filesz) 必须落在文件内（否则会读到文件之外
 *        的内容）；[p_vaddr, p_vaddr+memsz) 必须落在用户装载区
 *        [ELF_LOAD_MIN_ADDR, ELF_LOAD_MAX_ADDR)（否则程序能把自己的段
 *        “装”到内核/显存/帧缓冲上）；p_align>1 时 p_align 必须是 2 的
 *        幂且 p_vaddr 与 p_offset 对它同余（ELF 规范要求，也是 Day 54
 *        分页映射的前提）；各 PT_LOAD 的内存区间互不重叠。
 *     3. 至少有一个非空 PT_LOAD；e_entry 必须落在某个带 PF_X 的 PT_LOAD
 *        的“文件部分”里——入口点指向 .bss 或不可执行段没有意义。
 *   所有区间运算都写成“先比上界再做减法”，避免 32 位加法回绕（例如
 *   p_vaddr=0xFFFFFFF0、p_memsz=0x20 相加回绕成 0x10，会骗过天真的
 *   “vaddr+memsz <= MAX”检查）。
 *
 *   ELF_LOAD_MIN_ADDR = 16 MB：内核镜像在 0x10000（.bss 到 ~0x9A000），
 *   栈顶 0x200000，GUI 后备缓冲 0x300000、SVGA prev_frame 0x400000、额外
 *   终端池 0x500000~0x58A000 都在 16 MB 以下；传统 i386 用户空间基址
 *   0x08048000（约 128 MB）则远在其上。ELF_LOAD_MAX_ADDR = 240 MB：QEMU
 *   默认 -m 256，留 16 MB 余量。Day 53 开分页后，这两个常量会被“用户
 *   地址空间”的概念取代。
 */
/*
 * ---------------------------------------------------------------------
 * DAY45 (Week 7, Day 45): "解析 PT_LOAD。"  验收："能将程序段加载到内存。"
 * ---------------------------------------------------------------------
 *   Day 44 之后我们手里有一份“已验证、尚未装入内存”的 elf_image_t。
 *   今天做加载的后半场：按 Program Header 的指示，把文件里的字节**搬到
 *   p_vaddr 指定的内存地址**。Day 46 才会跳到入口去执行。
 *
 *   为什么必须放在“p_vaddr 那个地址”？
 *     .sw 是 ET_EXEC（固定地址的可执行文件，没有重定位）。链接器在生成它
 *     时已经把所有绝对地址（函数调用目标、全局变量地址、字符串地址…）
 *     算死成“程序被放在 p_vaddr 时”的值。放错 1 个字节，call/mov 就会
 *     指向别处。所以加载器没有选择权：段在文件的哪里（p_offset）和段在
 *     内存的哪里（p_vaddr）是两个独立的数，加载就是做一次
 *         内存[p_vaddr .. p_vaddr+p_filesz) = 文件[p_offset .. +p_filesz)
 *     的搬运。（LumaOS 现在没开分页，物理地址 == 线性地址，所以“映射”就
 *     是直接 memcpy；Day 53/54 开分页后同样的循环会变成“先给 p_vaddr 建
 *     页表映射、再拷贝”，本函数的结构不变。）
 *
 *   .bss 的清零（今天最容易写错的一点）：
 *     一个段有两个大小：p_filesz（文件里有多少字节）与 p_memsz（运行时要
 *     占多少字节），p_memsz >= p_filesz。差额 [filesz, memsz) 就是 .bss：
 *     C 语言保证“未初始化的全局/静态变量为 0”，但把一大片 0 存进文件是
 *     浪费，所以文件里不存，由加载器在内存里补 0。加载器**不能**假设
 *     那块内存本来就是 0——开机后的 RAM 里留着 BIOS/引导程序/上一个程序
 *     的残留。漏掉清零的后果非常隐蔽：程序偶尔读到“上一个程序的数据”，
 *     结果取决于运行顺序。所以每个段的步骤是：先拷文件部分，再把尾部
 *     清零。elfmap 命令在加载前会把每个段的目标区域先填成 0xA5，这样
 *     “清零漏了”会直接看成 a5 a5 a5……，而不是碰巧是 0。
 *
 *   “谁说了算”——信任边界：
 *     目标地址与长度只来自已验证的 elf_image_t；文件只提供“字节内容”。
 *     即使文件在 Day 44 校验之后被别的东西改过，最坏结果是程序内容变成
 *     垃圾，而不是内核内存被写坏。为了不盲信调用者传进来的结构（它可能被
 *     手工拼出来或被改过），elf32_load_segments() 开头会对 img 把 Header 与
 *     Program Header 重新校验一遍（几十次比较，几乎不要钱），并以重新
 *     校验的结果为准。
 *
 *   “回读校验”：
 *     每拷完一小块（512 字节）就立刻把目标内存读回来与源缓冲逐字节比较。
 *     这样“p_vaddr 处根本没有 RAM”（例如用 qemu -m 64 启动，而 0x08048000
 *     在 128 MB 处）会立刻变成 ELF_ERR_VERIFY，而不是“加载成功、跳过去后
 *     乱跑”。（QEMU 里不存在的物理地址：写入被丢弃，读回来是 0x00——本机
 *     QEMU 8.2 的 pc 机型实测；别的总线可能读到 0xFF，两种都会被发现。
 *     局限：① 若一个段的文件部分碰巧全是 0，读回的 0 与写入的 0 一致，
 *     检测不到——真实可执行文件的段里总有非 0 字节，所以实际不是问题；
 *     ② 真实硬件上 CPU 缓存可能让刚写入的值被读回，这个检查只是模拟器里
 *     的保险；真正的“有多少 RAM”要靠开机时用 BIOS int 0x15/E820 探测，
 *     那是以后的工作。）目标内存用 volatile 指针访问，防止编译器认为
 *     “刚写进去的值当然读得回来”而把比较优化掉。
 *
 *   “要么全成功，要么不算加载”：
 *     elf_image_t.loaded 在加载开始时置 0，只有所有 PT_LOAD 都完成并通过
 *     回读校验后才置 1。中途失败时，已经写过的内存没有回滚（目标区域是
 *     内核不使用的用户装载区，留着垃圾不会伤害内核），但 loaded 仍是 0，
 *     Day 46 的 elf_run() 将拒绝执行 loaded==0 的映像。
 *
 *   模块分层与 Day 44 完全一致：
 *     elf.c         elf32_load_segments()：纯内存，字节从一个“读函数”
 *                   (elf_read_fn) 来——宿主机单元测试里它读内存数组，
 *                   内核里它读 VFS 文件（tests/day45_elf_host_test.c 用
 *                   mmap 把 0x08048000 映射出来，真的把段装进去并逐字节验证）。
 *     elf_loader.c  elf_load_segments(path, img, st)：打开文件（FAT 或
 *                   LMFS，与 elf_load 同一套 elf_open），把 file_t 包成
 *                   elf_read_fn，调用上面的纯函数。
 *     elfmap 命令   加载并展示结果：段表、读回的字节、校验和、.bss 是否清零。
 */
#ifndef LUMA_ELF_H
#define LUMA_ELF_H

#include <stdint.h>

/* ===== e_ident[]：文件最开头的 16 字节“身份证” ===== */
#define EI_NIDENT      16
#define EI_MAG0        0      /* 0x7F                         */
#define EI_MAG1        1      /* 'E'                          */
#define EI_MAG2        2      /* 'L'                          */
#define EI_MAG3        3      /* 'F'                          */
#define EI_CLASS       4      /* 1 = 32 位, 2 = 64 位         */
#define EI_DATA        5      /* 1 = 小端,  2 = 大端          */
#define EI_VERSION     6      /* 恒为 1 (EV_CURRENT)          */
#define EI_OSABI       7      /* 0 = System V                 */
#define EI_ABIVERSION  8
#define EI_PAD         9      /* 9..15 填 0                   */

#define ELFMAG0  0x7F
#define ELFMAG1  'E'
#define ELFMAG2  'L'
#define ELFMAG3  'F'

#define ELFCLASSNONE  0
#define ELFCLASS32    1
#define ELFCLASS64    2

#define ELFDATANONE   0
#define ELFDATA2LSB   1
#define ELFDATA2MSB   2

#define EV_NONE       0
#define EV_CURRENT    1

#define ELFOSABI_SYSV   0
#define ELFOSABI_LINUX  3

/* ===== e_type ===== */
#define ET_NONE  0
#define ET_REL   1   /* 可重定位目标文件 (.o)              */
#define ET_EXEC  2   /* 可执行文件（固定加载地址）← LumaOS 只接受这种 */
#define ET_DYN   3   /* 共享库 / 位置无关可执行文件 (PIE) */
#define ET_CORE  4

/* ===== e_machine ===== */
#define EM_NONE   0
#define EM_386    3    /* Intel 80386 ← LumaOS 唯一支持的架构 */
#define EM_X86_64 62

/* ===== Program Header: p_type / p_flags（Day 45 才真正使用，先定义） ===== */
#define PT_NULL     0
#define PT_LOAD     1   /* 需要装入内存的段 */
#define PT_DYNAMIC  2
#define PT_INTERP   3
#define PT_NOTE     4
#define PT_SHLIB    5
#define PT_PHDR     6
#define PT_TLS      7

#define PF_X  0x1
#define PF_W  0x2
#define PF_R  0x4

/* ===== Section Header: sh_type（只定义常用的几个） ===== */
#define SHT_NULL      0
#define SHT_PROGBITS  1
#define SHT_SYMTAB    2
#define SHT_STRTAB    3
#define SHT_NOBITS    8

/* ===== 三个核心结构体（packed：字段顺序/大小 = 磁盘上的字节布局） ===== */

/* ELF Header — 52 字节，永远在文件偏移 0 */
typedef struct {
    unsigned char e_ident[EI_NIDENT]; /*  0: 魔数 + 类别 + 字节序 + 版本…  */
    uint16_t      e_type;             /* 16: ET_*                           */
    uint16_t      e_machine;          /* 18: EM_*                           */
    uint32_t      e_version;          /* 20: 恒为 1                         */
    uint32_t      e_entry;            /* 24: 入口（程序第一条指令的虚拟地址）*/
    uint32_t      e_phoff;            /* 28: Program Header 表的文件偏移    */
    uint32_t      e_shoff;            /* 32: Section Header 表的文件偏移    */
    uint32_t      e_flags;            /* 36: 架构相关标志（i386 恒为 0）    */
    uint16_t      e_ehsize;           /* 40: 本头的大小，必须 = 52          */
    uint16_t      e_phentsize;        /* 42: 每个 Program Header 的大小 = 32*/
    uint16_t      e_phnum;            /* 44: Program Header 的个数          */
    uint16_t      e_shentsize;        /* 46: 每个 Section Header 的大小 = 40*/
    uint16_t      e_shnum;            /* 48: Section Header 的个数          */
    uint16_t      e_shstrndx;         /* 50: 段名字符串表所在的节序号       */
} __attribute__((packed)) elf32_ehdr_t;

/* Program Header — 32 字节/项。注意：32 位版 p_flags 排在 p_memsz 之后，
 * 64 位版把它提前到了 p_type 之后——照抄 64 位资料是经典的坑。 */
typedef struct {
    uint32_t p_type;     /* PT_*                                   */
    uint32_t p_offset;   /* 段内容在文件里的偏移                   */
    uint32_t p_vaddr;    /* 段应放到的虚拟地址                     */
    uint32_t p_paddr;    /* 物理地址（现代系统忽略）               */
    uint32_t p_filesz;   /* 段在文件里占多少字节                   */
    uint32_t p_memsz;    /* 段在内存里占多少字节 (>= filesz, 差额=.bss 清零) */
    uint32_t p_flags;    /* PF_R / PF_W / PF_X                     */
    uint32_t p_align;    /* 对齐                                   */
} __attribute__((packed)) elf32_phdr_t;

/* Section Header — 40 字节/项（链接/调试用；LumaOS 加载器不依赖它） */
typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
} __attribute__((packed)) elf32_shdr_t;

_Static_assert(sizeof(elf32_ehdr_t) == 52, "ELF32 Ehdr must be 52 bytes");
_Static_assert(sizeof(elf32_phdr_t) == 32, "ELF32 Phdr must be 32 bytes");
_Static_assert(sizeof(elf32_shdr_t) == 40, "ELF32 Shdr must be 40 bytes");

#define ELF32_EHDR_SIZE  52u
#define ELF32_PHDR_SIZE  32u
#define ELF32_SHDR_SIZE  40u

/* ===== 返回码（Day 70 统一错误码之前，ELF 模块自己的一组负数） ===== */
#define ELF_OK                    0
#define ELF_ERR_NULL            (-1)   /* 空指针参数                          */
#define ELF_ERR_TOO_SHORT       (-2)   /* 数据不足 52 字节，装不下 ELF Header */
#define ELF_ERR_BAD_MAGIC       (-3)   /* 开头不是 7F 'E' 'L' 'F'             */
#define ELF_ERR_NOT_32BIT       (-4)   /* EI_CLASS != ELFCLASS32              */
#define ELF_ERR_NOT_LE          (-5)   /* EI_DATA  != ELFDATA2LSB             */
#define ELF_ERR_BAD_IDENT_VER   (-6)   /* EI_VERSION != 1                     */
#define ELF_ERR_BAD_VERSION     (-7)   /* e_version  != 1                     */
#define ELF_ERR_BAD_MACHINE     (-8)   /* e_machine  != EM_386                */
#define ELF_ERR_BAD_TYPE        (-9)   /* e_type     != ET_EXEC               */
#define ELF_ERR_BAD_EHSIZE     (-10)   /* e_ehsize   != 52                    */
#define ELF_ERR_BAD_PHENTSIZE  (-11)   /* e_phentsize != 32 (且 phnum > 0)    */
#define ELF_ERR_NO_PHDRS       (-12)   /* 可执行文件却没有 Program Header     */
#define ELF_ERR_PH_RANGE       (-13)   /* Program Header 表越出文件范围       */
#define ELF_ERR_NO_ENTRY       (-14)   /* e_entry == 0                        */
/* ---- DAY44: Program Header 校验 / elf_load() 的返回码 ---- */
#define ELF_ERR_TOO_MANY_PHDRS (-15)   /* e_phnum > ELF_MAX_PHDRS             */
#define ELF_ERR_DYNAMIC        (-16)   /* 含 PT_INTERP/PT_DYNAMIC（要动态链接）*/
#define ELF_ERR_NO_LOAD        (-17)   /* 没有任何非空 PT_LOAD                */
#define ELF_ERR_SEG_SIZE       (-18)   /* p_filesz > p_memsz                  */
#define ELF_ERR_SEG_FILE_RANGE (-19)   /* 段的文件内容越出文件                */
#define ELF_ERR_SEG_ADDR       (-20)   /* 段的内存区间不在用户装载区/回绕     */
#define ELF_ERR_SEG_ALIGN      (-21)   /* p_align 非 2 的幂，或 vaddr≢offset  */
#define ELF_ERR_SEG_OVERLAP    (-22)   /* 两个 PT_LOAD 的内存区间重叠         */
#define ELF_ERR_ENTRY_NOT_EXEC (-23)   /* e_entry 不在任何可执行 PT_LOAD 内   */
/* ---- DAY45: 把 PT_LOAD 装入内存时的返回码 ---- */
#define ELF_ERR_CHANGED        (-24)   /* 文件大小与校验时不同（被改过）      */
#define ELF_ERR_VERIFY         (-25)   /* 写入内存后读回不一致（该地址无 RAM?）*/
#define ELF_ERR_OPEN           (-30)   /* 文件打不开（不存在/路径不对）       */
#define ELF_ERR_IS_DIR         (-31)   /* 路径是目录                          */
#define ELF_ERR_IO             (-32)   /* 读盘失败，或 Program Header 读不全  */

/* elf32_parse_header(buf, len, out)
 *
 * 把 buf 开头的 52 字节按 ELF32 小端规则翻译进 *out。
 *   成功     -> ELF_OK，*out 的全部字段有效。
 *   失败时   ELF_ERR_NULL / ELF_ERR_TOO_SHORT / ELF_ERR_BAD_MAGIC：*out
 *            未必有意义（前两者没碰；BAD_MAGIC 时 e_ident 已拷入便于打印）。
 *            ELF_ERR_NOT_32BIT / ELF_ERR_NOT_LE：e_ident 已拷入 *out，
 *            其余字段不解释（字节序/字长都不对，解释出来没有意义）。
 * 只检查“能不能按 ELF32 小端读”，不检查能不能运行——后者见下面的 check。 */
int elf32_parse_header(const void *buf, uint32_t len, elf32_ehdr_t *out);

/* elf32_check_header(h, file_size)
 *
 * 检查一个已解析的 Header 描述的是不是“LumaOS 能运行的程序”：
 *   e_ident[EI_VERSION]==1, e_version==1, e_machine==EM_386,
 *   e_type==ET_EXEC, e_ehsize==52, e_entry!=0, 且 Program Header 表
 *   存在(e_phnum>0)、每项 32 字节、整张表落在 [0, file_size) 之内。
 * 返回 ELF_OK 或第一个发现的 ELF_ERR_*。
 * 只验证加载器真正依赖的部分；Section Header 表不检查（Linux 内核同样
 * 忽略它：它是给链接器和 readelf 看的，被 strip 掉也不影响运行）。 */
int elf32_check_header(const elf32_ehdr_t *h, uint32_t file_size);

/* 名字查询：给 elfinfo 打印用（readelf -h 风格）。未知值返回 "?"。 */
const char *elf_strerror(int err);
const char *elf_class_name(uint8_t v);
const char *elf_data_name(uint8_t v);
const char *elf_osabi_name(uint8_t v);
const char *elf_type_name(uint16_t v);
const char *elf_machine_name(uint16_t v);

/* ===================================================================
 * DAY44: Program Header 解析/校验 + 已验证镜像描述 elf_image_t
 * =================================================================== */

#define ELF_MAX_PHDRS      8            /* elf_image_t 最多容纳的 Program Header 数 */
#define ELF_LOAD_MIN_ADDR  0x01000000u  /* PT_LOAD 允许的最低虚拟地址 (16 MB)  */
#define ELF_LOAD_MAX_ADDR  0x0F000000u  /* PT_LOAD 区间的上界（不含）(240 MB)  */

/* elf_load() 的产出：一份“已完整验证、尚未装入内存”的程序描述。
 * Day 45 的段加载器只需要读它，不必再校验。 */
typedef struct {
    uint32_t     file_size;              /* 文件字节数                       */
    elf32_ehdr_t ehdr;                   /* 已解析并通过 check 的 ELF Header */
    elf32_phdr_t phdr[ELF_MAX_PHDRS];    /* 前 ehdr.e_phnum 项有效           */
    uint32_t     n_load;                 /* 非空 PT_LOAD 的个数 (>= 1)       */
    uint32_t     load_lo;                /* 所有 PT_LOAD 的最低 p_vaddr      */
    uint32_t     load_hi;                /* 所有 PT_LOAD 的最高 p_vaddr+memsz（不含）*/
    uint32_t     total_filesz;           /* 各 PT_LOAD 的 p_filesz 之和      */
    uint32_t     total_memsz;            /* 各 PT_LOAD 的 p_memsz 之和       */
    uint32_t     entry_seg;              /* e_entry 所在的 phdr 下标         */
    uint32_t     loaded;                 /* DAY45: 1 = 所有 PT_LOAD 已装入内存并通过回读校验 */
} elf_image_t;

/* elf32_parse_phdr(buf, out)
 * 把 buf 开头的 32 字节按小端翻译成一个 Program Header（显式逐字节，
 * 不依赖对齐/字节序）。调用者保证 buf 至少 32 字节。 */
void elf32_parse_phdr(const void *buf, elf32_phdr_t *out);

/* elf32_check_phdrs(h, ph, n, file_size, img)
 * 校验 n 个 Program Header（规则见本文件 DAY44 注释），成功返回 ELF_OK
 * 并填好 img 的 n_load/load_lo/load_hi/total_filesz/total_memsz/entry_seg 以及 img->phdr
 * 的拷贝与 img->ehdr/file_size；失败返回第一个发现的 ELF_ERR_*（此时
 * img 的内容不保证有意义）。n 必须 <= ELF_MAX_PHDRS。 */
int elf32_check_phdrs(const elf32_ehdr_t *h, const elf32_phdr_t *ph, uint32_t n,
                      uint32_t file_size, elf_image_t *img);

const char *elf_ptype_name(uint32_t p_type);   /* "LOAD" "GNU_STACK" ... */

/* ===================================================================
 * DAY45: 把 PT_LOAD 段装入内存
 * =================================================================== */

/* 字节来源：读 ctx 所代表的文件里 [off, off+n) 的字节到 buf。
 * 返回实际读到的字节数（< n 表示文件在那里结束），出错返回负数。 */
typedef int (*elf_read_fn)(void *ctx, uint32_t off, void *buf, uint32_t n);

/* 每次从文件读入并回读校验的块大小（栈上缓冲，512 字节）。 */
#define ELF_LOAD_CHUNK  512u

/* FNV-1a 32 位校验和的起始值与更新函数：elf32_load_segments 用它给每个段
 * 的“文件部分”算一个指纹，elfmap 与宿主机测试用同一个函数从内存/文件重算，
 * 三方一致才算数。h 传 ELF_FNV_SEED 开始，可连续喂多块。 */
#define ELF_FNV_SEED  0x811C9DC5u
uint32_t elf_fnv1a(uint32_t h, const void *buf, uint32_t n);

/* 加载统计（elf32_load_segments 的可选输出，可传 NULL）。 */
typedef struct {
    uint32_t segs;                       /* 已放入内存的非空 PT_LOAD 个数    */
    uint32_t bytes_copied;               /* 从文件拷入内存的字节数 (Σfilesz) */
    uint32_t bytes_zeroed;               /* 清零的 .bss 字节数 (Σ(memsz-filesz)) */
    uint32_t seg_sum[ELF_MAX_PHDRS];     /* 第 i 个 phdr 文件部分的 FNV-1a；非 LOAD 为 0 */
    uint32_t fail_addr;                  /* ELF_ERR_VERIFY：出错的内存地址   */
    uint32_t fail_want;                  /*   应该读到的字节                 */
    uint32_t fail_got;                   /*   实际读到的字节                 */
} elf_load_stats_t;

/* elf32_load_segments(img, rd, ctx, st)
 *
 * 把 img 描述的所有非空 PT_LOAD 装入内存（规则与理由见本文件 DAY45 注释）：
 *   0. 重新校验 img（Header + Program Header），以校验结果为准；失败则
 *      原样返回那个 ELF_ERR_*，一个字节都不写；
 *   1. 对每个 PT_LOAD：分块调用 rd() 读文件 [p_offset, +p_filesz)，拷到
 *      p_vaddr，并立刻读回比较（不一致 -> ELF_ERR_VERIFY，st->fail_*
 *      给出地址/期望值/实际值）；
 *   2. 把 [p_vaddr+p_filesz, p_vaddr+p_memsz)（.bss）清零并读回确认；
 *   3. 全部成功后 img->loaded = 1。
 * rd() 读不满（文件比 img 说的短）或返回负数 -> ELF_ERR_IO。
 * 返回 ELF_OK 或第一个失败原因。失败时 img->loaded == 0。
 *
 * 这是一个会写任意（但已被限定在 [ELF_LOAD_MIN_ADDR, ELF_LOAD_MAX_ADDR) 内）
 * 物理内存的函数：调用者必须保证那一段内存确实属于“用户装载区”、
 * 当前没有别的东西在用。 */
int elf32_load_segments(elf_image_t *img, elf_read_fn rd, void *ctx,
                        elf_load_stats_t *st);

#endif /* LUMA_ELF_H */