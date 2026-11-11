/*
 * Minimal MPEG-TS demuxer for IPTV audio. See ts_demux.h.
 *
 * Scope: PAT -> PMT -> pick the first audio stream -> PES payload reassembly.
 * Everything else (video, PCR-only packets, SI tables) is dropped. The radio
 * presets don't use this path.
 *
 * Assumptions that hold for the IPTV relay in use:
 *   - PAT and PMT sections fit in one TS packet (typical for a handful of
 *     streams); sections that span packets are skipped.
 *   - Audio PES packets carry a PES header on their first packet.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"

#include "ts_demux.h"

static const char *TAG = "ts_demux";

#define TS_PKT  188
#define ES_CAP  (32 * 1024)

enum {
    ST_AAC_ADTS   = 0x0F,
    ST_AAC_LATM   = 0x11,
    ST_MPEG1_AUD  = 0x03,
    ST_MPEG2_AUD  = 0x04,
};

static uint8_t s_acc[TS_PKT];    /* partial packet accumulator */
static int s_acc_len;
static int s_pmt_pid = -1;
static int s_audio_pid = -1;
static bool s_audio_seen;        /* a PES header has been seen on audio_pid */
static bool s_logged;            /* one log line per stream setup */
static bool is_audio_type(uint8_t st)
{
    return st == ST_AAC_ADTS || st == ST_AAC_LATM || st == ST_MPEG1_AUD || st == ST_MPEG2_AUD;
}

void ts_demux_reset(void)
{
    s_acc_len = 0;
    s_pmt_pid = -1;
    s_audio_pid = -1;
    s_audio_seen = false;
    s_logged = false;
}

static void handle_section(int pid, const uint8_t *payload, int len)
{
    if (len < 4) {
        return;
    }
    int pointer = payload[0];           /* pointer_field */
    if (pointer + 3 > len) {
        return;
    }
    const uint8_t *sec = payload + 1 + pointer;
    int slen = ((sec[1] & 0x0F) << 8) | sec[2];
    if (3 + slen > len - 1 - pointer) {
        return;                          /* section spans packets: skip */
    }
    if (pid == 0) {
        /* PAT: find the first program's PMT PID. */
        if (sec[0] != 0x00) {
            return;
        }
        int off = 3 + 5;                 /* header + tsid/ver/sec/last */
        int end = 3 + slen - 4;          /* before CRC */
        while (off + 4 <= end) {
            int prog = (sec[off] << 8) | sec[off + 1];
            int pmt = ((sec[off + 2] & 0x1F) << 8) | sec[off + 3];
            if (prog != 0 && s_pmt_pid != pmt) {
                s_pmt_pid = pmt;
                ESP_LOGI(TAG, "PAT: program %d -> PMT PID %d", prog, pmt);
            }
            off += 4;
        }
    } else if (pid == s_pmt_pid && sec[0] == 0x02 && s_audio_pid < 0) {
        /* PMT: pick the first audio stream. */
        int off = 3 + 9;
        int pil = ((sec[off - 2] & 0x0F) << 8) | sec[off - 1];
        off += pil;
        int end = 3 + slen - 4;
        while (off + 5 <= end) {
            uint8_t st = sec[off];
            int epid = ((sec[off + 1] & 0x1F) << 8) | sec[off + 2];
            int esl = ((sec[off + 3] & 0x0F) << 8) | sec[off + 4];
            if (is_audio_type(st) && s_audio_pid != epid) {
                s_audio_pid = epid;
                ESP_LOGI(TAG, "PMT: audio stream type 0x%02X PID %d", st, epid);
            }
            off += 5 + esl;
        }
    }
}

static void handle_packet(const uint8_t *p, uint8_t *es_buf, int es_cap, int *es_len)
{
    int pid = ((p[1] & 0x1F) << 8) | p[2];
    bool pusi = (p[1] >> 6) & 1;
    int afc = (p[3] >> 4) & 3;
    int i = 4;
    if (afc == 0) {
        return;                          /* reserved */
    }
    if (afc & 0x02) {
        int af_len = p[4];
        i += 1 + af_len;
        if (i >= TS_PKT) {
            return;
        }
    }
    int plen = TS_PKT - i;
    const uint8_t *payload = p + i;
    if (plen <= 0) {
        return;
    }

    if (pid == 0 || pid == s_pmt_pid) {
        if (pusi) {
            handle_section(pid, payload, plen);
        }
        return;
    }
    if (pid == s_audio_pid) {
        if (pusi) {
            if (plen < 9 || payload[0] != 0x00 || payload[1] != 0x00 ||
                payload[2] != 0x01) {
                return;                  /* not a PES packet */
            }
            s_audio_seen = true;
            int hdr = 9 + payload[8];    /* 3+2+2 header + flags + hdr data */
            if (hdr > plen) {
                return;
            }
            payload += hdr;
            plen -= hdr;
        } else if (!s_audio_seen) {
            return;                      /* waiting for the packet with header */
        }
        int room = es_cap - *es_len;
        int n = plen < room ? plen : room;
        if (n > 0) {
            memcpy(es_buf + *es_len, payload, n);
            *es_len += n;
            if (n < plen) {
                static bool warned;
                if (!warned) {
                    ESP_LOGW(TAG, "ES buffer overflow, dropping %d bytes", plen - n);
                    warned = true;
                }
            }
        }
    }
}

int ts_demux_feed(const uint8_t *data, int len, uint8_t *es_buf, int es_cap, int *es_len)
{
    int consumed = 0;
    while (consumed < len) {
        /* sync on 0x47 */
        while (consumed < len && data[consumed] != 0x47) {
            consumed++;
        }
        if (len - consumed < TS_PKT) {
            break;                       /* keep the tail for next call */
        }
        const uint8_t *p = data + consumed;
        /* verify next packet starts with 0x47 when possible (resync aid) */
        if (len - consumed >= TS_PKT + 1 && p[TS_PKT] != 0x47) {
            consumed++;                  /* false sync, skip one byte */
            continue;
        }
        handle_packet(p, es_buf, es_cap, es_len);
        consumed += TS_PKT;
    }
    return consumed;
}

bool ts_demux_audio_ready(void)
{
    return s_audio_pid >= 0;
}
