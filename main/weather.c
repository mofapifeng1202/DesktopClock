/*
 * Open-Meteo weather fetcher (HTTPS, no API key) + cJSON parsing.
 * City is hardcoded: Chongqing (29.56N, 106.55E), timezone Asia/Shanghai.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "weather.h"
#include "wifi_app.h"

static const char *TAG = "weather";

#define WEATHER_URL \
    "https://api.open-meteo.com/v1/forecast?latitude=29.56&longitude=106.55" \
    "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m" \
    "&daily=weather_code,temperature_2m_max,temperature_2m_min" \
    "&timezone=Asia%2FShanghai&forecast_days=3"

#define RX_BUF_SIZE 4096

static char s_rx[RX_BUF_SIZE];
static int s_rx_len;
static weather_data_t s_wx;
static uint32_t s_ver;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0 &&
        s_rx_len + evt->data_len < (int)sizeof(s_rx)) {
        memcpy(s_rx + s_rx_len, evt->data, evt->data_len);
        s_rx_len += evt->data_len;
    }
    return ESP_OK;
}

static bool parse_json(void)
{
    cJSON *root = cJSON_ParseWithLength(s_rx, s_rx_len);
    if (root == NULL) {
        ESP_LOGW(TAG, "json parse failed");
        return false;
    }

    weather_data_t wx = {0};
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
                wx.daily[i].code = (int)cJSON_GetArrayItem(ca, i)->valuedouble;
                wx.daily[i].tmax = (float)cJSON_GetArrayItem(ma, i)->valuedouble;
                wx.daily[i].tmin = (float)cJSON_GetArrayItem(na, i)->valuedouble;
            }
            wx.valid = true;
            ok = true;
        }
    }

    if (ok) {
        s_wx = wx;   /* single writer task; UI readers are best-effort */
        s_ver++;
        ESP_LOGI(TAG, "weather: %.1f°C code=%d RH=%d%% wind=%.1f (v%lu)",
                 (double)s_wx.temp, s_wx.code, s_wx.humidity, (double)s_wx.wind,
                 (unsigned long)s_ver);
    }
    cJSON_Delete(root);
    return ok;
}

static bool fetch_json(void)
{
    s_rx_len = 0;
    const esp_http_client_config_t cfg = {
        .url = WEATHER_URL,
        .event_handler = http_event_cb,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) {
        return false;
    }
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    if (err != ESP_OK || status != 200 || s_rx_len == 0) {
        ESP_LOGW(TAG, "fetch failed: err=%s status=%d len=%d",
                 esp_err_to_name(err), status, s_rx_len);
        return false;
    }
    s_rx[s_rx_len] = '\0';
    return parse_json();
}

static void weather_task(void *arg)
{
    (void)arg;
    while (wifi_app_state() != WIFI_APP_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    while (1) {
        if (fetch_json()) {
            vTaskDelay(pdMS_TO_TICKS(30 * 60 * 1000));   /* refresh every 30 min */
        } else {
            vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000));    /* retry sooner on failure */
        }
    }
}

void weather_start(void)
{
    xTaskCreate(weather_task, "weather", 12288, NULL, 4, NULL);
}

const weather_data_t *weather_get(void)
{
    return &s_wx;
}

uint32_t weather_version(void)
{
    return s_ver;
}
