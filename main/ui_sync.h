#pragma once

/* Volume is shared by four pages (clock / radio / TV / settings); one change
 * must update the audio device and every slider. Setters are loop-safe. */
void ui_sync_volume(int vol);
