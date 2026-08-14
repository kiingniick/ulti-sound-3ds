#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_STOPPED = 0,
    AUDIO_PLAYING,
    AUDIO_PAUSED
} AudioState;

bool audio_init(void);
void audio_exit(void);

/* Start playing a file. Any current track is stopped first.
 * Returns false if the file could not be opened. */
bool audio_play(const char* path);

void audio_stop(void);
void audio_toggle_pause(void);
void audio_set_paused(bool paused);

AudioState audio_state(void);
bool audio_is_paused(void);

/* True exactly once the current track has finished on its own (not stopped).
 * Used by the UI to auto-advance to the next track. */
bool audio_track_finished(void);

/* Playback position / duration in seconds. */
double audio_position_sec(void);
double audio_duration_sec(void);
uint32_t audio_sample_rate(void);
const char* audio_format_name(void);

/* Seek by a delta in seconds (clamped to the track bounds). */
void audio_seek_relative(double delta_sec);
void audio_seek_fraction(double frac); /* 0.0 .. 1.0 */

/* Volume 0..100. */
void audio_set_volume(int vol);
int  audio_get_volume(void);
void audio_volume_step(int delta);

/* Pre-amp gain 0..300 (%). Multiplies volume; >100 amplifies (can clip). */
void audio_set_preamp(int percent);
int  audio_get_preamp(void);

/* Audio enhancement (DSP): 5-band graphic EQ + virtual-surround width.
 * Presets provide beginner-friendly one-tap sounds; the manual EQ setters
 * switch to the "Custom" preset for audiophile fine-tuning. */
int         audio_effect_count(void);
const char* audio_effect_name(int preset);
int         audio_get_effect_preset(void);
void        audio_set_effect_preset(int preset);

/* Multiband graphic equalizer: EQ_BANDS peaking bands, each -12..+12 dB. */
int         audio_eq_band_count(void);
int         audio_eq_get(int band);          /* dB, -12..+12 */
void        audio_eq_set(int band, int db);
const char* audio_eq_label(int band);        /* e.g. "60", "3.6k" */

int  audio_get_width(void);          /* 0..100 (50 = normal) */
void audio_set_width(int w);

/* Copy up to `max` recent mono samples (normalized -1..1) for a visualizer.
 * Returns the number of samples written. */
int audio_get_wave(float* out, int max);

#ifdef __cplusplus
}
#endif
