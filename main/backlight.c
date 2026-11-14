/*
 * Backlight PWM (GPIO41) + auto screen-off. See backlight.h.
 *
 * The panel backlight used to be a plain on/off GPIO; a 10-bit LEDC PWM at
 * 5 kHz gives smooth dimming. "Off" means duty 0 (screen dark) while the rest
 * of the system keeps running (audio keeps playing).
 */
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "backlight.h"
#include "settings.h"

static const char *TAG = "backlight";

#define PIN_LCD_BL      41
#define BL_LEDC_TIMER   LEDC_TIMER_0
#define BL_LEDC_CH      LEDC_CHANNEL_0
#define BL_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BL_LEDC_BITS    LEDC_TIMER_10_BIT
#define BL_LEDC_FREQ    5000

static bool s_on;
static int s_pct = 100;
static volatile int64_t s_last_act;

static uint32_t pct_to_duty(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return (uint32_t)((1023 * pct) / 100);
}

static void apply_duty(void)
{
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CH, s_on ? pct_to_duty(s_pct) : 0);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CH);
}

void backlight_init(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode = BL_LEDC_MODE,
        .duty_resolution = BL_LEDC_BITS,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = BL_LEDC_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tcfg);
    ledc_channel_config_t ccfg = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CH,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ccfg);

    s_pct = settings_get()->brightness;
    s_on = false;               /* dark until the panel is initialised */
    s_last_act = esp_timer_get_time();
    apply_duty();
    ESP_LOGI(TAG, "pwm ready, brightness %d%%", s_pct);
}

void backlight_set(int pct)
{
    if (pct < 5) pct = 5;
    if (pct > 100) pct = 100;
    s_pct = pct;
    apply_duty();
}

int backlight_get(void)
{
    return s_pct;
}

void backlight_on(void)
{
    s_on = true;
    s_last_act = esp_timer_get_time();
    apply_duty();
}

void backlight_off(void)
{
    s_on = false;
    apply_duty();
}

bool backlight_is_on(void)
{
    return s_on;
}

void backlight_activity(void)
{
    s_last_act = esp_timer_get_time();
    if (!s_on) {
        backlight_on();
        ESP_LOGI(TAG, "wake");
    }
}

static void sleep_task(void *arg)
{
    (void)arg;
    bool was_on = true;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        int sleep_min = settings_get()->sleep_min;
        if (sleep_min <= 0) {
            continue;
        }
        if (!s_on) {
            continue;
        }
        int64_t idle_s = (esp_timer_get_time() - s_last_act) / 1000000;
        if (idle_s >= sleep_min * 60) {
            backlight_off();
            ESP_LOGI(TAG, "screen off after %d min idle", sleep_min);
        }
        was_on = s_on;
        (void)was_on;
    }
}

void backlight_start_sleep_task(void)
{
    xTaskCreate(sleep_task, "bl_sleep", 3072, NULL, 1, NULL);
}
