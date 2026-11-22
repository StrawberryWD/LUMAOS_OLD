/* kernel/exec/elf.c
 * LumaOS 5.0 - ELF32 Header 解析与校验
 *
 * DAY43：原理与设计说明见 elf.h 的文件头注释。
 * 本文件只依赖 <stdint.h>，不依赖内核其它部分，宿主机可直接编译测试。
 */
#include "elf.h"

/* 小端读取：显式逐字节组装，与 CPU 字节序、对齐无关。 */
static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

int elf32_parse_header(const void *buf, uint32_t len, elf32_ehdr_t *out)
{
    if (buf == (const void *)0 || out == (elf32_ehdr_t *)0)
        return ELF_ERR_NULL;
    if (len < ELF32_EHDR_SIZE)
        return ELF_ERR_TOO_SHORT;

    const unsigned char *p = (const unsigned char *)buf;

    /* 先拷出 16 字节 e_ident —— 无论后面成败，调用者都能打印它。 */
    for (int i = 0; i < EI_NIDENT; i++)
        out->e_ident[i] = p[i];

    /* 1) 魔数：7F 'E' 'L' 'F'。 */
    if (p[EI_MAG0] != ELFMAG0 || p[EI_MAG1] != ELFMAG1 ||
        p[EI_MAG2] != ELFMAG2 || p[EI_MAG3] != ELFMAG3)
        return ELF_ERR_BAD_MAGIC;

    /* 2) 字长：后面 Header 的字段宽度取决于它（32 位 Header 52 字节，
     *    64 位 Header 64 字节，字段偏移完全不同）。 */
    if (p[EI_CLASS] != ELFCLASS32)
        return ELF_ERR_NOT_32BIT;

    /* 3) 字节序：必须先确认是小端，才有资格按小端去解释多字节整数。 */
    if (p[EI_DATA] != ELFDATA2LSB)
        return ELF_ERR_NOT_LE;

    /* 4) 现在可以安全地解释 e_ident 之后的全部字段。 */
    out->e_type      = rd16(p + 16);
    out->e_machine   = rd16(p + 18);
    out->e_version   = rd32(p + 20);
    out->e_entry     = rd32(p + 24);
    out->e_phoff     = rd32(p + 28);
    out->e_shoff     = rd32(p + 32);
    out->e_flags     = rd32(p + 36);
    out->e_ehsize    = rd16(p + 40);
    out->e_phentsize = rd16(p + 42);
    out->e_phnum     = rd16(p + 44);
    out->e_shentsize = rd16(p + 46);
    out->e_shnum     = rd16(p + 48);
    out->e_shstrndx  = rd16(p + 50);
    return ELF_OK;
}

int elf32_check_header(const elf32_ehdr_t *h, uint32_t file_size)
{
    if (h == (const elf32_ehdr_t *)0)
        return ELF_ERR_NULL;

    if (h->e_ident[EI_VERSION] != EV_CURRENT) return ELF_ERR_BAD_IDENT_VER;
    if (h->e_version           != EV_CURRENT) return ELF_ERR_BAD_VERSION;
    if (h->e_machine           != EM_386)     return ELF_ERR_BAD_MACHINE;
    /* ET_DYN 也会在这里被拒绝：现代 gcc 默认产出 PIE（类型是 ET_DYN），
     * 它要求加载器做重定位。LumaOS 的 .sw 用 `-static -no-pie` 链接成
     * 固定地址的 ET_EXEC，加载器因此不需要重定位。 */
    if (h->e_type              != ET_EXEC)    return ELF_ERR_BAD_TYPE;
    if (h->e_ehsize            != ELF32_EHDR_SIZE) return ELF_ERR_BAD_EHSIZE;
    if (h->e_entry             == 0)          return ELF_ERR_NO_ENTRY;

    if (h->e_phnum == 0)
        return ELF_ERR_NO_PHDRS;
    if (h->e_phentsize != ELF32_PHDR_SIZE)
        return ELF_ERR_BAD_PHENTSIZE;

    /* Program Header 表必须整张落在文件里：
     *   phoff + phnum*phentsize <= file_size
     * 写成 “phoff <= size && tbl <= size - phoff” 避免 32 位加法溢出
     * （phnum 与 phentsize 都是 16 位，相乘最大 65535*65535 < 2^32，
     *  本身不会溢出）。同时表不能压在 ELF Header 里（phoff >= 52）。 */
    uint32_t tbl = (uint32_t)h->e_phnum * (uint32_t)h->e_phentsize;
    if (h->e_phoff < ELF32_EHDR_SIZE ||
        h->e_phoff > file_size ||
        tbl > file_size - h->e_phoff)
        return ELF_ERR_PH_RANGE;

    return ELF_OK;
}

const char *elf_strerror(int err)
{
    switch (err) {
    case ELF_OK:                return "ok";
    case ELF_ERR_NULL:          return "null argument";
    case ELF_ERR_TOO_SHORT:     return "file shorter than the 52-byte ELF header";
    case ELF_ERR_BAD_MAGIC:     return "bad magic (not 7F 45 4C 46)";
    case ELF_ERR_NOT_32BIT:     return "not ELF32 (EI_CLASS != 1)";
    case ELF_ERR_NOT_LE:        return "not little-endian (EI_DATA != 1)";
    case ELF_ERR_BAD_IDENT_VER: return "bad EI_VERSION (must be 1)";
    case ELF_ERR_BAD_VERSION:   return "bad e_version (must be 1)";
    case ELF_ERR_BAD_MACHINE:   return "wrong CPU (e_machine != 3 / i386)";
    case ELF_ERR_BAD_TYPE:      return "not ET_EXEC (link with -static -no-pie)";
    case ELF_ERR_BAD_EHSIZE:    return "bad e_ehsize (must be 52)";
    case ELF_ERR_BAD_PHENTSIZE: return "bad e_phentsize (must be 32)";
    case ELF_ERR_NO_PHDRS:      return "no program headers (e_phnum == 0)";
    case ELF_ERR_PH_RANGE:      return "program header table outside the file";
    case ELF_ERR_NO_ENTRY:      return "entry point is 0";
    default:                    return "unknown error";
    }
}

const char *elf_class_name(uint8_t v)
{
    switch (v) {
    case ELFCLASSNONE: return "none";
    case ELFCLASS32:   return "ELF32";
    case ELFCLASS64:   return "ELF64";
    default:           return "?";
    }
}

const char *elf_data_name(uint8_t v)
{
    switch (v) {
    case ELFDATANONE: return "none";
    case ELFDATA2LSB: return "little endian";
    case ELFDATA2MSB: return "big endian";
    default:          return "?";
    }
}

const char *elf_osabi_name(uint8_t v)
{
    switch (v) {
    case ELFOSABI_SYSV:  return "SYSV";
    case ELFOSABI_LINUX: return "Linux";
    default:             return "?";
    }
}

const char *elf_type_name(uint16_t v)
{
    switch (v) {
    case ET_NONE: return "NONE";
    case ET_REL:  return "REL";
    case ET_EXEC: return "EXEC";
    case ET_DYN:  return "DYN";
    case ET_CORE: return "CORE";
    default:      return "?";
    }
}

const char *elf_machine_name(uint16_t v)
{
    switch (v) {
    case EM_NONE:   return "none";
    case EM_386:    return "Intel 80386";
    case EM_X86_64: return "x86-64";
    default:        return "?";
    }
}