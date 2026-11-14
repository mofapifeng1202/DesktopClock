#pragma once

#include <stdbool.h>

/* LCD backlight (GPIO41) PWM + auto screen-off. */

void backlight_init(void);        /* LEDC setup, screen stays dark */
void backlight_on(void);          /* restore the configured brightness */
void backlight_off(void);         /* screen off (sleep / manual) */
void backlight_set(int pct);      /* 5..100, takes effect immediately */
int backlight_get(void);          /* configured brightness pct */
bool backlight_is_on(void);

/* Reset the idle timer; wakes the screen when it was off. Also drives the
 * auto screen-off check (call from any user interaction). */
void backlight_activity(void);

/* Spawn the task that turns the screen off after settings_get()->sleep_min. */
void backlight_start_sleep_task(void);
