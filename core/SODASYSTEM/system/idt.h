/* idt.h - 中断描述符表 / PIC / 中断分发 */
#ifndef SODA_IDT_H
#define SODA_IDT_H

#include "types.h"

/* ISR 桩的 C 端寄存器快照 */
typedef struct {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
} regs_t;

/* 每个 IRQ 注册的处理函数 */
typedef void (*irq_handler_t)(regs_t* r);

void idt_init(void);
void pic_remap(void);
void pic_send_eoi(uint8_t irq);
void irq_install_handler(uint8_t irq, irq_handler_t handler);
void irq_unmask(uint8_t irq);

#endif