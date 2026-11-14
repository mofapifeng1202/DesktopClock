#pragma once
#include <stdbool.h>
#include <stdint.h>

#define WEATHER_DAILY_DAYS 3

/*
 * Current conditions + a short forecast, from one of the sources in settings.h
 * (WX_SRC_CN / WX_SRC_OPENMETEO / WX_SRC_WTTR).
 *
 * `desc` is the condition text as the source reported it (Chinese for the CN
 * source); `code` is the WMO code when the source provides one (Open-Meteo),
 * otherwise -1 and the UI falls back to `desc`.
 */
typedef struct {
    float temp;             /* °C */
    int humidity;           /* % */
    int code;               /* WMO weather code, -1 when unknown */
    float wind;             /* km/h, -1 when unknown */
    char desc[24];          /* condition text, e.g. "霾" */
    char city[24];          /* display name of the configured city */
    struct {
        char date[8];       /* "MM-DD" */
        char desc[16];      /* condition text */
        int code;
        float tmax, tmin;   /* °C */
    } daily[WEATHER_DAILY_DAYS];
    bool valid;
} weather_data_t;

void weather_start(void);               /* spawn fetch task (30 min interval) */
const weather_data_t *weather_get(void);
uint32_t weather_version(void);         /* bump on every successful update */

/* Drop the current data and fetch again soon (called after settings change). */
void weather_refresh(void);
