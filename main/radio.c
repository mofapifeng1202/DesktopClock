/*
 * Media stream engine: HTTP(S) stream -> ring buffer -> decoder -> ES8311/I2S.
 *
 * Two content sources share one pipeline, with per-type decoders:
 *   - radio presets (MP3 streams, radio_stations.h) -> libhelix
 *   - TV channels  (TS over HTTP / HLS, loaded at runtime by media_list)
 *     -> ts_demux (own PAT/PMT/PES parser, audio stream only)
 *     -> esp_audio_simple_dec AAC decoder
 *
 * Two tasks:
 *   radio_net  - HTTP(S) fetch, HLS segment loop, fills a PSRAM ring buffer
 *   radio_play - feeds the decoder and writes PCM to I2S
 *
 * Design notes:
 *   - The network task must never block the audio path: it fills a 128 KB
 *     PSRAM ring buffer; the player task drains it and blocks on I2S writes.
 *     For IPTV the ring runs full (video bytes arrive much faster than audio
 *     is consumed) and the TCP connection is throttled by that backpressure.
 *   - Icy metadata is explicitly disabled: Chinese radio streams send
 *     StreamTitle blocks only when the client asks for them.
 *   - UI/touch runs at priority 6; both engine tasks stay below it.
 *   - The esp_audio_codec TS parser selects streams in a way that breaks on
 *     HEVC-first PMTs, which is why the tiny ts_demux module exists.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "mp3dec.h"

#include "main.h"
#include "media_list.h"
#include "radio.h"
#include "radio_stations.h"
#include "spectrum.h"
#include "ts_demux.h"
#include "wifi_app.h"

static const char *TAG = "radio";

#define RADIO_STATION_COUNT ((int)(sizeof(k_radio_stations) / sizeof(k_radio_stations[0])))

#define RING_CAP          (384 * 1024)
#define IN_CAP            (16 * 1024)
#define PREBUFFER_BYTES   (24 * 1024)
#define READ_CHUNK        4096
#define HTTP_CONNECT_MS   8000
#define HTTP_READ_MS      200
#define NET_STACK         10240
#define PLAY_STACK        12288
#define NET_PRIO          4
#define PLAY_PRIO         5

#define DEC_PCM_MIN       (32 * 1024)   /* AAC decoder output buffer, bytes */
#define DEC_PCM_MAX       (128 * 1024)
#define ES_BUF_SIZE       (32 * 1024)   /* AAC elementary stream feed buffer */
#define HLS_LIST_BUF      (64 * 1024)
#define HLS_MAX_SEGMENTS  512

typedef enum {
    CMD_PLAY = 0,
    CMD_PAUSE,
    CMD_NEXT,
    CMD_PREV,
    CMD_STOP,
} radio_cmd_t;

typedef enum {
    CTX_PRESET = 0,     /* radio presets (radio_stations.h) */
    CTX_TV,             /* TV channels (media_list) */
} media_ctx_t;

typedef struct {
    radio_cmd_t cmd;
    int arg;
} radio_msg_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;
static RingbufHandle_t s_ring;
static radio_status_t s_status;

static volatile bool s_stop_req;
static volatile media_ctx_t s_ctx = CTX_PRESET;
static volatile media_type_t s_active_type = MEDIA_MP3;
static volatile uint32_t s_stream_counter;
static uint32_t s_seen_stream_id;

/* libhelix MP3 decoder (radio presets) */
static HMP3Decoder s_mp3dec;
/* esp_audio_codec AAC decoder (IPTV) */
static esp_audio_simple_dec_handle_t s_sdec;
static bool s_sdec_open;

static int16_t s_pcm[2 * 1152];   /* libhelix output */
static int16_t *s_dec_pcm;        /* AAC decoder output (PSRAM) */
static size_t s_dec_pcm_size;
static int16_t *s_stereo;         /* mono -> stereo expansion (PSRAM) */
static size_t s_stereo_size;      /* shorts per channel */
static uint8_t *s_es;             /* AAC elementary stream feed buffer (PSRAM) */
static int s_es_cap;
static int s_es_len;

static uint8_t s_in[IN_CAP];
static int s_in_len;
static int s_in_pos;
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
static int64_t s_aac_us;         /* time spent in AAC decode, for the 5 s log */
static int s_prebuffer_target = PREBUFFER_BYTES;

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

static void set_name(const char *name)
{
    status_lock();
    strlcpy(s_status.name, name ? name : "", sizeof(s_status.name));
    status_unlock();
}

static void set_info(int bitrate_kbps, int sample_rate, int channels)
{
    bool changed = false;
    status_lock();
    /* VBR jitters the per-frame bitrate; quantize so the UI label doesn't
     * churn, and only log when the sample rate actually changes. */
    bitrate_kbps = (bitrate_kbps + 4) & ~7;
    if (s_status.bitrate_kbps != bitrate_kbps ||
        s_status.sample_rate != sample_rate ||
        s_status.channels != channels) {
        bool rate_changed = s_status.sample_rate != sample_rate;
        s_status.bitrate_kbps = bitrate_kbps;
        s_status.sample_rate = sample_rate;
        s_status.channels = channels;
        changed = rate_changed;
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
    for (int i = 0; i < 256; i++) {
        size_t n = 0;
        void *p = xRingbufferReceiveUpTo(s_ring, &n, 0, READ_CHUNK);
        if (p == NULL) {
            break;
        }
        vRingbufferReturnItem(s_ring, p);
    }
}

/* --------------------------- decoder ------------------------------ */

/* Pull more compressed bytes from the ring into the input buffer. */
static int fill_input(int timeout_ms)
{
    if (!s_ring) {
        return 0;
    }
    int avail = s_in_len - s_in_pos;
    if (s_in_pos > 0) {
        memmove(s_in, s_in + s_in_pos, avail);
        s_in_len = avail;
        s_in_pos = 0;
    }
    size_t space = IN_CAP - s_in_len;
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
    memcpy(s_in + s_in_len, ptr, n);
    s_in_len += n;
    vRingbufferReturnItem(s_ring, ptr);
    return (int)n;
}

static void dec_close(void)
{
    if (s_mp3dec) {
        MP3FreeDecoder(s_mp3dec);
        s_mp3dec = NULL;
    }
    if (s_sdec_open) {
        esp_audio_simple_dec_close(s_sdec);
        s_sdec_open = false;
    }
}

static void reset_decoder(void)
{
    dec_close();
    s_in_len = 0;
    s_in_pos = 0;
    s_es_len = 0;
    s_bad_frames = 0;
    s_underflow_retries = 0;
    /* Keep the codec at its current rate. If the next stream uses the same
     * rate we skip a pointless close/open cycle (and its I2S log noise). */
    s_cur_rate = audio_stream_rate();
    s_cur_ch = 2;
    ts_demux_reset();
    spectrum_reset(0);

    if (s_active_type == MEDIA_MP3) {
        s_mp3dec = MP3InitDecoder();
        if (s_mp3dec == NULL) {
            set_error("MP3 解码器初始化失败");
        }
    } else {
        esp_audio_simple_dec_cfg_t cfg = {
            .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC,
            .dec_cfg = NULL,
            .cfg_size = 0,
            .use_frame_dec = false,
        };
        int err = esp_audio_simple_dec_open(&cfg, &s_sdec);
        if (err != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "AAC simple dec open failed: %d", err);
            s_sdec_open = false;
            set_error("AAC 解码器打开失败");
        } else {
            s_sdec_open = true;
        }
    }
}

/* Apply decoder stream info: open/reopen the codec and remap the spectrum
 * when the sample rate changes. Returns channel count, or -1 on error. */
static int apply_stream_info(int sample_rate, int channels, int bitrate_bps)
{
    if (sample_rate == 0) {
        return 2;
    }
    if (sample_rate != s_cur_rate || s_cur_ch != 2) {
        esp_err_t e = audio_stream_open(sample_rate, 2);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "codec open %d Hz failed: %s", sample_rate, esp_err_to_name(e));
            set_error("音频输出失败");
            s_stop_req = true;
            return -1;
        }
        s_cur_rate = sample_rate;
        s_cur_ch = 2;
        spectrum_reset(sample_rate);
    }
    set_info(bitrate_bps / 1000, sample_rate, channels);
    return channels;
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

/* ----------------------- MP3 path (radio presets) ----------------- */

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

/* Decode one MP3 frame into s_pcm. Returns number of int16 samples (>0),
 * 0 if more compressed data is needed, or -1 if a frame was skipped. */
static int decode_one_frame(void)
{
    if (s_mp3dec == NULL) {
        return -1;
    }

    int avail = s_in_len - s_in_pos;
    if (s_in_pos > 0 && (avail < 2048 || s_in_pos > IN_CAP / 2)) {
        memmove(s_in, s_in + s_in_pos, avail);
        s_in_len = avail;
        s_in_pos = 0;
    }

    if (s_in_len - s_in_pos < 4) {
        if (fill_input(20) <= 0) {
            return 0;
        }
    }

    avail = s_in_len - s_in_pos;
    int off = find_frame(s_in + s_in_pos, avail);
    if (off < 0) {
        int keep = avail > 2 ? 2 : avail;
        s_in_pos = s_in_len - keep;
        return -1;
    }
    s_in_pos += off;
    avail = s_in_len - s_in_pos;

    int flen = mp3_frame_length(s_in + s_in_pos);
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

    unsigned char *p = s_in + s_in_pos;
    int left = avail;
    int err = MP3Decode(s_mp3dec, &p, &left, s_pcm, 0);
    int consumed = (int)(p - (s_in + s_in_pos));

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
    MP3GetLastFrameInfo(s_mp3dec, &info);
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

    if (apply_stream_info(info.samprate, info.nChans, info.bitrate) < 0) {
        return -1;
    }

    spectrum_feed_frame(s_pcm, samples);
    set_buffering(false);
    return samples;
}

/* ----------------------- AAC path (IPTV) -------------------------- */

static void grow_dec_pcm(size_t needed)
{
    if (needed <= s_dec_pcm_size || needed > DEC_PCM_MAX) {
        return;
    }
    void *p = heap_caps_realloc(s_dec_pcm, needed, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p != NULL) {
        s_dec_pcm = p;
        s_dec_pcm_size = needed;
        ESP_LOGI(TAG, "decoder pcm buffer -> %u KB", (unsigned)(needed / 1024));
    }
}

/* Pull raw TS bytes from the ring, demux to AAC ES, feed the AAC decoder and
 * write the PCM to the codec. Returns shorts written (>0), 0 when more
 * compressed data is needed, or -1 when data was skipped. */
static int decode_ts_chunk(void)
{
    if (!s_sdec_open) {
        return -1;
    }
    int avail = s_in_len - s_in_pos;
    if (s_in_pos > 0 && (avail < 4096 || s_in_pos > IN_CAP / 2)) {
        memmove(s_in, s_in + s_in_pos, avail);
        s_in_len = avail;
        s_in_pos = 0;
    }
    if (s_in_len - s_in_pos < 1024) {
        fill_input(20);
    }
    avail = s_in_len - s_in_pos;
    if (avail > 0) {
        int used = ts_demux_feed(s_in + s_in_pos, avail, s_es, s_es_cap, &s_es_len);
        s_in_pos += used;
        if (s_in_pos >= s_in_len) {
            s_in_len = 0;
            s_in_pos = 0;
        }
    }

    int written = 0;
    while (s_es_len > 0 && !s_stop_req) {
        esp_audio_simple_dec_raw_t raw = {
            .buffer = s_es,
            .len = s_es_len,
            .eos = false,
        };
        esp_audio_simple_dec_out_t out = {
            .buffer = (uint8_t *)s_dec_pcm,
            .len = (uint32_t)s_dec_pcm_size,
        };
        int64_t t0 = esp_timer_get_time();
        int err = esp_audio_simple_dec_process(s_sdec, &raw, &out);
        s_aac_us += esp_timer_get_time() - t0;
        if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            grow_dec_pcm(out.needed_size);
            if (out.needed_size > s_dec_pcm_size) {
                ESP_LOGW(TAG, "decoder pcm buffer too small (%u needed)",
                         (unsigned)out.needed_size);
                s_es_len = 0;
                return written > 0 ? written : -1;
            }
            continue;
        }
        if (err != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "AAC simple dec error %d", err);
            s_skips++;
            s_es_len = 0;                 /* drop and resync on next packets */
            return written > 0 ? written : -1;
        }
        int consumed = (int)raw.consumed;
        if (consumed > 0) {
            s_es_len -= consumed;
            if (s_es_len > 0) {
                memmove(s_es, s_es + consumed, s_es_len);
            }
        }
        if (out.decoded_size > 0) {
            /* Reconfigure the codec when the decoded rate changes — without
             * this a 44.1 kHz IPTV stream plays at the boot-time 16 kHz
             * (sounds like slow motion). */
            esp_audio_simple_dec_info_t info;
            int ch = 2;
            if (esp_audio_simple_dec_get_info(s_sdec, &info) == ESP_AUDIO_ERR_OK) {
                ch = apply_stream_info((int)info.sample_rate, (int)info.channel,
                                       (int)info.bitrate);
                if (ch < 0) {
                    return written > 0 ? written : -1;
                }
            }
            int shorts = (int)(out.decoded_size / 2);
            const int16_t *pcm = s_dec_pcm;
            if (ch == 1 && shorts > 0) {
                if ((size_t)shorts > s_stereo_size) {
                    shorts = (int)s_stereo_size;
                }
                for (int i = shorts - 1; i >= 0; i--) {
                    int16_t v = s_dec_pcm[i];
                    s_stereo[2 * i] = v;
                    s_stereo[2 * i + 1] = v;
                }
                pcm = s_stereo;
                shorts *= 2;
            }
            if (shorts > 0) {
                int ret = audio_stream_write(pcm, (size_t)shorts * sizeof(int16_t));
                if (ret != 0) {
                    ESP_LOGE(TAG, "codec write failed: %d", ret);
                    set_error("音频写入失败");
                    s_stop_req = true;
                    return written > 0 ? written : -1;
                }
                spectrum_feed_frame(pcm, shorts);
                update_level(pcm, shorts);
                written += shorts;
            }
        }
        if (consumed == 0) {
            break;                        /* decoder needs more input */
        }
    }
    return written;
}

/* ----------------------------- HTTP ------------------------------- */

static void flush_and_mark_stream(void)
{
    uint32_t sid = ++s_stream_counter;
    set_stream_id(sid);
    ring_flush();
}

static esp_http_client_handle_t http_open(const char *url, bool icy_off)
{
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
        return NULL;
    }
    if (icy_off) {
        esp_http_client_set_header(client, "Icy-MetaData", "0");
    }
    esp_http_client_set_header(client, "Accept", "*/*");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "User-Agent", "ESP32-S3-Radio/1.0");
    return client;
}

/* Open + handle redirects + verify HTTP 200. Returns true when the client is
 * ready for esp_http_client_read(). quiet=true suppresses the error state so
 * the caller can skip a dead HLS segment instead of killing the stream. */
static bool http_connect(esp_http_client_handle_t client, int *out_code, bool quiet)
{
    for (int hop = 0; hop < 5 && !s_stop_req; hop++) {
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            if (quiet) {
                ESP_LOGW(TAG, "connect failed (%s)", esp_err_to_name(err));
            } else {
                set_error("连接失败 (%s)", esp_err_to_name(err));
            }
            return false;
        }
        esp_http_client_fetch_headers(client);
        int code = esp_http_client_get_status_code(client);
        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
            char *loc = NULL;
            if (esp_http_client_get_header(client, "Location", &loc) == ESP_OK &&
                loc != NULL && loc[0]) {
                ESP_LOGI(TAG, "HTTP %d redirect -> %s", code, loc);
                esp_http_client_close(client);
                if (esp_http_client_set_url(client, loc) != ESP_OK) {
                    if (!quiet) set_error("重定向地址无效");
                    return false;
                }
                continue;
            }
            if (!quiet) set_error("重定向缺少 Location");
            return false;
        }
        char *ctype = NULL;
        esp_http_client_get_header(client, "Content-Type", &ctype);
        int64_t len = esp_http_client_get_content_length(client);
        ESP_LOGI(TAG, "HTTP %d, type=%s, length=%lld", code,
                 ctype ? ctype : "?", (long long)len);
        if (code != 200) {
            if (!quiet) set_error("HTTP %d", code);
            else ESP_LOGW(TAG, "segment HTTP %d", code);
            return false;
        }
        if (out_code) {
            *out_code = code;
        }
        return true;
    }
    return false;
}

/* Read an HTTP body into the ring buffer (backpressured by ring space).
 * Returns true when the body ended cleanly or was stopped by a command. */
static bool http_body_to_ring(esp_http_client_handle_t client)
{
    uint8_t *tmp = heap_caps_malloc(READ_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (tmp == NULL) {
        tmp = malloc(READ_CHUNK);
    }
    if (tmp == NULL) {
        set_error("内存不足");
        return false;
    }
    int last_pct = -1;
    bool ok = true;
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
            if (get_state() == RADIO_STATE_BUFFERING && fill >= s_prebuffer_target) {
                ESP_LOGI(TAG, "prebuffered %d KB, playing", fill / 1024);
                set_state(RADIO_STATE_PLAYING, NULL);
            }
        } else if (r == -ESP_ERR_HTTP_EAGAIN) {
            /* Read timeout with no bytes: normal for a live stream. */
        } else if (r == 0) {
            break;                        /* EOF or connection closed */
        } else {
            if (!s_stop_req) {
                set_error("读取失败 (%s)", esp_err_to_name(-r));
                ok = false;
            }
            break;
        }
    }
    free(tmp);
    return ok;
}

/* Fetch a whole HTTP body into a buffer (playlists, small files). */
static bool http_get_to_buf(const char *url, char *buf, size_t bufsize, int *out_len)
{
    esp_http_client_handle_t client = http_open(url, false);
    if (client == NULL) {
        return false;
    }
    bool ok = false;
    *out_len = 0;
    if (http_connect(client, NULL, false)) {
        esp_http_client_set_timeout_ms(client, HTTP_READ_MS);
        int total = 0;
        while (total < (int)bufsize - 1 && !s_stop_req) {
            int r = esp_http_client_read(client, buf + total, (int)bufsize - 1 - total);
            if (r > 0) {
                total += r;
            } else if (r == 0) {
                ok = true;
                break;
            } else if (r == -ESP_ERR_HTTP_EAGAIN) {
                continue;
            } else {
                break;
            }
        }
        if (total > 0) {
            ok = true;
        }
        *out_len = total;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (ok) {
        buf[*out_len] = '\0';
    }
    return ok;
}

/* ------------------------------ HLS ------------------------------- */

/* Join a (possibly relative) segment URL against the playlist URL. */
static void hls_abs_url(const char *base, const char *src, char *out, size_t outsz)
{
    if (strncasecmp(src, "http://", 7) == 0 || strncasecmp(src, "https://", 8) == 0) {
        snprintf(out, outsz, "%s", src);
        return;
    }
    const char *scheme = strstr(base, "://");
    if (scheme == NULL) {
        snprintf(out, outsz, "%s", src);
        return;
    }
    const char *host = scheme + 3;
    const char *path = strchr(host, '/');
    if (src[0] == '/' || path == NULL) {
        size_t host_len = (path ? (size_t)(path - base) : strlen(base));
        if (src[0] != '/') {
            snprintf(out, outsz, "%.*s/%s", (int)host_len, base, src);
        } else {
            snprintf(out, outsz, "%.*s%s", (int)host_len, base, src);
        }
        return;
    }
    /* relative to the playlist directory */
    size_t dir_len = (size_t)(strrchr(path, '/') - base + 1);
    if (dir_len > strlen(base)) {
        dir_len = strlen(base);
    }
    snprintf(out, outsz, "%.*s%s", (int)dir_len, base, src);
}

/* HLS session: fetch the playlist, stream every (new) segment into the ring,
 * then re-fetch for live playlists. VOD playlists end after the last segment. */
static void stream_hls(const char *url)
{
    char *list = heap_caps_malloc(HLS_LIST_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (list == NULL) {
        set_error("内存不足");
        return;
    }
    typedef struct {
        int start;
        int len;
    } seg_ref_t;
    seg_ref_t *segs = heap_caps_malloc(sizeof(seg_ref_t) * HLS_MAX_SEGMENTS,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (segs == NULL) {
        free(list);
        set_error("内存不足");
        return;
    }

    uint32_t last_seq = 0;
    bool have_last = false;
    bool first_fetch = true;
    int no_new = 0;

    while (!s_stop_req) {
        int len = 0;
        if (!http_get_to_buf(url, list, HLS_LIST_BUF, &len) || len == 0) {
            if (!s_stop_req) {
                set_error("播放列表获取失败");
            }
            break;
        }

        uint32_t media_seq = 0;
        bool endlist = false;
        int seg_count = 0;

        char *save = NULL;
        for (char *line = strtok_r(list, "\n", &save); line != NULL;
             line = strtok_r(NULL, "\n", &save)) {
            size_t l = strlen(line);
            while (l > 0 && (line[l - 1] == '\r' || line[l - 1] == ' ')) {
                line[--l] = '\0';
            }
            if (l == 0) {
                continue;
            }
            if (strncmp(line, "#EXT-X-TARGETDURATION:", 22) == 0) {
                /* parsed for completeness; segment polling is time-based */
            } else if (strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22) == 0) {
                media_seq = (uint32_t)strtoul(line + 22, NULL, 10);
            } else if (strncmp(line, "#EXT-X-MAP:", 11) == 0) {
                set_error("fMP4/HLS 暂不支持");
                goto done;
            } else if (strncmp(line, "#EXT-X-ENDLIST", 14) == 0) {
                endlist = true;
            } else if (line[0] == '#') {
                continue;
            } else {
                if (seg_count < HLS_MAX_SEGMENTS) {
                    segs[seg_count].start = (int)(line - list);
                    segs[seg_count].len = (int)l;
                    seg_count++;
                }
            }
        }

        bool advanced = false;
        int seg_fail = 0;
        for (int i = 0; i < seg_count && !s_stop_req; i++) {
            uint32_t seq = media_seq + (uint32_t)i;
            if (have_last && seq <= last_seq) {
                continue;                 /* already played on a previous fetch */
            }
            char src[512];
            snprintf(src, sizeof(src), "%.*s", segs[i].len, list + segs[i].start);
            char seg_url[768];
            hls_abs_url(url, src, seg_url, sizeof(seg_url));

            esp_http_client_handle_t client = http_open(seg_url, false);
            if (client == NULL) {
                set_error("HTTP 初始化失败");
                goto done;
            }
            if (!http_connect(client, NULL, true)) {
                /* A live window may have rotated past this segment; skip it
                 * instead of killing the whole stream. */
                esp_http_client_cleanup(client);
                if (++seg_fail >= 3) {
                    set_error("分段获取失败");
                    goto done;
                }
                continue;
            }
            seg_fail = 0;
            esp_http_client_set_timeout_ms(client, HTTP_READ_MS);
            if (first_fetch || !have_last) {
                set_state(RADIO_STATE_BUFFERING, NULL);
            }
            http_body_to_ring(client);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            last_seq = seq;
            have_last = true;
            advanced = true;
            first_fetch = false;
            if (get_state() == RADIO_STATE_ERROR) {
                goto done;
            }
        }

        if (endlist && !advanced) {
            break;                        /* finite playlist fully played */
        }
        /* Live playlist: poll for new segments quickly instead of waiting a
         * whole target duration (a 1.5 s ring cushion must not be drained by
         * a 6-10 s wait). Give up after ~60 s without new segments. */
        no_new = advanced ? 0 : no_new + 1;
        if (no_new > 30) {
            ESP_LOGW(TAG, "no new HLS segments for 60 s, stopping");
            break;
        }
        int waited = 0;
        while (waited < 2000 && !s_stop_req) {
            radio_msg_t pending;
            if (xQueuePeek(s_queue, &pending, 0) == pdTRUE) {
                s_stop_req = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            waited += 100;
        }
    }

done:
    free(segs);
    free(list);
}

/* --------------------------- stream start -------------------------- */

static bool preset_get(int idx, const char **name, const char **url, media_type_t *type)
{
    if (idx < 0 || idx >= RADIO_STATION_COUNT) {
        return false;
    }
    *name = k_radio_stations[idx].name;
    *url = k_radio_stations[idx].url;
    *type = MEDIA_MP3;
    return true;
}

static bool tv_get(int idx, const char **name, const char **url, media_type_t *type)
{
    if (media_list_state() != MEDIA_LIST_OK) {
        set_error("频道列表未加载");
        return false;
    }
    if (idx < 0 || idx >= media_list_count()) {
        return false;
    }
    *name = media_list_name(idx);
    *url = media_list_url(idx);
    *type = media_list_type(idx);
    return true;
}

static bool ctx_get(int idx, const char **name, const char **url, media_type_t *type)
{
    if (s_ctx == CTX_TV) {
        return tv_get(idx, name, url, type);
    }
    return preset_get(idx, name, url, type);
}

static int ctx_count(void)
{
    if (s_ctx == CTX_TV) {
        return media_list_state() == MEDIA_LIST_OK ? media_list_count() : 0;
    }
    return RADIO_STATION_COUNT;
}

static void start_stream(const char *name, const char *url, media_type_t type, int idx)
{
    if (type == MEDIA_UDP) {
        set_error("组播源暂不支持，请用 udpxy 转 http");
        return;
    }

    set_state(RADIO_STATE_CONNECTING, NULL);
    set_station(idx);
    set_name(name);
    set_info(0, 0, 0);
    set_buffer(0);
    set_buffering(false);
    /* IPTV (TS/HLS) carries video bytes too — the player must consume the
     * whole TS (≈2 Mbps) to get the ~130 kbps audio, so a small prebuffer
     * drains in a fraction of a second across HLS segment gaps. Buffer to
     * 90% of the ring (~1.5 s) before starting playback. */
    s_prebuffer_target = (type == MEDIA_MP3) ? PREBUFFER_BYTES
                                             : (int)(RING_CAP * 9 / 10);
    /* Set the decoder type before bumping stream_id: the play task re-inits
     * the decoder when it sees the new stream id. */
    s_active_type = type;
    flush_and_mark_stream();

    ESP_LOGI(TAG, "connecting [%d] %s: %s (type=%d)", idx, name, url, (int)type);

    if (type == MEDIA_HLS) {
        stream_hls(url);
    } else {
        char work[512];
        strlcpy(work, url, sizeof(work));
        esp_http_client_handle_t client = http_open(work, true);
        if (client == NULL) {
            set_error("HTTP 初始化失败");
        } else {
            if (http_connect(client, NULL, false)) {
                esp_http_client_set_timeout_ms(client, HTTP_READ_MS);
                set_state(RADIO_STATE_BUFFERING, NULL);
                http_body_to_ring(client);
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
        }
    }

    ring_flush();
    set_buffer(0);
    /* If the stream ended without an error (server closed, HLS VOD finished),
     * leave PLAYING/BUFFERING — the UI would show playback that is not there. */
    radio_state_t st = get_state();
    if (st == RADIO_STATE_PLAYING || st == RADIO_STATE_BUFFERING) {
        set_state(RADIO_STATE_IDLE, NULL);
    }
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
        const char *name = NULL;
        const char *url = NULL;
        media_type_t type = MEDIA_MP3;
        int station = m.arg;

        switch (m.cmd) {
        case CMD_PLAY:
            if (!ctx_get(station, &name, &url, &type)) {
                break;
            }
            /* Ghost taps can re-select the playing station; restarting the
             * stream for those is disruptive, so dedupe. */
            {
                radio_status_t st;
                radio_get_status(&st);
                if (st.station == station &&
                    (st.state == RADIO_STATE_PLAYING ||
                     st.state == RADIO_STATE_BUFFERING ||
                     st.state == RADIO_STATE_CONNECTING)) {
                    break;
                }
            }
            set_station(station);
            if (wifi_app_state() != WIFI_APP_CONNECTED) {
                set_error("WiFi 未连接");
                break;
            }
            s_stop_req = false;
            start_stream(name, url, type, station);
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
            int cnt = ctx_count();
            if (cnt <= 0) {
                set_error(s_ctx == CTX_TV ? "频道列表未加载" : "没有可用电台");
                break;
            }
            int delta = (m.cmd == CMD_NEXT) ? 1 : -1;
            int next = (st.station + delta + cnt) % cnt;
            if (!ctx_get(next, &name, &url, &type)) {
                break;
            }
            set_station(next);
            if (wifi_app_state() != WIFI_APP_CONNECTED) {
                set_error("WiFi 未连接");
                break;
            }
            s_stop_req = false;
            start_stream(name, url, type, next);
            break;
        }
        default:
            break;
        }
        (void)name;
        (void)url;
        (void)type;
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

        int shorts = 0;
        if (s_active_type == MEDIA_MP3) {
            shorts = decode_one_frame();
            if (shorts > 0) {
                int ret = audio_stream_write(s_pcm, (size_t)shorts * sizeof(int16_t));
                if (ret != 0) {
                    ESP_LOGE(TAG, "codec write failed: %d", ret);
                    set_error("音频写入失败");
                    s_stop_req = true;
                    continue;
                }
                s_frames++;
                update_level(s_pcm, shorts);
            } else if (shorts == 0) {
                if (st.state == RADIO_STATE_PLAYING) {
                    set_buffering(true);
                    s_underruns++;
                }
                spectrum_idle();
                /* Insert silence only when the stream is really starving.
                 * When the decoder is merely between frames (ES empty but the
                 * ring still has data), silence would click mid-audio. */
                if (audio_is_ready() && ring_fill() == 0) {
                    audio_stream_silence(20);
                } else if (!audio_is_ready()) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
            } else {
                s_skips++;
                vTaskDelay(pdMS_TO_TICKS(2));
            }
        } else {
            shorts = decode_ts_chunk();
            if (shorts > 0) {
                s_frames++;
            } else if (shorts == 0) {
                if (st.state == RADIO_STATE_PLAYING) {
                    set_buffering(true);
                    s_underruns++;
                }
                spectrum_idle();
                /* Insert silence only when the stream is really starving.
                 * When the decoder is merely between frames (ES empty but the
                 * ring still has data), silence would click mid-audio. */
                if (audio_is_ready() && ring_fill() == 0) {
                    audio_stream_silence(20);
                } else if (!audio_is_ready()) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
            } else {
                s_skips++;
                vTaskDelay(pdMS_TO_TICKS(2));
            }
        }

        int64_t now = esp_timer_get_time();
        if (now - s_last_log_us >= 5000000) {
            int64_t dt = now - s_last_log_us;
            s_last_log_us = now;
            ESP_LOGI(TAG, "frames=%u underruns=%u skips=%u buf=%d%% level=%d "
                          "aac=%d%% heap=%u psram=%u",
                     (unsigned)s_frames, (unsigned)s_underruns, (unsigned)s_skips,
                     ring_fill() * 100 / RING_CAP, s_level,
                     (int)(s_aac_us * 100 / dt),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            s_aac_us = 0;
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

    s_dec_pcm = heap_caps_malloc(DEC_PCM_MIN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_dec_pcm_size = s_dec_pcm ? DEC_PCM_MIN : 0;
    s_stereo = heap_caps_malloc(DEC_PCM_MIN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_stereo_size = s_stereo ? DEC_PCM_MIN / 2 : 0;
    s_es = heap_caps_malloc(ES_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_es_cap = s_es ? ES_BUF_SIZE : 0;

    memset(&s_status, 0, sizeof(s_status));
    s_status.state = RADIO_STATE_IDLE;
    s_status.station = 0;
    s_status.buffer_pct = 0;
    s_status.level = 0;
    s_status.buffering = false;
    s_status.stream_id = 0;
    s_last_log_us = esp_timer_get_time();
    s_last_level_us = s_last_log_us;

    /* Register the frame decoders (AAC/MP3) AND the simple-decoder wrappers.
     * The TS/AAC path needs both: the TS parser hands frames to the registered
     * AAC frame decoder. */
    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK ||
        esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "decoder registration failed");
    }

    if (s_lock == NULL || s_queue == NULL || s_ring == NULL ||
        s_dec_pcm == NULL || s_stereo == NULL || s_es == NULL) {
        ESP_LOGE(TAG, "radio init failed (lock=%p queue=%p ring=%p pcm=%p st=%p es=%p)",
                 s_lock, s_queue, s_ring, s_dec_pcm, s_stereo, s_es);
    } else {
        ESP_LOGI(TAG, "radio init ok: %d presets, ring %d KB (PSRAM)",
                 RADIO_STATION_COUNT, RING_CAP / 1024);
        media_list_start_load(NULL);
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
    /* Ghost taps on this panel are frequent; a repeat request for the
     * already-playing station must not abort the current stream. */
    radio_status_t st;
    radio_get_status(&st);
    if (s_ctx == CTX_PRESET && st.station == station &&
        (st.state == RADIO_STATE_PLAYING ||
         st.state == RADIO_STATE_BUFFERING ||
         st.state == RADIO_STATE_CONNECTING)) {
        return;
    }
    s_ctx = CTX_PRESET;
    radio_msg_t m = {.cmd = CMD_PLAY, .arg = station};
    xQueueSend(s_queue, &m, 0);
}

void radio_play_tv(int idx)
{
    if (s_queue == NULL) {
        return;
    }
    radio_status_t st;
    radio_get_status(&st);
    if (s_ctx == CTX_TV && st.station == idx &&
        (st.state == RADIO_STATE_PLAYING ||
         st.state == RADIO_STATE_BUFFERING ||
         st.state == RADIO_STATE_CONNECTING)) {
        return;
    }
    s_ctx = CTX_TV;
    radio_msg_t m = {.cmd = CMD_PLAY, .arg = idx};
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
