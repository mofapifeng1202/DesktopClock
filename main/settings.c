/*
 * Persistent system settings. See settings.h.
 *
 * Stored as one NVS blob so a save is a single write. Defaults: volume 70,
 * brightness 100, sleep 5 min, Wi-Fi from main/wifi_app_secret.h, TV playlist
 * from main/tv_config.h (both compile-time fallbacks for first boot).
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "settings.h"
#include "wifi_app.h"

/* The playlist URL fallback lives in main/tv_config.h (gitignored); see
 * tv_config.h.example. A placeholder is used when the file is absent. */
#if __has_include("tv_config.h")
#include "tv_config.h"
#endif
#ifndef TV_LIST_URL
#define TV_LIST_URL "http://iptv.example.com:1905/interface.m3u?relay=2"
#endif

static const char *TAG = "settings";
#define NVS_NS   "cfg"
#define NVS_KEY  "cfg"

#define CFG_MAGIC 0x43464731u   /* "CFG1" */

typedef struct {
    uint32_t magic;
    settings_t s;
} cfg_blob_t;

const int k_sleep_choices[SETTINGS_SLEEP_CHOICES] = {0, 1, 5, 10, 30};

static settings_t s_cfg;
static bool s_dirty;

static void load_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    snprintf(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), "%s", WIFI_APP_SSID);
    snprintf(s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), "%s", WIFI_APP_PASS);
    snprintf(s_cfg.tv_url, sizeof(s_cfg.tv_url), "%s", TV_LIST_URL);
    s_cfg.volume = 70;
    s_cfg.brightness = 100;
    s_cfg.sleep_min = 5;
    s_cfg.wx_src = WX_SRC_CN;
    snprintf(s_cfg.wx_city, sizeof(s_cfg.wx_city), "重庆");
    snprintf(s_cfg.wx_code, sizeof(s_cfg.wx_code), "101040100");
    s_cfg.wx_lat_x100 = 2956;
    s_cfg.wx_lon_x100 = 10655;
    s_cfg.lcd_mhz = 40;             /* the module's rated QSPI clock */
    s_cfg.lcd_mode = 2;             /* single buffer, 28 lines */
}

void settings_init(void)
{
    load_defaults();

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        cfg_blob_t blob;
        size_t len = sizeof(blob);
        esp_err_t r = nvs_get_blob(h, NVS_KEY, &blob, &len);
        if (r == ESP_OK && blob.magic == CFG_MAGIC && len == sizeof(blob)) {
            s_cfg = blob.s;

            /* A blob written by an older build may have empty strings. */
            if (s_cfg.tv_url[0] == '\0') {
                snprintf(s_cfg.tv_url, sizeof(s_cfg.tv_url), "%s", TV_LIST_URL);
            }
            if (s_cfg.brightness < 5) s_cfg.brightness = 5;
            if (s_cfg.brightness > 100) s_cfg.brightness = 100;
            if (s_cfg.volume < 0) s_cfg.volume = 0;
            if (s_cfg.volume > 100) s_cfg.volume = 100;
            if (s_cfg.lcd_mhz < 20 || s_cfg.lcd_mhz > 80) s_cfg.lcd_mhz = 40;
            if (s_cfg.lcd_mode < 0 || s_cfg.lcd_mode > 3) s_cfg.lcd_mode = 2;
            ESP_LOGI(TAG, "loaded: ssid=\"%s\" vol=%d bright=%d sleep=%d wx=%d/%s lcd=%dMHz",
                     s_cfg.wifi_ssid, s_cfg.volume, s_cfg.brightness, s_cfg.sleep_min,
                     s_cfg.wx_src, s_cfg.wx_city, s_cfg.lcd_mhz);
        } else if (r == ESP_OK && blob.magic == CFG_MAGIC && len > sizeof(uint32_t)) {
            /* Older, shorter blob: copy whatever part of the layout it had, so
             * adding settings does not throw away the Wi-Fi credentials. */
            size_t have = len - sizeof(uint32_t);
            size_t prefix = offsetof(settings_t, wx_src);
            if (have < prefix) {
                prefix = have;
            }
            memcpy(&s_cfg, &blob.s, prefix);
            if (have >= offsetof(settings_t, lcd_mhz)) {
                memcpy(&s_cfg.wx_src, &blob.s.wx_src, offsetof(settings_t, lcd_mhz) - offsetof(settings_t, wx_src));
            } else {
                s_cfg.wx_src = WX_SRC_CN;
                snprintf(s_cfg.wx_city, sizeof(s_cfg.wx_city), "重庆");
                snprintf(s_cfg.wx_code, sizeof(s_cfg.wx_code), "101040100");
                s_cfg.wx_lat_x100 = 2956;
                s_cfg.wx_lon_x100 = 10655;
            }
            s_dirty = true;
            ESP_LOGI(TAG, "migrated settings from a %u byte blob", (unsigned)len);
        } else {
            ESP_LOGI(TAG, "no saved settings, using defaults");
        }
        nvs_close(h);
    }
    if (s_dirty) {
        settings_save();        /* persist a migrated layout right away */
    }
    s_dirty = false;
}

const settings_t *settings_get(void)
{
    return &s_cfg;
}

const char *settings_wifi_ssid(void)
{
    return s_cfg.wifi_ssid;
}

void settings_set_volume(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (s_cfg.volume != v) {
        s_cfg.volume = v;
        s_dirty = true;
    }
}

void settings_set_brightness(int v)
{
    if (v < 5) v = 5;
    if (v > 100) v = 100;
    if (s_cfg.brightness != v) {
        s_cfg.brightness = v;
        s_dirty = true;
    }
}

void settings_set_sleep_min(int m)
{
    bool ok = false;
    for (int i = 0; i < SETTINGS_SLEEP_CHOICES; i++) {
        if (k_sleep_choices[i] == m) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        m = 5;
    }
    if (s_cfg.sleep_min != m) {
        s_cfg.sleep_min = m;
        s_dirty = true;
    }
}

void settings_set_wifi(const char *ssid, const char *pass)
{
    if (ssid == NULL) {
        return;
    }
    snprintf(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), "%s", ssid);
    snprintf(s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), "%s", pass ? pass : "");
    s_dirty = true;
}

void settings_set_tv_url(const char *url)
{
    if (url == NULL) {
        return;
    }
    snprintf(s_cfg.tv_url, sizeof(s_cfg.tv_url), "%s", url);
    s_dirty = true;
}

void settings_set_lcd_mode(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 3) mode = 3;
    if (s_cfg.lcd_mode != mode) {
        s_cfg.lcd_mode = mode;
        s_dirty = true;
    }
}

void settings_set_lcd_mhz(int mhz)
{
    if (mhz < 20) mhz = 20;
    if (mhz > 80) mhz = 80;
    if (s_cfg.lcd_mhz != mhz) {
        s_cfg.lcd_mhz = mhz;
        s_dirty = true;
    }
}

void settings_set_weather(int src, const char *city, const char *code,
                          int lat_x100, int lon_x100)
{
    if (src < WX_SRC_CN || src > WX_SRC_WTTR) {
        src = WX_SRC_CN;
    }
    s_cfg.wx_src = src;
    if (city != NULL) {
        snprintf(s_cfg.wx_city, sizeof(s_cfg.wx_city), "%s", city);
    }
    if (code != NULL) {
        snprintf(s_cfg.wx_code, sizeof(s_cfg.wx_code), "%s", code);
    }
    if (lat_x100 != 0) {
        s_cfg.wx_lat_x100 = lat_x100;
    }
    if (lon_x100 != 0) {
        s_cfg.wx_lon_x100 = lon_x100;
    }
    s_dirty = true;
}

void settings_save(void)
{
    if (!s_dirty) {
        return;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }
    cfg_blob_t blob = {.magic = CFG_MAGIC, .s = s_cfg};
    err = nvs_set_blob(h, NVS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_dirty = false;
        ESP_LOGI(TAG, "saved (vol=%d bright=%d sleep=%d)",
                 s_cfg.volume, s_cfg.brightness, s_cfg.sleep_min);
    } else {
        ESP_LOGW(TAG, "save failed: %s", esp_err_to_name(err));
    }
}

void settings_factory_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    load_defaults();
    s_dirty = false;
}
