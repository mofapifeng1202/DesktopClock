/*
 * IPTV page for the 480x320 landscape tileview.
 *
 * Same layout/language as the radio page, but the channel list comes from the
 * runtime m3u playlist (media_list). Channel names may contain characters that
 * are not in the generated font subsets, so every label on this page uses
 * font_radio_14 with the built-in Source Han Sans CJK font as fallback.
 */
#include <stdio.h>
#include <string.h>

#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "clock_ui.h"
#include "main.h"
#include "media_list.h"
#include "radio.h"
#include "spectrum.h"
#include "tv_ui.h"

LV_FONT_DECLARE(font_radio_14);
LV_FONT_DECLARE(font_radio_20);
LV_FONT_DECLARE(lv_font_source_han_sans_sc_16_cjk);

#define F14     (&font_radio_14)
#define F20     (&font_radio_20)
#define TF14    (&s_tv_font)   /* 14 px + CJK fallback for playlist names */

#define C_TEXT  0xffffff
#define C_DIM   0x6f86a6
#define C_CYAN  0x00c2ff
#define C_GREEN 0x24d17e
#define C_AMBER 0xffd479
#define C_RED   0xff5c5c
#define C_CARD  0x13294a
#define C_BTN   0x152b4a
#define C_BTN_SEL 0x123f5a

#define BTNS_PER_PAGE 8

static lv_font_t s_tv_font;   /* RAM copy of font_radio_14 + CJK fallback */

static lv_obj_t *s_status;    /* play state */
static lv_obj_t *s_list;      /* playlist load state */
static lv_obj_t *s_now;
static lv_obj_t *s_info;
static lv_obj_t *s_buf_lbl;
static lv_obj_t *s_buf_bar;
static lv_obj_t *s_spec;
static uint8_t s_spec_draw[SPECTRUM_BANDS];
static lv_obj_t *s_play_lbl;
static lv_obj_t *s_vol;
static lv_obj_t *s_vol_lbl;
static lv_obj_t *s_btns[BTNS_PER_PAGE];
static lv_obj_t *s_btn_lbls[BTNS_PER_PAGE];
static lv_obj_t *s_page_lbl;
static int s_page;
static int s_page_count;
static bool s_vol_sync;
static radio_state_t s_last_state = (radio_state_t)-1;
static bool s_last_buffering;
static int s_last_station = -1;
static int s_last_page = -1;
static int s_last_count = -1;
static media_list_state_t s_last_list_state = (media_list_state_t)-1;
static uint32_t s_last_info_color = 0xffffffff;
static uint32_t s_last_stream_id;

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

static void style_station_btn(lv_obj_t *btn, bool selected)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(selected ? C_BTN_SEL : C_BTN), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    if (selected) {
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(C_CYAN), 0);
    } else {
        lv_obj_set_style_border_width(btn, 0, 0);
    }
}

static void station_cb(lv_event_t *e)
{
    int idx_in_page = (int)(intptr_t)lv_event_get_user_data(e);
    int idx = s_page * BTNS_PER_PAGE + idx_in_page;
    if (idx >= 0 && idx < media_list_count()) {
        radio_play_tv(idx);
    }
}

static void apply_page(void)
{
    int count = media_list_count();
    for (int k = 0; k < BTNS_PER_PAGE; k++) {
        int idx = s_page * BTNS_PER_PAGE + k;
        if (idx < count) {
            lv_obj_remove_flag(s_btns[k], LV_OBJ_FLAG_HIDDEN);
            set_text(s_btn_lbls[k], media_list_name(idx));
        } else {
            lv_obj_add_flag(s_btns[k], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_page_lbl) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d/%d", s_page + 1, s_page_count);
        set_text(s_page_lbl, buf);
    }
}

static void show_page(int page)
{
    if (page < 0) {
        page = 0;
    }
    if (page > s_page_count - 1) {
        page = s_page_count - 1;
    }
    if (page == s_page) {
        return;
    }
    s_page = page;
    apply_page();
}

static void page_prev_cb(lv_event_t *e)
{
    (void)e;
    show_page(s_page - 1);
}

static void page_next_cb(lv_event_t *e)
{
    (void)e;
    show_page(s_page + 1);
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
    audio_set_vol(v);
    update_vol_label(v);
    clock_ui_set_volume(v);
}

static void play_cb(lv_event_t *e)
{
    (void)e;
    radio_toggle_pause();
}

static void prev_cb(lv_event_t *e)
{
    (void)e;
    radio_prev();
}

static void next_cb(lv_event_t *e)
{
    (void)e;
    radio_next();
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
    dsc.radius = 1;

    for (int k = 0; k < SPECTRUM_BANDS; k++) {
        int v = s_spec_draw[k];
        int x1 = a.x1 + k * w / SPECTRUM_BANDS;
        int x2 = a.x1 + (k + 1) * w / SPECTRUM_BANDS - 2;
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
    spectrum_copy(spec);
    if (memcmp(spec, s_spec_draw, sizeof(spec)) != 0) {
        memcpy(s_spec_draw, spec, sizeof(spec));
        lv_obj_invalidate(s_spec);
    }
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    radio_status_t st;
    radio_get_status(&st);
    int count = media_list_count();
    media_list_state_t lstate = media_list_state();

    /* Follow preset changes started elsewhere (engine prev/next). */
    if (st.stream_id != s_last_stream_id) {
        s_last_stream_id = st.stream_id;
        if (st.station >= 0 && st.station < count) {
            int want = st.station / BTNS_PER_PAGE;
            if (want != s_page) {
                show_page(want);
            }
        }
    }

    /* Recompute pager when the playlist size changes. */
    if (count != s_last_count) {
        s_last_count = count;
        s_page_count = (count + BTNS_PER_PAGE - 1) / BTNS_PER_PAGE;
        if (s_page >= s_page_count) {
            s_page = s_page_count - 1;
        }
        if (s_page < 0) s_page = 0;
        apply_page();
    }

    /* Playlist load state (next to the pager). */
    const char *lst = "列表待加载";
    uint32_t lcolor = C_DIM;
    if (lstate == MEDIA_LIST_LOADING) {
        lst = "列表加载中...";
        lcolor = C_AMBER;
    } else if (lstate == MEDIA_LIST_OK) {
        static char lstbuf[40];
        snprintf(lstbuf, sizeof(lstbuf), "共 %d 个频道", count);
        lst = lstbuf;
        lcolor = C_DIM;
    } else if (lstate == MEDIA_LIST_FAILED) {
        lst = "列表加载失败";
        lcolor = C_RED;
    }
    if (lstate != s_last_list_state) {
        lv_obj_set_style_text_color(s_list, lv_color_hex(lcolor), 0);
        s_last_list_state = lstate;
    }
    set_text(s_list, lst);

    const char *status = "已停止";
    uint32_t color = C_DIM;
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
    case RADIO_STATE_PAUSED:
        status = "已暂停";
        color = C_DIM;
        break;
    case RADIO_STATE_ERROR:
        status = "出错";
        color = C_RED;
        break;
    default:
        break;
    }
    bool visual_changed = (st.state != s_last_state) || (st.buffering != s_last_buffering);
    if (visual_changed) {
        lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
        s_last_state = st.state;
        s_last_buffering = st.buffering;
    }
    set_text(s_status, status);

    bool active = (st.state == RADIO_STATE_PLAYING ||
                   st.state == RADIO_STATE_BUFFERING ||
                   st.state == RADIO_STATE_CONNECTING);
    set_text(s_now, st.name[0] ? st.name : "未选择频道");

    char buf[96];
    uint32_t info_color = C_DIM;
    if (st.state == RADIO_STATE_ERROR) {
        set_text(s_info, st.error[0] ? st.error : "连接失败");
        info_color = C_RED;
    } else if (st.sample_rate > 0) {
        snprintf(buf, sizeof(buf), "%d kbps · %.1f kHz", st.bitrate_kbps,
                 st.sample_rate / 1000.0f);
        set_text(s_info, buf);
        info_color = C_CYAN;
    } else if (st.state == RADIO_STATE_CONNECTING) {
        set_text(s_info, "正在连接...");
        info_color = C_AMBER;
    } else {
        set_text(s_info, "点击频道开始播放");
    }
    if (info_color != s_last_info_color) {
        lv_obj_set_style_text_color(s_info, lv_color_hex(info_color), 0);
        s_last_info_color = info_color;
    }

    snprintf(buf, sizeof(buf), "缓冲 %d%%", st.buffer_pct);
    set_text(s_buf_lbl, buf);
    lv_bar_set_value(s_buf_bar, st.buffer_pct, LV_ANIM_OFF);

    set_text(s_play_lbl, active ? "暂停" : "播放");

    if (st.station != s_last_station || s_page != s_last_page) {
        for (int k = 0; k < BTNS_PER_PAGE; k++) {
            int idx = s_page * BTNS_PER_PAGE + k;
            style_station_btn(s_btns[k], idx == st.station && idx < count);
        }
        s_last_station = st.station;
        s_last_page = s_page;
    }
}

void tv_ui_build(lv_obj_t *parent)
{
    s_tv_font = font_radio_14;
    s_tv_font.fallback = &lv_font_source_han_sans_sc_16_cjk;

    lv_obj_set_style_bg_opa(parent, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    make_label(parent, F20, C_TEXT, 12, 2, "网络电视");
    s_status = make_label(parent, TF14, C_DIM, 0, 0, "已停止");
    lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -12, 6);

    /* channel grid: 8 buttons, one page at a time */
    s_page = 0;
    s_page_count = 1;
    for (int k = 0; k < BTNS_PER_PAGE; k++) {
        int col = k % 2;
        int row = k / 2;
        int x = 10 + col * 134;
        int y = 38 + row * 47;
        lv_obj_t *btn = lv_button_create(parent);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_size(btn, 128, 42);
        style_station_btn(btn, false);
        lv_obj_add_event_cb(btn, station_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, TF14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), 0);
        lv_label_set_text(lbl, "");
        lv_obj_center(lbl);
        s_btns[k] = btn;
        s_btn_lbls[k] = lbl;
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);   /* shown when the list loads */
    }

    /* header pager + playlist state + refresh */
    make_ctrl_button(parent, 208, 1, 32, 24, "<", F14, page_prev_cb);
    s_page_lbl = make_label(parent, F14, C_TEXT, 242, 7, "1/1");
    lv_obj_set_width(s_page_lbl, 32);
    lv_obj_set_style_text_align(s_page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    make_ctrl_button(parent, 278, 1, 32, 24, ">", F14, page_next_cb);

    s_list = make_label(parent, TF14, C_DIM, 0, 0, "列表待加载");
    lv_obj_align(s_list, LV_ALIGN_TOP_RIGHT, -78, 6);
    make_ctrl_button(parent, 406, 1, 52, 24, "刷新", F14, reload_cb);

    /* now-playing card */
    lv_obj_t *card = make_card(parent, 278, 38, 192, 194);
    make_label(card, TF14, C_DIM, 10, 4, "电视直播");
    s_now = make_label(card, TF14, C_TEXT, 10, 22, "未选择频道");
    lv_obj_set_width(s_now, 172);
    lv_label_set_long_mode(s_now, LV_LABEL_LONG_WRAP);
    s_info = make_label(card, TF14, C_DIM, 10, 56, "点击频道开始播放");

    s_spec = lv_obj_create(card);
    lv_obj_remove_style_all(s_spec);
    lv_obj_set_pos(s_spec, 10, 74);
    lv_obj_set_size(s_spec, 172, 52);
    lv_obj_remove_flag(s_spec, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_spec, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_spec, spectrum_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_buf_lbl = make_label(card, TF14, C_DIM, 10, 132, "缓冲 0%");
    s_buf_bar = lv_bar_create(card);
    lv_obj_set_pos(s_buf_bar, 84, 134);
    lv_obj_set_size(s_buf_bar, 98, 8);
    lv_bar_set_range(s_buf_bar, 0, 100);
    lv_obj_set_style_bg_color(s_buf_bar, lv_color_hex(0x0b1b33), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_buf_bar, lv_color_hex(C_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_buf_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_buf_bar, 4, LV_PART_INDICATOR);

    s_play_lbl = make_ctrl_button(card, 10, 152, 84, 34, "播放", TF14, play_cb);
    make_ctrl_button(card, 100, 152, 38, 34, "<", F20, prev_cb);
    make_ctrl_button(card, 144, 152, 38, 34, ">", F20, next_cb);

    /* volume */
    make_label(parent, F14, C_DIM, 14, 250, "音量");
    s_vol = lv_slider_create(parent);
    lv_obj_set_pos(s_vol, 58, 252);
    lv_obj_set_size(s_vol, 240, 12);
    lv_slider_set_range(s_vol, 0, 100);
    lv_slider_set_value(s_vol, audio_get_vol(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_vol, lv_color_hex(0x223a5e), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_vol, lv_color_hex(C_CYAN), LV_PART_INDICATOR);
    lv_obj_add_event_cb(s_vol, vol_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_vol_lbl = make_label(parent, F14, C_CYAN, 306, 250, "70");
    update_vol_label(audio_get_vol());
    make_label(parent, F14, C_DIM, 390, 250, "滑动切页");

    lv_timer_create(tick_cb, 200, NULL);
    lv_timer_create(spectrum_tick_cb, 33, NULL);
    tick_cb(NULL);
}
