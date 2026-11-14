/*
 * Cross-page UI sync. See ui_sync.h.
 *
 * Each page's volume setter only updates its own widgets (guarded against
 * recursion), so fanning out here is safe and the sliders cannot fight.
 */
#include "clock_ui.h"
#include "main.h"
#include "radio_ui.h"
#include "settings.h"
#include "settings_ui.h"
#include "tv_ui.h"
#include "ui_sync.h"

void ui_sync_volume(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    audio_set_vol(vol);
    settings_set_volume(vol);   /* persisted by the background save tick */
    clock_ui_set_volume(vol);
    radio_ui_set_volume(vol);
    tv_ui_set_volume(vol);
    settings_ui_set_volume(vol);
}
