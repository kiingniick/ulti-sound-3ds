#pragma once

#include "library.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VIEW_BROWSER = 0,
    VIEW_NOWPLAYING
} ViewMode;

typedef enum {
    TAB_MUSIC = 0,
    TAB_MOVIES,
    TAB_SETTINGS
} ViewTab;

typedef enum {
    VIZ_DEFAULT = 0,   /* album art + track info (classic now-playing) */
    VIZ_WAVES,         /* soundwave oscilloscope */
    VIZ_SPECTRUM,      /* frequency spectrum bars */
    VIZ_COUNT
} VizMode;

/* Rows in the Settings list (kept in sync between ui.c and main.c). */
typedef enum {
    SET_THEME = 0,
    SET_PREAMP,
    SET_EFFECT,
    SET_EQ,
    SET_WIDTH,
    SET_VIZ,
    SET_COVER,
    SET_COUNT
} SettingRow;

#define PL_PATH_MAX LIB_PATH_MAX

typedef struct {
    Library  lib;
    int      selected;   /* highlighted row in the browser */
    int      scroll;     /* index of the first visible row */
    ViewMode view;
    ViewTab  tab;        /* active bottom-screen tab */

    /* movies browser (separate from the music library) */
    Library  vlib;
    int      vselected;
    int      vscroll;

    /* settings */
    int      setSelected;   /* highlighted settings row */
    int      setScroll;     /* first visible settings row */
    int      preamp;        /* 0..300 (%) */

    /* visualizer */
    int      vizMode;       /* VizMode */

    /* appearance */
    bool     darkMode;      /* dark theme when true */

    /* multiband EQ editor overlay */
    bool     eqActive;      /* EQ editor is open */
    int      eqBand;        /* selected band in the editor */

    /* Cover Flow browse mode (iPod-style album carousel) */
    bool     coverflowActive;
    int      cfSel;         /* selected folder index */
    float    cfPos;         /* animated carousel position */

    /* album-cover picker (Settings -> choose an SD image) */
    bool     pickerActive;
    Library  plib;
    int      pselected;
    int      pscroll;
    char     npFolder[LIB_PATH_MAX];  /* folder the now-playing track lives in */

    /* video playback */
    bool     videoActive;   /* a movie is currently playing */

    /* playback queue captured from a folder */
    char   plFolder[LIB_PATH_MAX];
    char   plFolderName[LIB_NAME_MAX];
    char** plPaths;
    char** plNames;
    int    plCount;
    int    plIndex;

    bool   hasTrack;
    bool   shuffle;
    bool   repeat;

    /* song search */
    bool   searchActive;          /* bottom screen shows search results */
    char   searchQuery[96];
    char** srPaths;
    char** srNames;
    int    srCount;
    int    srCap;
    int    srSelected;
    int    srScroll;

    float  uiTime;       /* seconds, for subtle animation */
    char   status[128];  /* transient status line */
    float  statusTimer;
} AppState;

#ifdef __cplusplus
}
#endif
