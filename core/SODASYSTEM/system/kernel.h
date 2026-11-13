/* kernel.h */
#ifndef SODA_KERNEL_H
#define SODA_KERNEL_H

#include "types.h"

void terminal_initialize(void);
void terminal_putchar(char c);
void print(const char* str);

void terminal_clear(void);
void terminal_clear_row(size_t row);
void terminal_write_at(size_t row, size_t col, const char* s);
void terminal_set_cursor(size_t row, size_t col);
size_t terminal_get_row(void);
size_t terminal_get_col(void);

void kmain(void);

#endif
