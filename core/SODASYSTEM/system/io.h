/* io.h - 端口 I/O */
#ifndef SODA_IO_H
#define SODA_IO_H

#include "types.h"

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ __volatile__("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ __volatile__("outw %0, %1" : : "a"(val), "Nd"(port));
}

/* 恢复：PIC 初始化用的 I/O 延迟（标准写法：向未使用端口写 0） */
static inline void io_wait(void) {
    __asm__ __volatile__("outb %%al, $0x80" : : "a"(0));
}

/* 保留：ATA 用的 400ns 延迟（读 status 4 次） */
static inline void io_wait_400ns(void) {
    inb(0x3F6);
    inb(0x3F6);
    inb(0x3F6);
    inb(0x3F6);
}

#endif
