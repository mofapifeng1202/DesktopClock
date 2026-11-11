#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SPECTRUM_BANDS 16

/*
 * Small 256-point FFT spectrum analyzer for the radio page.
 *
 * The radio play task feeds every decoded MP3 frame; 256 windowed samples of
 * the left channel are transformed and mapped onto SPECTRUM_BANDS log-spaced
 * bands (0..100). Values are single bytes written by the play task and read by
 * the LVGL task, so no lock is needed (byte stores/loads are atomic).
 */

/* (Re)configure the band mapping for a sample rate and zero all bands.
 * rate <= 0 disables analysis until the next reset. ctx tags the source
 * (radio preset vs IPTV) so each UI page only sees its own spectrum. */
void spectrum_reset(int sample_rate, int ctx);

/* True when reset() was last called with a usable rate. */
bool spectrum_ready(void);

/* Feed one decoded MP3 frame: interleaved stereo 16-bit PCM. */
void spectrum_feed_frame(const int16_t *pcm, int total_samples);

/* Decay all bands toward zero; call when playback stalls or stops. */
void spectrum_idle(void);

/* Copy the current band values (0..100) into out[SPECTRUM_BANDS]. When the
 * playing source belongs to a different page (ctx mismatch), zeros are
 * returned so each page's spectrum only animates for its own audio. */
void spectrum_copy(int ctx, uint8_t *out);
