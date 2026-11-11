/*
 * ESP32-S3 (N16R8) + LCDWiki ES3C35P 3.5" 320x480 IPS (ST77922, QSPI)
 *
 * Desktop clock / info station:
 *   - Big NTP-synced clock page + weather page (Open-Meteo), swipe to switch
 *   - Status bar: battery, Wi-Fi state; volume slider on the clock page
 *   - BOOT button toggles the LCD backlight, RGB LED shows Wi-Fi status
 *
 * Wiring used by this board:
 *   LCD CS    -> IO10      LCD PCLK -> IO12
 *   LCD D0    -> IO11      LCD D1   -> IO13
 *   LCD D2    -> IO14      LCD D3   -> IO9
 *   LCD RST   -> board EN (not a GPIO)
 *   LCD BL    -> IO41 (active high)
 *   TP  SDA   -> IO38      TP SCL   -> IO39
 *   TP  RST   -> IO48      TP INT   -> IO47
 *   Audio EN  -> IO1 (active low)
 *   I2S MCLK  -> IO17      I2S BCLK -> IO18      I2S WS -> IO21
 *   I2S DOUT  -> IO15      I2S DIN  -> IO16
 *   RGB LED   -> IO40      BOOT btn -> IO0       Battery ADC -> IO8 (1:2)
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77922.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_st7123.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "led_strip.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "st77922_vendor_init.h"

#include "clock_ui.h"
#include "main.h"
#include "media_list.h"
#include "radio.h"
#include "wifi_app.h"
#include "weather.h"

static const char *TAG = "st77922";

/* Bring-up sequence: 1 = auto-play stations after Wi-Fi connects.
 * Kept at 0 for normal use (the radio page waits for a tap). */
#define RADIO_SELFTEST 0

#define LCD_HOST        SPI2_HOST
/* Landscape 480x320 is done with LVGL **software rotation**: driving this panel through
 * MADCTL.MV (hardware rotation) produces a scrambled image, which is exactly why the
 * esp_lcd_st77922 driver keeps MV as an opt-in "validated modules only" flag.
 * So the panel stays in its native (validated) portrait mode and LVGL rotates the
 * rendered frame on flush. Cost: one extra memcpy-ish pass per flushed area. */
#define LCD_PANEL_H_RES 320
#define LCD_PANEL_V_RES 480
#define LANDSCAPE_ROTATION  LV_DISPLAY_ROTATION_90

#define LCD_H_RES       480            /* logical / rotated resolution */
#define LCD_V_RES       320
#define LCD_BUF_LINES   16

/* Touch transform: NONE. LVGL already rotates indev input for a rotated display
 * (lv_indev.c -> lv_display_rotate_point(): X = ver_res-1-y, Y = x) and it expects the
 * driver to report the panel's native coordinates (x:0..319, y:0..479). Swapping here as
 * well double-rotates and leaves only part of the screen reachable.
 * Verified against measured corner samples: raw (9,443) -> LVGL (36,9) top-left,
 * (56,44) -> (435,56) top-right, (284,433) -> (46,284) bottom-left,
 * (290,31) -> (448,290) bottom-right. */
#define TP_LAND_SWAP     0
#define TP_LAND_MIRROR_X 0
#define TP_LAND_MIRROR_Y 0

#define PIN_LCD_CS      10
#define PIN_LCD_PCLK    12
#define PIN_LCD_D0      11
#define PIN_LCD_D1      13
#define PIN_LCD_D2      14
#define PIN_LCD_D3      9
#define PIN_LCD_BL      41
#define PIN_LCD_RST     (-1)   /* tied to board EN */

#define PIN_TP_SDA      38
#define PIN_TP_SCL      39
#define PIN_TP_RST      48
#define PIN_TP_INT      47

#define PIN_AUDIO_EN    1
#define PIN_I2S_MCLK    17
#define PIN_I2S_BCLK    18
#define PIN_I2S_WS      21
#define PIN_I2S_DOUT    15
#define PIN_I2S_DIN     16

#define PIN_RGB_LED     40
#define PIN_BOOT_BTN    0

/* 触摸诊断：每秒打印 INT 电平与控制器上报点数。正常使用置 0，
 * 排查触摸问题时再打开。 */
#define TP_PROBE        0

#define MIC_SAMPLE_RATE 16000

static i2c_master_bus_handle_t s_i2c_bus;
static esp_codec_dev_handle_t s_codec;
static SemaphoreHandle_t s_audio_lock;
static esp_lcd_touch_handle_t s_tp;
static led_strip_handle_t s_led;
static int s_volume = 70;
static int s_audio_rate = MIC_SAMPLE_RATE;
static int s_audio_ch = 2;

/* 触摸校准诊断：记录最近一次按下的原始/映射坐标，由 boot_btn_task 心跳抛出。
 * （不要在别的任务里主动读触摸，会把 LVGL 要用的点先消费掉） */
static volatile uint16_t s_dbg_raw_x, s_dbg_raw_y, s_dbg_map_x, s_dbg_map_y;
static volatile uint32_t s_dbg_count;
static bool s_bl_on = true;

/* ------------------------- Battery (ADC1_CH7) ------------------------ */

static adc_oneshot_unit_handle_t s_bat_adc;
static adc_cali_handle_t s_bat_cali;
static float s_bat_v;               /* smoothed battery voltage */
static bool s_bat_v_init;
static int s_bat_log;

static void battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&unit_cfg, &s_bat_adc) != ESP_OK) {
        return;
    }
    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_oneshot_config_channel(s_bat_adc, ADC_CHANNEL_7, &chan_cfg);

    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_7,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_bat_cali);
}

/* Returns battery voltage in volts, or -1 if the ADC is unavailable.
 * The board divides BAT+ by 2 with two 100K resistors (schematic R14/R15). */
float battery_volts(void)
{
    if (s_bat_adc == NULL || s_bat_cali == NULL) {
        return -1.0f;
    }
    int raw = 0;
    int sum = 0;
    /* Discard the first reads after idle (sample cap charge-sharing bias),
     * then average 16 samples to reject noise and Wi-Fi load spikes. */
    for (int i = 0; i < 20; i++) {
        if (adc_oneshot_read(s_bat_adc, ADC_CHANNEL_7, &raw) != ESP_OK) {
            return -1.0f;
        }
        if (i >= 4) {
            sum += raw;
        }
    }
    raw = sum / 16;
    int mv = 0;
    adc_cali_raw_to_voltage(s_bat_cali, raw, &mv);
    float v = (float)mv * 2.0f / 1000.0f;

    /* Exponential smoothing across calls (every 5 s) to hide load sag dips */
    if (s_bat_v_init) {
        s_bat_v = s_bat_v * 0.7f + v * 0.3f;
    } else {
        s_bat_v = v;
        s_bat_v_init = true;
    }

    s_bat_log++;
    if (s_bat_log >= 6) {   /* one raw diagnostic line every ~30 s */
        s_bat_log = 0;
        ESP_LOGI(TAG, "battery: raw=%d mv=%d smoothed=%.2fV", raw, mv, s_bat_v);
    }
    return s_bat_v;
}

#define BAT_SAMPLES 12
static const float bat_soc_v[BAT_SAMPLES] = {
    4.20f, 4.11f, 4.03f, 3.97f, 3.91f, 3.86f,
    3.81f, 3.77f, 3.73f, 3.68f, 3.60f, 3.40f,
};
static const int bat_soc_p[BAT_SAMPLES] = {
    100, 90, 80, 70, 60, 50, 40, 30, 20, 10, 5, 0,
};

/* Li-ion resting OCV curve, piecewise linear between table points */
int battery_percent(void)
{
    float v = battery_volts();
    if (v <= 0) {
        return -1;
    }
    if (v >= bat_soc_v[0]) {
        return 100;
    }
    for (int i = 0; i < BAT_SAMPLES - 1; i++) {
        if (v >= bat_soc_v[i + 1]) {
            float t = (v - bat_soc_v[i]) / (bat_soc_v[i + 1] - bat_soc_v[i]);
            return (int)(bat_soc_p[i] + t * (bat_soc_p[i + 1] - bat_soc_p[i]));
        }
    }
    return 0;
}

/* ------------------------------ RGB LED ------------------------------ */

static void rgb_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = PIN_RGB_LED,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    if (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_led) != ESP_OK) {
        ESP_LOGW(TAG, "RGB LED init failed");
        s_led = NULL;
        return;
    }
    led_strip_clear(s_led);
}

void rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_led == NULL) {
        return;
    }
    led_strip_set_pixel(s_led, 0, r, g, b);
    led_strip_refresh(s_led);
}

/* ------------------------------- Audio ------------------------------- */

static esp_err_t audio_init(void)
{
    gpio_config_t en_cfg = {.mode = GPIO_MODE_OUTPUT, .pin_bit_mask = 1ULL << PIN_AUDIO_EN};
    ESP_RETURN_ON_ERROR(gpio_config(&en_cfg), TAG, "audio en gpio");
    gpio_set_level(PIN_AUDIO_EN, 0);   /* active low */

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_chan_handle_t tx = NULL;
    i2s_chan_handle_t rx = NULL;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s new channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = PIN_I2S_DIN,
            .invert_flags = {false, false, false},
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx init");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx init");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx,
        .tx_handle = tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(data_if != NULL, ESP_FAIL, TAG, "codec data if");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if != NULL, ESP_FAIL, TAG, "codec ctrl if");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if != NULL, ESP_FAIL, TAG, "codec gpio if");

    es8311_codec_cfg_t codec_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = -1,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .no_dac_ref = true,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&codec_cfg);
    ESP_RETURN_ON_FALSE(codec_if != NULL, ESP_FAIL, TAG, "es8311 new");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec != NULL, ESP_FAIL, TAG, "codec dev new");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0,
        .sample_rate = MIC_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    esp_err_t err = esp_codec_dev_open(s_codec, &fs);
    ESP_RETURN_ON_FALSE(err == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "codec open %d", err);
    esp_codec_dev_set_out_vol(s_codec, s_volume);
    s_audio_rate = MIC_SAMPLE_RATE;
    s_audio_ch = 2;

    s_audio_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_audio_lock != NULL, ESP_FAIL, TAG, "audio mutex");
    return ESP_OK;
}

void audio_set_vol(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    s_volume = vol;
    if (s_codec == NULL) {
        return;
    }
    if (s_audio_lock != NULL) {
        if (xSemaphoreTake(s_audio_lock, portMAX_DELAY) == pdTRUE) {
            esp_codec_dev_set_out_vol(s_codec, vol);
            xSemaphoreGive(s_audio_lock);
        }
    } else {
        esp_codec_dev_set_out_vol(s_codec, vol);
    }
}

int audio_get_vol(void)
{
    return s_volume;
}

bool audio_is_ready(void)
{
    return s_codec != NULL && s_audio_lock != NULL;
}

/* Reopen the codec at a new MP3 sample rate. The ES8311 + I2S data interface
 * reconfigure themselves inside esp_codec_dev_open() when the format changes. */
esp_err_t audio_stream_open(int sample_rate, int channels)
{
    if (sample_rate < 8000 || sample_rate > 48000 || channels != 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!audio_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_rate == s_audio_rate && channels == s_audio_ch) {
        return ESP_OK;
    }
    if (xSemaphoreTake(s_audio_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_codec_dev_close(s_codec);
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = channels,
        .channel_mask = 0,
        .sample_rate = sample_rate,
        .mclk_multiple = 0,
    };
    int err = esp_codec_dev_open(s_codec, &fs);
    if (err == ESP_CODEC_DEV_OK) {
        s_audio_rate = sample_rate;
        s_audio_ch = channels;
        esp_codec_dev_set_out_vol(s_codec, s_volume);
        ESP_LOGI(TAG, "audio stream: %d Hz, %d ch", sample_rate, channels);
    } else {
        ESP_LOGE(TAG, "audio stream reopen %d Hz failed: %d", sample_rate, err);
        /* Try to get back to the previous rate so the UI/chime still works. */
        esp_codec_dev_sample_info_t old_fs = {
            .bits_per_sample = 16,
            .channel = s_audio_ch,
            .channel_mask = 0,
            .sample_rate = s_audio_rate,
            .mclk_multiple = 0,
        };
        if (esp_codec_dev_open(s_codec, &old_fs) == ESP_CODEC_DEV_OK) {
            esp_codec_dev_set_out_vol(s_codec, s_volume);
        }
    }
    xSemaphoreGive(s_audio_lock);
    return err == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

int audio_stream_write(const void *data, size_t len)
{
    if (!audio_is_ready() || data == NULL || len == 0) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    if (xSemaphoreTake(s_audio_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_CODEC_DEV_TIMEOUT;
    }
    int ret = esp_codec_dev_write(s_codec, (void *)data, len);
    xSemaphoreGive(s_audio_lock);
    return ret;
}

void audio_stream_silence(int ms)
{
    if (!audio_is_ready() || ms <= 0) {
        return;
    }
    static int16_t silence[512];   /* 256 stereo frames */
    int frames = (s_audio_rate * ms) / 1000;
    if (frames <= 0) {
        frames = 1;
    }
    if (xSemaphoreTake(s_audio_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    while (frames > 0) {
        int n = frames > 256 ? 256 : frames;
        esp_codec_dev_write(s_codec, silence, n * 4);
        frames -= n;
    }
    xSemaphoreGive(s_audio_lock);
}

int audio_stream_rate(void)
{
    return s_audio_rate;
}

static void speaker_chime(uint32_t ms)
{
    if (s_codec == NULL) {
        return;
    }
    static int16_t tone[512];
    for (int i = 0; i < 256; i++) {
        int16_t v = (int16_t)(9000.0f * sinf(2.0f * 3.14159265f * 660.0f * i / MIC_SAMPLE_RATE));
        tone[2 * i] = v;
        tone[2 * i + 1] = v;
    }
    if (xSemaphoreTake(s_audio_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    uint32_t frames = ms * MIC_SAMPLE_RATE / 1000;
    while (frames > 0) {
        uint32_t n = frames > 256 ? 256 : frames;
        esp_codec_dev_write(s_codec, tone, n * 4);
        frames -= n;
    }
    /* I2S repeats the last DMA content when idle, so end with silence */
    static int16_t silence[512];
    memset(silence, 0, sizeof(silence));
    uint32_t silent_frames = 320 * MIC_SAMPLE_RATE / 1000;
    while (silent_frames > 0) {
        uint32_t n = silent_frames > 256 ? 256 : silent_frames;
        esp_codec_dev_write(s_codec, silence, n * 4);
        silent_frames -= n;
    }
    xSemaphoreGive(s_audio_lock);
}

/* ------------------------- BOOT = backlight toggle ------------------- */

static void boot_btn_task(void *arg)
{
    (void)arg;
    gpio_config_t cfg = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << PIN_BOOT_BTN,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);

    int last = 1;
    int tick = 0;
    while (1) {
        int level = gpio_get_level(PIN_BOOT_BTN);
        if (level != last) {
            last = level;
            if (level == 0) {
                s_bl_on = !s_bl_on;
                gpio_set_level(PIN_LCD_BL, s_bl_on ? 1 : 0);
                ESP_LOGI(TAG, "backlight %s", s_bl_on ? "on" : "off");
            }
        }
        /* Touch calibration heartbeat: never touches the controller itself (reading
         * here would consume the points LVGL needs), just reports the last press that
         * the indev callback recorded. */
        if (TP_PROBE && ++tick % 20 == 0) {
            static uint32_t seen = 0;
            if (s_dbg_count != seen) {
                seen = s_dbg_count;
                ESP_LOGI(TAG, "press#%u raw=%d,%d mapped=%d,%d INT=%d",
                         (unsigned)s_dbg_count, s_dbg_raw_x, s_dbg_raw_y,
                         s_dbg_map_x, s_dbg_map_y, gpio_get_level(PIN_TP_INT));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* Watches the 1 s LVGL heartbeat. If the screen freezes while the radio task
 * keeps running, this prints a clear "UI STALL" line with heap/stack data. */
static void ui_monitor_task(void *arg)
{
    (void)arg;
    uint32_t last_hb = g_ui_heartbeat;
    int64_t last_change = esp_timer_get_time();
    bool logged = false;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        uint32_t hb = g_ui_heartbeat;
        int64_t now = esp_timer_get_time();
        if (hb != last_hb) {
            last_hb = hb;
            last_change = now;
            logged = false;
            continue;
        }
        int64_t stalled_ms = (now - last_change) / 1000;
        if (stalled_ms >= 5000 && !logged) {
            logged = true;
            TaskHandle_t lvgl = xTaskGetHandle("taskLVGL");
            UBaseType_t stack_free = lvgl ? uxTaskGetStackHighWaterMark(lvgl) : 0;
            ESP_LOGW(TAG, "UI STALL %lld ms: int heap %u, psram %u, lvgl stack free %u",
                     (long long)stalled_ms,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)stack_free);
        }
        if (stalled_ms >= 10000) {
            ESP_LOGE(TAG, "UI stalled for %lld ms, restarting to recover",
                     (long long)stalled_ms);
            esp_restart();
        }
    }
}

#if RADIO_SELFTEST
static void radio_selftest_task(void *arg)
{
    (void)arg;
    while (wifi_app_state() != WIFI_APP_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "SELFTEST: wifi up");

    /* Wait for the IPTV playlist (loaded async after radio_init). */
    int wait = 0;
    while (media_list_state() != MEDIA_LIST_OK && wait < 20) {
        ESP_LOGI(TAG, "SELFTEST: playlist state=%d", (int)media_list_state());
        vTaskDelay(pdMS_TO_TICKS(1000));
        wait++;
    }
    ESP_LOGI(TAG, "SELFTEST: playlist %d channels, radio in 3 s",
             media_list_count());
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "SELFTEST: radio preset 0 (MP3 path regression)");
    radio_play(0);
    vTaskDelay(pdMS_TO_TICKS(10000));

    if (media_list_count() > 0) {
        ESP_LOGI(TAG, "SELFTEST: TV ch 0 (%s)", media_list_url(0));
        radio_play_tv(0);
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
    if (media_list_count() > 1) {
        ESP_LOGI(TAG, "SELFTEST: TV ch 1 (%s)", media_list_url(1));
        radio_play_tv(1);
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
    if (media_list_count() > 2) {
        ESP_LOGI(TAG, "SELFTEST: TV ch 2 (udp, expect error)");
        radio_play_tv(2);
        vTaskDelay(pdMS_TO_TICKS(6000));
    }

    ESP_LOGI(TAG, "SELFTEST: stop");
    radio_stop();
    vTaskDelete(NULL);
}
#endif

/* ------------------------------- LCD / LVGL -------------------------- */

static esp_err_t i2c_bus_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_TP_SDA,
        .scl_io_num = PIN_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
}

static esp_err_t touch_init(esp_lcd_touch_handle_t *tp_out)
{
    gpio_config_t rst_cfg = {.mode = GPIO_MODE_OUTPUT, .pin_bit_mask = 1ULL << PIN_TP_RST};
    gpio_config(&rst_cfg);
    /* The controller needs a long settle time before it answers on I2C.
     * The driver's built-in reset only waits 30 ms, which causes NACKs, so
     * the reset is done here and the pin is passed to the driver as GPIO_NUM_NC. */
    gpio_set_level(PIN_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(300));

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_ST7123_CONFIG();
    tp_io_cfg.scl_speed_hz = 100000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c_v2(s_i2c_bus, &tp_io_cfg, &tp_io), TAG, "touch io");

    /* Touch raw axes stay the panel's native portrait 320x480 (ST7123 reports
     * Max.X=320 / Max.Y=480); mirroring is done in that raw space, then the axes
     * are swapped, so these are NOT the LVGL landscape dimensions. */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = 320,
        .y_max = 480,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = PIN_TP_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            /* Raw coordinates are kept as-is; the landscape mapping is applied in
             * tp_indev_read_cb (TP_LAND_* defines) so touch and display live in one place. */
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    return esp_lcd_touch_new_i2c_st7123(tp_io, &tp_cfg, tp_out);
}

/* ST77922 requires the CASET window (panel X) to start/end on multiples of 4.
 * With software rotation 90/270 the panel X axis is the logical Y axis, so the
 * rounding has to follow the rotation — otherwise every flushed strip is
 * misaligned and the widgets come out garbled. */
static void lvgl_rounder_cb(lv_area_t *area)
{
#if (LANDSCAPE_ROTATION == LV_DISPLAY_ROTATION_90) || (LANDSCAPE_ROTATION == LV_DISPLAY_ROTATION_270)
    area->y1 = (area->y1 >> 2) << 2;
    area->y2 = ((area->y2 >> 2) << 2) + 3;
#else
    area->x1 = (area->x1 >> 2) << 2;
    area->x2 = ((area->x2 >> 2) << 2) + 3;
#endif
}

static void tp_indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    /* The ST7123 driver configures no thresholds and the panel is electrically noisy:
     * real measurements show ~40 large teleports per minute (e.g. (292,258) -> (5,354)),
     * i.e. ghost contacts and/or corrupted-but-valid I2C reads. Strategy:
     *   - keep the contact with the largest area (a real finger beats a ghost)
     *   - clamp per-poll movement instead of rejecting frames, so a swipe keeps its
     *     direction and velocity while ghosts are cut down to a small step
     *   - report the release immediately (any hold-off destroys LVGL's flick velocity)
     *
     * The limit is now time-based, not a fixed 60 px: 3000 px/s at the measured
     * interval. This matters because the LVGL task can be delayed by radio work;
     * a fixed 60 px per poll would then turn a normal flick into a slow drag.
     * Ghost teleports are hundreds of px in one interval, so they are still cut. */
#define TP_MAX_POINTS     5
#define TP_MAX_SPEED_PX_S 3000
#define TP_MIN_STEP       30
#define TP_MAX_STEP_CAP   240
#define TP_DEBUG_COORDS   0   /* 打印每次按下的落点，用于横屏坐标映射校准；正常使用置 0 */

    static int last_x, last_y;
    static bool have_last;
    static int64_t last_us;
    static uint32_t clipped_total, clipped_logged;

    data->state = LV_INDEV_STATE_RELEASED;
    if (s_tp == NULL) {
        return;
    }

    uint16_t x[TP_MAX_POINTS] = {0};
    uint16_t y[TP_MAX_POINTS] = {0};
    uint16_t strength[TP_MAX_POINTS] = {0};
    uint8_t cnt = 0;
    esp_lcd_touch_read_data(s_tp);
    /* returns true when at least one point is valid (bool, not esp_err_t) */
    if (!esp_lcd_touch_get_coordinates(s_tp, x, y, strength, &cnt, TP_MAX_POINTS)) {
        cnt = 0;
    }
    if (cnt == 0) {
        have_last = false;   /* finger lifted: next touch starts a fresh reference */
        return;
    }

    int best = 0;
    for (int i = 1; i < cnt; i++) {
        if (strength[i] > strength[best]) {
            best = i;
        }
    }
    int nx = x[best];
    int ny = y[best];

#if TP_LAND_SWAP
    int tmp = nx;
    nx = ny;              /* 0..479 across the landscape screen */
    ny = tmp;             /* 0..319 down the landscape screen */
#endif
#if TP_LAND_MIRROR_X
    nx = (LCD_H_RES - 1) - nx;
#endif
#if TP_LAND_MIRROR_Y
    ny = (LCD_V_RES - 1) - ny;
#endif

    int64_t now = esp_timer_get_time();
    if (!have_last) {
        last_us = now;
#if TP_DEBUG_COORDS
        /* Orientation calibration: remember the press, boot_btn_task reports it. */
        s_dbg_raw_x = x[best];
        s_dbg_raw_y = y[best];
        s_dbg_map_x = nx;
        s_dbg_map_y = ny;
        s_dbg_count++;
#endif
    }

    if (have_last) {
        int64_t dt_us = now - last_us;
        /* The timer can be delayed by radio/TLS work; bound the interval so a
         * long stall does not open a huge ghost window. */
        if (dt_us < 5000) dt_us = 5000;
        if (dt_us > 100000) dt_us = 100000;
        int max_step = (int)((int64_t)TP_MAX_SPEED_PX_S * dt_us / 1000000);
        if (max_step < TP_MIN_STEP) max_step = TP_MIN_STEP;
        if (max_step > TP_MAX_STEP_CAP) max_step = TP_MAX_STEP_CAP;

        int dx = nx - last_x;
        int dy = ny - last_y;
        if (dx > max_step || dx < -max_step || dy > max_step || dy < -max_step) {
            /* implausible step: treat as ghost, move at most max_step pixels */
            if (dx > max_step) dx = max_step;
            if (dx < -max_step) dx = -max_step;
            if (dy > max_step) dy = max_step;
            if (dy < -max_step) dy = -max_step;
            nx = last_x + dx;
            ny = last_y + dy;
            clipped_total++;
            if (clipped_total - clipped_logged >= 10) {
                clipped_logged = clipped_total;
                ESP_LOGI(TAG, "touch ghost clipped (%u total, max_step=%d): %d,%d",
                         (unsigned)clipped_total, max_step, nx, ny);
            }
        }
    }

    last_x = nx;
    last_y = ny;
    last_us = now;
    have_last = true;
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = nx;
    data->point.y = ny;
}

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external-pin";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt-wdt";
    case ESP_RST_TASK_WDT:  return "task-wdt";
    case ESP_RST_WDT:       return "other-wdt";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    case ESP_RST_USB:       return "usb";
    case ESP_RST_JTAG:      return "jtag";
    case ESP_RST_EFUSE:     return "efuse";
    case ESP_RST_PWR_GLITCH:return "power-glitch";
    case ESP_RST_CPU_LOCKUP:return "cpu-lockup";
    default:                return "unknown";
    }
}

void app_main(void)
{
    esp_reset_reason_t rr = esp_reset_reason();
    ESP_LOGI(TAG, "reset reason: %d (%s)", (int)rr, reset_reason_name(rr));

    /* Backlight off until the panel is initialized */
    gpio_config_t bl_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    gpio_set_level(PIN_LCD_BL, 0);

    ESP_LOGI(TAG, "Initialize QSPI bus");
    const spi_bus_config_t buscfg = ST77922_PANEL_BUS_QSPI_CONFIG(
        PIN_LCD_PCLK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3,
        LCD_H_RES * LCD_BUF_LINES * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    const esp_lcd_panel_io_spi_config_t io_config =
        ST77922_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, NULL, NULL);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    ESP_LOGI(TAG, "Install ST77922 driver");
    st77922_vendor_config_t vendor_config = {
        .init_cmds = vendor_init_cmds,
        .init_cmds_size = sizeof(vendor_init_cmds) / sizeof(vendor_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
            /* MADCTL.MV (hardware rotation) stays disabled: it scrambles this module. */
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st77922(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    gpio_set_level(PIN_LCD_BL, 1);

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    /* Keep UI/touch above the radio tasks (play=5, net=4). The old priority 4
     * let a TLS handshake or a long MP3 decode burst starve the LVGL task,
     * which looks exactly like a frozen screen and a dead touch panel.
     * The extra stack covers the radio page's label formatting. */
    port_cfg.task_priority = 6;
    port_cfg.task_stack = 8192;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = LCD_H_RES * LCD_BUF_LINES,
        .double_buffer = true,
        .hres = LCD_PANEL_H_RES,
        .vres = LCD_PANEL_V_RES,
        .monochrome = false,
        .rounder_cb = lvgl_rounder_cb,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,  /* SPI DMA cannot read PSRAM buffers */
            .swap_bytes = true,
            .sw_rotate = true,     /* landscape by software rotation */
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return;
    }
    lv_display_set_default(disp);
    /* Switch the logical resolution to 480x320; with sw_rotate the panel keeps its
     * native portrait MADCTL and LVGL rotates each flushed area. */
    lv_display_set_rotation(disp, LANDSCAPE_ROTATION);
    ESP_LOGI(TAG, "display %dx%d (panel %dx%d, sw rotation %d)",
             (int)lv_display_get_horizontal_resolution(disp),
             (int)lv_display_get_vertical_resolution(disp),
             LCD_PANEL_H_RES, LCD_PANEL_V_RES, (int)LANDSCAPE_ROTATION);

    ESP_ERROR_CHECK(i2c_bus_init());

    ESP_LOGI(TAG, "Initialize ST7123 touch");
    esp_lcd_touch_handle_t tp = NULL;
    if (touch_init(&tp) != ESP_OK) {
        ESP_LOGE(TAG, "touch init failed, continuing without touch");
    } else {
        s_tp = tp;
    }

    nvs_init();
    radio_init();
    battery_init();
    rgb_init();

    if (lvgl_port_lock(0)) {
        clock_ui_build();

        lv_indev_t *tp_indev = lv_indev_create();
        lv_indev_set_type(tp_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(tp_indev, tp_indev_read_cb);
        lv_indev_set_display(tp_indev, disp);
        /* Poll touch at 50 Hz instead of the default 30 Hz. The ST7123 driver
         * reads all 5 reports (~3 ms on the 100 kHz bus), so 15 ms would spend
         * too much of the LVGL task in I2C; 20 ms is a good compromise. */
        lv_timer_t *tp_timer = lv_indev_get_read_timer(tp_indev);
        if (tp_timer) {
            lv_timer_set_period(tp_timer, 20);
        }
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "Initialize ES8311 codec (speaker + volume)");
    if (audio_init() == ESP_OK) {
        speaker_chime(250);
    } else {
        ESP_LOGE(TAG, "audio init failed");
    }

    xTaskCreate(boot_btn_task, "boot_btn", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "Start Wi-Fi + weather + radio");
    wifi_app_start();
    weather_start();
    radio_start();
    xTaskCreate(ui_monitor_task, "ui_mon", 3072, NULL, 1, NULL);
#if RADIO_SELFTEST
    xTaskCreate(radio_selftest_task, "radio_test", 4096, NULL, 3, NULL);
#endif
}
