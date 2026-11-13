/* types.h - SODA 内核基础类型定义 */
#ifndef SODA_TYPES_H
#define SODA_TYPES_H

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;

typedef unsigned int       uintptr_t;
typedef int                intptr_t;

typedef unsigned int       size_t;
typedef int                ssize_t;

#if !defined(__cplusplus) && \
    (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L)
typedef _Bool              bool;
#endif

#ifndef true
#define true  1
#endif
#ifndef false
#define false 0
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

#endif