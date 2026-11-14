#pragma once

#include <stdbool.h>

/*
 * Persistent system settings (NVS namespace "cfg").
 *
 * Values are loaded once at boot; setters update RAM immediately and mark the
 * blob dirty. settings_save() persists (called on slider release / web POST /
 * before restart) so dragging a slider does not hammer the flash.
 */

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char tv_url[193];
    int volume;        /* 0..100 */
    int brightness;    /* 5..100 */
    int sleep_min;     /* screen-off timeout, 0 = never */
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

/* Persist to NVS (no-op when nothing changed). */
void settings_save(void);

/* Wipe the namespace and restore defaults (in RAM; caller restarts). */
void settings_factory_reset(void);

/* "" when no credentials are stored. */
const char *settings_wifi_ssid(void);
