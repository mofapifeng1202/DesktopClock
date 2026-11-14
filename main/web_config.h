#pragma once

#include <stdbool.h>

/*
 * Web configuration page.
 *
 * - Always available on the LAN once Wi-Fi is connected: http://<device-ip>/
 * - Also served from the setup AP ("DesktopClock-Setup", http://192.168.4.1)
 *   when the device has no credentials or the user enters setup mode from the
 *   settings page / the web page itself.
 *
 * Endpoints (JSON): /api/status, /api/scan, /api/wifi, /api/settings,
 * /api/restart, /api/factory, /api/apmode.
 */

void web_config_start(void);          /* idempotent; starts the http server */
void web_config_enter_ap_mode(void);  /* switch to setup AP (5 min, then reboot) */
bool web_config_ap_mode(void);
