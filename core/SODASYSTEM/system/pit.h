/* pit.h - 可编程间隔定时器 */
#ifndef SODA_PIT_H
#define SODA_PIT_H

#include "types.h"

#define PIT_BASE_FREQ   1193182u    /* 8253 输入时钟频率 */
#define PIT_DEFAULT_HZ  100u        /* 默认 100 Hz = 10ms 一次 */

void     pit_init(uint32_t hz);
void     pit_wait_ticks(uint32_t ticks);
void     pit_sleep_ms(uint32_t ms);
uint32_t pit_uptime_ticks(void);
uint32_t pit_uptime_ms(void);
void     pit_print_uptime(void);

#endif