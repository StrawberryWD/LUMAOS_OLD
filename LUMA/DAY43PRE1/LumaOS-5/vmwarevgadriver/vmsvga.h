#ifndef VMSVGA_H
#define VMSVGA_H

#include <stdint.h>

typedef struct {
    uint8_t *fb;        /* 帧缓冲起始地址（已加 FB_OFFSET） */
    uint32_t width;
    uint32_t height;
    uint32_t bpp;       /* 32 */
    uint32_t pitch;     /* 每行字节数——一定用它，不要假设 width*4 */
} vmsvga_info_t;

/* 返回 0 = 找到 VMware SVGA II 设备（PCI 15AD:0405） */
int  vmsvga_detect(void);

/* 设置 w x h x 32bpp。成功返回 0，失败返回负数 */
int  vmsvga_init(uint32_t w, uint32_t h, vmsvga_info_t *out);

/* 通知宿主：该矩形的帧缓冲内容已变化（必须调用，否则画面可能不刷新） */
void vmsvga_update(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* 刷新整屏 */
void vmsvga_flush(void);

#endif
