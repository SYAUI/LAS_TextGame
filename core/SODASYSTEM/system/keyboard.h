/* keyboard.h */
#ifndef SODA_KEYBOARD_H
#define SODA_KEYBOARD_H

#include "types.h"

/* keyboard_getchar 返回值：0~0xFF 普通字符，-1 无输入，以下为特殊键 */
#define KEY_UP       0x100
#define KEY_DOWN     0x101
#define KEY_LEFT     0x102
#define KEY_RIGHT    0x103
#define KEY_HOME     0x104
#define KEY_END      0x105
#define KEY_DELETE   0x106
#define KEY_PGUP     0x107
#define KEY_PGDN     0x108
#define KEY_ESC      0x109

void keyboard_init(void);
int  keyboard_getchar(void);      /* -1 = 无输入 */

#endif
