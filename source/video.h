#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stable MJPEG (.avi) video player with synchronized PCM audio.
 *
 * The 3DS cannot software-decode H.264/HEVC at usable quality, so videos are
 * played as Motion-JPEG AVI (each frame is a JPEG). Convert other videos with:
 *   ffmpeg -i in.mp4 -vf scale=400:-2 -c:v mjpeg -q:v 5 -c:a pcm_s16le out.avi
 */

void video_init(void);   /* allocate frame textures (call once, main thread)  */
void video_exit(void);   /* free textures                                     */

bool video_play(const char* path);   /* start playback on a worker thread     */
void video_stop(void);
bool video_is_playing(void);
bool video_is_finished(void);        /* reached end on its own                */

void video_toggle_pause(void);
bool video_is_paused(void);
void video_seek_fraction(double frac);   /* 0..1 (scans to nearest frame) */

double video_position_sec(void);
double video_duration_sec(void);
int    video_width(void);
int    video_height(void);
bool   video_has_audio(void);
const char* video_message(void);   /* non-NULL when playback failed (e.g. H.264) */

/* Draw the current frame centered/fitted inside the given box (main thread,
 * inside a citro2d scene). Returns false if no frame is ready yet. */
bool video_draw(float x, float y, float boxW, float boxH);

#ifdef __cplusplus
}
#endif
