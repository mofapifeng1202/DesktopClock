#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Board services implemented in main.c, used by clock_ui.c / radio.c */
float battery_volts(void);      /* volts, or -1 if ADC unavailable */
int battery_percent(void);      /* 0..100 from Li-ion curve, or -1 */
void audio_set_vol(int vol);    /* speaker volume 0..100 */
int audio_get_vol(void);
void rgb_set(uint8_t r, uint8_t g, uint8_t b);

/* Streaming audio output for the radio (ES8311 + I2S).
 * audio_stream_open() reconfigures the codec when the MP3 sample rate
 * changes; audio_stream_write() accepts interleaved 16-bit stereo PCM. */
bool audio_is_ready(void);
esp_err_t audio_stream_open(int sample_rate, int channels);
int audio_stream_write(const void *data, size_t len);
void audio_stream_silence(int ms);
int audio_stream_rate(void);
