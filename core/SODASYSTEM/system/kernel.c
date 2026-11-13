// kernel.c - 32位C内核（IDT + 键盘 + PIT + VGA 辅助 API）
#include "types.h"
#include "kernel.h"
#include "lib.h"
#include "idt.h"
#include "keyboard.h"
#include "pit.h"
#include "io.h"
#include "shell.h"
#include "ata.h"
#include "fat12.h"

#define VGA_BUFFER ((volatile uint16_t*)0xB8000)
#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define TAB_SIZE   4

static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;
static volatile uint16_t* terminal_buffer;

static void terminal_update_cursor(void) {
    uint16_t pos = (uint16_t)(terminal_row * VGA_WIDTH + terminal_column);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void terminal_scroll(void) {
    for (size_t y = 1; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            terminal_buffer[(y - 1) * VGA_WIDTH + x] =
                terminal_buffer[y * VGA_WIDTH + x];
    for (size_t x = 0; x < VGA_WIDTH; x++)
        terminal_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] =
            (uint16_t)terminal_color << 8 | ' ';
}

static void terminal_newline(void) {
    terminal_column = 0;
    if (++terminal_row >= VGA_HEIGHT) {
        terminal_scroll();
        terminal_row = VGA_HEIGHT - 1;
    }
}

void terminal_initialize(void) {
    terminal_row = 0;
    terminal_column = 0;
    terminal_color = 0x0F;
    terminal_buffer = VGA_BUFFER;
    for (size_t y = 0; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            terminal_buffer[y * VGA_WIDTH + x] =
                (uint16_t)terminal_color << 8 | ' ';
    terminal_update_cursor();
}

void terminal_putchar(char c) {
    switch (c) {
        case '\n':
            terminal_newline();
            terminal_update_cursor();
            return;
        case '\r':
            terminal_column = 0;
            terminal_update_cursor();
            return;
        case '\t': {
            size_t next = (terminal_column + TAB_SIZE) & ~(size_t)(TAB_SIZE - 1);
            while (terminal_column < next) terminal_putchar(' ');
            return;
        }
        case '\b':
            if (terminal_column > 0) {
                terminal_column--;
                terminal_buffer[terminal_row * VGA_WIDTH + terminal_column] =
                    (uint16_t)terminal_color << 8 | ' ';
                terminal_update_cursor();
            }
            return;
    }
    terminal_buffer[terminal_row * VGA_WIDTH + terminal_column] =
        (uint16_t)terminal_color << 8 | (uint8_t)c;
    if (++terminal_column >= VGA_WIDTH) terminal_newline();
    terminal_update_cursor();
}

void print(const char* str) {
    for (size_t i = 0; str[i] != '\0'; i++) terminal_putchar(str[i]);
}

/* ---- VGA 辅助 API（供 shell 使用） ---- */

void terminal_clear(void) {
    for (size_t y = 0; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            terminal_buffer[y * VGA_WIDTH + x] =
                (uint16_t)terminal_color << 8 | ' ';
    terminal_row = 0;
    terminal_column = 0;
    terminal_update_cursor();
}

void terminal_clear_row(size_t row) {
    if (row >= VGA_HEIGHT) return;
    for (size_t x = 0; x < VGA_WIDTH; x++)
        terminal_buffer[row * VGA_WIDTH + x] =
            (uint16_t)terminal_color << 8 | ' ';
}

/* 在 (row, col) 写字符串，不改变内部光标/行列状态 */
void terminal_write_at(size_t row, size_t col, const char* s) {
    if (row >= VGA_HEIGHT) return;
    size_t x = col;
    while (*s && x < VGA_WIDTH) {
        terminal_buffer[row * VGA_WIDTH + x] =
            (uint16_t)terminal_color << 8 | (uint8_t)*s;
        s++;
        x++;
    }
}

/* 只移动硬件光标，不改内部 terminal_row/col */
void terminal_set_cursor(size_t row, size_t col) {
    if (row >= VGA_HEIGHT || col >= VGA_WIDTH) return;
    uint16_t pos = (uint16_t)(row * VGA_WIDTH + col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

size_t terminal_get_row(void) { return terminal_row; }
size_t terminal_get_col(void) { return terminal_column; }

/* ---------------- 内核入口 ---------------- */
void kmain(void) {
    terminal_initialize();

    idt_init();
    pic_remap();
    pit_init(PIT_DEFAULT_HZ);
    keyboard_init();

    __asm__ __volatile__("sti");

    kprintf("SODA v0.6.0 - IDT + PIT + keyboard + shell + ATA\n");
    kprintf("IDT=256  PIT=%uHz  Keyboard=IRQ1\n", PIT_DEFAULT_HZ);

    int ata_r = ata_init();
    if (ata_r == ATA_OK) {
        kprintf("ATA: %s (%u sectors)\n", ata_get_model(), ata_get_sector_count());
    } else if (ata_r == ATA_ERR_NO_DISK) {
        kprintf("ATA: no disk detected.\n");
    } else {
        kprintf("ATA: initialization failed (code %d).\n", ata_r);
    }
    
    if (ata_is_present()) {
        int fr = fat12_mount();
        if (fr == FAT12_OK)
            kprintf("FAT12 mounted: %u clusters, data at LBA %u\n",
                    (unsigned int)fat12_get_layout()->cluster_count,
                    (unsigned int)fat12_get_layout()->data_start);
        else
            kprintf("FAT12 mount failed (code %d). Disk may not be FAT12.\n", fr);
    } else {
        kprintf("FAT12 not mounted (no ATA disk).\n");
    } 

    kprintf("\n");

    shell_run();
    for (;;) __asm__ __volatile__("hlt");
}
