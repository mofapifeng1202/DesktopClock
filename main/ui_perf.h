#pragma once

#include <stdint.h>

#include "lvgl.h"

/*
 * Small performance counters for the display pipeline.
 *
 * LVGL reports every flushed area through the display events, so we can tell
 * where a frame's time goes:
 *
 *   frame  = from the first flush of a screen update to the last one
 *   flush  = LV_EVENT_FLUSH_START .. LV_EVENT_FLUSH_FINISH, i.e. software
 *            rotation + byte swap + handing the buffer to the SPI DMA
 *   frame - flush ~= pure LVGL rendering
 *
 * Exposed over /api/status and the 10 s "UI alive" log so performance
 * questions can be answered with numbers instead of guesses.
 */

typedef struct {
    uint32_t frames;      /* completed screen updates in the window */
    uint32_t frame_us;    /* average frame time (us) */
    uint32_t flush_us;    /* average flush time per area (us) */
    uint32_t areas;       /* flushed areas per completed frame */
    uint32_t px_per_s;    /* pixels pushed to the panel per second */
} ui_perf_snapshot_t;

void ui_perf_attach(lv_display_t *disp);   /* call once, after lvgl_port_add_disp */

/* Averages over the last window (~1 s), zeroed when nothing was drawn. */
void ui_perf_get(ui_perf_snapshot_t *out);

/* Total pixels pushed since boot; used by the full-repaint benchmark. */
uint32_t ui_perf_total_px(void);

/* Raw monotonic counters (areas flushed, total flush time, pixels). */
void ui_perf_raw(uint32_t *areas, uint32_t *flush_us, uint32_t *px);

/* One line summary for logs. */
void ui_perf_log(const char *tag);
