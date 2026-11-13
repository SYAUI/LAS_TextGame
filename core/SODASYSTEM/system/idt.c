/* idt.c - IDT + PIC + ISR 分发 */
#include "idt.h"
#include "io.h"
#include "kernel.h"
#include "lib.h"

/* ---- IDT 门描述符 ---- */
struct idt_entry {
    uint16_t isr_low;
    uint16_t kernel_cs;
    uint8_t  reserved;
    uint8_t  attributes;
    uint16_t isr_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

/* ---- 外部 ISR 桩 ---- */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);  extern void irq3(void);
extern void irq4(void);  extern void irq5(void);  extern void irq6(void);  extern void irq7(void);
extern void irq8(void);  extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);

/* ---- IRQ 处理表 ---- */
static irq_handler_t irq_handlers[16] = { 0 };

/* ---- 内部辅助 ---- */
static void idt_set_gate(uint8_t n, uint32_t handler) {
    idt[n].isr_low    = handler & 0xFFFF;
    idt[n].kernel_cs  = 0x08;              /* 内核代码段 */
    idt[n].reserved   = 0;
    idt[n].attributes = 0x8E;              /* P=1, DPL=0, 32-bit 中断门 */
    idt[n].isr_high   = (handler >> 16) & 0xFFFF;
}

void idt_init(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint32_t)&idt;

    memset(idt, 0, sizeof(idt));

    /* 32 个 CPU 异常 */
    uint32_t isr_ptrs[32] = {
        (uint32_t)isr0,  (uint32_t)isr1,  (uint32_t)isr2,  (uint32_t)isr3,
        (uint32_t)isr4,  (uint32_t)isr5,  (uint32_t)isr6,  (uint32_t)isr7,
        (uint32_t)isr8,  (uint32_t)isr9,  (uint32_t)isr10, (uint32_t)isr11,
        (uint32_t)isr12, (uint32_t)isr13, (uint32_t)isr14, (uint32_t)isr15,
        (uint32_t)isr16, (uint32_t)isr17, (uint32_t)isr18, (uint32_t)isr19,
        (uint32_t)isr20, (uint32_t)isr21, (uint32_t)isr22, (uint32_t)isr23,
        (uint32_t)isr24, (uint32_t)isr25, (uint32_t)isr26, (uint32_t)isr27,
        (uint32_t)isr28, (uint32_t)isr29, (uint32_t)isr30, (uint32_t)isr31,
    };
    for (int i = 0; i < 32; i++) idt_set_gate(i, isr_ptrs[i]);

    /* 16 个 IRQ → INT 0x20~0x2F */
    uint32_t irq_ptrs[16] = {
        (uint32_t)irq0,  (uint32_t)irq1,  (uint32_t)irq2,  (uint32_t)irq3,
        (uint32_t)irq4,  (uint32_t)irq5,  (uint32_t)irq6,  (uint32_t)irq7,
        (uint32_t)irq8,  (uint32_t)irq9,  (uint32_t)irq10, (uint32_t)irq11,
        (uint32_t)irq12, (uint32_t)irq13, (uint32_t)irq14, (uint32_t)irq15,
    };
    for (int i = 0; i < 16; i++) idt_set_gate(0x20 + i, irq_ptrs[i]);

    __asm__ __volatile__("lidt %0" : : "m"(idtp));
}

/* ---- PIC 重映射 ---- */
void pic_remap(void) {
    /* ICW1: 开始初始化，需要 ICW4 */
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();

    /* ICW2: 主 PIC 起始向量 = 0x20，从 PIC 起始向量 = 0x28 */
    outb(0x21, 0x20); io_wait();
    outb(0xA1, 0x28); io_wait();

    /* ICW3: 主从连接 —— 从片接在主片的 IRQ2 */
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();

    /* ICW4: 8086 模式 */
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();

    /* OCW1: 屏蔽所有 IRQ（初始化完成后再按需开） */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) outb(0xA0, 0x20);   /* 从片 EOI */
    outb(0x20, 0x20);                  /* 主片 EOI */
}

void irq_install_handler(uint8_t irq, irq_handler_t handler) {
    if (irq < 16) irq_handlers[irq] = handler;
}

void irq_unmask(uint8_t irq) {
    uint16_t port = (irq < 8) ? 0x21 : 0xA1;
    uint8_t  bit  = (uint8_t)(1u << (irq & 7));
    uint8_t  mask = inb(port);
    outb(port, mask & ~bit);
}

/* ---- ISR 分发 ---- */
void isr_handler(regs_t* r) {
    if (r->int_no < 0x20) {
        /* CPU 异常 */
        kprintf("\n[EXC %u] err=0x%x eip=0x%x cs=0x%x eflags=0x%x\n",
                r->int_no, r->err_code, r->eip, r->cs, r->eflags);
        if (r->err_code) {
            kprintf("  cr2=0x%x\n", 0);    /* 后续如果实现 CR2 读取可以改 */
        }
        kprintf("  System halted.\n");
        for (;;) __asm__ __volatile__("cli; hlt");
    } else {
        /* 硬件中断 */
        uint8_t irq = (uint8_t)(r->int_no - 0x20);
        if (irq < 16 && irq_handlers[irq]) {
            irq_handlers[irq](r);
        }
        pic_send_eoi(irq);
    }
}