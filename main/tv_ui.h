#pragma once

#include "lvgl.h"

/* Build the IPTV ("网络电视") page: paginated channel list from the runtime
 * m3u playlist + now-playing card with spectrum. */
void tv_ui_build(lv_obj_t *parent);
