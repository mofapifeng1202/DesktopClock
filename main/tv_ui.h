#pragma once

#include "lvgl.h"

/* Build the IPTV ("网络电视") page: paginated channel list from the runtime
 * m3u playlist + now-playing card with spectrum. */
void tv_ui_build(lv_obj_t *parent);

/* Update the TV page volume slider without triggering callbacks. */
void tv_ui_set_volume(int vol);

/* Diagnostics: open/close the channel drawer. */
void tv_ui_debug_drawer(bool open);
