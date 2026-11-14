#pragma once
#include <stdbool.h>

/*
 * ---- Wi-Fi 凭据 ----
 * 真实账号密码放在 main/wifi_app_secret.h（已被 .gitignore 排除，不会进仓库）。
 * 首次开机使用该文件里的值作为默认；在设置页/网页里配置后会存入 NVS 并优先使用。
 * 未提供该文件时使用占位值，编译通过但连不上网，设备会进入配网热点模式。
 */
#if __has_include("wifi_app_secret.h")
#include "wifi_app_secret.h"
#else
#define WIFI_APP_SSID   "YOUR_SSID"
#define WIFI_APP_PASS   "YOUR_PASSWORD"
#endif

typedef enum {
    WIFI_APP_IDLE = 0,
    WIFI_APP_CONNECTING,
    WIFI_APP_CONNECTED,
    WIFI_APP_FAILED,
} wifi_app_state_t;

void wifi_app_start(void);
wifi_app_state_t wifi_app_state(void);
int wifi_app_rssi(void);            /* dBm of the current AP, 0 if unknown */
bool wifi_app_time_synced(void);    /* true once SNTP has synced */

/* Setup AP ("DesktopClock-Setup", open) used by the web config page. */
void wifi_app_ap_start(void);
bool wifi_app_ap_active(void);

/* Current STA IP ("192.168.x.x") or "" when not connected. */
const char *wifi_app_ip(void);
