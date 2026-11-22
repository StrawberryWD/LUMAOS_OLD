/* kernel/drivers/vga_gfx.c
 * LumaOS 4.1 - Bochs VBE (BGA) Graphics Driver
 *
 * DAY43 PRE-2 (Week 7): VMware SVGA II backend added.
 *   vga_init_graphics() now probes for a VMware SVGA II device first
 *   (kernel/drivers/vmsvga.c; VMware Workstation/Player/Fusion, QEMU
 *   `-vga vmware`). If it is there and accepts 800x450x32 that backend
 *   is used, otherwise the original BGA 800x450x8 path below runs
 *   unchanged (QEMU `-vga std`, Bochs, VirtualBox "VBoxVGA").
 *
 *   Why the GUI code did not have to change: everything above this
 *   layer (gui, shell, write overlay, font code) draws into the 8bpp
 *   back buffer with palette INDICES through vga_put_pixel/draw_rect/
 *   vga_set_palette/vga_flip. The two backends differ only in the last
 *   step, vga_flip():
 *     BGA   : copy the 8bpp back buffer into the 8bpp LFB; the
 *             hardware DAC turns index -> colour.
 *     SVGA  : the framebuffer is 32bpp, there is no DAC, so we keep a
 *             software copy of the palette (pal32[]; vga_set_palette()
 *             fills it), convert only the rows that changed since the
 *             previous frame (compared against a shadow of the last
 *             frame at 0x00400000) and then send ONE SVGA_CMD_UPDATE
 *             for the changed band.  A theme switch reprograms the
 *             palette, which forces the next flip to convert every row.
 *
 *   Memory map (flat, below the 256 MB QEMU gives us):
 *     0x00010000 kernel   0x00200000 stack top   0x00300000 back buffer
 *     0x00400000 shadow of the last frame (SVGA backend only, 360 KB)
 *
 * STAGE1-GFX: Replaced VGA Mode 13h (320x200) with Bochs VBE BGA interface.
 *   Resolution : 800x450, 8bpp (256-color palette), linear framebuffer.
 *   Back buffer : 0x00300000 (3 MB mark, 360 KB required).
 *
 * FIX GFX-1 (CRITICAL — black screen root cause):
 *   The LFB physical address is NOT a fixed constant. The previous code
 *   hard-coded 0xE0000000, which is only correct when QEMU treats the
 *   Bochs display as an ISA-only device. With "-machine type=pc" (the
 *   i440FX/PIIX chipset used by run.sh / run_acpi.sh), the Bochs VGA
 *   device sits on PCI, and SeaBIOS assigns its framebuffer base (BAR0)
 *   dynamically — commonly something like 0xFD000000, NOT 0xE0000000.
 *
 *   Symptom this caused: BGA was correctly configured (800x450 8bpp),
 *   the palette was correctly programmed, vga_flip() ran every frame
 *   with no crash or hang — but every copy went to an address that was
 *   never mapped to real video memory, so nothing ever appeared on
 *   screen. Total black screen with an otherwise fully healthy kernel.
 *
 *   Fix: read the real framebuffer base from PCI config space (BAR0 of
 *   vendor 0x1234 / device 0x1111, the standard Bochs/QEMU VGA ID) at
 *   graphics-init time. Fall back to the legacy 0xE0000000 constant
 *   only if no such PCI device is found (e.g. real hardware or an
 *   ISA-only emulator without a PCI bus).
 *
 * BGA I/O ports:
 *   0x01CE = VBE_DISPI_IOPORT_INDEX
 *   0x01CF = VBE_DISPI_IOPORT_DATA
 *
 * Index registers (write index to 0x01CE, value to 0x01CF):
 *   0x00 = VBE_DISPI_INDEX_ID            (read: 0xB0C0..0xB0C5)
 *   0x01 = VBE_DISPI_INDEX_XRES
 *   0x02 = VBE_DISPI_INDEX_YRES
 *   0x03 = VBE_DISPI_INDEX_BPP
 *   0x04 = VBE_DISPI_INDEX_ENABLE
 *   0x05 = VBE_DISPI_INDEX_BANK
 *   0x06 = VBE_DISPI_INDEX_VIRT_WIDTH
 *   0x07 = VBE_DISPI_INDEX_VIRT_HEIGHT
 *   0x08 = VBE_DISPI_INDEX_X_OFFSET
 *   0x09 = VBE_DISPI_INDEX_Y_OFFSET
 *
 * VBE_DISPI_ENABLED  = 0x01
 * VBE_DISPI_LFB_ENABLED = 0x40
 * VBE_DISPI_NOCLEARMEM  = 0x80
 *
 * IMPORTANT: Build must link with -m elf_i386; back buffer at 3MB is
 * above kernel (~50-80 KB at 0x10000) and above the 2MB kernel stack.
 * The LFB (wherever PCI places it) is in high physical memory; the GDT
 * flat 4 GB data segment (base=0, limit=0xFFFFFFFF) covers it with a
 * plain 32-bit pointer, so no paging/segmentation changes are needed.
 */

#include "kernel.h"
#include "vmsvga.h"
#include <stdint.h>

/* ===== Screen geometry ===== */
#define VGA_WIDTH    800
#define VGA_HEIGHT   450
#define VGA_BPP      8
#define VGA_STRIDE   VGA_WIDTH               /* bytes per row (8bpp) */
#define VGA_SIZE     (VGA_WIDTH * VGA_HEIGHT) /* 360 000 bytes */

/* ===== BGA ports & constants ===== */
#define BGA_IDX   0x01CE
#define BGA_DATA  0x01CF

#define BGA_ID           0x00
#define BGA_XRES         0x01
#define BGA_YRES         0x02
#define BGA_BPP          0x03
#define BGA_ENABLE       0x04
#define BGA_BANK         0x05
#define BGA_VIRT_W       0x06
#define BGA_VIRT_H       0x07
#define BGA_X_OFFSET     0x08
#define BGA_Y_OFFSET     0x09

#define BGA_DISABLED         0x00
#define BGA_ENABLED          0x01
#define BGA_LFB_ENABLED      0x40
#define BGA_NOCLEARMEM       0x80

/* ===== NEW (FIX GFX-1): minimal PCI config-space access =====
 * Just enough to find BAR0 of the Bochs/QEMU standard VGA device —
 * not a general-purpose PCI driver.                                   */
#define PCI_CONFIG_ADDR  0x0CF8
#define PCI_CONFIG_DATA  0x0CFC

static inline uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn  << 8)
                  | (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

/* Scan bus 0, all 32 device slots, function 0, for the Bochs/QEMU
 * standard VGA device (vendor 0x1234, device 0x1111) and return its
 * BAR0 (LFB physical base), masked to a clean page-aligned address.
 * Returns 0 if the device is not found on bus 0.                      */
static uint32_t pci_find_bochs_lfb(void)
{
    for (uint8_t dev = 0; dev < 32; dev++) {
        uint32_t id = pci_cfg_read32(0, dev, 0, 0x00);
        if (id == 0xFFFFFFFFu) continue;              /* no device here */
        uint16_t vendor = (uint16_t)(id & 0xFFFF);
        uint16_t device = (uint16_t)(id >> 16);
        if (vendor == 0x1234 && device == 0x1111) {   /* Bochs/QEMU std VGA */
            uint32_t bar0 = pci_cfg_read32(0, dev, 0, 0x10);
            /* Mask off low info bits (bit0=space, bits1-2=type,
             * bit3=prefetchable) to get the actual base address.      */
            return bar0 & 0xFFFFFFF0u;
        }
    }
    return 0;
}

/* ===== NEW (FIX GFX-1): minimal serial (COM1) debug output =====
 * Used only for pre-GUI diagnostics, since text-mode console output
 * is still available at this point but graphics output is not yet
 * verified. Safe to leave in permanently — costs nothing if nobody
 * is listening on the port.                                           */
#define COM1 0x3F8

static void serial_init(void)
{
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static void serial_putc(char c)
{
    while ((inb(COM1 + 5) & 0x20) == 0) { }
    outb(COM1, (uint8_t)c);
}

static void serial_puts(const char *s)
{
    while (*s) serial_putc(*s++);
}

static void serial_hex32(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = 28; i >= 0; i -= 4)
        serial_putc(hex[(v >> i) & 0xF]);
}

/* ===== Linear framebuffer (physical) =====
 * FIX GFX-1: no longer `const` — the real value is discovered at
 * runtime in vga_init_graphics(). Falls back to the legacy ISA
 * address if PCI probing finds nothing.                                */
static unsigned char *vga_mem = (unsigned char *)0xE0000000;

/* ===== Back buffer (in normal RAM at 3 MB) ===== */
/* 360 000 bytes.  entry.asm sets esp = 0x00200000 (2 MB, top of the
 * kernel stack in BSS). The back buffer lives at 0x00300000 (3 MB) to
 * stay well clear of the stack.                                       */
static unsigned char * const back_buffer = (unsigned char *)0x00300000;

/* ===== DAY43 PRE-2: VMware SVGA backend state ===== */
static int            g_svga        = 0;   /* 1 = VMware SVGA II backend active */
static uint8_t       *svga_fb       = 0;   /* 32bpp framebuffer */
static uint32_t       svga_pitch    = 0;   /* bytes per framebuffer row */
static uint32_t       pal32[256];          /* index -> 0x00RRGGBB (software DAC) */
static int            svga_force_full = 1; /* next flip converts every row */
static unsigned       svga_flips    = 0;
/* shadow copy of the last frame sent to the host (8bpp, same layout as
 * back_buffer). Fixed address, NOT .bss: the kernel's BSS is already
 * ~290 KB above a 0x10000 load address and another 360 KB would run into
 * the 0xA0000 VGA/BIOS hole. */
static uint8_t * const prev_frame = (uint8_t *)0x00400000;

/* ===== BGA helper ===== */
static inline void bga_write(uint16_t index, uint16_t value)
{
    outw(BGA_IDX,  index);
    outw(BGA_DATA, value);
}

/* ===== vga_set_palette ===== */
/* VGA DAC (palette) registers are still present on the Bochs VGA device
 * even in LFB mode.  8bpp indexed color uses the same DAC as Mode 13h. */
void vga_set_palette(unsigned char index, unsigned char r,
                     unsigned char g, unsigned char b)
{
    /* DAY43 PRE-2: always keep the software copy (6-bit VGA DAC value
     * 0..63 -> 8-bit: v<<2 | v>>4, so 63 -> 255). The SVGA backend has
     * no DAC and converts through this table in vga_flip(). */
    uint32_t r8 = ((uint32_t)(r & 63) << 2) | ((r & 63) >> 4);
    uint32_t g8 = ((uint32_t)(g & 63) << 2) | ((g & 63) >> 4);
    uint32_t b8 = ((uint32_t)(b & 63) << 2) | ((b & 63) >> 4);
    pal32[index] = (r8 << 16) | (g8 << 8) | b8;
    svga_force_full = 1;                 /* colours changed: repaint everything */
    if (g_svga) return;                  /* no VGA DAC to program */

    outb(0x3C8, index);
    outb(0x3C9, r);
    outb(0x3C9, g);
    outb(0x3C9, b);
}

/* ===== vga_init_graphics ===== */
void vga_init_graphics(void)
{
    /* FIX GFX-1: probe PCI for the real framebuffer address before
     * touching anything else. Serial output lets you confirm what
     * address was actually used even though nothing is on screen yet. */
    serial_init();
    serial_puts("[vga_gfx] probing display hardware (VMware SVGA II, then Bochs BGA)...\r\n");

    /* DAY43 PRE-2: VMware SVGA II first. Probe/initialise it; on any
     * failure fall through to the Bochs VBE path below. */
    g_svga = 0;
    if (vmsvga_detect() == 0) {
        vmsvga_info_t si;
        int rc = vmsvga_init(VGA_WIDTH, VGA_HEIGHT, &si);
        if (rc == 0 && si.width == VGA_WIDTH && si.height == VGA_HEIGHT) {
            g_svga     = 1;
            svga_fb    = si.fb;
            svga_pitch = si.pitch;
            svga_force_full = 1;
            serial_puts("[vga_gfx] backend = VMware SVGA II 800x450x32, fb=");
            serial_hex32((uint32_t)si.fb);
            serial_puts(" pitch=");
            serial_hex32(si.pitch);
            serial_puts("\r\n");
        } else {
            serial_puts("[vga_gfx] VMware SVGA II found but init failed (rc=");
            serial_hex32((uint32_t)rc);
            serial_puts("), falling back to BGA\r\n");
        }
    }

    uint32_t lfb = g_svga ? 0 : pci_find_bochs_lfb();
    if (!g_svga) {
    if (lfb != 0) {
        vga_mem = (unsigned char *)lfb;
        serial_puts("[vga_gfx] found BAR0 = ");
        serial_hex32(lfb);
        serial_puts("  -> using as LFB\r\n");
    } else {
        serial_puts("[vga_gfx] PCI probe found no Bochs VGA device, "
                     "falling back to 0xE0000000\r\n");
    }

    /* 1. Disable VBE extension while we configure */
    bga_write(BGA_ENABLE, BGA_DISABLED);

    /* 2. Set resolution and colour depth */
    bga_write(BGA_XRES, (uint16_t)VGA_WIDTH);
    bga_write(BGA_YRES, (uint16_t)VGA_HEIGHT);
    bga_write(BGA_BPP,  (uint16_t)VGA_BPP);

    /* 3. Virtual size == physical size (no double-height trick needed) */
    bga_write(BGA_VIRT_W, (uint16_t)VGA_WIDTH);
    bga_write(BGA_VIRT_H, (uint16_t)VGA_HEIGHT);
    bga_write(BGA_X_OFFSET, 0);
    bga_write(BGA_Y_OFFSET, 0);

    /* 4. Enable LFB mode (do NOT clear mem so we can prefill palette) */
    bga_write(BGA_ENABLE, BGA_ENABLED | BGA_LFB_ENABLED);
    }   /* !g_svga */

    /* 5. Set up the 256-colour palette (same values as old Mode 13h) */
    /* Standard 16 CGA colours (indices 0-15) */
    vga_set_palette( 0,  0,  0,  0);
    vga_set_palette( 1,  0,  0, 42);
    vga_set_palette( 2,  0, 42,  0);
    vga_set_palette( 3,  0, 42, 42);
    vga_set_palette( 4, 42,  0,  0);
    vga_set_palette( 5, 42,  0, 42);
    vga_set_palette( 6, 42, 21,  0);
    vga_set_palette( 7, 42, 42, 42);
    vga_set_palette( 8, 21, 21, 21);
    vga_set_palette( 9, 21, 21, 63);
    vga_set_palette(10, 21, 63, 21);
    vga_set_palette(11, 21, 63, 63);
    vga_set_palette(12, 63, 21, 21);
    vga_set_palette(13, 63, 21, 63);
    vga_set_palette(14, 63, 63, 21);
    vga_set_palette(15, 63, 63, 63);

    /* Extended GUI palette (indices 16-35) — filled by gui_set_palette().
     * We zero them here so the screen is black on first flip.          */
    for (int i = 16; i < 256; i++)
        vga_set_palette((unsigned char)i, 0, 0, 0);

    /* 6. Clear back buffer to colour 0 (black) */
    for (int i = 0; i < VGA_SIZE; i++)
        back_buffer[i] = 0;

    serial_puts("[vga_gfx] init complete\r\n");
}

/* ===== svga_flip (DAY43 PRE-2) =====
 * 8bpp back buffer -> 32bpp VMware framebuffer, only for rows that
 * differ from the previous frame (or all rows after a palette change),
 * then a single UPDATE command covering the changed band of rows. */
static void svga_flip(void)
{
    const uint8_t *src  = back_buffer;
    uint8_t       *prev = prev_frame;
    int y0 = -1, y1 = -1;

    for (int y = 0; y < VGA_HEIGHT; y++) {
        const uint8_t  *srow = src  + y * VGA_STRIDE;
        uint8_t        *prow = prev + y * VGA_STRIDE;
        if (!svga_force_full) {
            const uint32_t *s32 = (const uint32_t *)srow;
            const uint32_t *p32 = (const uint32_t *)prow;
            int same = 1;
            for (int i = 0; i < VGA_STRIDE / 4; i++)
                if (s32[i] != p32[i]) { same = 0; break; }
            if (same) continue;
        }
        uint32_t *dst = (uint32_t *)(svga_fb + (uint32_t)y * svga_pitch);
        for (int x = 0; x < VGA_WIDTH; x++) {
            unsigned char c = srow[x];
            dst[x]  = pal32[c];
            prow[x] = c;
        }
        if (y0 < 0) y0 = y;
        y1 = y;
    }
    svga_force_full = 0;
    svga_flips++;

    if (y0 >= 0)
        vmsvga_update(0, (uint32_t)y0, VGA_WIDTH, (uint32_t)(y1 - y0 + 1));
    else if ((svga_flips & 63u) == 0)
        vmsvga_flush();      /* nothing changed: still re-announce the whole
                              * screen now and then in case the host dropped one */
}

/* ===== vga_flip ===== */
/* BGA: copy back buffer -> LFB (800*450 = 360 000 bytes, 32 bits at a
 * time). SVGA: see svga_flip() above.                                  */
void vga_flip(void)
{
    if (g_svga) { svga_flip(); return; }

    uint32_t       *dst = (uint32_t *)vga_mem;
    const uint32_t *src = (const uint32_t *)back_buffer;
    /* 360 000 / 4 = 90 000 dwords */
    for (int i = 0; i < (VGA_SIZE / 4); i++)
        dst[i] = src[i];
    /* Tail bytes (360 000 % 4 == 0, so none needed) */
}

/* ===== vga_clear_screen ===== */
void vga_clear_screen(unsigned char color)
{
    /* Fill all 4 bytes at once */
    uint32_t val = (uint32_t)color
                 | ((uint32_t)color << 8)
                 | ((uint32_t)color << 16)
                 | ((uint32_t)color << 24);
    uint32_t *buf = (uint32_t *)back_buffer;
    for (int i = 0; i < (VGA_SIZE / 4); i++)
        buf[i] = val;
}

/* ===== vga_put_pixel ===== */
void vga_put_pixel(int x, int y, unsigned char color)
{
    if ((unsigned)x >= VGA_WIDTH || (unsigned)y >= VGA_HEIGHT) return;
    back_buffer[y * VGA_STRIDE + x] = color;
}

/* ===== vga_get_pixel ===== */
unsigned char vga_get_pixel(int x, int y)
{
    if ((unsigned)x >= VGA_WIDTH || (unsigned)y >= VGA_HEIGHT) return 0;
    return back_buffer[y * VGA_STRIDE + x];
}

/* ===== vga_draw_rect ===== */
void vga_draw_rect(int x, int y, int w, int h, unsigned char color)
{
    /* Clip */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > VGA_WIDTH)  w = VGA_WIDTH  - x;
    if (y + h > VGA_HEIGHT) h = VGA_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    for (int row = 0; row < h; row++) {
        unsigned char *p = back_buffer + (y + row) * VGA_STRIDE + x;
        for (int col = 0; col < w; col++)
            p[col] = color;
    }
}

/* ===== vga_draw_hline ===== */
void vga_draw_hline(int x, int y, int len, unsigned char color)
{
    if ((unsigned)y >= VGA_HEIGHT) return;
    if (x < 0) { len += x; x = 0; }
    if (x + len > VGA_WIDTH) len = VGA_WIDTH - x;
    if (len <= 0) return;
    unsigned char *p = back_buffer + y * VGA_STRIDE + x;
    for (int i = 0; i < len; i++) p[i] = color;
}

/* ===== vga_draw_vline ===== */
void vga_draw_vline(int x, int y, int len, unsigned char color)
{
    if ((unsigned)x >= VGA_WIDTH) return;
    if (y < 0) { len += y; y = 0; }
    if (y + len > VGA_HEIGHT) len = VGA_HEIGHT - y;
    if (len <= 0) return;
    for (int i = 0; i < len; i++)
        back_buffer[(y + i) * VGA_STRIDE + x] = color;
}

/* ===== vga_draw_circle (Bresenham) ===== */
void vga_draw_circle(int cx, int cy, int radius, unsigned char color)
{
    int x = radius, y = 0, d = 3 - 2 * radius;
    while (x >= y) {
        vga_put_pixel(cx+x, cy+y, color); vga_put_pixel(cx-x, cy+y, color);
        vga_put_pixel(cx+x, cy-y, color); vga_put_pixel(cx-x, cy-y, color);
        vga_put_pixel(cx+y, cy+x, color); vga_put_pixel(cx-y, cy+x, color);
        vga_put_pixel(cx+y, cy-x, color); vga_put_pixel(cx-y, cy-x, color);
        if (d < 0) { d += 4*y+6; }
        else       { d += 4*(y-x)+10; x--; }
        y++;
    }
}

/* ===== Geometry accessors ===== */
int vga_gfx_width(void)  { return VGA_WIDTH;  }
int vga_gfx_height(void) { return VGA_HEIGHT; }

/* ===== DAY43 PRE-2: backend introspection (Info window, serial log) ===== */
int vga_gfx_is_svga(void) { return g_svga; }

const char *vga_gfx_backend_name(void)
{
    return g_svga ? "VMware SVGA II 800x450 32bpp"
                  : "BGA 800x450 256-color";
}

/* physical address of the framebuffer actually being written */
uint32_t vga_gfx_fb_addr(void)
{
    return g_svga ? (uint32_t)svga_fb : (uint32_t)vga_mem;
}
