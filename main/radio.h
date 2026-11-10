#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Network radio engine: HTTP(S) MP3 stream -> software MP3 decode -> ES8311.
 *
 * The engine runs two tasks:
 *   radio_net  - keeps the HTTP connection alive and fills a PSRAM ring buffer
 *   radio_play - decodes MP3 frames from the ring buffer and writes PCM to I2S
 *
 * The UI only talks to the small API below; status is polled.
 */

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
    int station;              /* index into the preset table */
    int bitrate_kbps;
    int sample_rate;
    int channels;
    int buffer_pct;           /* 0..100 of the compressed-data ring buffer */
    int level;                /* 0..100 smoothed output RMS */
    bool buffering;           /* player ran dry while state == PLAYING */
    uint32_t stream_id;       /* changes on every new connection */
    char error[96];
} radio_status_t;

/* Create mutex/queue/ring buffer. Call once before building the UI. */
void radio_init(void);

/* Start the network/player tasks. Call after Wi-Fi has been initialised. */
void radio_start(void);

void radio_play(int station);
void radio_toggle_pause(void);
void radio_next(void);
void radio_prev(void);
void radio_stop(void);

void radio_get_status(radio_status_t *out);
int radio_station_count(void);
const char *radio_station_name(int idx);
const char *radio_station_desc(int idx);
