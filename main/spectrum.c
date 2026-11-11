/*
 * 256-point radix-2 FFT spectrum analyzer. See spectrum.h.
 *
 * CPU cost: one FFT per decoded frame (~26 ms at 44.1/48 kHz) is roughly
 * 0.3 ms at 240 MHz, i.e. ~1% of one core. Tables (~7 KB) live in PSRAM so
 * the internal heap is untouched.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "spectrum.h"

#define FFT_N     256
#define FFT_HALF  (FFT_N / 2)
#define FFT_BITS  8

/* Set to 1 for a 1 s band dump on the serial log (bring-up only). */
#define SPECTRUM_DEBUG 0

#define BAND_F_MIN     150.0f   /* lowest analyzed frequency, Hz */
#define BAND_F_MAX     14000.0f /* highest analyzed frequency, Hz */
#define BAND_SCALE     1.8f     /* raw magnitude -> percent */
#define BAND_HF_TILT   0.04f    /* per-band gain boost for high frequencies */
#define BAND_FALL_PER_S 110.0f  /* decay speed, percent per second (time-based) */

static const char *TAG = "spectrum";

static float *s_win;      /* FFT_N */
static float *s_re;       /* FFT_N */
static float *s_im;       /* FFT_N */
static float *s_cos_t;    /* FFT_HALF */
static float *s_sin_t;    /* FFT_HALF */
static int *s_rev;        /* FFT_N bit-reversal */
static bool s_tables_ready;

static int s_rate;        /* 0 = disabled */
static uint8_t s_band_lo[SPECTRUM_BANDS];
static uint8_t s_band_hi[SPECTRUM_BANDS];
static float s_band_gain[SPECTRUM_BANDS];
static volatile uint8_t s_bands[SPECTRUM_BANDS];
static int64_t s_last_feed_us;

static void *spec_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

static bool ensure_tables(void)
{
    if (s_tables_ready) {
        return true;
    }
    s_win   = spec_alloc(sizeof(float) * FFT_N);
    s_re    = spec_alloc(sizeof(float) * FFT_N);
    s_im    = spec_alloc(sizeof(float) * FFT_N);
    s_cos_t = spec_alloc(sizeof(float) * FFT_HALF);
    s_sin_t = spec_alloc(sizeof(float) * FFT_HALF);
    s_rev   = spec_alloc(sizeof(int) * FFT_N);
    if (!s_win || !s_re || !s_im || !s_cos_t || !s_sin_t || !s_rev) {
        ESP_LOGE(TAG, "no memory for FFT tables");
        free(s_win);  free(s_re);  free(s_im);
        free(s_cos_t); free(s_sin_t); free(s_rev);
        s_win = NULL; s_re = NULL; s_im = NULL;
        s_cos_t = NULL; s_sin_t = NULL; s_rev = NULL;
        return false;
    }

    for (int i = 0; i < FFT_N; i++) {
        s_win[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / FFT_N));
        int r = 0;
        for (int b = 0; b < FFT_BITS; b++) {
            if (i & (1 << b)) {
                r |= 1 << (FFT_BITS - 1 - b);
            }
        }
        s_rev[i] = r;
    }
    for (int k = 0; k < FFT_HALF; k++) {
        s_cos_t[k] = cosf(2.0f * (float)M_PI * k / FFT_N);
        s_sin_t[k] = sinf(2.0f * (float)M_PI * k / FFT_N);
    }
    s_tables_ready = true;
    return true;
}

static void fft_run(void)
{
    for (int i = 0; i < FFT_N; i++) {
        int j = s_rev[i];
        if (j > i) {
            float tr = s_re[i]; s_re[i] = s_re[j]; s_re[j] = tr;
            float ti = s_im[i]; s_im[i] = s_im[j]; s_im[j] = ti;
        }
    }
    for (int m = 1; m < FFT_N; m <<= 1) {
        int wstep = FFT_N / (2 * m);
        for (int base = 0; base < FFT_N; base += m << 1) {
            for (int j = 0; j < m; j++) {
                int w = j * wstep;
                float wr = s_cos_t[w];
                float wi = -s_sin_t[w];
                int i0 = base + j;
                int i1 = i0 + m;
                float xr = s_re[i1], xi = s_im[i1];
                float tr = xr * wr - xi * wi;
                float ti = xr * wi + xi * wr;
                s_re[i1] = s_re[i0] - tr;
                s_im[i1] = s_im[i0] - ti;
                s_re[i0] += tr;
                s_im[i0] += ti;
            }
        }
    }
}

void spectrum_reset(int sample_rate)
{
    memset((void *)s_bands, 0, SPECTRUM_BANDS);
    s_last_feed_us = esp_timer_get_time();
    if (sample_rate <= 0) {
        s_rate = 0;
        return;
    }
    if (!ensure_tables()) {
        s_rate = 0;
        return;
    }

    float binw = (float)sample_rate / FFT_N;
    float f_hi = (float)sample_rate * 0.475f;
    if (f_hi > BAND_F_MAX) f_hi = BAND_F_MAX;
    if (f_hi <= BAND_F_MIN) f_hi = BAND_F_MIN * 2.0f;
    float ratio = powf(f_hi / BAND_F_MIN, 1.0f / SPECTRUM_BANDS);

    for (int b = 0; b < SPECTRUM_BANDS; b++) {
        int lo = (int)floorf(BAND_F_MIN * powf(ratio, (float)b) / binw);
        int hi = (int)ceilf(BAND_F_MIN * powf(ratio, (float)(b + 1)) / binw);
        if (lo < 1) lo = 1;                       /* skip DC */
        if (hi < lo) hi = lo;
        if (lo > FFT_HALF - 1) lo = FFT_HALF - 1;
        if (hi > FFT_HALF - 1) hi = FFT_HALF - 1;
        s_band_lo[b] = (uint8_t)lo;
        s_band_hi[b] = (uint8_t)hi;
        s_band_gain[b] = 1.0f + BAND_HF_TILT * b;
    }
    s_rate = sample_rate;
    ESP_LOGI(TAG, "spectrum ready: %d Hz, bin %.1f Hz, bands %u..%u",
             sample_rate, binw, s_band_lo[0], s_band_hi[SPECTRUM_BANDS - 1]);
}

void spectrum_feed_frame(const int16_t *pcm, int total_samples)
{
    if (s_rate == 0 || pcm == NULL || total_samples < 2) {
        return;
    }
    int ch = total_samples / 2;               /* always stereo here */
    int stride = ch / FFT_N;
    if (stride < 1) stride = 1;
    for (int i = 0; i < FFT_N; i++) {
        int idx = i * stride;
        if (idx >= ch) idx = ch - 1;
        s_re[i] = (pcm[2 * idx] / 32768.0f) * s_win[i];
        s_im[i] = 0.0f;
    }
    fft_run();

    /* Time-based decay: the frame rate differs between sample rates
     * (19 Hz at 22.05 kHz vs 42 Hz at 48 kHz), so the fall speed is
     * expressed in percent/second instead of percent/frame. */
    int64_t now = esp_timer_get_time();
    float dt_s = (now - s_last_feed_us) / 1e6f;
    s_last_feed_us = now;
    if (dt_s < 0.005f) dt_s = 0.005f;
    if (dt_s > 0.2f) dt_s = 0.2f;
    int fall = (int)(BAND_FALL_PER_S * dt_s + 0.5f);
    if (fall < 1) fall = 1;

    for (int b = 0; b < SPECTRUM_BANDS; b++) {
        float m2max = 0.0f;
        for (int k = s_band_lo[b]; k <= s_band_hi[b]; k++) {
            float m2 = s_re[k] * s_re[k] + s_im[k] * s_im[k];
            if (m2 > m2max) m2max = m2;
        }
        int v = (int)(sqrtf(m2max) * s_band_gain[b] * BAND_SCALE);
        if (v > 100) v = 100;
        int prev = s_bands[b];
        if (v < prev) {                       /* attack instant, fall linear */
            v = prev - fall;
            if (v < 0) v = 0;
        }
        s_bands[b] = (uint8_t)v;
    }

#if SPECTRUM_DEBUG
    {
        static int64_t last_log;
        int64_t now = esp_timer_get_time();
        if (now - last_log >= 1000000) {
            last_log = now;
            char buf[128];
            int off = 0;
            for (int b = 0; b < SPECTRUM_BANDS; b++) {
                off += snprintf(buf + off, sizeof(buf) - off, "%3u ",
                                (unsigned)s_bands[b]);
            }
            ESP_LOGI(TAG, "%s", buf);
        }
    }
#endif
}

void spectrum_idle(void)
{
    for (int b = 0; b < SPECTRUM_BANDS; b++) {
        int v = s_bands[b];
        if (v > 0) {
            s_bands[b] = (uint8_t)(v > 8 ? v - 8 : 0);
        }
    }
}

void spectrum_copy(uint8_t *out)
{
    if (out == NULL) {
        return;
    }
    memcpy(out, (const uint8_t *)s_bands, SPECTRUM_BANDS);
}
