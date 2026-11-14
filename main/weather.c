/*
 * Weather fetcher with three selectable sources (see settings.h):
 *
 *   WX_SRC_CN        t.weather.itboy.net  (中国天气网 镜像, plain HTTP, 城市编码)
 *   WX_SRC_OPENMETEO api.open-meteo.com   (HTTPS, 经纬度, WMO code)
 *   WX_SRC_WTTR      wttr.in              (HTTPS, 经纬度, 紧凑文本格式)
 *
 * The CN source is the default: api.open-meteo.com is frequently unreachable
 * from mainland China, and the CN source needs no TLS at all (much less RAM).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "settings.h"
#include "weather.h"
#include "wifi_app.h"

static const char *TAG = "weather";

#define RX_BUF_SIZE 4096

static char s_rx[RX_BUF_SIZE];
static int s_rx_len;
static weather_data_t s_wx;
static volatile uint32_t s_ver;
static volatile bool s_refresh_now;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0 &&
        s_rx_len + evt->data_len < (int)sizeof(s_rx)) {
        memcpy(s_rx + s_rx_len, evt->data, evt->data_len);
        s_rx_len += evt->data_len;
    }
    return ESP_OK;
}

/* ------------------------------ helpers ---------------------------- */

/* "高温 21℃" / "低温 15℃" / "21℃" / "-3℃" -> float */
static float parse_temp_cn(const char *s)
{
    if (s == NULL) {
        return 0;
    }
    while (*s && (*s < '0' || *s > '9') && *s != '-') {
        s++;
    }
    return (float)atof(s);
}

/* "57%" -> 57, "" -> -1 */
static int parse_pct(const char *s)
{
    if (s == NULL || s[0] == '\0') {
        return -1;
    }
    return atoi(s);
}

/* Copy at most n-1 bytes, but never split a UTF-8 sequence: a truncated
 * multi-byte char would be drawn as garbage ("乱码") on the panel. */
static void copy_str(char *dst, size_t n, const char *src)
{
    if (dst == NULL || n == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t lim = n - 1;
    size_t len = strlen(src);
    if (len > lim) {
        len = lim;
        /* back off to the start of the character we would cut in half */
        while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) {
            len--;
        }
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* ------------------------- source: 中国天气网 镜像 ------------------ */

static bool parse_cn(void)
{
    cJSON *root = cJSON_ParseWithLength(s_rx, s_rx_len);
    if (root == NULL) {
        ESP_LOGW(TAG, "cn: json parse failed (%d bytes)", s_rx_len);
        return false;
    }
    weather_data_t wx = {0};
    wx.code = -1;
    wx.wind = -1;
    bool ok = false;

    cJSON *cityinfo = cJSON_GetObjectItem(root, "cityInfo");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsObject(data)) {
        const char *wendu = cJSON_GetStringValue(cJSON_GetObjectItem(data, "wendu"));
        const char *shidu = cJSON_GetStringValue(cJSON_GetObjectItem(data, "shidu"));
        if (wendu != NULL) {
            wx.temp = (float)atof(wendu);
            wx.humidity = parse_pct(shidu);
            wx.valid = true;
            ok = true;
        }
        cJSON *fc = cJSON_GetObjectItem(data, "forecast");
        if (cJSON_IsArray(fc)) {
            int n = cJSON_GetArraySize(fc);
            if (n > WEATHER_DAILY_DAYS) {
                n = WEATHER_DAILY_DAYS;
            }
            for (int i = 0; i < n; i++) {
                cJSON *d = cJSON_GetArrayItem(fc, i);
                const char *ymd = cJSON_GetStringValue(cJSON_GetObjectItem(d, "ymd"));
                const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(d, "type"));
                const char *high = cJSON_GetStringValue(cJSON_GetObjectItem(d, "high"));
                const char *low = cJSON_GetStringValue(cJSON_GetObjectItem(d, "low"));
                if (ymd != NULL && strlen(ymd) >= 10) {
                    snprintf(wx.daily[i].date, sizeof(wx.daily[i].date), "%s", ymd + 5);
                }
                copy_str(wx.daily[i].desc, sizeof(wx.daily[i].desc), type);
                wx.daily[i].code = -1;
                wx.daily[i].tmax = parse_temp_cn(high);
                wx.daily[i].tmin = parse_temp_cn(low);
                /* the first entry describes "now" as well */
                if (i == 0) {
                    copy_str(wx.desc, sizeof(wx.desc), type);
                }
            }
        }
    }
    if (cJSON_IsObject(cityinfo)) {
        const char *city = cJSON_GetStringValue(cJSON_GetObjectItem(cityinfo, "city"));
        if (city != NULL && city[0] != '\0') {
            copy_str(wx.city, sizeof(wx.city), city);
        }
    }
    if (wx.city[0] == '\0') {
        copy_str(wx.city, sizeof(wx.city), settings_get()->wx_city);
    }
    if (ok) {
        s_wx = wx;
        s_ver++;
        ESP_LOGI(TAG, "cn: %s %.1f°C %s RH=%d%%", s_wx.city, (double)s_wx.temp,
                 s_wx.desc, s_wx.humidity);
    } else {
        ESP_LOGW(TAG, "cn: unexpected payload");
    }
    cJSON_Delete(root);
    return ok;
}

/* WMO weather code -> Chinese text, so every source fills `desc`. */
static const char *wmo_text(int code)
{
    if (code == 0) return "晴";
    if (code == 1) return "晴间多云";
    if (code == 2) return "多云";
    if (code == 3) return "阴";
    if (code == 45 || code == 48) return "雾";
    if (code >= 51 && code <= 57) return "毛毛雨";
    if (code >= 61 && code <= 67) return "雨";
    if (code >= 71 && code <= 77) return "雪";
    if (code >= 80 && code <= 82) return "阵雨";
    if (code >= 95) return "雷雨";
    return "未知";
}

/* --------------------------- source: Open-Meteo -------------------- */

static bool parse_openmeteo(void)
{
    cJSON *root = cJSON_ParseWithLength(s_rx, s_rx_len);
    if (root == NULL) {
        ESP_LOGW(TAG, "openmeteo: json parse failed");
        return false;
    }
    weather_data_t wx = {0};
    wx.code = -1;
    wx.wind = -1;
    bool ok = false;

    cJSON *cur = cJSON_GetObjectItem(root, "current");
    cJSON *daily = cJSON_GetObjectItem(root, "daily");
    if (cJSON_IsObject(cur) && cJSON_IsObject(daily)) {
        cJSON *t = cJSON_GetObjectItem(cur, "temperature_2m");
        cJSON *h = cJSON_GetObjectItem(cur, "relative_humidity_2m");
        cJSON *c = cJSON_GetObjectItem(cur, "weather_code");
        cJSON *w = cJSON_GetObjectItem(cur, "wind_speed_10m");
        cJSON *ta = cJSON_GetObjectItem(daily, "time");
        cJSON *ca = cJSON_GetObjectItem(daily, "weather_code");
        cJSON *ma = cJSON_GetObjectItem(daily, "temperature_2m_max");
        cJSON *na = cJSON_GetObjectItem(daily, "temperature_2m_min");
        if (t && h && c && w && ta && ca && ma && na) {
            wx.temp = (float)t->valuedouble;
            wx.humidity = (int)h->valuedouble;
            wx.code = (int)c->valuedouble;
            wx.wind = (float)w->valuedouble;
            int n = cJSON_GetArraySize(ta);
            if (n > WEATHER_DAILY_DAYS) {
                n = WEATHER_DAILY_DAYS;
            }
            for (int i = 0; i < n; i++) {
                const char *d = cJSON_GetStringValue(cJSON_GetArrayItem(ta, i));
                snprintf(wx.daily[i].date, sizeof(wx.daily[i].date), "%.5s", d ? d + 5 : "--");
                cJSON *dc = cJSON_GetArrayItem(ca, i);
                wx.daily[i].code = dc ? (int)dc->valuedouble : -1;
                cJSON *dm = cJSON_GetArrayItem(ma, i);
                cJSON *dn = cJSON_GetArrayItem(na, i);
                wx.daily[i].tmax = dm ? (float)dm->valuedouble : 0;
                wx.daily[i].tmin = dn ? (float)dn->valuedouble : 0;
            }
            copy_str(wx.desc, sizeof(wx.desc), wmo_text(wx.code));
            for (int i = 0; i < WEATHER_DAILY_DAYS; i++) {
                if (wx.daily[i].date[0] != '\0') {
                    copy_str(wx.daily[i].desc, sizeof(wx.daily[i].desc),
                             wmo_text(wx.daily[i].code));
                }
            }
            wx.valid = true;
            ok = true;
        }
    }
    copy_str(wx.city, sizeof(wx.city), settings_get()->wx_city);
    if (ok) {
        s_wx = wx;
        s_ver++;
        ESP_LOGI(TAG, "openmeteo: %s %.1f°C code=%d RH=%d%% wind=%.1f",
                 s_wx.city, (double)s_wx.temp, s_wx.code, s_wx.humidity, (double)s_wx.wind);
    }
    cJSON_Delete(root);
    return ok;
}

/* ----------------------------- source: wttr.in --------------------- */

static const char *wttr_desc_cn(const char *en)
{
    if (en == NULL) return "";
    if (strstr(en, "Thunder")) return "雷雨";
    if (strstr(en, "Snow") || strstr(en, "Blizzard")) return "雪";
    if (strstr(en, "Sleet") || strstr(en, "Ice")) return "雨夹雪";
    if (strstr(en, "Heavy rain")) return "大雨";
    if (strstr(en, "rain") || strstr(en, "Rain") || strstr(en, "Shower")) return "雨";
    if (strstr(en, "Drizzle") || strstr(en, "drizzle")) return "毛毛雨";
    if (strstr(en, "Fog") || strstr(en, "Mist") || strstr(en, "Haze") ||
        strstr(en, "haze") || strstr(en, "Smoky")) return "雾霾";
    if (strstr(en, "Overcast")) return "阴";
    if (strstr(en, "Cloudy") || strstr(en, "cloudy")) return "多云";
    if (strstr(en, "Sunny") || strstr(en, "Clear")) return "晴";
    return en;
}

/* wttr's compact format: "+16°C|73%|↙8km/h|Smoky haze" */
static bool parse_wttr(void)
{
    s_rx[s_rx_len] = '\0';
    char *p1 = strchr(s_rx, '|');
    if (p1 == NULL) {
        ESP_LOGW(TAG, "wttr: unexpected payload");
        return false;
    }
    *p1++ = '\0';
    char *p2 = strchr(p1, '|');
    if (p2 != NULL) {
        *p2++ = '\0';
    }
    char *p3 = (p2 != NULL) ? strchr(p2, '|') : NULL;
    if (p3 != NULL) {
        *p3++ = '\0';
    }

    weather_data_t wx = {0};
    wx.code = -1;
    wx.temp = parse_temp_cn(s_rx);
    wx.humidity = parse_pct(p1);
    wx.wind = (p2 != NULL) ? parse_temp_cn(p2) : -1;   /* "8km/h" -> 8 */
    copy_str(wx.desc, sizeof(wx.desc), wttr_desc_cn(p3));
    copy_str(wx.city, sizeof(wx.city), settings_get()->wx_city);
    wx.valid = true;
    s_wx = wx;
    s_ver++;
    ESP_LOGI(TAG, "wttr: %s %.1f°C %s RH=%d%% wind=%.1f", s_wx.city,
             (double)s_wx.temp, s_wx.desc, s_wx.humidity, (double)s_wx.wind);
    return true;
}

/* ------------------------------ fetching --------------------------- */

static void build_url(char *buf, size_t n)
{
    const settings_t *c = settings_get();
    switch (c->wx_src) {
    case WX_SRC_OPENMETEO:
        snprintf(buf, n,
                 "https://api.open-meteo.com/v1/forecast?latitude=%d.%02d&longitude=%d.%02d"
                 "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
                 "&daily=weather_code,temperature_2m_max,temperature_2m_min"
                 "&timezone=Asia%%2FShanghai&forecast_days=3",
                 c->wx_lat_x100 / 100, abs(c->wx_lat_x100 % 100),
                 c->wx_lon_x100 / 100, abs(c->wx_lon_x100 % 100));
        break;
    case WX_SRC_WTTR:
        snprintf(buf, n, "https://wttr.in/%d.%02d,%d.%02d?format=%%t|%%h|%%w|%%C&m",
                 c->wx_lat_x100 / 100, abs(c->wx_lat_x100 % 100),
                 c->wx_lon_x100 / 100, abs(c->wx_lon_x100 % 100));
        break;
    case WX_SRC_CN:
    default:
        snprintf(buf, n, "http://t.weather.itboy.net/api/weather/city/%s",
                 c->wx_code[0] ? c->wx_code : "101040100");
        break;
    }
}

static bool fetch_json(void)
{
    char url[256];
    build_url(url, sizeof(url));
    s_rx_len = 0;

    const esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event_cb,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) {
        return false;
    }
    /* the CN mirror wants a browser-ish UA, wttr too */
    esp_http_client_set_header(cli, "User-Agent", "ESP32-DesktopClock/1.0");
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    if (err != ESP_OK || status != 200 || s_rx_len == 0) {
        ESP_LOGW(TAG, "fetch failed: err=%s status=%d len=%d url=%s",
                 esp_err_to_name(err), status, s_rx_len, url);
        return false;
    }
    s_rx[s_rx_len] = '\0';

    switch (settings_get()->wx_src) {
    case WX_SRC_OPENMETEO: return parse_openmeteo();
    case WX_SRC_WTTR:      return parse_wttr();
    case WX_SRC_CN:
    default:               return parse_cn();
    }
}

static void weather_task(void *arg)
{
    (void)arg;
    while (wifi_app_state() != WIFI_APP_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    while (1) {
        bool ok = fetch_json();
        if (ok) {
            /* sleep 30 min, but wake up early when the settings changed */
            for (int i = 0; i < 30 * 60; i++) {
                if (s_refresh_now) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        } else {
            for (int i = 0; i < 60; i++) {      /* retry in ~1 min */
                if (s_refresh_now) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
        if (s_refresh_now) {
            s_refresh_now = false;
        }
    }
}

void weather_start(void)
{
    xTaskCreate(weather_task, "weather", 10240, NULL, 4, NULL);
}

void weather_refresh(void)
{
    s_wx.valid = false;
    s_refresh_now = true;
}

const weather_data_t *weather_get(void)
{
    return &s_wx;
}

uint32_t weather_version(void)
{
    return s_ver;
}
