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

#endif /* LUMA_ELF_H */