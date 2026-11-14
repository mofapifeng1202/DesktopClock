#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

/*
 * Uploadable background picture.
 *
 * The web page converts the chosen JPEG/PNG in the browser to a raw 480x320
 * RGB565 bitmap, so the device needs no image decoder: the bytes are stored in
 * the "wallpaper" flash partition and copied into PSRAM at boot.
 */

#define WP_W 480
#define WP_H 320
#define WP_BYTES ((uint32_t)WP_W * WP_H * 2)

/* Find the partition and load a previously saved picture into PSRAM. */
void wallpaper_init(void);

/* NULL when no picture is stored. */
const lv_image_dsc_t *wallpaper_image(void);
bool wallpaper_present(void);

/* Store raw RGB565 (WP_BYTES long) and make it current. */
esp_err_t wallpaper_save(const void *rgb565, uint32_t len);

/* Forget the stored picture (falls back to the plain background colour). */
esp_err_t wallpaper_clear(void);
