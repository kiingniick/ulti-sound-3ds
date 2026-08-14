#pragma once

#include <citro2d.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Small LRU cache of folder cover-art textures, used by the Cover Flow
 * visualizer to show several album covers scrolling at once. */

void covers_init(void);
void covers_exit(void);

/* Return a cached cover image for `folderPath` (loads + caches on first use),
 * or NULL if the folder has no usable cover art. `outHas` (optional) reports
 * whether a real cover exists vs. a miss. */
const C2D_Image* covers_get(const char* folderPath);

#ifdef __cplusplus
}
#endif
