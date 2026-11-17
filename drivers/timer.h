// timer.h -- Defines the interface for all PIT-related functions.
//            Written for JamesM's kernel development tutorials.

#ifndef TIMER_H
#define TIMER_H

#include "../lib/common.h"
#include <stdint.h>
extern u32int tick;

void init_timer(u32int frequency);

void sleep(uint32_t timer_ticks);

/*
 * timer_ticks_ms -- монотонные миллисекунды с момента init_timer().
 *
 * PIT программируется на 1000 Гц (см. init_timer(1000) в kernel/main.c),
 * поэтому tick == миллисекунды.  Используется сисколлом TICKS_MS (196),
 * которым пользуются игры (DOOM) для игрового цикла.
 */
uint64_t timer_ticks_ms(void);

#endif
