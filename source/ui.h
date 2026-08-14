#pragma once

#include "app.h"

#ifdef __cplusplus
extern "C" {
#endif

bool ui_init(void);
void ui_exit(void);

/* Switch between the light and dark colour themes. */
void ui_set_theme(bool dark);

/* Render a full frame (both screens) for the given app state. */
void ui_render(AppState* app);

/* Number of browser rows that fit on the bottom screen. */
int  ui_visible_rows(void);

/* Map a bottom-screen touch y coordinate to a visible row (0-based),
 * or -1 if the touch is outside the list area. */
int  ui_row_at_touch(int py);

/* True if a bottom-screen touch hit the search (magnifier) button. */
bool ui_touch_in_search_button(int px, int py);

/* True if a bottom-screen touch hit the Cover Flow button (music title bar). */
bool ui_touch_coverflow_button(int px, int py);

/* Which tab (0=Music,1=Movies,2=Settings) a bottom-screen touch hit in the
 * tab bar, or -1 if the touch was not on the tab bar. */
int  ui_touch_tab(int px, int py);

/* Settings interaction helpers. */
int  ui_settings_row_at_touch(int py);   /* visible row (0-based) or -1 */
int  ui_touch_dir(int px);               /* -1 left third, +1 right third, 0 middle */

/* Video transport (bottom screen) touch hit-testing. */
typedef enum { VC_NONE = 0, VC_PLAYPAUSE, VC_STOP, VC_SEEK, VC_BACK } VideoCtl;
VideoCtl ui_video_hit(int px, int py, float* outFrac);

/* Cover Flow bottom-screen transport touch hit-testing. */
typedef enum { CF_NONE = 0, CF_PREV, CF_NEXT, CF_OPEN, CF_BACK } CoverFlowCtl;
CoverFlowCtl ui_coverflow_hit(int px, int py);

/* EQ editor: band index a bottom-screen touch hit (-1 if none). When a band is
 * hit, *outDb receives the dB value that corresponds to the touch height. */
int ui_eq_hit(int px, int py, int* outDb);

#ifdef __cplusplus
}
#endif
