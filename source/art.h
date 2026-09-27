#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void art_init(void);
void art_exit(void);

/* Point the "now playing" cover at a folder. If the folder changed, the
 * previous cover is freed and a new one is loaded (from the folder's cover
 * sidecar / common cover filenames), if any. */
void art_set_folder(const char* folderPath);

/* Force a reload of the current folder's cover (after it was changed). */
void art_reload(void);

/* Reload the artist picture for `folderPath` (or the current folder if NULL).
 * Looks for `_ultisound_artist.jpg` inside the folder. */
void art_reload_artist(const char* folderPath);

bool art_available(void);
bool art_artist_available(void);

/* Draw the current cover into a square box of the given size. Returns false
 * if there is no cover loaded (caller should draw a placeholder). */
bool art_draw(float x, float y, float size);

/* Draw the artist picture (if any) into a square box. Returns false if none. */
bool art_draw_artist(float x, float y, float size);

/* Persist `imageName` as the album art for `folderPath` (applies to every
 * track in that folder). Returns true on success. */
bool art_set_cover_for_folder(const char* folderPath, const char* imageName);

#ifdef __cplusplus
}
#endif
