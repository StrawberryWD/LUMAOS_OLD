; kernel/arch/interrupts.asm - LumaOS 4.0 ISR/IRQ stubs
;
; ISR-1 (CRITICAL FIX): removed "pop esp" from both isr_common and
;   irq_common.  The original code pushed esp then popped it back,
;   restoring a stale (already-decremented) value into the ESP register
;   and corrupting the entire unwind sequence → guaranteed triple fault
;   on first interrupt.  Replaced with "add esp, 4" to skip the slot.

BITS 32
section .text

extern isr_handler
extern irq_handler

%macro ISR_NOERR 1
global isr%1
isr%1:
    cli
    push dword 0
    push dword %1
    jmp  isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    cli
    push dword %1
    jmp  isr_common
%endmacro

ISR_NOERR  0
ISR_NOERR  1
ISR_NOERR  2
ISR_NOERR  3
ISR_NOERR  4
ISR_NOERR  5
ISR_NOERR  6
ISR_NOERR  7
ISR_ERR    8
ISR_NOERR  9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15

isr_common:
    push eax
    push ecx
    push edx
    push ebx
    lea  eax, [esp + 16]        ; snapshot pre-push ESP for diagnostics
    push eax                    ; push esp_snapshot (never restored to %esp)
    push ebp
    push esi
    push edi
    mov  ax, ds
    push eax
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    push esp
    call isr_handler
    add  esp, 4
    pop  eax
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    pop  edi
    pop  esi
    pop  ebp
    add  esp, 4                 ; FIX ISR-1: skip esp_snapshot — do NOT pop esp
    pop  ebx
    pop  edx
    pop  ecx
    pop  eax
    add  esp, 8
    iret

%macro IRQ 1
global irq%1
irq%1:
    cli
    push dword 0
    push dword (32 + %1)
    jmp  irq_common
%endmacro

IRQ  0
IRQ  1
IRQ  2
IRQ  3
IRQ  4
IRQ  5
IRQ  6
IRQ  7
IRQ  8
IRQ  9
IRQ 10
IRQ 11
IRQ 12
IRQ 13
IRQ 14
IRQ 15

irq_common:
    push eax
    push ecx
    push edx
    push ebx
    lea  eax, [esp + 16]
    push eax
    push ebp
    push esi
    push edi
    mov  ax, ds
    push eax
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    push esp
    call irq_handler
    add  esp, 4
    pop  eax
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    pop  edi
    pop  esi
    pop  ebp
    add  esp, 4                 ; FIX ISR-1: skip esp_snapshot
    pop  ebx
    pop  edx
    pop  ecx
    pop  eax
    add  esp, 8
    iret

section .note.GNU-stack noalloc noexec nowrite progbits
