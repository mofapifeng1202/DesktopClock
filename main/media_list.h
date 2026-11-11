#pragma once

#include <stdbool.h>
#include "radio.h"

/*
 * Runtime IPTV channel list, loaded from an m3u playlist on the LAN.
 *
 * media_list_start_load() spawns a task that waits for Wi-Fi, fetches the
 * playlist and parses #EXTINF names + URLs. Entries are kept in PSRAM and
 * are read by the engine and the TV page while the load task may refresh
 * them -- names/urls are only swapped in atomically after a full parse.
 */

typedef enum {
    MEDIA_LIST_IDLE = 0,
    MEDIA_LIST_LOADING,
    MEDIA_LIST_OK,
    MEDIA_LIST_FAILED,
} media_list_state_t;

/* Start (or restart) loading the playlist from url. Non-blocking. */
void media_list_start_load(const char *url);

media_list_state_t media_list_state(void);
int media_list_count(void);
const char *media_list_name(int idx);      /* "" when out of range */
const char *media_list_url(int idx);       /* "" when out of range */
media_type_t media_list_type(int idx);     /* MEDIA_UDP when out of range */
const char *media_list_source(void);       /* configured playlist URL */
