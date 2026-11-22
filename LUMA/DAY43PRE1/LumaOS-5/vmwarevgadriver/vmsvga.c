/*
 * vmsvga.c — VMware SVGA II (PCI 15AD:0405) 最小驱动
 *   - PCI 扫描、BAR 解析
 *   - 版本协商 (SVGA_ID_2)
 *   - 设置分辨率 32bpp
 *   - 初始化 FIFO，用 SVGA_CMD_UPDATE 刷新画面
 *
 * 前提：内核里 BAR1(帧缓冲) / BAR2(FIFO) 的物理地址可直接访问
 *       （未开分页，或已做恒等映射）。
 */
#include "vmsvga.h"

/* ---------- 端口 I/O（加前缀避免与内核已有函数冲突） ---------- */
static inline void vm_outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint32_t vm_inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }

/* ---------- PCI ---------- */
#define PCI_VENDOR_VMWARE 0x15AD
#define PCI_DEVICE_SVGA2  0x0405

static uint32_t pci_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t off)
{
    vm_outl(0xCF8, 0x80000000u | ((uint32_t)b << 16) | ((uint32_t)d << 11) | ((uint32_t)f << 8) | (off & 0xFC));
    return vm_inl(0xCFC);
}
static void pci_write32(uint8_t b, uint8_t d, uint8_t f, uint8_t off, uint32_t v)
{
    vm_outl(0xCF8, 0x80000000u | ((uint32_t)b << 16) | ((uint32_t)d << 11) | ((uint32_t)f << 8) | (off & 0xFC));
    vm_outl(0xCFC, v);
}
static int pci_find(uint8_t *B, uint8_t *D, uint8_t *F)
{
    for (uint32_t b = 0; b < 32; b++)
        for (uint32_t d = 0; d < 32; d++)
            for (uint32_t f = 0; f < 8; f++) {
                uint32_t id = pci_read32(b, d, f, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) { if (f == 0) break; else continue; }
                if ((id & 0xFFFF) == PCI_VENDOR_VMWARE && (id >> 16) == PCI_DEVICE_SVGA2) {
                    *B = b; *D = d; *F = f; return 0;
                }
            }
    return -1;
}

/* ---------- SVGA 寄存器 ---------- */
#define SVGA_ID_2          0x90000002u
#define SVGA_CAP_EXT_FIFO  0x00008000u

enum {
    REG_ID = 0, REG_ENABLE = 1, REG_WIDTH = 2, REG_HEIGHT = 3,
    REG_MAX_WIDTH = 4, REG_MAX_HEIGHT = 5, REG_DEPTH = 6, REG_BPP = 7,
    REG_BYTES_PER_LINE = 12, REG_FB_START = 13, REG_FB_OFFSET = 14,
    REG_VRAM_SIZE = 15, REG_FB_SIZE = 16, REG_CAPS = 17,
    REG_MEM_START = 18, REG_MEM_SIZE = 19, REG_CONFIG_DONE = 20,
    REG_SYNC = 21, REG_BUSY = 22, REG_MEM_REGS = 30
};
/* FIFO 头部 4 个 dword */
enum { FIFO_MIN = 0, FIFO_MAX = 1, FIFO_NEXT_CMD = 2, FIFO_STOP = 3 };
#define SVGA_CMD_UPDATE 1u

static uint16_t io_base;
static volatile uint32_t *fifo;
static uint32_t cur_w, cur_h;

/* 索引端口 = base+0，数值端口 = base+4 */
static void     wreg(uint32_t i, uint32_t v) { vm_outl(io_base, i); vm_outl(io_base + 4, v); }
static uint32_t rreg(uint32_t i)             { vm_outl(io_base, i); return vm_inl(io_base + 4); }

int vmsvga_detect(void)
{
    uint8_t b, d, f;
    return pci_find(&b, &d, &f);
}

int vmsvga_init(uint32_t w, uint32_t h, vmsvga_info_t *out)
{
    uint8_t b, d, f;
    if (pci_find(&b, &d, &f)) return -1;

    /* 打开 I/O 空间 + 内存空间 + Bus Master。
       高 16 位是 Status（写 1 清零），必须屏蔽掉。 */
    uint32_t cmd = pci_read32(b, d, f, 0x04);
    pci_write32(b, d, f, 0x04, (cmd & 0xFFFF) | 0x7);

    uint32_t bar0 = pci_read32(b, d, f, 0x10);  /* I/O 端口 */
    uint32_t bar1 = pci_read32(b, d, f, 0x14);  /* 帧缓冲   */
    uint32_t bar2 = pci_read32(b, d, f, 0x18);  /* FIFO     */
    io_base = (uint16_t)(bar0 & 0xFFFCu);
    uint32_t fb_phys   = bar1 & 0xFFFFFFF0u;
    uint32_t fifo_phys = bar2 & 0xFFFFFFF0u;

    /* 版本协商：写入 ID_2，读回相同才说明支持 */
    wreg(REG_ID, SVGA_ID_2);
    if (rreg(REG_ID) != SVGA_ID_2) return -2;

    if (w > rreg(REG_MAX_WIDTH) || h > rreg(REG_MAX_HEIGHT)) return -3;
    uint32_t vram = rreg(REG_VRAM_SIZE);
    if (vram && (uint64_t)w * h * 4 > vram) return -4;

    /* 设置模式 */
    wreg(REG_WIDTH, w);
    wreg(REG_HEIGHT, h);
    wreg(REG_BPP, 32);

    /* 先 ENABLE，再初始化 FIFO，最后 CONFIG_DONE（与 Linux vmwgfx 顺序一致） */
    wreg(REG_ENABLE, 1);

    uint32_t caps = rreg(REG_CAPS);
    uint32_t regs = 4;
    if (caps & SVGA_CAP_EXT_FIFO) {
        regs = rreg(REG_MEM_REGS);
        if (regs < 4) regs = 4;
    }
    uint32_t fifo_size = rreg(REG_MEM_SIZE);
    fifo = (volatile uint32_t *)fifo_phys;
    fifo[FIFO_MIN]      = regs * 4;
    fifo[FIFO_MAX]      = fifo_size;
    fifo[FIFO_NEXT_CMD] = regs * 4;
    fifo[FIFO_STOP]     = regs * 4;
    __asm__ volatile("" ::: "memory");
    wreg(REG_CONFIG_DONE, 1);

    cur_w = w; cur_h = h;
    if (out) {
        out->fb     = (uint8_t *)(fb_phys + rreg(REG_FB_OFFSET));
        out->width  = rreg(REG_WIDTH);
        out->height = rreg(REG_HEIGHT);
        out->bpp    = rreg(REG_BPP);
        out->pitch  = rreg(REG_BYTES_PER_LINE);
    }
    return 0;
}

void vmsvga_update(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t cmd[5] = { SVGA_CMD_UPDATE, x, y, w, h };
    uint32_t min  = fifo[FIFO_MIN];
    uint32_t max  = fifo[FIFO_MAX];
    uint32_t next = fifo[FIFO_NEXT_CMD];

    /* 等待环形队列有至少 20 字节 + 1 个 dword 空隙 */
    for (int spin = 0; ; spin++) {
        uint32_t stop = fifo[FIFO_STOP];
        uint32_t used = (next >= stop) ? (next - stop) : ((max - min) - (stop - next));
        uint32_t freeb = (max - min) - used;
        if (freeb > sizeof(cmd) + 4) break;
        wreg(REG_SYNC, 1);
        for (int i = 0; i < 1000000 && rreg(REG_BUSY); i++) { }
        if (spin > 100) return;          /* 放弃，避免死循环 */
    }

    for (int i = 0; i < 5; i++) {
        fifo[next >> 2] = cmd[i];
        next += 4;
        if (next >= max) next = min;
    }
    __asm__ volatile("" ::: "memory");
    fifo[FIFO_NEXT_CMD] = next;          /* 整条命令写完后再推进 */
}

void vmsvga_flush(void)
{
    vmsvga_update(0, 0, cur_w, cur_h);
}
