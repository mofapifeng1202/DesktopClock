#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Media stream engine: HTTP(S) stream -> (optional TS/HLS parse) -> decoder
 * -> ES8311/I2S.
 *
 * Two content sources share one pipeline:
 *   - radio presets (MP3 streams, hardcoded in radio_stations.h)
 *   - TV channels  (TS over HTTP / HLS, loaded at runtime by media_list)
 *
 * Two tasks:
 *   radio_net  - HTTP(S) fetch, HLS segment loop, fills a PSRAM ring buffer
 *   radio_play - feeds the decoder and writes PCM to I2S
 *
 * The UI only talks to the small API below; status is polled.
 */

typedef enum {
    MEDIA_MP3 = 0,   /* plain MP3 stream (radio presets) */
    MEDIA_TS,        /* MPEG-TS over HTTP, audio extracted by the decoder */
    MEDIA_HLS,       /* HLS playlist (.m3u8) of TS segments */
    MEDIA_UDP,       /* udp/rtp multicast -- not supported, needs udpxy */
} media_type_t;

/* Which channel list a stream/playback request belongs to. Each UI page
 * controls and reports only its own source. */
typedef enum {
    CTX_PRESET = 0,  /* radio presets (radio_stations.h) */
    CTX_TV,          /* IPTV channels (media_list) */
} media_ctx_t;

typedef enum {
    RADIO_STATE_IDLE = 0,
    RADIO_STATE_CONNECTING,
    RADIO_STATE_BUFFERING,
    RADIO_STATE_PLAYING,
    RADIO_STATE_PAUSED,
    RADIO_STATE_ERROR,
} radio_state_t;

typedef struct {
    radio_state_t state;
    media_ctx_t ctx;          /* which list the current stream belongs to */
    int station;              /* index into the active list */
    char name[48];            /* what is playing right now */
    int bitrate_kbps;
    int sample_rate;
    int channels;
    int buffer_pct;           /* 0..100 of the compressed-data ring buffer */
    int level;                /* 0..100 smoothed output RMS */
    bool buffering;           /* player ran dry while state == PLAYING */
    uint32_t stream_id;       /* changes on every new connection */
    char error[96];
} radio_status_t;

/* Create mutex/queue/ring buffer and register decoders. Call once before
 * building the UI. Also kicks off the TV playlist load. */
void radio_init(void);

/* Start the network/player tasks. Call after Wi-Fi has been initialised. */
void radio_start(void);

void radio_play(int station);      /* radio preset (MP3) */
void radio_play_tv(int idx);       /* TV channel (media_list entry) */
void radio_pause_toggle(media_ctx_t ctx);  /* play/pause one page's source */
void radio_next(media_ctx_t ctx);
void radio_prev(media_ctx_t ctx);
void radio_stop(void);

void radio_get_status(radio_status_t *out);
int radio_station_count(void);
const char *radio_station_name(int idx);
const char *radio_station_desc(int idx);
