/*
 * Uploadable background picture. See wallpaper.h.
 *
 * Layout of the "wallpaper" data partition (see partitions.csv):
 *
 *   0x0000  header  { magic "BGF1", w, h, reserved }
 *   0x1000  raw RGB565 pixels, WP_W * WP_H * 2 = 307200 bytes
 *
 * The pixels are copied into PSRAM once at boot and described by an
 * lv_image_dsc_t, which the clock page uses as its bg_image_src. Writing the
 * partition takes ~1 s (76 sectors), so it only happens on upload.
 */
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"

#include "wallpaper.h"

static const char *TAG = "wallpaper";

#define WP_PART_LABEL  "wallpaper"
#define WP_MAGIC       0x31464742u   /* "BGF1" */
#define WP_DATA_OFS    0x1000

typedef struct {
    uint32_t magic;
    uint16_t w;
    uint16_t h;
    uint32_t reserved;
} wp_header_t;

static const esp_partition_t *s_part;
static uint16_t *s_pixels;           /* PSRAM, WP_W * WP_H */
static lv_image_dsc_t s_dsc;
static bool s_present;

static void dsc_init(void)
{
    if (s_pixels == NULL) {
        return;
    }
    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.w = WP_W;
    s_dsc.header.h = WP_H;
    s_dsc.header.stride = WP_W * 2;
    s_dsc.data_size = WP_BYTES;
    s_dsc.data = (const uint8_t *)s_pixels;
}

void wallpaper_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, WP_PART_LABEL);
    if (s_part == NULL) {
        ESP_LOGW(TAG, "no \"%s\" partition in the table, background upload disabled",
                 WP_PART_LABEL);
        return;
    }
    if (s_part->size < WP_DATA_OFS + WP_BYTES) {
        ESP_LOGW(TAG, "partition too small (%u bytes)", (unsigned)s_part->size);
        return;
    }
    s_pixels = heap_caps_malloc(WP_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_pixels == NULL) {
        ESP_LOGW(TAG, "no PSRAM for the background buffer");
        return;
    }
    dsc_init();

    wp_header_t hdr;
    if (esp_partition_read(s_part, 0, &hdr, sizeof(hdr)) != ESP_OK ||
        hdr.magic != WP_MAGIC || hdr.w != WP_W || hdr.h != WP_H) {
        ESP_LOGI(TAG, "no stored background");
        return;
    }
    if (esp_partition_read(s_part, WP_DATA_OFS, s_pixels, WP_BYTES) != ESP_OK) {
        ESP_LOGW(TAG, "reading the stored background failed");
        return;
    }
    s_present = true;
    ESP_LOGI(TAG, "background loaded from flash (%u bytes)", (unsigned)WP_BYTES);
}

const lv_image_dsc_t *wallpaper_image(void)
{
    return s_present ? &s_dsc : NULL;
}

bool wallpaper_present(void)
{
    return s_present;
}

esp_err_t wallpaper_save(const void *rgb565, uint32_t len)
{
    if (s_part == NULL || s_pixels == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (rgb565 == NULL || len != WP_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint32_t erase_len = WP_DATA_OFS + WP_BYTES;   /* 4 KB aligned */
    esp_err_t err = esp_partition_erase_range(s_part, 0, erase_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase failed: %s", esp_err_to_name(err));
        return err;
    }
    const wp_header_t hdr = {.magic = WP_MAGIC, .w = WP_W, .h = WP_H, .reserved = 0};
    err = esp_partition_write(s_part, 0, &hdr, sizeof(hdr));
    if (err == ESP_OK) {
        err = esp_partition_write(s_part, WP_DATA_OFS, rgb565, WP_BYTES);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write failed: %s", esp_err_to_name(err));
        return err;
    }
    /* Read the header back: a silent write failure would otherwise only show
     * up after the next reboot. */
    wp_header_t check;
    if (esp_partition_read(s_part, 0, &check, sizeof(check)) != ESP_OK ||
        check.magic != WP_MAGIC) {
        ESP_LOGE(TAG, "stored header did not verify");
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(s_pixels, rgb565, WP_BYTES);
    s_present = true;
    ESP_LOGI(TAG, "background stored and applied");
    return ESP_OK;
}

esp_err_t wallpaper_clear(void)
{
    s_present = false;
    if (s_part == NULL) {
        return ESP_OK;
    }
    const wp_header_t zero = {0};
    esp_err_t err = ESP_ERR_INVALID_STATE;
    for (int attempt = 0; attempt < 2; attempt++) {
        err = esp_partition_erase_range(s_part, 0, WP_DATA_OFS);
        if (err == ESP_OK) {
            err = esp_partition_write(s_part, 0, &zero, sizeof(zero));
        }
        wp_header_t check;
        if (err == ESP_OK &&
            esp_partition_read(s_part, 0, &check, sizeof(check)) == ESP_OK &&
            check.magic != WP_MAGIC) {
            ESP_LOGI(TAG, "background cleared");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "clear attempt %d failed (%s), retrying", attempt + 1,
                 esp_err_to_name(err));
    }
    return err;
}
