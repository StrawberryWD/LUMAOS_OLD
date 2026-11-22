/*
 * font_reader.h — 读取 hzk24.bin (GB2312汉字) 和 asc24.bin (变宽ASCII) 字库
 *
 * ============================ hzk24.bin 格式 ============================
 * 无文件头，从第0字节开始就是字模数据，按 GB2312 区位码顺序（区16~87）连续排列。
 * 每个汉字固定占 72 字节 = 24行 x 3字节(24bit宽，MSB在前)。
 * 汉字索引 idx = (区号-16)*94 + (位号-1)，区号/位号来自 GB2312 内码：
 *     区号 = byte1 - 0xA0，位号 = byte2 - 0xA0   （byte1/byte2 是 GB2312 两字节内码）
 * 该汉字的字模偏移 = idx * 72
 *
 * ============================ asc24.bin 格式 ============================
 * 文件头 8 字节：
 *   [0..3]  magic = "ASC1"
 *   [4..5]  count       (u16, 小端) — 字符数量，本字库固定 95 (0x20~0x7E)
 *   [6]     cell_height (u8)        — 固定 24
 *   [7]     first_char  (u8)        — 起始字符码，固定 0x20 (空格)
 * 紧接着是宽度表：count 个字节，widths[i] = 第 i 个字符的实际像素宽度（每个字符宽度不同）
 * 紧接着是字模数据：按同样顺序连续排列，第 i 个字符占
 *     cell_height 行 x row_bytes(i) 字节，其中 row_bytes(i) = ceil(widths[i]/8)
 * 注意：每个字符的行字节数不同（因为宽度不同），必须先查宽度表才能算出下一个字符的偏移，
 * 不能假设固定字节跨度。
 *
 * 两种字库的位图存储都是 MSB-first：某行第 x 个像素 = 
 *     (row_byte[x/8] >> (7 - x%8)) & 1        (1=墨点, 0=空白)
 *
 * 基线：两种字库的字形都以 cell 顶部往下第 21 行为基线对齐，中英文混排时上边缘对齐即可，
 * 不需要额外做基线偏移计算。
 */

#ifndef FONT_READER_H
#define FONT_READER_H

#include <stdint.h>
#include <stddef.h>

#define HZK_CELL_SIZE   24      /* 汉字固定 24x24 */
#define HZK_ROW_BYTES   3       /* 24 / 8 */
#define HZK_GLYPH_BYTES (HZK_CELL_SIZE * HZK_ROW_BYTES)   /* 72 字节/字 */

#define ASC_MAGIC       "ASC1"
#define ASC_HEADER_SIZE 8

typedef struct {
    const uint8_t *data;   /* asc24.bin 整个文件加载到内存后的指针 */
    uint16_t count;
    uint8_t  cell_height;
    uint8_t  first_char;
    const uint8_t *widths;      /* 指向宽度表, count 字节 */
    const uint8_t *glyph_base;  /* 指向字模数据起始处 */
} AsciiFont;

typedef struct {
    const uint8_t *data;   /* hzk24.bin 整个文件加载到内存后的指针 */
    size_t size;
} HzkFont;

/* ---------- 初始化：直接把整个文件读进内存，data 指针长期持有 ---------- */

static inline int ascii_font_init(AsciiFont *f, const uint8_t *file_data, size_t file_size) {
    if (file_size < ASC_HEADER_SIZE) return -1;
    if (file_data[0] != 'A' || file_data[1] != 'S' || file_data[2] != 'C' || file_data[3] != '1')
        return -1;
    f->data = file_data;
    f->count       = (uint16_t)(file_data[4] | (file_data[5] << 8));
    f->cell_height = file_data[6];
    f->first_char  = file_data[7];
    f->widths      = file_data + ASC_HEADER_SIZE;
    f->glyph_base  = f->widths + f->count;   /* 宽度表之后紧跟字模数据 */
    return 0;
}

static inline int hzk_font_init(HzkFont *f, const uint8_t *file_data, size_t file_size) {
    f->data = file_data;
    f->size = file_size;
    return 0;
}

/* ---------- 查字：返回字模指针 + 宽度/行字节数，不做拷贝 ---------- */

/* ASCII: ch 必须在 [first_char, first_char+count-1] 范围内，否则返回 NULL */
static inline const uint8_t *ascii_get_glyph(const AsciiFont *f, char ch,
                                              uint8_t *out_width, uint8_t *out_row_bytes) {
    if ((uint8_t)ch < f->first_char || (uint8_t)ch >= f->first_char + f->count)
        return NULL;

    uint16_t idx = (uint8_t)ch - f->first_char;

    /* 累加前面所有字符的字节数，算出第 idx 个字符的偏移 */
    size_t offset = 0;
    for (uint16_t i = 0; i < idx; i++) {
        uint8_t w = f->widths[i];
        uint8_t rb = (w + 7) / 8;
        offset += (size_t)rb * f->cell_height;
    }

    uint8_t width = f->widths[idx];
    *out_width = width;
    *out_row_bytes = (width + 7) / 8;
    return f->glyph_base + offset;
}

/* CJK: gb_byte1/gb_byte2 是 GB2312 两字节内码（例如 GBK/GB2312 编码文本里读到的两个字节）
 * 返回 NULL 表示不在本字库范围内（区16~87，即一二级汉字区） */
static inline const uint8_t *hzk_get_glyph(const HzkFont *f, uint8_t gb_byte1, uint8_t gb_byte2) {
    int qu  = gb_byte1 - 0xA0;   /* 区号 */
    int wei = gb_byte2 - 0xA0;   /* 位号 */
    if (qu < 16 || qu > 87 || wei < 1 || wei > 94) return NULL;

    long idx = (long)(qu - 16) * 94 + (wei - 1);
    size_t offset = (size_t)idx * HZK_GLYPH_BYTES;
    if (offset + HZK_GLYPH_BYTES > f->size) return NULL;
    return f->data + offset;
}

/* ---------- 取像素：第 row 行第 col 列是否点亮 ---------- */

static inline int glyph_pixel(const uint8_t *glyph_rows, uint8_t row_bytes, int row, int col) {
    const uint8_t *row_ptr = glyph_rows + (size_t)row * row_bytes;
    int byte_idx = col / 8;
    int bit_idx  = 7 - (col % 8);
    return (row_ptr[byte_idx] >> bit_idx) & 1;
}

#endif /* FONT_READER_H */
