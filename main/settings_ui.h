#pragma once

#include "lvgl.h"

void settings_ui_build(lv_obj_t *parent);

/* Update the settings page's sliders without re-triggering callbacks. */
void settings_ui_set_volume(int vol);
