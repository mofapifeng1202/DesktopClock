#pragma once
#include <stdbool.h>
#include <stdint.h>

#define WEATHER_DAILY_DAYS 3

/* Parsed Open-Meteo forecast for one city (hardcoded in weather.c) */
typedef struct {
    float temp;             /* °C */
    int humidity;           /* % */
    int code;               /* WMO weather code */
    float wind;             /* km/h */
    struct {
        char date[8];       /* "MM-DD" */
        int code;
        float tmax, tmin;   /* °C */
    } daily[WEATHER_DAILY_DAYS];
    bool valid;
} weather_data_t;

void weather_start(void);               /* spawn fetch task (30 min interval) */
const weather_data_t *weather_get(void);
uint32_t weather_version(void);         /* bump on every successful update */
