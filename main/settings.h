#pragma once

#include <stdbool.h>

/*
 * Persistent system settings (NVS namespace "cfg").
 *
 * Values are loaded once at boot; setters update RAM immediately and mark the
 * blob dirty. settings_save() persists (called on slider release / web POST /
 * before restart) so dragging a slider does not hammer the flash.
 */

/* Weather sources. Keep in sync with the web page's dropdown. */
enum {
    WX_SRC_CN = 0,      /* t.weather.itboy.net (中国天气网 镜像, 城市编码) */
    WX_SRC_OPENMETEO,   /* api.open-meteo.com (经纬度) */
    WX_SRC_WTTR,        /* wttr.in (经纬度/城市名) */
};

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char tv_url[193];
    int volume;        /* 0..100 */
    int brightness;    /* 5..100 */
    int sleep_min;     /* screen-off timeout, 0 = never */
    /* weather */
    int wx_src;        /* WX_SRC_* */
    char wx_city[24];  /* display name, e.g. "重庆" */
    char wx_code[16];  /* 中国天气网 city code, e.g. "101040100" */
    int wx_lat_x100;   /* latitude  x 100 for the coordinate based sources */
    int wx_lon_x100;   /* longitude x 100 */
} settings_t;

#define SETTINGS_SLEEP_CHOICES 5
extern const int k_sleep_choices[SETTINGS_SLEEP_CHOICES];   /* 0,1,5,10,30 */

void settings_init(void);

const settings_t *settings_get(void);

void settings_set_volume(int v);        /* RAM only, call settings_save() */
void settings_set_brightness(int v);    /* RAM only */
void settings_set_sleep_min(int m);     /* RAM only */
void settings_set_wifi(const char *ssid, const char *pass);
void settings_set_tv_url(const char *url);
void settings_set_weather(int src, const char *city, const char *code,
                          int lat_x100, int lon_x100);

/* Persist to NVS (no-op when nothing changed). */
void settings_save(void);

/* Wipe the namespace and restore defaults (in RAM; caller restarts). */
void settings_factory_reset(void);

/* "" when no credentials are stored. */
const char *settings_wifi_ssid(void);
