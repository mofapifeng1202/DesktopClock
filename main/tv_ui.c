/*
 * IPTV page for the 480x320 landscape tileview.
 *
 * Layout:
 *   header (title / play state / playlist state / refresh)
 *   big now-playing panel: channel name + spectrum + buffer, tap -> drawer
 *   controls: play / prev / next / volume
 *   left drawer (slides in): grouped channel list (央视 / 卫视 / 其他),
 *   tap a channel to switch; rows are built on open and freed on close
 *
 * The channel list comes from the runtime m3u playlist (media_list). Channel
 * names may contain characters outside the generated font subsets, so every
 * text on this page uses font_radio_14 with the built-in Source Han Sans CJK
 * font as fallback.
 *
 * Video is not decodable on ESP32-S3 (HEVC), so this page is audio-only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "clock_ui.h"
#include "main.h"
#include "media_list.h"
#include "settings.h"
#include "ui_sync.h"
#include "radio.h"
#include "spectrum.h"
#include "tv_ui.h"

LV_FONT_DECLARE(font_radio_14);
LV_FONT_DECLARE(font_radio_20);

#define F14     (&font_radio_14)
#define F20     (&font_radio_20)

#define C_TEXT  0xffffff
#define C_DIM   0x6f86a6
#define C_CYAN  0x00c2ff
#define C_GREEN 0x24d17e
#define C_AMBER 0xffd479
#define C_RED   0xff5c5c
#define C_CARD  0x13294a
#define C_BTN   0x152b4a
#define C_BTN_SEL 0x123f5a
#define C_DRAWER 0x0e2038

#define DRAWER_W 320

static lv_obj_t *s_status;    /* play state */
static lv_obj_t *s_list;      /* playlist load state */
static lv_obj_t *s_now;       /* big channel name */
static lv_obj_t *s_info;
static lv_obj_t *s_buf_lbl;
static lv_obj_t *s_buf_bar;
static lv_obj_t *s_spec;
static uint8_t s_spec_draw[SPECTRUM_BANDS];
static lv_obj_t *s_play_lbl;
static lv_obj_t *s_vol;
static lv_obj_t *s_vol_lbl;

static lv_obj_t *s_drawer;    /* slides from the left */
static lv_obj_t *s_scrim;     /* dark overlay behind the drawer */
static lv_obj_t *s_drawer_list;
static lv_obj_t *s_drawer_title;
static bool s_drawer_open;
static uint32_t s_drawer_opened_ms;   /* lv_tick when the drawer was opened */
static uint32_t s_press_x, s_press_y;
static bool s_press_valid;

/* Ghost taps on this panel are common; ignore anything that arrives right
 * after the drawer opened, otherwise it closes again immediately. */
#define DRAWER_GRACE_MS 700
#define DRAWER_TAP_SLOP 45          /* px a "tap" may wander, ghost jumps exceed it */

/* The channel list is virtualised: a flat item array (group headers plus
 * channels) and a handful of recycled row widgets, so opening the drawer for
 * 86 channels costs ~4 KB of heap instead of ~25 KB. */
#define DRAWER_ROW_H   40
#define DRAWER_SLOTS   8          /* visible rows + overscan */
#define ITEM_HEADER    0x8000     /* item is a header, low bits = group id */

static uint16_t *s_items;
static int s_item_count;
static lv_obj_t *s_sizer;         /* invisible spacer that sets the scroll range */
static lv_obj_t *s_slot_row[DRAWER_SLOTS];
static lv_obj_t *s_slot_lbl[DRAWER_SLOTS];
static int s_slot_chan[DRAWER_SLOTS];

static void drawer_update_rows(void);

static bool s_vol_sync;
static char s_last_status_txt[16] = "已停止";
static int s_last_station = -1;
static media_list_state_t s_last_list_state = (media_list_state_t)-1;
static uint32_t s_last_info_color = 0xffffffff;
static uint32_t s_last_stream_id;

static const char *lstate_text(void);
static void drawer_close(void);
static void drawer_open(void);

static void set_text(lv_obj_t *lbl, const char *txt)
{
    if (lbl == NULL) {
        return;
    }
    if (strcmp(lv_label_get_text(lbl), txt) != 0) {
        lv_label_set_text(lbl, txt);
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                            int x, int y, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 10, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void update_vol_label(int vol)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", vol);
    set_text(s_vol_lbl, buf);
}

static void vol_cb(lv_event_t *e)
{
    if (s_vol_sync) {
        return;
    }
    int v = (int)lv_slider_get_value(lv_event_get_target(e));
    update_vol_label(v);
    ui_sync_volume(v);
}

/* Diagnostics: open/close the channel drawer from outside the LVGL task
 * (the web page uses it to measure the drawer's heap cost). */
void tv_ui_debug_drawer(bool open)
{
    if (open) {
        drawer_open();
    } else {
        drawer_close();
    }
}

void tv_ui_set_volume(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    s_vol_sync = true;
    if (s_vol) {
        lv_slider_set_value(s_vol, vol, LV_ANIM_OFF);
    }
    update_vol_label(vol);
    s_vol_sync = false;
}

static void play_cb(lv_event_t *e)
{
    (void)e;
    radio_pause_toggle(CTX_TV);
}

static void prev_cb(lv_event_t *e)
{
    (void)e;
    radio_prev(CTX_TV);
}

static void next_cb(lv_event_t *e)
{
    (void)e;
    radio_next(CTX_TV);
}

static void reload_cb(lv_event_t *e)
{
    (void)e;
    media_list_start_load(media_list_source());
}

static lv_obj_t *make_ctrl_button(lv_obj_t *parent, int x, int y, int w, int h,
                                  const char *txt, const lv_font_t *font,
                                  lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_BTN), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_BTN_SEL), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), 0);
    lv_label_set_text(lbl, txt);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return lbl;
}

/* Spectrum bars, drawn directly: 16 columns, height = band value (0..100). */
static void spectrum_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int w = a.x2 - a.x1 + 1;
    int h = a.y2 - a.y1 + 1;
    if (w <= 0 || h <= 0) {
        return;
    }

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = 2;

    for (int k = 0; k < SPECTRUM_BANDS; k++) {
        int v = s_spec_draw[k];
        int x1 = a.x1 + k * w / SPECTRUM_BANDS;
        int x2 = a.x1 + (k + 1) * w / SPECTRUM_BANDS - 3;
        if (x2 < x1) x2 = x1;
        if (x2 > a.x2) x2 = a.x2;
        int bh = 2 + (int)((h - 2) * v / 100);
        if (bh > h) bh = h;
        lv_area_t bar = { x1, a.y2 - bh + 1, x2, a.y2 };
        if (v < 50) {
            dsc.bg_color = lv_color_hex(0x24d17e);
        } else if (v < 80) {
            dsc.bg_color = lv_color_hex(0xffd479);
        } else {
            dsc.bg_color = lv_color_hex(0xff5c5c);
        }
        lv_draw_rect(layer, &dsc, &bar);
    }
}

static void spectrum_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (s_spec == NULL) {
        return;
    }
    uint8_t spec[SPECTRUM_BANDS];
    spectrum_copy(CTX_TV, spec);
    if (memcmp(spec, s_spec_draw, sizeof(spec)) != 0) {
        memcpy(s_spec_draw, spec, sizeof(spec));
        lv_obj_invalidate(s_spec);
    }
}

/* ------------------------------ drawer ---------------------------- */

static const char *group_name(int g)
{
    if (g == 0) return "央视";
    if (g == 1) return "地方卫视";
    return "其他";
}

static void channel_row_cb(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (lv_tick_elaps(s_drawer_opened_ms) < DRAWER_GRACE_MS) {
        return;
    }
    if (slot >= 0 && slot < DRAWER_SLOTS) {
        int idx = s_slot_chan[slot];
        if (idx >= 0 && idx < media_list_count()) {
            radio_play_tv(idx);
        }
    }
    drawer_close();
}

static void list_scroll_cb(lv_event_t *e)
{
    (void)e;
    drawer_update_rows();
}

static void drawer_close_anim_ready(lv_anim_t *a)
{
    lv_obj_t *drawer = (lv_obj_t *)a->var;
    /* Re-opened while the close animation was still running: keep it. */
    if (!s_drawer_open) {
        lv_obj_add_flag(drawer, LV_OBJ_FLAG_HIDDEN);
    }
}

static void drawer_slide(lv_obj_t *drawer, int32_t to, bool hide_at_end)
{
    lv_anim_delete(drawer, NULL);      /* a half-finished slide must not win */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, drawer);
    lv_anim_set_values(&a, (int32_t)lv_obj_get_x(drawer), to);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_time(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (hide_at_end) {
        lv_anim_set_ready_cb(&a, drawer_close_anim_ready);
    }
    lv_anim_start(&a);
}

static void drawer_update_rows(void)
{
    if (s_drawer_list == NULL || s_items == NULL) {
        return;
    }
    int32_t scroll_y = lv_obj_get_scroll_y(s_drawer_list);
    if (scroll_y < 0) {
        scroll_y = 0;
    }
    int first = (int)(scroll_y / DRAWER_ROW_H) - 1;   /* one row of overscan */
    if (first < 0) {
        first = 0;
    }

    radio_status_t st;
    radio_get_status(&st);
    bool playing = (st.state == RADIO_STATE_PLAYING ||
                    st.state == RADIO_STATE_BUFFERING ||
                    st.state == RADIO_STATE_PAUSED ||
                    st.state == RADIO_STATE_CONNECTING);

    for (int i = 0; i < DRAWER_SLOTS; i++) {
        int idx = first + i;
        lv_obj_t *row = s_slot_row[i];
        lv_obj_t *lbl = s_slot_lbl[i];
        if (idx >= s_item_count) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            s_slot_chan[i] = -1;
            continue;
        }
        uint16_t item = s_items[idx];
        lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(row, 4, idx * DRAWER_ROW_H);

        if (item & ITEM_HEADER) {
            s_slot_chan[i] = -1;
            lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(C_AMBER), 0);
            lv_label_set_text(lbl, group_name(item & 0x7fff));
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);
            continue;
        }

        int ch = item;
        s_slot_chan[i] = ch;
        bool sel = playing && ch == st.station;
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(sel ? C_BTN_SEL : C_BTN), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), 0);
        lv_label_set_text(lbl, media_list_name(ch));
        lv_obj_center(lbl);
    }
}

static void drawer_build_items(void)
{
    free(s_items);
    s_items = NULL;
    s_item_count = 0;

    int count = media_list_count();
    if (count > 0) {
        /* worst case: every channel starts a new group */
        uint16_t *items = malloc(((size_t)count * 2 + 4) * sizeof(uint16_t));
        if (items != NULL) {
            int n = 0;
            int last_group = -1;
            for (int i = 0; i < count; i++) {
                int g = media_list_group(i);
                if (g != last_group) {
                    items[n++] = (uint16_t)(ITEM_HEADER | (g & 0x7fff));
                    last_group = g;
                }
                items[n++] = (uint16_t)i;
            }
            s_items = items;
            s_item_count = n;
        }
    }
    if (s_sizer != NULL) {
        lv_obj_set_height(s_sizer, s_item_count ? s_item_count * DRAWER_ROW_H : 1);
    }
}

static void drawer_open(void)
{
    if (s_drawer_open) {
        return;
    }
    s_drawer_open = true;
    s_drawer_opened_ms = lv_tick_get();
    lv_anim_delete(s_drawer, NULL);
    lv_obj_remove_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_drawer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_scrim);
    lv_obj_move_foreground(s_drawer);

    int count = media_list_count();
    char buf[48];
    snprintf(buf, sizeof(buf), "频道列表 · %d 个", count);
    set_text(s_drawer_title, buf);

    drawer_build_items();

    /* Scroll to the playing channel, then fill the visible rows. */
    radio_status_t st;
    radio_get_status(&st);
    int32_t sel_item = 0;
    for (int i = 0; i < s_item_count; i++) {
        if (!(s_items[i] & ITEM_HEADER) && (int)s_items[i] == st.station) {
            sel_item = i;
            break;
        }
    }
    int32_t y = sel_item * DRAWER_ROW_H - 2 * DRAWER_ROW_H;
    if (y < 0) {
        y = 0;
    }
    lv_obj_scroll_to(s_drawer_list, 0, y, LV_ANIM_OFF);

    if (s_item_count == 0) {
        /* loading / failed state in the first slot */
        for (int i = 0; i < DRAWER_SLOTS; i++) {
            lv_obj_add_flag(s_slot_row[i], LV_OBJ_FLAG_HIDDEN);
            s_slot_chan[i] = -1;
        }
        lv_obj_remove_flag(s_slot_row[0], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_slot_row[0], 4, 0);
        lv_obj_set_style_bg_opa(s_slot_row[0], LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(s_slot_row[0], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_text_color(s_slot_lbl[0], lv_color_hex(C_DIM), 0);
        lv_label_set_text(s_slot_lbl[0], lstate_text());
        lv_obj_align(s_slot_lbl[0], LV_ALIGN_LEFT_MID, 8, 0);
    } else {
        drawer_update_rows();
    }

    drawer_slide(s_drawer, 0, false);
}

static void drawer_close(void)
{
    if (!s_drawer_open) {
        return;
    }
    s_drawer_open = false;
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    drawer_slide(s_drawer, -DRAWER_W, true);
}

static void scrim_cb(lv_event_t *e)
{
    (void)e;
    if (lv_tick_elaps(s_drawer_opened_ms) < DRAWER_GRACE_MS) {
        return;
    }
    drawer_close();
}

static void open_cb(lv_event_t *e)
{
    (void)e;
    drawer_open();
}

/* A touch with a few pixels of noise (this panel is very noisy) makes LVGL
 * treat the tap as a scroll, so CLICKED never arrives. Remember where the
 * press started and open the drawer on press-lost too when it barely moved. */
static void card_pressed_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        s_press_valid = false;
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_press_x = (uint32_t)p.x;
    s_press_y = (uint32_t)p.y;
    s_press_valid = true;
    (void)e;
}

static void card_released_cb(lv_event_t *e)
{
    s_press_valid = false;
    (void)e;
}

static void card_press_lost_cb(lv_event_t *e)
{
    if (!s_press_valid) {
        return;
    }
    s_press_valid = false;
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dx = (int32_t)p.x - (int32_t)s_press_x;
    int32_t dy = (int32_t)p.y - (int32_t)s_press_y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx + dy <= DRAWER_TAP_SLOP) {
        drawer_open();
    }
    (void)e;
}

/* ------------------------------ tick ------------------------------ */

static const char *lstate_text(void)
{
    media_list_state_t lstate = media_list_state();
    if (lstate == MEDIA_LIST_LOADING) {
        return "列表加载中...";
    }
    if (lstate == MEDIA_LIST_FAILED) {
        return "列表加载失败";
    }
    return "列表待加载";
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    radio_status_t st;
    radio_get_status(&st);
    int count = media_list_count();
    media_list_state_t lstate = media_list_state();

    /* Follow channel changes started elsewhere (engine prev/next). */
    if (st.stream_id != s_last_stream_id) {
        s_last_stream_id = st.stream_id;
        set_text(s_now, st.name[0] ? st.name : "未选择频道");
    }

    /* Playlist load state (top right, next to the play state). */
    const char *lst = lstate_text();
    uint32_t lcolor = C_DIM;
    if (lstate == MEDIA_LIST_LOADING) {
        lcolor = C_AMBER;
    } else if (lstate == MEDIA_LIST_FAILED) {
        lcolor = C_RED;
    } else if (lstate == MEDIA_LIST_OK && s_last_list_state != MEDIA_LIST_OK) {
        static char lstbuf[48];
        snprintf(lstbuf, sizeof(lstbuf), "%d 个频道", count);
        lst = lstbuf;
    }
    if (lstate != s_last_list_state) {
        lv_obj_set_style_text_color(s_list, lv_color_hex(lcolor), 0);
        s_last_list_state = lstate;
    }
    set_text(s_list, lst);

    /* Per-page state: this page only reports its own source. When the radio
     * page is playing instead, say so rather than syncing the play state. */
    bool page_active = (st.ctx == CTX_TV) &&
                       (st.state == RADIO_STATE_PLAYING ||
                        st.state == RADIO_STATE_BUFFERING ||
                        st.state == RADIO_STATE_CONNECTING);
    const char *status = "已停止";
    uint32_t color = C_DIM;
    if (st.state == RADIO_STATE_ERROR) {
        status = "出错";
        color = C_RED;
    } else if (page_active) {
        switch (st.state) {
        case RADIO_STATE_CONNECTING:
            status = "连接中";
            color = C_AMBER;
            break;
        case RADIO_STATE_BUFFERING:
            status = "缓冲中";
            color = C_AMBER;
            break;
        case RADIO_STATE_PLAYING:
            status = st.buffering ? "缓冲中" : "播放中";
            color = st.buffering ? C_AMBER : C_GREEN;
            break;
        default:
            status = "播放中";
            color = C_GREEN;
            break;
        }
    } else if (st.state == RADIO_STATE_PLAYING ||
               st.state == RADIO_STATE_BUFFERING ||
               st.state == RADIO_STATE_CONNECTING) {
        status = (st.ctx == CTX_PRESET) ? "电台播放中" : "电视播放中";
    } else if (st.state == RADIO_STATE_PAUSED && st.ctx == CTX_TV) {
        status = "已暂停";
    }
    bool visual_changed = (strcmp(status, s_last_status_txt) != 0);
    if (visual_changed) {
        lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
        strlcpy(s_last_status_txt, status, sizeof(s_last_status_txt));
    }
    set_text(s_status, status);

    set_text(s_now, (st.name[0] && st.ctx == CTX_TV) ? st.name : "未选择频道");

    char buf[96];
    uint32_t info_color = C_DIM;
    if (st.state == RADIO_STATE_ERROR) {
        set_text(s_info, st.error[0] ? st.error : "连接失败");
        info_color = C_RED;
    } else if (st.sample_rate > 0) {
        snprintf(buf, sizeof(buf), "%d kbps · %.1f kHz · %d ch", st.bitrate_kbps,
                 st.sample_rate / 1000.0f, st.channels);
        set_text(s_info, buf);
        info_color = C_CYAN;
    } else if (st.state == RADIO_STATE_CONNECTING) {
        set_text(s_info, "正在连接...");
        info_color = C_AMBER;
    } else {
        set_text(s_info, "点击这里打开频道列表换台");
    }
    if (info_color != s_last_info_color) {
        lv_obj_set_style_text_color(s_info, lv_color_hex(info_color), 0);
        s_last_info_color = info_color;
    }

    snprintf(buf, sizeof(buf), "缓冲 %d%%", st.buffer_pct);
    set_text(s_buf_lbl, buf);
    lv_bar_set_value(s_buf_bar, st.buffer_pct, LV_ANIM_OFF);

    set_text(s_play_lbl, page_active ? "暂停" : "播放");
}

void tv_ui_build(lv_obj_t *parent)
{
    lv_obj_set_style_bg_opa(parent, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    make_label(parent, F20, C_TEXT, 12, 2, "网络电视");
    s_status = make_label(parent, F14, C_DIM, 0, 0, "已停止");
    lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -84, 6);
    s_list = make_label(parent, F14, C_DIM, 0, 0, lstate_text());
    lv_obj_align(s_list, LV_ALIGN_TOP_RIGHT, -172, 6);
    make_ctrl_button(parent, 406, 1, 72, 24, "刷新列表", F14, reload_cb);

    /* big now-playing panel, tap to open the channel drawer */
    lv_obj_t *card = make_card(parent, 10, 36, 460, 150);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(card, card_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(card, card_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(card, card_press_lost_cb, LV_EVENT_PRESS_LOST, NULL);
    s_now = make_label(card, F14, C_TEXT, 16, 6, "未选择频道");
    s_info = make_label(card, F14, C_DIM, 16, 26, "点击这里打开频道列表换台");

    s_spec = lv_obj_create(card);
    lv_obj_remove_style_all(s_spec);
    lv_obj_set_pos(s_spec, 16, 48);
    lv_obj_set_size(s_spec, 428, 74);
    lv_obj_remove_flag(s_spec, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_spec, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_spec, spectrum_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_buf_lbl = make_label(card, F14, C_DIM, 16, 126, "缓冲 0%");
    s_buf_bar = lv_bar_create(card);
    lv_obj_set_pos(s_buf_bar, 90, 128);
    lv_obj_set_size(s_buf_bar, 354, 8);
    lv_bar_set_range(s_buf_bar, 0, 100);
    lv_obj_set_style_bg_color(s_buf_bar, lv_color_hex(0x0b1b33), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_buf_bar, lv_color_hex(C_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_buf_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_buf_bar, 4, LV_PART_INDICATOR);

    /* controls */
    s_play_lbl = make_ctrl_button(parent, 10, 196, 90, 40, "播放", F14, play_cb);
    make_ctrl_button(parent, 106, 196, 44, 40, "<", F20, prev_cb);
    make_ctrl_button(parent, 156, 196, 44, 40, ">", F20, next_cb);
    make_label(parent, F14, C_DIM, 216, 206, "音量");
    s_vol = lv_slider_create(parent);
    lv_obj_set_pos(s_vol, 262, 208);
    lv_obj_set_size(s_vol, 160, 14);
    lv_slider_set_range(s_vol, 0, 100);
    lv_slider_set_value(s_vol, audio_get_vol(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_vol, lv_color_hex(0x223a5e), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_vol, lv_color_hex(C_CYAN), LV_PART_INDICATOR);
    lv_obj_add_event_cb(s_vol, vol_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_vol_lbl = make_label(parent, F14, C_CYAN, 428, 206, "70");
    update_vol_label(audio_get_vol());

    make_label(parent, F14, C_DIM, 10, 250, "点中间区域换台 · 左右滑动切页");

    /* scrim + drawer (created after the main UI so they sit on top) */
    s_scrim = lv_obj_create(parent);
    lv_obj_remove_style_all(s_scrim);
    lv_obj_set_pos(s_scrim, 0, 0);
    lv_obj_set_size(s_scrim, 480, 284);
    /* Opaque: a semi-transparent overlay makes LVGL allocate a 24 KB layer
     * buffer for the whole area while the drawer is open. */
    lv_obj_set_style_bg_color(s_scrim, lv_color_hex(0x081426), 0);
    lv_obj_set_style_bg_opa(s_scrim, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_scrim, scrim_cb, LV_EVENT_CLICKED, NULL);

    s_drawer = lv_obj_create(parent);
    lv_obj_remove_style_all(s_drawer);
    lv_obj_set_pos(s_drawer, -DRAWER_W, 0);
    lv_obj_set_size(s_drawer, DRAWER_W, 284);
    lv_obj_set_style_bg_color(s_drawer, lv_color_hex(C_DRAWER), 0);
    lv_obj_set_style_bg_opa(s_drawer, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_drawer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_drawer, LV_OBJ_FLAG_SCROLLABLE);

    s_drawer_title = make_label(s_drawer, F14, C_TEXT, 12, 10, "频道列表");
    make_ctrl_button(s_drawer, DRAWER_W - 74, 6, 62, 26, "收起", F14, scrim_cb);

    s_drawer_list = lv_obj_create(s_drawer);
    lv_obj_remove_style_all(s_drawer_list);
    lv_obj_set_pos(s_drawer_list, 4, 38);
    lv_obj_set_size(s_drawer_list, DRAWER_W - 8, 284 - 44);
    lv_obj_add_flag(s_drawer_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_drawer_list, LV_DIR_VER);
    lv_obj_set_style_bg_opa(s_drawer_list, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(s_drawer_list, list_scroll_cb, LV_EVENT_SCROLL, NULL);

    /* Invisible spacer that gives the scroll range of the whole list. */
    s_sizer = lv_obj_create(s_drawer_list);
    lv_obj_remove_style_all(s_sizer);
    lv_obj_set_pos(s_sizer, 0, 0);
    lv_obj_set_size(s_sizer, 1, 1);
    lv_obj_remove_flag(s_sizer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_sizer, LV_OBJ_FLAG_SCROLLABLE);

    /* Recycled rows; content and styling are set in drawer_update_rows(). */
    for (int i = 0; i < DRAWER_SLOTS; i++) {
        lv_obj_t *row = lv_button_create(s_drawer_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, DRAWER_W - 16, DRAWER_ROW_H - 4);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_BTN), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_BTN_SEL), LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(row, channel_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, F14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), 0);
        lv_label_set_text(lbl, "");

        s_slot_row[i] = row;
        s_slot_lbl[i] = lbl;
        s_slot_chan[i] = -1;
    }

    lv_timer_create(tick_cb, 200, NULL);
    lv_timer_create(spectrum_tick_cb, 33, NULL);
    tick_cb(NULL);
}
