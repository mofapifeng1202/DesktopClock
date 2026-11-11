/*
 * Runtime IPTV channel list loader (see media_list.h).
 *
 * The playlist is fetched into a PSRAM buffer and parsed in place:
 *   #EXTM3U
 *   #EXTINF:-1 [,]Channel Name
 *   http://...
 *
 * Supported entry types after classification:
 *   MEDIA_HLS  url ends with .m3u8
 *   MEDIA_TS   anything else served over http(s)
 *   MEDIA_UDP  udp:// / rtp:// -- listed but not playable without udpxy
 *
 * Names are sanitized to valid UTF-8 (invalid bytes become '?'): the UI has a
 * fallback font for common Chinese glyphs, but broken byte sequences would
 * still render garbage.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "media_list.h"
#include "wifi_app.h"

static const char *TAG = "media_list";

/* The playlist URL lives in main/tv_config.h (gitignored; see
 * tv_config.h.example) so the LAN address isn't baked into the repo. */
#if __has_include("tv_config.h")
#include "tv_config.h"
#endif

#ifndef TV_LIST_URL
#define TV_LIST_URL "http://192.168.1.98:1905/interface.m3u?relay=2"
#endif

#define LIST_MAX_ENTRIES 256
#define LIST_BUF_SIZE    (256 * 1024)
#define LIST_NAME_MAX    96
#define LIST_URL_MAX     320

typedef struct {
    char *name;
    char *url;
    media_type_t type;
} media_entry_t;

static media_entry_t *s_entries;      /* PSRAM, swapped in after full parse */
static int s_count;
static volatile media_list_state_t s_state = MEDIA_LIST_IDLE;
static char s_source[256];

static char *dup_str(const char *src, size_t max)
{
    size_t n = strnlen(src, max);
    char *p = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = malloc(n + 1);
    }
    if (p != NULL) {
        memcpy(p, src, n);
        p[n] = '\0';
    }
    return p;
}

/* Replace byte sequences that are not valid UTF-8 with '?' so the text can be
 * handed to LVGL without rendering garbage. */
static void sanitize_utf8(char *s)
{
    while (*s) {
        unsigned char c = (unsigned char)*s;
        int len = 0;
        if (c < 0x80) {
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        }
        bool ok = false;
        if (len > 1) {
            ok = true;
            for (int i = 1; i < len; i++) {
                if (((unsigned char)s[i] & 0xC0) != 0x80) {
                    ok = false;
                    break;
                }
            }
        }
        if (len == 1) {
            s++;
        } else if (ok && len > 0) {
            s += len;
        } else {
            *s++ = '?';
        }
    }
}

static media_type_t classify_url(const char *url)
{
    if (strncasecmp(url, "udp://", 6) == 0 || strncasecmp(url, "rtp://", 6) == 0) {
        return MEDIA_UDP;
    }
    const char *q = strchr(url, '?');
    size_t n = (q != NULL) ? (size_t)(q - url) : strlen(url);
    if (n >= 5 && strncasecmp(url + n - 5, ".m3u8", 5) == 0) {
        return MEDIA_HLS;
    }
    if (n >= 3 && strncasecmp(url + n - 3, ".ts", 3) == 0) {
        return MEDIA_TS;
    }
    return MEDIA_TS;
}

static void free_entries(media_entry_t *entries, int count)
{
    for (int i = 0; i < count; i++) {
        free(entries[i].name);
        free(entries[i].url);
    }
    free(entries);
}

static bool line_is_ws(const char *s)
{
    while (*s) {
        if (*s != ' ' && *s != '\r' && *s != '\t') {
            return false;
        }
        s++;
    }
    return true;
}

static int parse_m3u(char *text, media_entry_t **out)
{
    media_entry_t *entries = heap_caps_malloc(sizeof(media_entry_t) * LIST_MAX_ENTRIES,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (entries == NULL) {
        entries = malloc(sizeof(media_entry_t) * LIST_MAX_ENTRIES);
    }
    if (entries == NULL) {
        return -1;
    }
    memset(entries, 0, sizeof(media_entry_t) * LIST_MAX_ENTRIES);
    int count = 0;
    char pending_name[LIST_NAME_MAX] = {0};

    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        /* strip \r */
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\r' || line[l - 1] == ' ' || line[l - 1] == '\t')) {
            line[--l] = '\0';
        }
        if (l == 0 || line_is_ws(line)) {
            continue;
        }
        if (strncmp(line, "#EXTINF", 7) == 0) {
            const char *comma = strrchr(line, ',');
            const char *name = comma ? comma + 1 : NULL;
            if (name == NULL || *name == '\0') {
                name = "未命名频道";
            }
            snprintf(pending_name, sizeof(pending_name), "%s", name);
            sanitize_utf8(pending_name);
            continue;
        }
        if (line[0] == '#') {
            continue;
        }
        /* URL line */
        if (count >= LIST_MAX_ENTRIES) {
            ESP_LOGW(TAG, "playlist truncated at %d entries", count);
            break;
        }
        char *name = dup_str(pending_name[0] ? pending_name : "未命名频道", LIST_NAME_MAX);
        char *url = dup_str(line, LIST_URL_MAX);
        if (name == NULL || url == NULL) {
            free(name);
            free(url);
            break;
        }
        sanitize_utf8(url);
        entries[count].name = name;
        entries[count].url = url;
        entries[count].type = classify_url(url);
        pending_name[0] = '\0';
        count++;
    }
    *out = entries;
    return count;
}

static bool http_get(const char *url, char *buf, size_t bufsize, int *out_len, int *out_code)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 8000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return false;
    }
    esp_http_client_set_header(c, "User-Agent", "ESP32-S3-Radio/1.0");
    bool ok = false;
    *out_len = 0;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int code = esp_http_client_get_status_code(c);
        *out_code = code;
        if (code == 200) {
            int total = 0;
            while (total < (int)bufsize - 1) {
                int r = esp_http_client_read(c, buf + total, (int)bufsize - 1 - total);
                if (r > 0) {
                    total += r;
                } else if (r == 0) {
                    ok = true;      /* clean EOF */
                    break;
                } else if (r == -ESP_ERR_HTTP_EAGAIN) {
                    continue;
                } else {
                    ok = total > 0; /* stream cut, keep what we have */
                    break;
                }
            }
            *out_len = total;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (ok) {
        buf[*out_len] = '\0';
    }
    return ok;
}

static void load_task(void *arg)
{
    (void)arg;
    char *url = (char *)arg;
    ESP_LOGI(TAG, "loading playlist: %s", url);

    /* Wait for Wi-Fi (the load may start before the STA is up). */
    int wait = 0;
    while (wifi_app_state() != WIFI_APP_CONNECTED && wait++ < 30) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (wifi_app_state() != WIFI_APP_CONNECTED) {
        ESP_LOGW(TAG, "no Wi-Fi, playlist load aborted");
        s_state = MEDIA_LIST_FAILED;
        free(url);
        vTaskDelete(NULL);
        return;
    }

    char *buf = heap_caps_malloc(LIST_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        s_state = MEDIA_LIST_FAILED;
        free(url);
        vTaskDelete(NULL);
        return;
    }
    int len = 0, code = 0;
    bool ok = http_get(url, buf, LIST_BUF_SIZE, &len, &code);
    if (!ok || len == 0) {
        ESP_LOGW(TAG, "playlist fetch failed (http=%d)", code);
        s_state = MEDIA_LIST_FAILED;
        free(buf);
        free(url);
        vTaskDelete(NULL);
        return;
    }

    media_entry_t *entries = NULL;
    int count = parse_m3u(buf, &entries);
    free(buf);
    if (count <= 0) {
        ESP_LOGW(TAG, "no channels parsed");
        if (entries) free(entries);
        s_state = MEDIA_LIST_FAILED;
        free(url);
        vTaskDelete(NULL);
        return;
    }

    /* Swap in atomically. */
    media_entry_t *old = s_entries;
    int old_count = s_count;
    s_entries = entries;
    s_count = count;
    s_state = MEDIA_LIST_OK;
    free_entries(old, old_count);

    int n_udp = 0;
    for (int i = 0; i < count; i++) {
        if (media_list_type(i) == MEDIA_UDP) n_udp++;
    }
    ESP_LOGI(TAG, "playlist ready: %d channels (%d udp multicast)", count, n_udp);
    free(url);
    vTaskDelete(NULL);
}

void media_list_start_load(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        url = TV_LIST_URL;
    }
    snprintf(s_source, sizeof(s_source), "%s", url);
    s_state = MEDIA_LIST_LOADING;
    char *arg = strdup(url);
    if (arg == NULL) {
        s_state = MEDIA_LIST_FAILED;
        return;
    }
    if (xTaskCreate(load_task, "m3u_load", 8192, arg, 4, NULL) != pdPASS) {
        free(arg);
        s_state = MEDIA_LIST_FAILED;
    }
}

media_list_state_t media_list_state(void)
{
    return s_state;
}

int media_list_count(void)
{
    return s_count;
}

const char *media_list_name(int idx)
{
    if (idx < 0 || idx >= s_count || s_entries == NULL || s_entries[idx].name == NULL) {
        return "";
    }
    return s_entries[idx].name;
}

const char *media_list_url(int idx)
{
    if (idx < 0 || idx >= s_count || s_entries == NULL || s_entries[idx].url == NULL) {
        return "";
    }
    return s_entries[idx].url;
}

media_type_t media_list_type(int idx)
{
    if (idx < 0 || idx >= s_count || s_entries == NULL) {
        return MEDIA_UDP;
    }
    return s_entries[idx].type;
}

const char *media_list_source(void)
{
    return s_source;
}
