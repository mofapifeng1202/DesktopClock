/*
 * Network radio: HTTP(S) MP3 stream -> libhelix MP3 decoder -> ES8311/I2S.
 *
 * Design notes:
 *   - Two tasks. The network task must never block the audio path, otherwise
 *     a slow Wi-Fi read shows up as an I2S underrun. It fills a 128 KB PSRAM
 *     ring buffer; the player task drains it and blocks on I2S writes.
 *   - The decoder needs whole MP3 frames. We parse the frame header ourselves
 *     (bitrate/sample-rate/padding) and wait until the full frame is buffered
 *     before calling MP3Decode(), so libhelix never sees a partial frame.
 *   - Icy metadata is explicitly disabled: the Chinese qtfm streams send
 *     StreamTitle blocks only when the client asks for them.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "mp3dec.h"

#include "main.h"
#include "radio.h"
#include "radio_stations.h"
#include "spectrum.h"
#include "wifi_app.h"

static const char *TAG = "radio";

#define RADIO_STATION_COUNT ((int)(sizeof(k_radio_stations) / sizeof(k_radio_stations[0])))

#define RING_CAP          (128 * 1024)
#define MP3_IN_CAP        (16 * 1024)
#define PREBUFFER_BYTES   (24 * 1024)
#define READ_CHUNK        4096
#define HTTP_CONNECT_MS   8000
#define HTTP_READ_MS      200
#define NET_STACK         10240
#define PLAY_STACK        8192
/* UI/touch runs at priority 6. Keep both radio tasks below it: a TLS
 * handshake or a decode burst must never starve the LVGL task. The 128 KB
 * ring buffer absorbs the resulting short scheduling delays. */
#define NET_PRIO          4
#define PLAY_PRIO         5

typedef enum {
    CMD_PLAY = 0,
    CMD_PAUSE,
    CMD_NEXT,
    CMD_PREV,
    CMD_STOP,
} radio_cmd_t;

typedef struct {
    radio_cmd_t cmd;
    int arg;
} radio_msg_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;
static RingbufHandle_t s_ring;
static radio_status_t s_status;

static volatile bool s_stop_req;
static volatile uint32_t s_stream_counter;
static uint32_t s_seen_stream_id;

static HMP3Decoder s_decoder;
static uint8_t s_mp3_in[MP3_IN_CAP];
static int s_in_len;
static int s_in_pos;
static int16_t s_pcm[2 * 1152];
static int s_cur_rate;
static int s_cur_ch;
static int s_bad_frames;
static int s_underflow_retries;

static int s_level;
static int64_t s_last_level_us;
static int64_t s_last_log_us;
static uint32_t s_frames;
static uint32_t s_underruns;
static uint32_t s_skips;

/* ----------------------------- status ----------------------------- */

static void status_lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void status_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static radio_state_t get_state(void)
{
    status_lock();
    radio_state_t st = s_status.state;
    status_unlock();
    return st;
}

static void set_state(radio_state_t state, const char *error)
{
    status_lock();
    s_status.state = state;
    if (error && error[0]) {
        strlcpy(s_status.error, error, sizeof(s_status.error));
    } else if (state != RADIO_STATE_ERROR) {
        s_status.error[0] = '\0';
    }
    if (state != RADIO_STATE_PLAYING) {
        s_status.buffering = false;
    }
    status_unlock();
}

static void set_error(const char *fmt, ...)
{
    char buf[sizeof(((radio_status_t *)0)->error)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOGE(TAG, "%s", buf);
    set_state(RADIO_STATE_ERROR, buf);
}

static void set_station(int idx)
{
    status_lock();
    s_status.station = idx;
    status_unlock();
}

static void set_info(int bitrate_kbps, int sample_rate, int channels)
{
    bool changed = false;
    status_lock();
    if (s_status.bitrate_kbps != bitrate_kbps ||
        s_status.sample_rate != sample_rate ||
        s_status.channels != channels) {
        s_status.bitrate_kbps = bitrate_kbps;
        s_status.sample_rate = sample_rate;
        s_status.channels = channels;
        changed = true;
    }
    status_unlock();
    if (changed && sample_rate > 0) {
        ESP_LOGI(TAG, "stream: %d kbps, %d Hz, %d ch", bitrate_kbps, sample_rate, channels);
    }
}

static void set_buffer(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    status_lock();
    s_status.buffer_pct = pct;
    status_unlock();
}

static void set_level(int level)
{
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    status_lock();
    s_status.level = level;
    status_unlock();
}

static void set_buffering(bool on)
{
    status_lock();
    if (s_status.buffering != on) {
        s_status.buffering = on;
    }
    status_unlock();
}

static void set_stream_id(uint32_t id)
{
    status_lock();
    s_status.stream_id = id;
    status_unlock();
}

/* --------------------------- ring buffer -------------------------- */

static int ring_fill(void)
{
    if (!s_ring) {
        return 0;
    }
    size_t free_bytes = xRingbufferGetCurFreeSize(s_ring);
    int fill = RING_CAP - (int)free_bytes;
    if (fill < 0) fill = 0;
    if (fill > RING_CAP) fill = RING_CAP;
    return fill;
}

static void ring_flush(void)
{
    if (!s_ring) {
        return;
    }
    for (int i = 0; i < 64; i++) {
        size_t n = 0;
        void *p = xRingbufferReceiveUpTo(s_ring, &n, 0, READ_CHUNK);
        if (p == NULL) {
            break;
        }
        vRingbufferReturnItem(s_ring, p);
    }
}

/* --------------------------- MP3 helpers -------------------------- */

/* Return frame length for a valid MPEG-1/2/2.5 Layer III header, else -1. */
static int mp3_frame_length(const uint8_t *h)
{
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) {
        return -1;
    }
    int version = (h[1] >> 3) & 0x03;   /* 0=MPEG2.5, 2=MPEG2, 3=MPEG1 */
    int layer = (h[1] >> 1) & 0x03;     /* 1=Layer III */
    if (version == 1 || layer != 1) {
        return -1;
    }
    int br_idx = (h[2] >> 4) & 0x0F;
    int sr_idx = (h[2] >> 2) & 0x03;
    int pad = (h[2] >> 1) & 0x01;
    if (br_idx == 0 || br_idx == 15 || sr_idx == 3) {
        return -1;
    }
    static const int br_mpeg1[16] = {0, 32, 40, 48, 56, 64, 80, 96,
                                     112, 128, 160, 192, 224, 256, 320, 0};
    static const int br_mpeg2[16] = {0, 8, 16, 24, 32, 40, 48, 56,
                                     64, 80, 96, 112, 128, 144, 160, 0};
    static const int sr[4][3] = {
        {11025, 12000, 8000},   /* MPEG 2.5 */
        {0, 0, 0},              /* reserved */
        {22050, 24000, 16000},  /* MPEG 2 */
        {44100, 48000, 32000},  /* MPEG 1 */
    };
    int bitrate = (version == 3) ? br_mpeg1[br_idx] : br_mpeg2[br_idx];
    int rate = sr[version][sr_idx];
    if (bitrate <= 0 || rate <= 0) {
        return -1;
    }
    int len = ((version == 3) ? 144 : 72) * bitrate * 1000 / rate + pad;
    if (len < 24 || len > 2048) {
        return -1;
    }
    return len;
}

/* Find the first plausible frame start. Returns offset or -1. */
static int find_frame(const uint8_t *buf, int len)
{
    for (int i = 0; i + 4 <= len; i++) {
        int flen = mp3_frame_length(buf + i);
        if (flen <= 0) {
            continue;
        }
        if (i + flen > len) {
            /* Frame is not complete yet; wait for more data. */
            return i;
        }
        if (i + flen + 2 <= len) {
            /* Verify the next frame header to reject false syncs in ID3 tags. */
            if (mp3_frame_length(buf + i + flen) > 0) {
                return i;
            }
            /* Near the end of the buffer the next header may be truncated. */
            if (i + flen + 8 > len) {
                return i;
            }
            continue;
        }
        return i;
    }
    return -1;
}

static int fill_input(int timeout_ms)
{
    if (!s_ring) {
        return 0;
    }
    int avail = s_in_len - s_in_pos;
    if (s_in_pos > 0) {
        memmove(s_mp3_in, s_mp3_in + s_in_pos, avail);
        s_in_len = avail;
        s_in_pos = 0;
    }
    size_t space = MP3_IN_CAP - s_in_len;
    if (space < 256) {
        return 0;
    }
    size_t n = 0;
    uint8_t *ptr = (uint8_t *)xRingbufferReceiveUpTo(
        s_ring, &n, pdMS_TO_TICKS(timeout_ms), space);
    if (ptr == NULL || n == 0) {
        if (ptr) {
            vRingbufferReturnItem(s_ring, ptr);
        }
        return 0;
    }
    memcpy(s_mp3_in + s_in_len, ptr, n);
    s_in_len += n;
    vRingbufferReturnItem(s_ring, ptr);
    return (int)n;
}

static void reset_decoder(void)
{
    if (s_decoder) {
        MP3FreeDecoder(s_decoder);
    }
    s_decoder = MP3InitDecoder();
    s_in_len = 0;
    s_in_pos = 0;
    /* Keep the codec at its current rate. If the next station uses the same
     * rate we skip a pointless close/open cycle (and its I2S log noise). */
    s_cur_rate = audio_stream_rate();
    s_cur_ch = 2;
    s_bad_frames = 0;
    s_underflow_retries = 0;
    /* Bars fall while the new stream is connecting. */
    spectrum_reset(0);
    if (s_decoder == NULL) {
        set_error("MP3 解码器初始化失败");
    }
}

/* Decode one frame into s_pcm. Returns number of int16 samples (>0),
 * 0 if more compressed data is needed, or -1 if a frame was skipped. */
static int decode_one_frame(void)
{
    if (s_decoder == NULL) {
        return -1;
    }

    int avail = s_in_len - s_in_pos;
    if (s_in_pos > 0 && (avail < 2048 || s_in_pos > MP3_IN_CAP / 2)) {
        memmove(s_mp3_in, s_mp3_in + s_in_pos, avail);
        s_in_len = avail;
        s_in_pos = 0;
    }

    if (s_in_len - s_in_pos < 4) {
        if (fill_input(20) <= 0) {
            return 0;
        }
    }

    avail = s_in_len - s_in_pos;
    int off = find_frame(s_mp3_in + s_in_pos, avail);
    if (off < 0) {
        int keep = avail > 2 ? 2 : avail;
        s_in_pos = s_in_len - keep;
        return -1;
    }
    s_in_pos += off;
    avail = s_in_len - s_in_pos;

    int flen = mp3_frame_length(s_mp3_in + s_in_pos);
    if (flen <= 0) {
        s_in_pos++;
        return -1;
    }
    if (avail < flen) {
        if (fill_input(20) <= 0) {
            return 0;
        }
        avail = s_in_len - s_in_pos;
        if (avail < flen) {
            return 0;
        }
    }

    unsigned char *p = s_mp3_in + s_in_pos;
    int left = avail;
    int err = MP3Decode(s_decoder, &p, &left, s_pcm, 0);
    int consumed = (int)(p - (s_mp3_in + s_in_pos));

    if (err == ERR_MP3_INDATA_UNDERFLOW) {
        /* Keep s_in_pos at the frame start so a retry sees the whole frame.
         * If it keeps happening with a complete-looking frame, the header
         * parser guessed a frame length that is too small; skip one byte. */
        if (++s_underflow_retries > 4) {
            s_underflow_retries = 0;
            s_in_pos++;
        }
        return 0;
    }
    s_underflow_retries = 0;
    if (consumed > 0) {
        s_in_pos += consumed;
    } else {
        s_in_pos++;
    }
    if (err != 0) {
        if (err != ERR_MP3_MAINDATA_UNDERFLOW) {
            s_bad_frames++;
            if (s_bad_frames < 5) {
                ESP_LOGW(TAG, "MP3 decode error %d", err);
            }
        }
        return -1;
    }
    s_bad_frames = 0;

    MP3FrameInfo info;
    MP3GetLastFrameInfo(s_decoder, &info);
    int samples = info.outputSamps;
    if (samples <= 0 || samples > 2 * 1152) {
        return -1;
    }

    if (info.nChans == 1) {
        /* ES8311 is opened in stereo; duplicate mono into both channels. */
        for (int i = samples - 1; i >= 0; i--) {
            int16_t v = s_pcm[i];
            s_pcm[2 * i] = v;
            s_pcm[2 * i + 1] = v;
        }
        samples *= 2;
    }

    if (info.samprate != s_cur_rate || s_cur_ch != 2) {
        esp_err_t e = audio_stream_open(info.samprate, 2);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "codec open %d Hz failed: %s", info.samprate, esp_err_to_name(e));
            set_error("音频输出失败");
            s_stop_req = true;
            return -1;
        }
        s_cur_rate = info.samprate;
        s_cur_ch = 2;
        spectrum_reset(info.samprate);
    }

    spectrum_feed_frame(s_pcm, samples);
    set_info(info.bitrate / 1000, info.samprate, info.nChans);
    set_buffering(false);
    return samples;
}

static void update_level(const int16_t *pcm, int samples)
{
    int64_t sum = 0;
    for (int i = 0; i < samples; i++) {
        int32_t v = pcm[i];
        sum += (int64_t)v * v;
    }
    int rms = (int)sqrtf((float)sum / (float)samples);
    int level = rms * 100 / 12000;
    if (level > 100) level = 100;
    s_level = (s_level * 7 + level) / 8;

    int64_t now = esp_timer_get_time();
    if (now - s_last_level_us >= 100000) {
        s_last_level_us = now;
        set_level(s_level);
    }
}

/* ----------------------------- HTTP ------------------------------- */

static void flush_and_mark_stream(void)
{
    uint32_t sid = ++s_stream_counter;
    set_stream_id(sid);
    ring_flush();
}

static void stream_station(int idx)
{
    if (idx < 0 || idx >= RADIO_STATION_COUNT) {
        return;
    }
    const radio_station_t *station = &k_radio_stations[idx];

    set_state(RADIO_STATE_CONNECTING, NULL);
    set_station(idx);
    set_info(0, 0, 0);
    set_buffer(0);
    set_buffering(false);
    flush_and_mark_stream();

    ESP_LOGI(TAG, "connecting [%d] %s: %s", idx, station->name, station->url);

    char url[512];
    strlcpy(url, station->url, sizeof(url));

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_CONNECT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        set_error("HTTP 初始化失败");
        return;
    }
    esp_http_client_set_header(client, "Icy-MetaData", "0");
    esp_http_client_set_header(client, "Accept", "*/*");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "User-Agent", "ESP32-S3-Radio/1.0");

    esp_err_t err = ESP_FAIL;
    int code = 0;
    bool opened = false;

    for (int hop = 0; hop < 5 && !s_stop_req; hop++) {
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            set_error("连接失败 (%s)", esp_err_to_name(err));
            break;
        }
        int64_t len = esp_http_client_fetch_headers(client);
        code = esp_http_client_get_status_code(client);

        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
            char *loc = NULL;
            if (esp_http_client_get_header(client, "Location", &loc) == ESP_OK &&
                loc != NULL && loc[0]) {
                ESP_LOGI(TAG, "HTTP %d redirect -> %s", code, loc);
                strlcpy(url, loc, sizeof(url));
                esp_http_client_close(client);
                if (esp_http_client_set_url(client, url) != ESP_OK) {
                    set_error("重定向地址无效");
                    break;
                }
                continue;
            }
            set_error("重定向缺少 Location");
            break;
        }

        char *ctype = NULL;
        esp_http_client_get_header(client, "Content-Type", &ctype);
        ESP_LOGI(TAG, "HTTP %d, type=%s, length=%lld", code,
                 ctype ? ctype : "?", (long long)len);
        if (code != 200) {
            set_error("HTTP %d", code);
            break;
        }
        opened = true;
        break;
    }

    if (!opened || s_stop_req) {
        goto done;
    }

    esp_http_client_set_timeout_ms(client, HTTP_READ_MS);
    set_state(RADIO_STATE_BUFFERING, NULL);

    uint8_t *tmp = heap_caps_malloc(READ_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (tmp == NULL) {
        tmp = malloc(READ_CHUNK);
    }
    if (tmp == NULL) {
        set_error("内存不足");
        goto done;
    }

    int last_pct = -1;
    while (!s_stop_req) {
        radio_msg_t pending;
        if (xQueuePeek(s_queue, &pending, 0) == pdTRUE) {
            s_stop_req = true;
            break;
        }

        size_t free_bytes = xRingbufferGetCurFreeSize(s_ring);
        if (free_bytes < 512) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        int want = free_bytes < READ_CHUNK ? (int)free_bytes : READ_CHUNK;
        int r = esp_http_client_read(client, (char *)tmp, want);

        if (r > 0) {
            if (xRingbufferSend(s_ring, tmp, r, 0) != pdTRUE) {
                s_skips++;
            }
            int fill = ring_fill();
            int pct = fill * 100 / RING_CAP;
            if (pct != last_pct) {
                last_pct = pct;
                set_buffer(pct);
            }
            if (get_state() == RADIO_STATE_BUFFERING && fill >= PREBUFFER_BYTES) {
                ESP_LOGI(TAG, "prebuffered %d KB, playing", fill / 1024);
                set_state(RADIO_STATE_PLAYING, NULL);
            }
        } else if (r == -ESP_ERR_HTTP_EAGAIN) {
            /* Read timeout with no bytes: normal for a live stream. */
        } else if (r == 0) {
            if (!s_stop_req) {
                set_error("电台已断开");
            }
            break;
        } else {
            if (!s_stop_req) {
                set_error("读取失败 (%s)", esp_err_to_name(-r));
            }
            break;
        }
    }

    free(tmp);

done:
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    ring_flush();
    set_buffer(0);
    ESP_LOGI(TAG, "stream stopped (state=%d)", (int)get_state());
}

/* ----------------------------- tasks ------------------------------ */

static void net_task(void *arg)
{
    (void)arg;
    radio_msg_t m;
    while (1) {
        if (xQueueReceive(s_queue, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        int station = m.arg;
        if (station < 0 || station >= RADIO_STATION_COUNT) {
            station = 0;
        }

        switch (m.cmd) {
        case CMD_PLAY:
            set_station(station);
            if (wifi_app_state() != WIFI_APP_CONNECTED) {
                set_error("WiFi 未连接");
                break;
            }
            s_stop_req = false;
            stream_station(station);
            break;
        case CMD_PAUSE:
            s_stop_req = true;
            set_state(RADIO_STATE_PAUSED, NULL);
            set_buffer(0);
            break;
        case CMD_STOP:
            s_stop_req = true;
            set_state(RADIO_STATE_IDLE, NULL);
            set_buffer(0);
            break;
        case CMD_NEXT:
        case CMD_PREV: {
            radio_status_t st;
            radio_get_status(&st);
            int delta = (m.cmd == CMD_NEXT) ? 1 : -1;
            int next = (st.station + delta + RADIO_STATION_COUNT) % RADIO_STATION_COUNT;
            set_station(next);
            if (wifi_app_state() != WIFI_APP_CONNECTED) {
                set_error("WiFi 未连接");
                break;
            }
            s_stop_req = false;
            stream_station(next);
            break;
        }
        default:
            break;
        }
    }
}

static void play_task(void *arg)
{
    (void)arg;
    bool was_active = false;

    while (1) {
        radio_status_t st;
        radio_get_status(&st);

        if (st.stream_id != s_seen_stream_id) {
            s_seen_stream_id = st.stream_id;
            reset_decoder();
        }

        bool active = (st.state == RADIO_STATE_CONNECTING ||
                       st.state == RADIO_STATE_BUFFERING ||
                       st.state == RADIO_STATE_PLAYING);
        if (!active) {
            if (was_active) {
                audio_stream_silence(80);
                was_active = false;
                set_buffering(false);
            }
            spectrum_idle();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        was_active = true;

        int samples = decode_one_frame();
        if (samples > 0) {
            int ret = audio_stream_write(s_pcm, (size_t)samples * sizeof(int16_t));
            if (ret != 0) {
                ESP_LOGE(TAG, "codec write failed: %d", ret);
                set_error("音频写入失败");
                s_stop_req = true;
                continue;
            }
            s_frames++;
            update_level(s_pcm, samples);
        } else if (samples == 0) {
            if (st.state == RADIO_STATE_PLAYING) {
                set_buffering(true);
                s_underruns++;
            }
            spectrum_idle();
            if (audio_is_ready()) {
                audio_stream_silence(20);
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        } else {
            s_skips++;
            vTaskDelay(pdMS_TO_TICKS(2));
        }

        int64_t now = esp_timer_get_time();
        if (now - s_last_log_us >= 5000000) {
            s_last_log_us = now;
            ESP_LOGI(TAG, "frames=%u underruns=%u skips=%u buf=%d%% level=%d "
                          "heap=%u psram=%u",
                     (unsigned)s_frames, (unsigned)s_underruns, (unsigned)s_skips,
                     ring_fill() * 100 / RING_CAP, s_level,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
    }
}

/* ----------------------------- public API ------------------------- */

void radio_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(8, sizeof(radio_msg_t));
    s_ring = xRingbufferCreateWithCaps(
        RING_CAP, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ring == NULL) {
        s_ring = xRingbufferCreate(RING_CAP, RINGBUF_TYPE_BYTEBUF);
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.state = RADIO_STATE_IDLE;
    s_status.station = 0;
    s_status.buffer_pct = 0;
    s_status.level = 0;
    s_status.buffering = false;
    s_status.stream_id = 0;
    s_last_log_us = esp_timer_get_time();
    s_last_level_us = s_last_log_us;

    if (s_lock == NULL || s_queue == NULL || s_ring == NULL) {
        ESP_LOGE(TAG, "radio init failed (lock=%p queue=%p ring=%p)",
                 s_lock, s_queue, s_ring);
    } else {
        ESP_LOGI(TAG, "radio init ok: %d presets, ring %d KB (%s)",
                 RADIO_STATION_COUNT, RING_CAP / 1024,
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0 ? "PSRAM" : "internal");
    }
}

void radio_start(void)
{
    if (s_queue == NULL || s_ring == NULL) {
        ESP_LOGE(TAG, "radio_start called before radio_init");
        return;
    }
    xTaskCreatePinnedToCore(net_task, "radio_net", NET_STACK, NULL, NET_PRIO, NULL, 0);
    xTaskCreatePinnedToCore(play_task, "radio_play", PLAY_STACK, NULL, PLAY_PRIO, NULL, 1);
    ESP_LOGI(TAG, "radio tasks started");
}

void radio_play(int station)
{
    if (s_queue == NULL) {
        return;
    }
    radio_msg_t m = {.cmd = CMD_PLAY, .arg = station};
    xQueueSend(s_queue, &m, 0);
}

void radio_toggle_pause(void)
{
    radio_status_t st;
    radio_get_status(&st);
    if (st.state == RADIO_STATE_PLAYING ||
        st.state == RADIO_STATE_BUFFERING ||
        st.state == RADIO_STATE_CONNECTING) {
        radio_msg_t m = {.cmd = CMD_PAUSE, .arg = 0};
        if (s_queue) xQueueSend(s_queue, &m, 0);
    } else {
        radio_msg_t m = {.cmd = CMD_PLAY, .arg = st.station};
        if (s_queue) xQueueSend(s_queue, &m, 0);
    }
}

void radio_next(void)
{
    if (s_queue) {
        radio_msg_t m = {.cmd = CMD_NEXT, .arg = 0};
        xQueueSend(s_queue, &m, 0);
    }
}

void radio_prev(void)
{
    if (s_queue) {
        radio_msg_t m = {.cmd = CMD_PREV, .arg = 0};
        xQueueSend(s_queue, &m, 0);
    }
}

void radio_stop(void)
{
    if (s_queue) {
        radio_msg_t m = {.cmd = CMD_STOP, .arg = 0};
        xQueueSend(s_queue, &m, 0);
    }
}

void radio_get_status(radio_status_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_lock == NULL) {
        memset(out, 0, sizeof(*out));
        out->state = RADIO_STATE_IDLE;
        return;
    }
    status_lock();
    *out = s_status;
    status_unlock();
}

int radio_station_count(void)
{
    return RADIO_STATION_COUNT;
}

const char *radio_station_name(int idx)
{
    if (idx < 0 || idx >= RADIO_STATION_COUNT) {
        return "";
    }
    return k_radio_stations[idx].name;
}

const char *radio_station_desc(int idx)
{
    if (idx < 0 || idx >= RADIO_STATION_COUNT) {
        return "";
    }
    return k_radio_stations[idx].genre;
}
