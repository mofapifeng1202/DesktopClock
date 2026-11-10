/*
 * Radio page for the 480x320 landscape tileview.
 *
 * Layout:
 *   title + state                      (top)
 *   2 x 4 preset buttons | now-playing  (middle)
 *   volume slider                       (bottom)
 *
 * All Chinese glyphs on this page come from the generated font_radio_14/20
 * subsets. If you add new text, regenerate them with tools/gen_radio_font.sh,
 * otherwise the new characters render as blank boxes.
 */
#include <stdio.h>
#include <string.h>

#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "clock_ui.h"
#include "main.h"
#include "radio.h"
#include "radio_ui.h"

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

#define MAX_BTNS 8   /* 2 columns x 4 rows fits the 480x320 page */

static lv_obj_t *s_status;
static lv_obj_t *s_now;
static lv_obj_t *s_genre;
static lv_obj_t *s_info;
static lv_obj_t *s_buf_lbl;
static lv_obj_t *s_buf_bar;
static lv_obj_t *s_level_bar;
static lv_obj_t *s_play_lbl;
static lv_obj_t *s_vol;
static lv_obj_t *s_vol_lbl;
static lv_obj_t *s_btns[MAX_BTNS];
static int s_btn_count;
static bool s_vol_sync;
static radio_state_t s_last_state = (radio_state_t)-1;
static bool s_last_buffering;
static int s_last_station = -1;
static uint32_t s_last_info_color = 0xffffffff;

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
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
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
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    radio_play(idx);
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

void radio_ui_set_volume(int vol)
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

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    radio_status_t st;
    radio_get_status(&st);

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

    char buf[96];
    if (st.station >= 0 && st.station < radio_station_count()) {
        set_text(s_now, radio_station_name(st.station));
        snprintf(buf, sizeof(buf), "%s · %s", active ? "正在播放" : "已选电台",
                 radio_station_desc(st.station));
        set_text(s_genre, buf);
    } else {
        set_text(s_now, "未选择电台");
        set_text(s_genre, "正在播放");
    }

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
        set_text(s_info, "等待连接");
    }
    if (info_color != s_last_info_color) {
        lv_obj_set_style_text_color(s_info, lv_color_hex(info_color), 0);
        s_last_info_color = info_color;
    }

    snprintf(buf, sizeof(buf), "缓冲 %d%%", st.buffer_pct);
    set_text(s_buf_lbl, buf);
    lv_bar_set_value(s_buf_bar, st.buffer_pct, LV_ANIM_OFF);
    lv_bar_set_value(s_level_bar, st.level, LV_ANIM_OFF);

    set_text(s_play_lbl, active ? "暂停" : "播放");

    if (st.station != s_last_station) {
        for (int i = 0; i < s_btn_count; i++) {
            style_station_btn(s_btns[i], i == st.station);
        }
        s_last_station = st.station;
    }
}

void radio_ui_build(lv_obj_t *parent)
{
    lv_obj_set_style_bg_opa(parent, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    make_label(parent, F20, C_TEXT, 12, 2, "网络收音机");
    s_status = make_label(parent, F14, C_DIM, 0, 0, "已停止");
    lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -12, 6);

    /* preset grid: 2 columns x 4 rows */
    s_btn_count = radio_station_count();
    if (s_btn_count > MAX_BTNS) {
        s_btn_count = MAX_BTNS;
    }
    for (int i = 0; i < s_btn_count; i++) {
        int col = i % 2;
        int row = i / 2;
        int x = 10 + col * 134;
        int y = 38 + row * 47;
        lv_obj_t *btn = lv_button_create(parent);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_size(btn, 128, 42);
        style_station_btn(btn, false);
        lv_obj_add_event_cb(btn, station_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, F14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), 0);
        lv_label_set_text(lbl, radio_station_name(i));
        lv_obj_center(lbl);
        s_btns[i] = btn;
    }

    /* now-playing card */
    lv_obj_t *card = make_card(parent, 278, 38, 192, 194);
    s_genre = make_label(card, F14, C_DIM, 10, 6, "正在播放");
    s_now = make_label(card, F20, C_TEXT, 10, 25, "未选择电台");
    lv_obj_set_width(s_now, 172);
    lv_label_set_long_mode(s_now, LV_LABEL_LONG_WRAP);
    s_info = make_label(card, F14, C_DIM, 10, 74, "等待连接");

    s_buf_lbl = make_label(card, F14, C_DIM, 10, 96, "缓冲 0%");
    s_buf_bar = lv_bar_create(card);
    lv_obj_set_pos(s_buf_bar, 10, 116);
    lv_obj_set_size(s_buf_bar, 172, 8);
    lv_bar_set_range(s_buf_bar, 0, 100);
    lv_obj_set_style_bg_color(s_buf_bar, lv_color_hex(0x0b1b33), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_buf_bar, lv_color_hex(C_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_buf_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_buf_bar, 4, LV_PART_INDICATOR);

    make_label(card, F14, C_DIM, 10, 132, "电平");
    s_level_bar = lv_bar_create(card);
    lv_obj_set_pos(s_level_bar, 48, 134);
    lv_obj_set_size(s_level_bar, 134, 8);
    lv_bar_set_range(s_level_bar, 0, 100);
    lv_obj_set_style_bg_color(s_level_bar, lv_color_hex(0x0b1b33), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_level_bar, lv_color_hex(C_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_level_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_level_bar, 4, LV_PART_INDICATOR);

    s_play_lbl = make_ctrl_button(card, 10, 152, 84, 34, "播放", F14, play_cb);
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
    tick_cb(NULL);
}
