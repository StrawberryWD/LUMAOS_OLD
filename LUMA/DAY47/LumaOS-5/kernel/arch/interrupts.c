/* kernel/arch/interrupts.c - LumaOS 4.0 Interrupt Dispatch
 *
 * ISR-3 (CRITICAL FIX): isr_context_t was missing the 'ds' field.
 *   The asm stub pushes DS (as 32-bit) between eax and int_no, so
 *   without that field ctx->int_no silently read the DS value (0x10)
 *   and every switch() fell through to default, masking all faults.
 *
 * Stack layout built by interrupts.asm (low → high):
 *   [esp+ 0] edi
 *   [esp+ 4] esi
 *   [esp+ 8] ebp
 *   [esp+12] esp_snapshot
 *   [esp+16] ebx
 *   [esp+20] edx
 *   [esp+24] ecx
 *   [esp+28] eax
 *   [esp+32] ds           ← was missing
 *   [esp+36] int_no
 *   [esp+40] err_code
 *   [esp+44] eip  (CPU)
 *   [esp+48] cs   (CPU)
 *   [esp+52] eflags (CPU)
 */
#include <stdint.h>

extern void pic_eoi(uint8_t irq);

typedef struct {
    uint32_t edi, esi, ebp, esp;
    uint32_t ebx, edx, ecx, eax;
    uint32_t ds;            /* FIX ISR-3: was missing */
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
} __attribute__((packed)) isr_context_t;

static uint32_t interrupt_count = 0;

void isr_handler(isr_context_t *ctx)
{
    interrupt_count++;
    switch (ctx->int_no) {
    case 0: case 1: case 2: case 3: case 4: case 5:
    case 6: case 7: case 8: case 9: case 10: case 11:
    case 12: case 13: case 14: case 15:
    default: break;
    }
}

extern void keyboard_irq_handler(void);
extern void mouse_irq_handler(void);

void irq_handler(isr_context_t *ctx)
{
    uint32_t irq = ctx->int_no - 32;
    interrupt_count++;
    switch (irq) {
    case 0:  break;
    case 1:  keyboard_irq_handler(); break;
    case 2:  break;
    case 12: mouse_irq_handler();    break;
    default: break;
    }
    pic_eoi((uint8_t)irq);
}

void     enable_interrupts(void)  { __asm__ volatile ("sti"); }
void     disable_interrupts(void) { __asm__ volatile ("cli"); }
uint32_t get_interrupt_count(void)  { return interrupt_count; }
void     reset_interrupt_count(void){ interrupt_count = 0; }

uint32_t interrupts_enabled(void)
{
    uint32_t f;
    __asm__ volatile ("pushf; pop %0" : "=r"(f));
    return (f >> 9) & 1;
}
