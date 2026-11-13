/* lib.h - 基础字符串/内存/格式化函数 */
#ifndef SODA_LIB_H
#define SODA_LIB_H

#include "types.h"

/* 字符串 */
size_t strlen(const char* s);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);

/* 内存 */
void*  memset(void* dst, int c, size_t n);
void*  memcpy(void* dst, const void* src, size_t n);
void*  memmove(void* dst, const void* src, size_t n);

int memcmp(const void* a, const void* b, size_t n);
/* 格式化输出（变参） */
int kprintf(const char* fmt, ...);

#endif
