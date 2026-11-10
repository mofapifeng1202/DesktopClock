#pragma once
#include <stdbool.h>

/*
 * ---- Wi-Fi 凭据 ----
 * 真实账号密码放在 main/wifi_app_secret.h（已被 .gitignore 排除，不会进仓库）。
 * 首次使用请复制模板：
 *     cp main/wifi_app_secret.h.example main/wifi_app_secret.h
 * 然后填入自己的 2.4GHz Wi-Fi（ESP32-S3 只支持 2.4GHz）。
 * 未提供该文件时使用下面的占位值，编译仍然通过但连不上网。
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
