#pragma once

#include <stdint.h>

/*
 * Minimal MPEG-TS demuxer for IPTV: PAT -> PMT -> audio PES -> elementary
 * stream. Only the audio stream is extracted; video and other PIDs are
 * dropped. The output (AAC ADTS / MP3 ES bytes) feeds esp_audio_simple_dec,
 * which does its own frame parsing.
 *
 * feed() accepts arbitrary-length input and keeps the tail for packet
 * alignment. Extracted elementary-stream bytes are appended to the
 * caller-provided buffer (es_buf/es_cap, new length in *es_len).
 */

void ts_demux_reset(void);

/* Consume raw TS bytes; returns the number of bytes consumed (a partial
 * packet tail may be kept internally). Extracted ES bytes are appended to
 * es_buf (capacity es_cap, updated length in *es_len). */
int ts_demux_feed(const uint8_t *data, int len, uint8_t *es_buf, int es_cap, int *es_len);

/* True once a PMT with a known audio stream has been seen. */
bool ts_demux_audio_ready(void);
