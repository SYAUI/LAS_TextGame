/* pit.c - PIT 定时器 + IRQ0 处理 */
#include "pit.h"
#include "io.h"
#include "idt.h"
#include "kernel.h"
#include "lib.h"

static volatile uint32_t pit_ticks = 0;
static uint32_t          pit_hz    = PIT_DEFAULT_HZ;

/* IRQ0 处理：只做最少的事 */
static void pit_irq(regs_t* r) {
    (void)r;
    pit_ticks++;
}

void pit_init(uint32_t hz) {
    if (hz == 0) hz = PIT_DEFAULT_HZ;

    uint32_t divisor = PIT_BASE_FREQ / hz;
    if (divisor == 0)     divisor = 1;
    if (divisor > 0xFFFF) divisor = 0xFFFF;

    pit_hz    = PIT_BASE_FREQ / divisor;   /* 实际频率 */
    pit_ticks = 0;

    /* 命令字 0x36 = 通道0 | 先低后高 | 模式3(方波) | 二进制 */
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));

    irq_install_handler(0, pit_irq);
    irq_unmask(0);
}

/* 忙等 ticks 个节拍（同时允许中断） */
void pit_wait_ticks(uint32_t ticks) {
    uint32_t prev    = pit_ticks;
    uint32_t elapsed = 0;
    while (elapsed < ticks) {
        __asm__ __volatile__("hlt");
        uint32_t now = pit_ticks;
        elapsed += now - prev;    /* 无符号减法自动处理回绕 */
        prev = now;
    }
}

void pit_sleep_ms(uint32_t ms) {
    if (pit_hz == 0) return;
    uint32_t ticks = (ms * pit_hz + 999) / 1000;
    pit_wait_ticks(ticks);
}

uint32_t pit_uptime_ticks(void) {
    return pit_ticks;
}

/* ★ 精确计算，无 64 位除法、无溢出（pit_hz <= 10000 时安全） */
uint32_t pit_uptime_ms(void) {
    if (pit_hz == 0) return 0;
    uint32_t secs = pit_ticks / pit_hz;
    uint32_t rem  = pit_ticks % pit_hz;
    return secs * 1000 + (rem * 1000) / pit_hz;
}

void pit_print_uptime(void) {
    uint32_t ticks    = pit_ticks;
    uint32_t total_ms = pit_uptime_ms();
    uint32_t s  = total_ms / 1000;
    uint32_t ms = total_ms % 1000;
    uint32_t m  = s / 60;
    s = s % 60;
    kprintf("Uptime: %um %u.%03us (%u ticks)\n", m, s, ms, ticks);
}