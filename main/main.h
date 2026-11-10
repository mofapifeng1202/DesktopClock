#pragma once
#include <stdint.h>

/* Board services implemented in main.c, used by clock_ui.c */
float battery_volts(void);      /* volts, or -1 if ADC unavailable */
int battery_percent(void);      /* 0..100 from Li-ion curve, or -1 */
void audio_set_vol(int vol);    /* speaker volume 0..100 */
int audio_get_vol(void);
void rgb_set(uint8_t r, uint8_t g, uint8_t b);
