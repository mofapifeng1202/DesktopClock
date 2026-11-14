/*
 * Display pipeline performance counters. See ui_perf.h.
 *
 * Everything runs inside the LVGL task (display events), so the counters are
 * plain statics: no locking, and the numbers are read from another task only
 * for a rough "what is the frame time" figure.
 */
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "ui_perf.h"

static volatile uint32_t s_flush_us;     /* accumulated flush time (window) */
static volatile uint32_t s_flush_us_total;
static volatile uint32_t s_flush_cnt_total;
static volatile uint32_t s_flush_cnt;    /* flushed areas */
static volatile uint32_t s_px;           /* pixels pushed */
static volatile uint32_t s_frames;       /* completed screen updates */
static volatile uint32_t s_frame_us;     /* accumulated frame time */
static volatile int64_t s_flush_t0;
static volatile int64_t s_frame_t0;

/* snapshot of the previous window for rate calculation */
static uint32_t s_prev_frames, s_prev_px;
static int64_t s_prev_us;
static ui_perf_snapshot_t s_snap;

static void on_flush_start(lv_event_t *e)
{
    (void)e;
    s_flush_t0 = esp_timer_get_time();
}

static void on_flush_finish(lv_event_t *e)
{
    const lv_area_t *a = (const lv_area_t *)lv_event_get_param(e);
    int64_t now = esp_timer_get_time();
    if (s_flush_t0 != 0) {
        uint32_t d = (uint32_t)(now - s_flush_t0);
        s_flush_us += d;
        s_flush_us_total += d;
    }
    if (a != NULL) {
        s_px += (uint32_t)(lv_area_get_width(a) * lv_area_get_height(a));
    }
    s_flush_cnt++;
    s_flush_cnt_total++;
}

static void on_render_start(lv_event_t *e)
{
    (void)e;
    s_frame_t0 = esp_timer_get_time();
}

static void on_render_ready(lv_event_t *e)
{
    (void)e;
    if (s_frame_t0 != 0) {
        s_frame_us += (uint32_t)(esp_timer_get_time() - s_frame_t0);
        s_frames++;
    }
}

static void perf_timer_cb(lv_timer_t *t)
{
    (void)t;
    int64_t now = esp_timer_get_time();
    int64_t dt = now - s_prev_us;
    if (dt <= 0) {
        return;
    }
    uint32_t frames = s_frames - s_prev_frames;
    uint32_t px = s_px - s_prev_px;
    s_snap.frames = frames;
    s_snap.frame_us = frames ? (s_frame_us / frames) : 0;
    s_snap.flush_us = s_flush_cnt ? (s_flush_us / s_flush_cnt) : 0;
    s_snap.areas = frames ? (s_flush_cnt / frames) : 0;
    s_snap.px_per_s = (uint32_t)((uint64_t)px * 1000000ULL / (uint64_t)dt);
    s_prev_frames = s_frames;
    s_prev_px = s_px;
    s_prev_us = now;
    /* window counters */
    s_flush_us = 0;
    s_flush_cnt = 0;
    s_frame_us = 0;
}

void ui_perf_attach(lv_display_t *disp)
{
    lv_display_add_event_cb(disp, on_render_start, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(disp, on_render_ready, LV_EVENT_RENDER_READY, NULL);
    lv_display_add_event_cb(disp, on_flush_start, LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(disp, on_flush_finish, LV_EVENT_FLUSH_FINISH, NULL);
    lv_timer_create(perf_timer_cb, 1000, NULL);
    s_prev_us = esp_timer_get_time();
}

void ui_perf_get(ui_perf_snapshot_t *out)
{
    if (out != NULL) {
        *out = s_snap;
    }
}

void ui_perf_raw(uint32_t *areas, uint32_t *flush_us, uint32_t *px)
{
    if (areas) *areas = s_flush_cnt_total;
    if (flush_us) *flush_us = s_flush_us_total;
    if (px) *px = s_px;
}

uint32_t ui_perf_total_px(void)
{
    return s_px;
}

void ui_perf_log(const char *tag)
{
    ESP_LOGI(tag, "ui: %u fps-ish, frame %u us, flush %u us x%u, %u kpx/s",
             (unsigned)s_snap.frames,
             (unsigned)s_snap.frame_us,
             (unsigned)s_snap.flush_us,
             (unsigned)s_snap.areas,
             (unsigned)(s_snap.px_per_s / 1000));
}
