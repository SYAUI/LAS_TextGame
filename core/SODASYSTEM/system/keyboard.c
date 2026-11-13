/* keyboard.c - 键盘驱动（含扩展键） */
#include "keyboard.h"
#include "io.h"
#include "idt.h"

#define KBD_BUF_SIZE 128

static volatile int kbd_buf[KBD_BUF_SIZE];
static volatile int kbd_head = 0;
static volatile int kbd_tail = 0;

static const char kbd_lower[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t','q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   '\\','z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0,   '*', 0,   ' ',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0
};

static const char kbd_shift[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t','Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0,   '*', 0,   ' ',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0
};

static int shift_down = 0;
static int caps_lock  = 0;

static void kbd_push(int c) {
    int next = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next != kbd_tail) {
        kbd_buf[kbd_head] = c;
        kbd_head = next;
    }
}

static void keyboard_irq(regs_t* r) {
    (void)r;
    static int extended = 0;
    uint8_t sc = inb(0x60);

    /* 0xE0 前缀：下一个扫描码是扩展键 */
    if (sc == 0xE0) { extended = 1; return; }

    if (extended) {
        extended = 0;
        if (sc & 0x80) return;      /* 扩展键释放：忽略 */
        switch (sc) {
            case 0x48: kbd_push(KEY_UP);     return;
            case 0x50: kbd_push(KEY_DOWN);   return;
            case 0x4B: kbd_push(KEY_LEFT);   return;
            case 0x4D: kbd_push(KEY_RIGHT);  return;
            case 0x47: kbd_push(KEY_HOME);   return;
            case 0x4F: kbd_push(KEY_END);    return;
            case 0x53: kbd_push(KEY_DELETE); return;
            case 0x49: kbd_push(KEY_PGUP);   return;
            case 0x51: kbd_push(KEY_PGDN);   return;
            case 0x01: kbd_push(KEY_ESC);    return;
        }
        return;
    }

    if (sc == 0x2A || sc == 0x36) { shift_down = 1; return; }
    if (sc == 0xAA || sc == 0xB6) { shift_down = 0; return; }
    if (sc == 0x3A) { caps_lock ^= 1; return; }
    if (sc & 0x80) return;

    char c = shift_down ? kbd_shift[sc] : kbd_lower[sc];
    if (caps_lock && c >= 'a' && c <= 'z') c -= 32;
    else if (caps_lock && shift_down && c >= 'A' && c <= 'Z') c += 32;

    if (c) kbd_push((int)(unsigned char)c);
}

void keyboard_init(void) {
    kbd_head = kbd_tail = 0;
    shift_down = caps_lock = 0;
    irq_install_handler(1, keyboard_irq);
    irq_unmask(1);
    while (inb(0x64) & 0x01) inb(0x60);
}

int keyboard_getchar(void) {
    if (kbd_head == kbd_tail) return -1;
    int c = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return c;
}
