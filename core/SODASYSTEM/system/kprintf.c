/* kprintf.c - 格式化输出 */
#include "lib.h"
#include "kernel.h"

/* 不依赖 <stdarg.h>，使用 GCC 内建变参支持 */
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)

/* ---- 内部辅助 ---- */

/* 无符号数转字符串，返回起始指针（缓冲区以 '\0' 结尾）*/
static char* utoa_buf(char* end, unsigned int num, unsigned int base, int upper) {
    static const char digits_lo[] = "0123456789abcdef";
    static const char digits_up[] = "0123456789ABCDEF";
    const char* digits = upper ? digits_up : digits_lo;
    char* p = end;
    *p = '\0';
    if (num == 0) {
        *--p = '0';
        return p;
    }
    while (num) {
        *--p = digits[num % base];
        num /= base;
    }
    return p;
}

static int emit_pad(char c, int n) {
    int count = 0;
    while (n-- > 0) { terminal_putchar(c); count++; }
    return count;
}

static int emit_str(const char* s, int n) {
    int count = 0;
    while (n-- > 0) { terminal_putchar(*s++); count++; }
    return count;
}

/* ---- kprintf 主函数 ---- */
int kprintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int count = 0;

    for (const char* p = fmt; *p; p++) {
        if (*p != '%') {
            terminal_putchar(*p);
            count++;
            continue;
        }

        p++;

        /* 解析标志 */
        int left_align = 0, zero_pad = 0, plus = 0, space = 0;
        for (;; p++) {
            if (*p == '-')      left_align = 1;
            else if (*p == '0') zero_pad = 1;
            else if (*p == '+') plus = 1;
            else if (*p == ' ') space = 1;
            else break;
        }

        /* 解析宽度 */
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        /* 解析精度 */
        int precision = -1;
        if (*p == '.') {
            p++;
            precision = 0;
            while (*p >= '0' && *p <= '9') {
                precision = precision * 10 + (*p - '0');
                p++;
            }
        }

        /* 忽略长度修饰符 */
        while (*p == 'l' || *p == 'h' || *p == 'z') p++;

        char type = *p;
        char buf[32];
        const char* s = buf;
        int  len         = 0;
        int  zero_extend = 0;
        char sign        = 0;
        int  is_pointer  = 0;

        switch (type) {
            case 'd':
            case 'i': {
                int v = va_arg(ap, int);
                unsigned int uv;
                if (v < 0) {
                    sign = '-';
                    uv = (unsigned int)(-(v + 1)) + 1;   /* 安全处理 INT_MIN */
                } else {
                    if (plus)       sign = '+';
                    else if (space) sign = ' ';
                    uv = (unsigned int)v;
                }
                s = utoa_buf(buf + sizeof(buf) - 1, uv, 10, 0);
                len = (int)((buf + sizeof(buf) - 1) - s);
                if (precision > len) zero_extend = precision - len;
                break;
            }
            case 'u': {
                unsigned int v = va_arg(ap, unsigned int);
                s = utoa_buf(buf + sizeof(buf) - 1, v, 10, 0);
                len = (int)((buf + sizeof(buf) - 1) - s);
                if (precision > len) zero_extend = precision - len;
                break;
            }
            case 'x':
            case 'X': {
                unsigned int v = va_arg(ap, unsigned int);
                s = utoa_buf(buf + sizeof(buf) - 1, v, 16, type == 'X');
                len = (int)((buf + sizeof(buf) - 1) - s);
                if (precision > len) zero_extend = precision - len;
                break;
            }
            case 'p': {
                unsigned int v = (unsigned int)(uintptr_t)va_arg(ap, void*);
                s = utoa_buf(buf + sizeof(buf) - 1, v, 16, 0);
                len = (int)((buf + sizeof(buf) - 1) - s);
                if (len < 8) zero_extend = 8 - len;
                is_pointer = 1;
                break;
            }
            case 'c': {
                buf[0] = (char)va_arg(ap, int);
                s = buf;
                len = 1;
                break;
            }
            case 's': {
                s = va_arg(ap, const char*);
                if (!s) s = "(null)";
                if (precision >= 0) {
                    int n = 0;
                    while (n < precision && s[n]) n++;
                    len = n;
                } else {
                    len = (int)strlen(s);
                }
                break;
            }
            case '%': {
                terminal_putchar('%');
                count++;
                continue;
            }
            default: {
                terminal_putchar('%');
                terminal_putchar(type);
                count += 2;
                continue;
            }
        }

        int total = len + zero_extend;
        if (sign)       total++;
        if (is_pointer) total += 2;               /* "0x" */
        int pad = width > total ? width - total : 0;

        if (!left_align && !zero_pad) {
            count += emit_pad(' ', pad);
        }
        if (sign) {
            terminal_putchar(sign);
            count++;
        }
        if (is_pointer) {
            terminal_putchar('0');
            terminal_putchar('x');
            count += 2;
        }
        if (!left_align && zero_pad) {
            count += emit_pad('0', pad);
        }
        count += emit_pad('0', zero_extend);
        count += emit_str(s, len);
        if (left_align) {
            count += emit_pad(' ', pad);
        }
    }

    va_end(ap);
    return count;
}