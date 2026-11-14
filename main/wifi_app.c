/*
 * Wi-Fi STA connection with auto reconnect + SNTP time sync (CST, UTC+8).
 *
 * Credentials come from the settings module (NVS, web-configurable) and fall
 * back to main/wifi_app_secret.h on first boot. If no credentials are stored
 * the device starts an open setup AP ("DesktopClock-Setup") with the web
 * config page at http://192.168.4.1.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "lwip/ip_addr.h"
#include "nvs_flash.h"

#include "settings.h"
#include "wifi_app.h"

static const char *TAG = "wifi_app";

#define AP_SSID "DesktopClock-Setup"

static volatile wifi_app_state_t s_state = WIFI_APP_IDLE;
static volatile int s_retry;
static volatile bool s_time_synced;
static char s_ip[16];
static bool s_ap_active;
static bool s_wifi_started;

/* SNTP only works once we have an IP; timezone is China Standard Time */
static void sntp_start(void)
{
    if (esp_sntp_enabled()) {
        return;
    }
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started (ntp.aliyun.com, TZ=CST-8)");
}

/* Scan first, then connect. Connecting straight from STA_START can fail with
 * "no AP found" on some routers because the AP list is not populated yet. */
static void wifi_scan_connect_task(void *arg)
{
    (void)arg;
    const wifi_scan_config_t sc = {.show_hidden = true};
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t num = 0;
        esp_wifi_scan_get_ap_num(&num);
        wifi_ap_record_t *recs = calloc(num ? num : 1, sizeof(wifi_ap_record_t));
        if (recs != NULL) {
            uint16_t got = num;
            esp_wifi_scan_get_ap_records(&got, recs);
            for (int i = 0; i < got; i++) {
                if (strcmp((char *)recs[i].ssid, settings_get()->wifi_ssid) == 0) {
                    ESP_LOGI(TAG, "target AP found: ch%d %d dBm", recs[i].primary, recs[i].rssi);
                    break;
                }
            }
            free(recs);
        }
    }
    esp_wifi_connect();
    vTaskDelete(NULL);
}

static void apply_sta_credentials(void)
{
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, settings_get()->wifi_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, settings_get()->wifi_pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    if (wc.sta.password[0] == '\0') {
        wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_state = WIFI_APP_CONNECTING;
        if (settings_get()->wifi_ssid[0] != '\0') {
            xTaskCreate(wifi_scan_connect_task, "wiscan", 4096, NULL, 4, NULL);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        s_retry++;
        /* auto-reconnect keeps running; FAILED is only a UI hint after a long outage */
        s_state = (s_retry > 20) ? WIFI_APP_FAILED : WIFI_APP_CONNECTING;
        ESP_LOGW(TAG, "disconnected reason=%d retry=%d", d->reason, s_retry);
        if (s_ap_active) {
            /* In setup mode we stay in APSTA just for the config page. */
            return;
        }
        if (settings_get()->wifi_ssid[0] != '\0') {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_retry = 0;
        s_state = WIFI_APP_CONNECTED;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "got ip %s", s_ip);
        sntp_start();
    }
}

void wifi_app_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    if (settings_get()->wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "no Wi-Fi credentials, starting setup AP");
        wifi_app_ap_start();
        return;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    apply_sta_credentials();
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_started = true;
    ESP_LOGI(TAG, "Wi-Fi STA started, connecting to \"%s\"", settings_get()->wifi_ssid);
}

void wifi_app_ap_start(void)
{
    wifi_config_t ap = {0};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", AP_SSID);
    ap.ap.ssid_len = strlen(AP_SSID);
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    if (!s_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    }
    s_ap_active = true;
    ESP_LOGI(TAG, "setup AP \"%s\" up, open http://192.168.4.1", AP_SSID);
}

bool wifi_app_ap_active(void)
{
    return s_ap_active;
}

const char *wifi_app_ip(void)
{
    return s_ip;
}

wifi_app_state_t wifi_app_state(void)
{
    return s_state;
}

int wifi_app_rssi(void)
{
    if (s_state != WIFI_APP_CONNECTED) {
        return 0;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

bool wifi_app_time_synced(void)
{
    if (!s_time_synced && esp_sntp_enabled() &&
        esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        ESP_LOGI(TAG, "time synced: %04d-%02d-%02d %02d:%02d:%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
        s_time_synced = true;
    }
    return s_time_synced;
}
