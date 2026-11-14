/*
 * System settings page (5th tile).
 *
 * On-device quick settings; the full configuration (Wi-Fi, playlist URL,
 * system info, factory reset) is on the web page at http://<device-ip>/
 * or http://192.168.4.1 in setup-AP mode.
 */
#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#include "esp_heap_caps.h"
#include "esp_system.h"

#include "backlight.h"
#include "main.h"
#include "radio.h"
#include "settings.h"
#include "settings_ui.h"
#include "ui_sync.h"
#include "web_config.h"
#include "wifi_app.h"

LV_FONT_DECLARE(font_radio_14);
LV_FONT_DECLARE(font_radio_20);

#define F14 (&font_radio_14)
#define F20 (&font_radio_20)

#define C_TEXT  0xffffff
#define C_DIM   0x6f86a6
#define C_CYAN  0x00c2ff
#define C_GREEN 0x24d17e
#define C_AMBER 0xffd479
#define C_RED   0xff5c5c
#define C_CARD  0x13294a
#define C_BTN   0x152b4a
#define C_BTN_SEL 0x123f5a

static lv_obj_t *s_vol, *s_vol_lbl;
static lv_obj_t *s_bri, *s_bri_lbl;
static lv_obj_t *s_sleep_lbl;
static lv_obj_t *s_wifi_lbl;
static lv_obj_t *s_about_lbl;
static lv_obj_t *s_ap_hint;
static bool s_vol_sync;
static bool s_bri_sync;

static void set_text(lv_obj_t *lbl, const char *txt)
{
    if (lbl && strcmp(lv_label_get_text(lbl), txt) != 0) {
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

static lv_obj_t *make_btn(lv_obj_t *parent, int x, int y, int w, int h,
                          const char *txt, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_BTN), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_BTN_SEL), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, F14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), 0);
    lv_label_set_text(lbl, txt);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

static lv_obj_t *make_slider(lv_obj_t *parent, int x, int y, int w, int v,
                             lv_event_cb_t cb)
{
    lv_obj_t *sl = lv_slider_create(parent);
    lv_obj_set_pos(sl, x, y);
    lv_obj_set_size(sl, w, 14);
    lv_slider_set_range(sl, 0, 100);
    lv_slider_set_value(sl, v, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0x223a5e), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_CYAN), LV_PART_INDICATOR);
    lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sl, cb, LV_EVENT_RELEASED, NULL);
    return sl;
}

static void vol_cb(lv_event_t *e)
{
    if (s_vol_sync) {
        return;
    }
    int v = (int)lv_slider_get_value(lv_event_get_target(e));
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", v);
    set_text(s_vol_lbl, buf);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        ui_sync_volume(v);
    } else {
        settings_save();
    }
}

void settings_ui_set_volume(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    s_vol_sync = true;
    if (s_vol) {
        lv_slider_set_value(s_vol, vol, LV_ANIM_OFF);
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", vol);
    set_text(s_vol_lbl, buf);
    s_vol_sync = false;
}

static void bri_cb(lv_event_t *e)
{
    if (s_bri_sync) {
        return;
    }
    int v = (int)lv_slider_get_value(lv_event_get_target(e));
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", v);
    set_text(s_bri_lbl, buf);
    backlight_set(v);
    settings_set_brightness(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        settings_save();
    }
}

static void sleep_cb(lv_event_t *e)
{
    (void)e;
    int cur = settings_get()->sleep_min;
    int next = k_sleep_choices[0];
    for (int i = 0; i < SETTINGS_SLEEP_CHOICES; i++) {
        if (k_sleep_choices[i] == cur) {
            next = k_sleep_choices[(i + 1) % SETTINGS_SLEEP_CHOICES];
            break;
        }
    }
    settings_set_sleep_min(next);
    settings_save();
    char buf[32];
    if (next == 0) {
        snprintf(buf, sizeof(buf), "熄屏 从不");
    } else {
        snprintf(buf, sizeof(buf), "熄屏 %d 分", next);
    }
    set_text(s_sleep_lbl, buf);
}

static void ap_cb(lv_event_t *e)
{
    (void)e;
    web_config_enter_ap_mode();
    lv_obj_remove_flag(s_ap_hint, LV_OBJ_FLAG_HIDDEN);
}

static void restart_cb(lv_event_t *e)
{
    (void)e;
    settings_save();
    esp_restart();
}

static void factory_cb(lv_event_t *e)
{
    (void)e;
    settings_factory_reset();
    esp_restart();
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    const settings_t *c = settings_get();

    /* keep the brightness slider in sync when changed from the web page */
    int bri = backlight_get();
    if ((int)lv_slider_get_value(s_bri) != bri) {
        s_bri_sync = true;
        lv_slider_set_value(s_bri, bri, LV_ANIM_OFF);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", bri);
        set_text(s_bri_lbl, buf);
        s_bri_sync = false;
    }

    char buf[96];
    if (web_config_ap_mode()) {
        set_text(s_wifi_lbl, "配网模式：手机连 DesktopClock-Setup");
    } else if (wifi_app_state() == WIFI_APP_CONNECTED) {
        snprintf(buf, sizeof(buf), "Wi-Fi %s  %s  %ddBm", c->wifi_ssid,
                 wifi_app_ip(), wifi_app_rssi());
        set_text(s_wifi_lbl, buf);
    } else {
        set_text(s_wifi_lbl, "Wi-Fi 未连接");
    }

    snprintf(buf, sizeof(buf), "网页设置: http://%s/   内存 %uKB(最低 %uKB)",
             wifi_app_ip()[0] ? wifi_app_ip() : "192.168.4.1",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
    set_text(s_about_lbl, buf);
}

void settings_ui_build(lv_obj_t *parent)
{
    lv_obj_set_style_bg_opa(parent, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    make_label(parent, F20, C_TEXT, 12, 2, "设置");
    make_label(parent, F14, C_DIM, 70, 8, "完整配置请用浏览器打开设备 IP");

    /* volume */
    make_label(parent, F14, C_DIM, 16, 46, "音量");
    s_vol = make_slider(parent, 80, 48, 300, settings_get()->volume, vol_cb);
    s_vol_lbl = make_label(parent, F14, C_CYAN, 396, 46, "70");
    settings_ui_set_volume(settings_get()->volume);

    /* brightness */
    make_label(parent, F14, C_DIM, 16, 90, "亮度");
    s_bri = make_slider(parent, 80, 92, 300, backlight_get(), bri_cb);
    s_bri_lbl = make_label(parent, F14, C_CYAN, 396, 90, "100%");
    {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", backlight_get());
        set_text(s_bri_lbl, buf);
    }

    /* sleep + setup AP */
    char sbuf[32];
    if (settings_get()->sleep_min == 0) {
        snprintf(sbuf, sizeof(sbuf), "熄屏 从不");
    } else {
        snprintf(sbuf, sizeof(sbuf), "熄屏 %d 分", settings_get()->sleep_min);
    }
    lv_obj_t *sleep_btn = make_btn(parent, 16, 132, 110, 34, sbuf, sleep_cb);
    s_sleep_lbl = lv_obj_get_child(sleep_btn, 0);

    make_btn(parent, 140, 132, 120, 34, "手机配网", ap_cb);
    make_btn(parent, 270, 132, 90, 34, "重启", restart_cb);
    make_btn(parent, 370, 132, 100, 34, "恢复出厂", factory_cb);

    /* status */
    s_wifi_lbl = make_label(parent, F14, C_TEXT, 16, 182, "Wi-Fi 未连接");
    s_about_lbl = make_label(parent, F14, C_DIM, 16, 208, "");

    s_ap_hint = make_label(parent, F14, C_AMBER, 16, 240,
                           "手机连接 Wi-Fi: DesktopClock-Setup，打开 http://192.168.4.1");
    lv_obj_add_flag(s_ap_hint, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(tick_cb, 1000, NULL);
    tick_cb(NULL);
}
