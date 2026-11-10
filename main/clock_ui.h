#pragma once

/* Build the whole desktop-clock UI (status bar + clock/weather/radio tileview). */
void clock_ui_build(void);

/* Update the clock page's volume slider without re-triggering audio_set_vol().
 * Used by the radio page so both sliders stay in sync. */
void clock_ui_set_volume(int vol);
