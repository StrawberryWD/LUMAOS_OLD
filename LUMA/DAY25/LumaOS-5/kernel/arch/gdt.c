/* kernel/arch/gdt.c - LumaOS 4.0 GDT
 * GDT-1: push/retfl replaces unreliable ljmp inline asm under GCC -O2
 * GDT-2: FS and GS are now reloaded after lgdt (were missing before)
 */
#include <stdint.h>

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) gdt_ptr_t;

#define GDT_ENTRIES 5
static gdt_entry_t gdt[GDT_ENTRIES];
static gdt_ptr_t   gdt_ptr;

static void gdt_set_entry(int n, uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t gran)
{
    if (n < 0 || n >= GDT_ENTRIES) return;
    gdt[n].base_low   = (uint16_t)(base  & 0xFFFF);
    gdt[n].base_mid   = (uint8_t)((base  >> 16) & 0xFF);
    gdt[n].base_high  = (uint8_t)((base  >> 24) & 0xFF);
    gdt[n].limit_low  = (uint16_t)(limit & 0xFFFF);
    gdt[n].granularity= (uint8_t)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    gdt[n].access     = access;
}

void gdt_init(void)
{
    gdt_ptr.limit = (uint16_t)(sizeof(gdt_entry_t) * GDT_ENTRIES - 1);
    gdt_ptr.base  = (uint32_t)&gdt;

    gdt_set_entry(0, 0, 0,          0x00, 0x00); /* null        */
    gdt_set_entry(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); /* kernel code */
    gdt_set_entry(2, 0, 0xFFFFFFFF, 0x92, 0xCF); /* kernel data */
    gdt_set_entry(3, 0, 0xFFFFFFFF, 0x92, 0xCF); /* kernel stack*/
    gdt_set_entry(4, 0, 0xFFFFFFFF, 0xFA, 0xCF); /* user code   */

    __asm__ volatile ("lgdt %0" : : "m"(gdt_ptr) : "memory");

    /* GDT-1: push/retfl is reliable across all GCC/binutils versions.
     * "ljmp $0x08,$1f" can mis-relocate under -O2. */
    __asm__ volatile (
        "pushl $0x08\n\t"
        "pushl $1f\n\t"
        "retfl\n\t"
        "1:\n\t"
        : : : "memory"
    );

    /* GDT-2: reload ALL data segment registers including FS and GS */
    __asm__ volatile (
        "movl $0x10, %%eax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        : : : "eax", "memory"
    );
}
