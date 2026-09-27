#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include "app.h"
#include "ui.h"
#include "audio.h"
#include "library.h"
#include "decoder.h"
#include "art.h"
#include "video.h"
#include "covers.h"
#include "net.h"
#include "online.h"

#define SD_ROOT     "sdmc:/"
#define CFG_DIR     "sdmc:/3ds/ulti-sound"
#define CFG_PATH    "sdmc:/3ds/ulti-sound/settings.cfg"

static void set_status(AppState* app, const char* msg) {
    strncpy(app->status, msg, sizeof(app->status) - 1);
    app->status[sizeof(app->status) - 1] = 0;
    app->statusTimer = 2.5f;
}

/* ------------------------- playlist management ------------------------- */

static void free_playlist(AppState* app) {
    if (app->plPaths) {
        for (int i = 0; i < app->plCount; ++i) free(app->plPaths[i]);
        free(app->plPaths);
    }
    if (app->plNames) {
        for (int i = 0; i < app->plCount; ++i) free(app->plNames[i]);
        free(app->plNames);
    }
    app->plPaths = NULL;
    app->plNames = NULL;
    app->plCount = 0;
    app->plIndex = -1;
}

/* Directory portion of a path (keeps the trailing slash). */
static void dir_of(const char* path, char* out, size_t n) {
    strncpy(out, path, n - 1);
    out[n - 1] = 0;
    char* slash = strrchr(out, '/');
    if (slash) slash[1] = 0;
    else out[0] = 0;
}

/* Case-insensitive substring search. */
static const char* stristr_(const char* hay, const char* needle) {
    if (!*needle) return hay;
    for (; *hay; ++hay) {
        const char* h = hay;
        const char* n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { ++h; ++n; }
        if (!*n) return hay;
    }
    return NULL;
}

static void basename_of(const char* dirPath, char* out, size_t n) {
    char tmp[LIB_PATH_MAX];
    strncpy(tmp, dirPath, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    size_t len = strlen(tmp);
    if (len && tmp[len - 1] == '/') tmp[--len] = 0;
    char* slash = strrchr(tmp, '/');
    const char* base = slash ? slash + 1 : tmp;
    if (!*base) base = "SD Card";
    strncpy(out, base, n - 1);
    out[n - 1] = 0;
}

/* Capture the audio files of the currently browsed folder as the queue. */
static bool build_playlist(AppState* app) {
    free_playlist(app);

    int n = library_audio_count(&app->lib);
    if (n == 0) return false;

    app->plPaths = (char**)calloc(n, sizeof(char*));
    app->plNames = (char**)calloc(n, sizeof(char*));
    if (!app->plPaths || !app->plNames) { free_playlist(app); return false; }

    int k = 0;
    for (int i = 0; i < app->lib.count && k < n; ++i) {
        if (!app->lib.entries[i].isAudio) continue;
        char full[LIB_PATH_MAX];
        library_entry_path(&app->lib, i, full, sizeof(full));
        app->plPaths[k] = strdup(full);
        app->plNames[k] = strdup(app->lib.entries[i].name);
        ++k;
    }
    app->plCount = k;
    app->plIndex = -1;

    strncpy(app->plFolder, app->lib.path, sizeof(app->plFolder) - 1);
    app->plFolder[sizeof(app->plFolder) - 1] = 0;
    basename_of(app->lib.path, app->plFolderName, sizeof(app->plFolderName));
    return true;
}

static void play_index(AppState* app, int i) {
    if (i < 0 || i >= app->plCount) return;
    if (audio_play(app->plPaths[i])) {
        app->plIndex = i;
        app->hasTrack = true;
        /* Derive the now-playing folder from the track path so both folder
         * playback and cross-folder search results show the right cover. */
        char dir[LIB_PATH_MAX];
        dir_of(app->plPaths[i], dir, sizeof(dir));
        basename_of(dir, app->plFolderName, sizeof(app->plFolderName));
        strncpy(app->npFolder, dir, sizeof(app->npFolder) - 1);
        app->npFolder[sizeof(app->npFolder) - 1] = 0;
        art_set_folder(dir);
    } else {
        set_status(app, "Failed to open track");
    }
}

/* ------------------------------ search ------------------------------- */

#define SEARCH_MAX_RESULTS 600
#define SEARCH_MAX_DEPTH   10

static void free_search(AppState* app) {
    if (app->srPaths) { for (int i = 0; i < app->srCount; ++i) free(app->srPaths[i]); free(app->srPaths); }
    if (app->srNames) { for (int i = 0; i < app->srCount; ++i) free(app->srNames[i]); free(app->srNames); }
    app->srPaths = app->srNames = NULL;
    app->srCount = app->srCap = 0;
    app->srSelected = app->srScroll = 0;
}

static void search_add(AppState* app, const char* full, const char* name) {
    if (app->srCount >= app->srCap) {
        int ncap = app->srCap ? app->srCap * 2 : 64;
        char** np = (char**)realloc(app->srPaths, ncap * sizeof(char*));
        char** nn = (char**)realloc(app->srNames, ncap * sizeof(char*));
        if (!np || !nn) { free(np); free(nn); return; }
        app->srPaths = np; app->srNames = nn; app->srCap = ncap;
    }
    app->srPaths[app->srCount] = strdup(full);
    app->srNames[app->srCount] = strdup(name);
    app->srCount++;
}

static void search_walk(AppState* app, const char* dir, const char* q, int depth, int* budget) {
    if (app->srCount >= SEARCH_MAX_RESULTS || depth > SEARCH_MAX_DEPTH || *budget <= 0) return;
    DIR* d = opendir(dir);
    if (!d) return;

    /* Name of the folder we're inside (its album/compilation title). If it
     * matches the query, every track in it counts as a hit -- this is what
     * makes searching a "Compilations" album by its title return its songs. */
    char folderName[LIB_NAME_MAX];
    basename_of(dir, folderName, sizeof(folderName));
    bool folderMatches = stristr_(folderName, q) != NULL;

    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (app->srCount >= SEARCH_MAX_RESULTS || *budget <= 0) break;
        (*budget)--;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;

        char child[LIB_PATH_MAX];
        snprintf(child, sizeof(child), "%s%s", dir, e->d_name);

        bool isDir = false;
        if (e->d_type == DT_DIR) isDir = true;
        else if (e->d_type == DT_UNKNOWN) {
            struct stat st;
            if (stat(child, &st) == 0) isDir = S_ISDIR(st.st_mode);
        }

        if (isDir) {
            char sub[LIB_PATH_MAX];
            snprintf(sub, sizeof(sub), "%s/", child);
            search_walk(app, sub, q, depth + 1, budget);
        } else if (decoder_is_supported(e->d_name) &&
                   (stristr_(e->d_name, q) || folderMatches)) {
            search_add(app, child, e->d_name);
        }
    }
    closedir(d);
}

static void do_search(AppState* app) {
    SwkbdState kbd;
    char buf[96] = "";
    swkbdInit(&kbd, SWKBD_TYPE_QWERTY, 2, sizeof(buf) - 1);
    swkbdSetHintText(&kbd, "Search songs by name");
    SwkbdButton btn = swkbdInputText(&kbd, buf, sizeof(buf));
    if (btn == SWKBD_BUTTON_LEFT || btn == SWKBD_BUTTON_NONE) return; /* cancelled */
    if (buf[0] == 0) return;

    strncpy(app->searchQuery, buf, sizeof(app->searchQuery) - 1);
    app->searchQuery[sizeof(app->searchQuery) - 1] = 0;

    free_search(app);
    int budget = 300000;
    search_walk(app, SD_ROOT, buf, 0, &budget);

    app->searchActive = true;
    app->srSelected = 0;
    app->srScroll = 0;

    char msg[64];
    snprintf(msg, sizeof(msg), "%d result(s)", app->srCount);
    set_status(app, msg);
}

/* Make the play queue be the current search results. */
static void queue_from_search(AppState* app) {
    free_playlist(app);
    if (app->srCount == 0) return;
    app->plPaths = (char**)calloc(app->srCount, sizeof(char*));
    app->plNames = (char**)calloc(app->srCount, sizeof(char*));
    if (!app->plPaths || !app->plNames) { free_playlist(app); return; }
    for (int i = 0; i < app->srCount; ++i) {
        app->plPaths[i] = strdup(app->srPaths[i]);
        app->plNames[i] = strdup(app->srNames[i]);
    }
    app->plCount = app->srCount;
    app->plIndex = -1;
    strcpy(app->plFolder, "(search)");
    strncpy(app->plFolderName, "Search", sizeof(app->plFolderName) - 1);
}

static void play_next(AppState* app, bool autoAdvance) {
    if (app->plCount == 0) return;
    int next;
    if (app->shuffle && app->plCount > 1) {
        do { next = rand() % app->plCount; } while (next == app->plIndex);
    } else {
        next = app->plIndex + 1;
        if (next >= app->plCount) {
            if (app->repeat) next = 0;
            else { if (autoAdvance) set_status(app, "End of folder"); return; }
        }
    }
    play_index(app, next);
}

static void play_prev(AppState* app) {
    if (app->plCount == 0) return;
    if (audio_position_sec() > 3.0) { audio_seek_fraction(0.0); return; }
    int prev;
    if (app->shuffle && app->plCount > 1) {
        do { prev = rand() % app->plCount; } while (prev == app->plIndex);
    } else {
        prev = app->plIndex - 1;
        if (prev < 0) prev = app->repeat ? app->plCount - 1 : 0;
    }
    play_index(app, prev);
}

/* Start playing the audio entry at browser index `entryIdx` (must be audio). */
static void play_from_entry(AppState* app, int entryIdx) {
    if (!build_playlist(app)) return;
    int ai = library_audio_index_of(&app->lib, entryIdx);
    if (ai < 0) ai = 0;
    play_index(app, ai);
}

/* ------------------------- browser navigation ------------------------- */

static void clamp_scroll(AppState* app) {
    int visible = ui_visible_rows();
    if (app->selected < 0) app->selected = 0;
    if (app->selected >= app->lib.count) app->selected = app->lib.count - 1;
    if (app->selected < 0) app->selected = 0;

    if (app->selected < app->scroll) app->scroll = app->selected;
    if (app->selected >= app->scroll + visible) app->scroll = app->selected - visible + 1;
    if (app->scroll < 0) app->scroll = 0;
}

static void move_selection(AppState* app, int delta) {
    if (app->lib.count == 0) return;
    app->selected += delta;
    clamp_scroll(app);
}

static void move_search_selection(AppState* app, int delta) {
    if (app->srCount == 0) return;
    int visible = ui_visible_rows();
    app->srSelected += delta;
    if (app->srSelected < 0) app->srSelected = 0;
    if (app->srSelected >= app->srCount) app->srSelected = app->srCount - 1;
    if (app->srSelected < app->srScroll) app->srScroll = app->srSelected;
    if (app->srSelected >= app->srScroll + visible) app->srScroll = app->srSelected - visible + 1;
    if (app->srScroll < 0) app->srScroll = 0;
}

static void play_search_selected(AppState* app) {
    if (app->srCount == 0) return;
    int sel = app->srSelected;
    queue_from_search(app);
    play_index(app, sel);
}

static void enter_selected(AppState* app) {
    if (app->lib.count == 0) return;
    LibEntry* e = &app->lib.entries[app->selected];
    if (e->isDir) {
        if (library_enter(&app->lib, app->selected)) {
            app->selected = 0;
            app->scroll = 0;
        }
    } else if (e->isAudio) {
        play_from_entry(app, app->selected);
    } else if (e->isImage) {
        if (art_set_cover_for_folder(app->lib.path, e->name))
            set_status(app, "Album art set for this folder");
    }
}

static void go_up(AppState* app) {
    if (library_up(&app->lib)) {
        app->selected = 0;
        app->scroll = 0;
    }
}

/* ------------------------------ settings ----------------------------- */

static void settings_load(AppState* app) {
    app->preamp = 100;
    app->vizMode = 0;
    app->darkMode = false;
    int effect = 0, width = 50, dark = 0;
    int nb = audio_eq_band_count();
    int eq[16]; for (int i = 0; i < 16; ++i) eq[i] = 0;

    FILE* f = fopen(CFG_PATH, "rb");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            int v, band;
            if      (sscanf(line, "preamp=%d", &v) == 1) app->preamp = v;
            else if (sscanf(line, "effect=%d", &v) == 1) effect = v;
            else if (sscanf(line, "width=%d",  &v) == 1) width = v;
            else if (sscanf(line, "viz=%d",    &v) == 1) app->vizMode = v;
            else if (sscanf(line, "dark=%d",   &v) == 1) dark = v;
            else if (sscanf(line, "eq%d=%d", &band, &v) == 2 && band >= 0 && band < 16) eq[band] = v;
        }
        fclose(f);
    }
    if (app->preamp < 0) app->preamp = 0;
    if (app->preamp > 300) app->preamp = 300;
    if (app->vizMode < 0 || app->vizMode >= VIZ_COUNT) app->vizMode = 0;

    app->darkMode = dark != 0;
    ui_set_theme(app->darkMode);

    audio_set_preamp(app->preamp);
    audio_set_effect_preset(effect);
    if (effect == audio_effect_count() - 1) {  /* Custom: restore manual EQ */
        for (int i = 0; i < nb; ++i) audio_eq_set(i, eq[i]);
        audio_set_width(width);
    }
}

static void settings_save(AppState* app) {
    mkdir("sdmc:/3ds", 0777);
    mkdir(CFG_DIR, 0777);
    FILE* f = fopen(CFG_PATH, "wb");
    if (!f) return;
    fprintf(f, "preamp=%d\n", app->preamp);
    fprintf(f, "effect=%d\n", audio_get_effect_preset());
    fprintf(f, "width=%d\n",  audio_get_width());
    fprintf(f, "viz=%d\n",    app->vizMode);
    fprintf(f, "dark=%d\n",   app->darkMode ? 1 : 0);
    for (int i = 0; i < audio_eq_band_count(); ++i)
        fprintf(f, "eq%d=%d\n", i, audio_eq_get(i));
    fclose(f);
}

static void set_preamp(AppState* app, int v) {
    if (v < 0) v = 0;
    if (v > 300) v = 300;
    if (v == app->preamp) return;
    app->preamp = v;
    audio_set_preamp(v);
}

/* ------------------------------- video ------------------------------- */

static void stop_video(AppState* app) {
    if (app->videoActive || video_is_playing()) {
        video_stop();
        app->videoActive = false;
    }
}

static void play_video(AppState* app, const char* path) {
    audio_stop();          /* free the CPU/DSP for decoding */
    app->hasTrack = false;
    video_stop();
    if (video_play(path)) {
        app->videoActive = true;
        set_status(app, "Playing video");
    } else {
        set_status(app, "Failed to open video");
    }
}

static void move_vselection(AppState* app, int delta) {
    if (app->vlib.count == 0) return;
    int visible = ui_visible_rows();
    app->vselected += delta;
    if (app->vselected < 0) app->vselected = 0;
    if (app->vselected >= app->vlib.count) app->vselected = app->vlib.count - 1;
    if (app->vselected < app->vscroll) app->vscroll = app->vselected;
    if (app->vselected >= app->vscroll + visible) app->vscroll = app->vselected - visible + 1;
    if (app->vscroll < 0) app->vscroll = 0;
}

static void enter_video_selected(AppState* app) {
    if (app->vlib.count == 0) return;
    LibEntry* e = &app->vlib.entries[app->vselected];
    if (e->isDir) {
        if (library_enter(&app->vlib, app->vselected)) { app->vselected = 0; app->vscroll = 0; }
    } else if (e->isVideo) {
        char full[LIB_PATH_MAX];
        library_entry_path(&app->vlib, app->vselected, full, sizeof(full));
        play_video(app, full);
    }
}

static void video_go_up(AppState* app) {
    if (library_up(&app->vlib)) { app->vselected = 0; app->vscroll = 0; }
}

/* --------------------------- cover picker ---------------------------- */

static void open_picker(AppState* app) {
    library_open(&app->plib, SD_ROOT);
    app->pselected = 0;
    app->pscroll = 0;
    app->pickerActive = true;
}

static void move_pselection(AppState* app, int delta) {
    if (app->plib.count == 0) return;
    int visible = ui_visible_rows();
    app->pselected += delta;
    if (app->pselected < 0) app->pselected = 0;
    if (app->pselected >= app->plib.count) app->pselected = app->plib.count - 1;
    if (app->pselected < app->pscroll) app->pscroll = app->pselected;
    if (app->pselected >= app->pscroll + visible) app->pscroll = app->pselected - visible + 1;
    if (app->pscroll < 0) app->pscroll = 0;
}

static void picker_choose(AppState* app) {
    if (app->plib.count == 0) return;
    LibEntry* e = &app->plib.entries[app->pselected];
    if (e->isDir) {
        if (library_enter(&app->plib, app->pselected)) { app->pselected = 0; app->pscroll = 0; }
        return;
    }
    if (!e->isImage) return;

    char full[LIB_PATH_MAX];
    library_entry_path(&app->plib, app->pselected, full, sizeof(full));
    const char* target = app->npFolder[0] ? app->npFolder : app->lib.path;
    if (art_set_cover_for_folder(target, full)) {
        art_set_folder(target);
        art_reload();
        set_status(app, "Album cover updated");
    } else {
        set_status(app, "Could not set cover");
    }
    app->pickerActive = false;
}

static void picker_back(AppState* app) {
    if (!library_up(&app->plib)) app->pickerActive = false;
    else { app->pselected = 0; app->pscroll = 0; }
}

static void switch_tab(AppState* app, ViewTab tab) {
    if (app->tab == TAB_SETTINGS && tab != TAB_SETTINGS) settings_save(app);
    if (tab != TAB_MUSIC) app->searchActive = false;
    app->tab = tab;
}

/* -------------------------- settings list -------------------------- */

static void move_setsel(AppState* app, int delta) {
    int visible = ui_visible_rows();
    app->setSelected += delta;
    if (app->setSelected < 0) app->setSelected = 0;
    if (app->setSelected >= SET_COUNT) app->setSelected = SET_COUNT - 1;
    if (app->setSelected < app->setScroll) app->setScroll = app->setSelected;
    if (app->setSelected >= app->setScroll + visible) app->setScroll = app->setSelected - visible + 1;
    if (app->setScroll < 0) app->setScroll = 0;
}

static void set_theme(AppState* app, bool dark) {
    app->darkMode = dark;
    ui_set_theme(dark);
}

static void settings_adjust(AppState* app, int row, int delta) {
    switch (row) {
        case SET_THEME:  set_theme(app, !app->darkMode); break;
        case SET_PREAMP: set_preamp(app, app->preamp + delta * 5); break;
        case SET_EFFECT: {
            int n = audio_effect_count();
            audio_set_effect_preset((audio_get_effect_preset() + delta + n) % n);
            break;
        }
        case SET_WIDTH:  audio_set_width(audio_get_width() + delta * 5); break;
        case SET_VIZ:    app->vizMode = (app->vizMode + delta + VIZ_COUNT) % VIZ_COUNT; break;
        default: break;
    }
}

/* --------------------- online album / artist art --------------------- */

static bool str_ieq(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == 0 && *b == 0;
}

/* Parent directory of a folder path (keeps a trailing slash). */
static void parent_of(const char* folder, char* out, size_t n) {
    char tmp[LIB_PATH_MAX];
    strncpy(tmp, folder, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    size_t len = strlen(tmp);
    if (len && tmp[len - 1] == '/') tmp[--len] = 0;   /* drop trailing slash */
    char* slash = strrchr(tmp, '/');
    if (slash) slash[1] = 0; else tmp[0] = 0;
    strncpy(out, tmp, n - 1);
    out[n - 1] = 0;
}

/* Fetch cover (+ artist picture when it's not a compilation) for one folder. */
static OnlineResult online_one(const char* folder) {
    char album[LIB_NAME_MAX];
    basename_of(folder, album, sizeof(album));

    char parent[LIB_PATH_MAX];
    parent_of(folder, parent, sizeof(parent));
    char artist[LIB_NAME_MAX];
    basename_of(parent, artist, sizeof(artist));

    bool comp = str_ieq(artist, "Compilations");
    return online_fetch_art(folder, comp ? NULL : artist, album, !comp);
}

static void download_online_art(AppState* app) {
    if (!net_available()) { set_status(app, "No Wi-Fi connection"); return; }

    const char* folder = app->npFolder[0] ? app->npFolder : app->lib.path;
    char base[LIB_NAME_MAX];
    basename_of(folder, base, sizeof(base));

    set_status(app, "Downloading art...");
    ui_render(app);   /* best-effort: flush the status before we block */

    /* If pointed at the "Compilations" container itself, do each album inside
     * it, matching every child folder by its own title. */
    if (str_ieq(base, "Compilations")) {
        DIR* d = opendir(folder);
        int ok = 0, total = 0;
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.') continue;
                char sub[LIB_PATH_MAX];
                snprintf(sub, sizeof(sub), "%s%s", folder, e->d_name);
                struct stat st;
                if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
                char subf[LIB_PATH_MAX];
                snprintf(subf, sizeof(subf), "%s/", sub);
                ++total;
                if (online_fetch_art(subf, NULL, e->d_name, false) == ONLINE_OK) ++ok;
            }
            closedir(d);
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "Compilations: %d/%d covers", ok, total);
        set_status(app, msg);
        return;
    }

    OnlineResult r = online_one(folder);
    set_status(app, online_result_str(r));
}

static void settings_activate(AppState* app, int row) {
    if (row == SET_COVER)       open_picker(app);
    else if (row == SET_ONLINE_ART) download_online_art(app);
    else if (row == SET_EQ)     { app->eqActive = true; app->eqBand = 0; }
    else if (row == SET_THEME)  set_theme(app, !app->darkMode);
    else if (row == SET_EFFECT || row == SET_VIZ) settings_adjust(app, row, +1);
}

/* ----------------------------- Cover Flow ---------------------------- */

/* Entry index of the sel-th folder in the music library, or -1. */
static int cf_dir_entry(AppState* app, int sel) {
    int nd = 0;
    for (int i = 0; i < app->lib.count; ++i) {
        if (!app->lib.entries[i].isDir) continue;
        if (nd == sel) return i;
        ++nd;
    }
    return -1;
}

static int cf_dir_count(AppState* app) {
    int nd = 0;
    for (int i = 0; i < app->lib.count; ++i)
        if (app->lib.entries[i].isDir) ++nd;
    return nd;
}

static void coverflow_open(AppState* app) {
    app->coverflowActive = true;
    app->searchActive = false;
    app->cfSel = 0;
    app->cfPos = 0.0f;
}

static void coverflow_move(AppState* app, int delta) {
    int nd = cf_dir_count(app);
    if (nd == 0) return;
    app->cfSel += delta;
    if (app->cfSel < 0) app->cfSel = 0;
    if (app->cfSel >= nd) app->cfSel = nd - 1;
}

/* Open the centred album: enter it, then play if it holds audio, else drill in. */
static void coverflow_activate(AppState* app) {
    int ei = cf_dir_entry(app, app->cfSel);
    if (ei < 0) return;
    if (!library_enter(&app->lib, ei)) return;
    app->selected = 0;
    app->scroll = 0;
    if (library_audio_count(&app->lib) > 0) {
        build_playlist(app);
        play_index(app, 0);
        app->coverflowActive = false;   /* jump to now-playing */
    } else {
        /* only subfolders: keep flipping through them */
        app->cfSel = 0;
        app->cfPos = 0.0f;
    }
}

static void coverflow_back(AppState* app) {
    if (library_up(&app->lib)) {
        app->selected = 0; app->scroll = 0;
        app->cfSel = 0; app->cfPos = 0.0f;
    } else {
        app->coverflowActive = false;
    }
}

/* ------------------------- video transport ------------------------- */

static void video_seek_relative(double delta) {
    double dur = video_duration_sec();
    if (dur <= 0) return;
    video_seek_fraction((video_position_sec() + delta) / dur);
}

/* ------------------------------- input ------------------------------- */

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    osSetSpeedupEnable(true);

    romfsInit(); /* harmless if there is no romfs */

    AppState app;
    memset(&app, 0, sizeof(app));
    app.plIndex = -1;
    app.view = VIEW_BROWSER;
    app.tab  = TAB_MUSIC;

    srand((unsigned)svcGetSystemTick());

    if (!ui_init()) return 1;
    art_init();
    covers_init();
    video_init();
    net_init();   /* HTTP client for online album/artist art (harmless if offline) */
    if (!audio_init()) { net_exit(); video_exit(); covers_exit(); art_exit(); ui_exit(); return 1; }

    settings_load(&app);

    library_init(&app.lib, SD_ROOT);
    library_open(&app.lib, SD_ROOT);

    library_init(&app.vlib, SD_ROOT);
    library_set_filter(&app.vlib, LIBF_VIDEO);
    library_open(&app.vlib, SD_ROOT);

    library_init(&app.plib, SD_ROOT);
    library_set_filter(&app.plib, LIBF_IMAGE);

    int holdTimer = 0;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();

        if (kDown & KEY_START) break;

        /* ---- list navigation with key repeat ---- */
        int navUp = 0, navDown = 0;
        if (kDown & KEY_DUP)   navUp = 1;
        if (kDown & KEY_DDOWN) navDown = 1;
        if (kHeld & (KEY_DUP | KEY_DDOWN)) {
            holdTimer++;
            if (holdTimer > 18 && (holdTimer % 3) == 0) {
                if (kHeld & KEY_DUP)   navUp = 1;
                if (kHeld & KEY_DDOWN) navDown = 1;
            }
        } else {
            holdTimer = 0;
        }

        /* ================= state-based input routing ================= */
        if (app.videoActive) {
            /* --- video playback --- */
            if (kDown & KEY_X) video_toggle_pause();
            if (kDown & (KEY_B | KEY_Y)) stop_video(&app);
            if (kDown & KEY_DLEFT)  video_seek_relative(-10.0);
            if (kDown & KEY_DRIGHT) video_seek_relative(+10.0);
            if (video_is_finished()) stop_video(&app);
        } else if (app.coverflowActive) {
            /* --- Cover Flow menu --- */
            if (kDown & KEY_DLEFT)  coverflow_move(&app, -1);
            if (kDown & KEY_DRIGHT) coverflow_move(&app, +1);
            if (kDown & KEY_A) coverflow_activate(&app);
            if (kDown & KEY_B) coverflow_back(&app);
        } else if (app.eqActive) {
            /* --- Equalizer editor --- */
            int nb = audio_eq_band_count();
            if (kDown & KEY_DLEFT)  app.eqBand = (app.eqBand - 1 + nb) % nb;
            if (kDown & KEY_DRIGHT) app.eqBand = (app.eqBand + 1) % nb;
            if (navUp)   audio_eq_set(app.eqBand, audio_eq_get(app.eqBand) + 1);
            if (navDown) audio_eq_set(app.eqBand, audio_eq_get(app.eqBand) - 1);
            if (kDown & KEY_R) audio_set_effect_preset((audio_get_effect_preset() + 1) % audio_effect_count());
            if (kDown & KEY_L) audio_set_effect_preset((audio_get_effect_preset() - 1 + audio_effect_count()) % audio_effect_count());
            if (kDown & (KEY_B | KEY_A)) app.eqActive = false;
        } else if (app.pickerActive) {
            /* --- album-cover image picker --- */
            if (navUp)   move_pselection(&app, -1);
            if (navDown) move_pselection(&app, +1);
            if (kDown & KEY_A) picker_choose(&app);
            if (kDown & KEY_B) picker_back(&app);
        } else if (app.tab == TAB_MOVIES) {
            /* --- movies browser --- */
            if (navUp)   move_vselection(&app, -1);
            if (navDown) move_vselection(&app, +1);
            if (kDown & KEY_A) enter_video_selected(&app);
            if (kDown & KEY_B) video_go_up(&app);
        } else if (app.tab == TAB_SETTINGS) {
            /* --- settings list --- */
            if (navUp)   move_setsel(&app, -1);
            if (navDown) move_setsel(&app, +1);
            if (kDown & KEY_DLEFT)  settings_adjust(&app, app.setSelected, -1);
            if (kDown & KEY_DRIGHT) settings_adjust(&app, app.setSelected, +1);
            if (kHeld & (KEY_DLEFT | KEY_DRIGHT)) {
                static int prTimer = 0;
                prTimer++;
                if (prTimer > 16 && prTimer % 3 == 0)
                    settings_adjust(&app, app.setSelected, (kHeld & KEY_DRIGHT) ? +1 : -1);
            }
            if (kDown & KEY_A) settings_activate(&app, app.setSelected);
        } else {
            /* --- music browser / search --- */
            if (app.searchActive) {
                if (navUp)   move_search_selection(&app, -1);
                if (navDown) move_search_selection(&app, +1);
                if (kDown & KEY_A) play_search_selected(&app);
                if (kDown & KEY_B) app.searchActive = false;
            } else {
                if (navUp)   move_selection(&app, -1);
                if (navDown) move_selection(&app, +1);
                if (kDown & KEY_A) enter_selected(&app);
                if (kDown & KEY_B) go_up(&app);
            }
        }

        /* ============= music transport (not during video) ============= */
        if (!app.videoActive && !app.pickerActive && !app.coverflowActive && !app.eqActive) {
            if (kDown & KEY_X) audio_toggle_pause();

            bool chordSearch = (app.tab == TAB_MUSIC) && (kHeld & KEY_L) && (kHeld & KEY_R);
            if (chordSearch && (kDown & (KEY_L | KEY_R))) {
                do_search(&app);
            } else if (!chordSearch) {
                if (kDown & KEY_R) play_next(&app, false);
                if (kDown & KEY_L) play_prev(&app);
            }
            if (kDown & KEY_Y) { app.shuffle = !app.shuffle; set_status(&app, app.shuffle ? "Shuffle on" : "Shuffle off"); }
            if (kDown & KEY_SELECT) { app.repeat = !app.repeat; set_status(&app, app.repeat ? "Repeat on" : "Repeat off"); }

            /* d-pad L/R seeks music, except in Settings where it adjusts pre-amp */
            if (app.tab != TAB_SETTINGS) {
                if (kDown & KEY_DLEFT)  audio_seek_relative(-5.0);
                if (kDown & KEY_DRIGHT) audio_seek_relative(+5.0);
            }

            circlePosition cp;
            hidCircleRead(&cp);
            static int volTimer = 0;
            if (cp.dy > 40 || cp.dy < -40) {
                volTimer++;
                if (volTimer == 1 || (volTimer > 10 && volTimer % 3 == 0))
                    audio_volume_step(cp.dy > 0 ? +2 : -2);
            } else {
                volTimer = 0;
            }

            /* circle-pad left/right cycles the music visualizer */
            static int vizTimer = 0;
            if ((cp.dx > 90 || cp.dx < -90) && !(cp.dy > 40 || cp.dy < -40)) {
                vizTimer++;
                if (vizTimer == 1) {
                    app.vizMode = (app.vizMode + (cp.dx > 0 ? 1 : VIZ_COUNT - 1)) % VIZ_COUNT;
                    set_status(&app, app.vizMode == VIZ_DEFAULT ? "Visualizer: Default"
                                   : app.vizMode == VIZ_WAVES   ? "Visualizer: Soundwaves"
                                                                : "Visualizer: Spectrum");
                }
            } else {
                vizTimer = 0;
            }
        }

        /* ============================ touch ============================ */
        if ((kDown & KEY_TOUCH) && app.videoActive) {
            touchPosition t;
            hidTouchRead(&t);
            float frac = 0;
            switch (ui_video_hit(t.px, t.py, &frac)) {
                case VC_PLAYPAUSE: video_toggle_pause(); break;
                case VC_STOP:      stop_video(&app);      break;
                case VC_BACK:      stop_video(&app);      break;
                case VC_SEEK:      video_seek_fraction(frac); break;
                default: break;
            }
        } else if ((kDown & KEY_TOUCH) && app.coverflowActive) {
            touchPosition t;
            hidTouchRead(&t);
            switch (ui_coverflow_hit(t.px, t.py)) {
                case CF_PREV: coverflow_move(&app, -1);  break;
                case CF_NEXT: coverflow_move(&app, +1);  break;
                case CF_OPEN: coverflow_activate(&app);  break;
                case CF_BACK: coverflow_back(&app);      break;
                default: break;
            }
        } else if ((kDown & KEY_TOUCH) && app.eqActive) {
            touchPosition t;
            hidTouchRead(&t);
            int db = 0;
            int band = ui_eq_hit(t.px, t.py, &db);
            if (band >= 0) { app.eqBand = band; audio_eq_set(band, db); }
            else if (t.py >= 214) app.eqActive = false;   /* footer = back */
        } else if ((kDown & KEY_TOUCH) && !app.videoActive) {
            touchPosition t;
            hidTouchRead(&t);

            if (app.pickerActive) {
                int row = ui_row_at_touch(t.py);
                if (row >= 0) {
                    int idx = app.pscroll + row;
                    if (idx >= 0 && idx < app.plib.count) { app.pselected = idx; picker_choose(&app); }
                }
            } else {
                int tab = ui_touch_tab(t.px, t.py);
                if (tab >= 0) {
                    switch_tab(&app, (ViewTab)tab);
                } else if (app.tab == TAB_MOVIES) {
                    int row = ui_row_at_touch(t.py);
                    if (row >= 0) {
                        int idx = app.vscroll + row;
                        if (idx >= 0 && idx < app.vlib.count) { app.vselected = idx; enter_video_selected(&app); }
                    }
                } else if (app.tab == TAB_SETTINGS) {
                    int vr = ui_settings_row_at_touch(t.py);
                    if (vr >= 0) {
                        int row = app.setScroll + vr;
                        if (row >= 0 && row < SET_COUNT) {
                            app.setSelected = row;
                            int dir = ui_touch_dir(t.px);
                            bool slider = (row == SET_PREAMP || row == SET_WIDTH);
                            if (slider) { if (dir != 0) settings_adjust(&app, row, dir); }
                            else if (row == SET_COVER || row == SET_EQ || row == SET_ONLINE_ART)
                                settings_activate(&app, row);
                            else settings_adjust(&app, row, dir != 0 ? dir : +1); /* theme/effect/viz */
                        }
                    }
                } else { /* TAB_MUSIC */
                    if (ui_touch_in_search_button(t.px, t.py)) {
                        do_search(&app);
                    } else if (ui_touch_coverflow_button(t.px, t.py)) {
                        coverflow_open(&app);
                    } else {
                        int row = ui_row_at_touch(t.py);
                        if (row >= 0) {
                            if (app.searchActive) {
                                int idx = app.srScroll + row;
                                if (idx >= 0 && idx < app.srCount) { app.srSelected = idx; play_search_selected(&app); }
                            } else {
                                int idx = app.scroll + row;
                                if (idx >= 0 && idx < app.lib.count) { app.selected = idx; enter_selected(&app); }
                            }
                        }
                    }
                }
            }
        }

        /* ---- auto-advance music (only when not watching video) ---- */
        if (!app.videoActive && audio_track_finished()) play_next(&app, true);

        if (app.statusTimer > 0) app.statusTimer -= 1.0f / 60.0f;
        app.uiTime += 1.0f / 60.0f;

        ui_render(&app);
    }

    settings_save(&app);
    stop_video(&app);
    audio_stop();
    free_playlist(&app);
    free_search(&app);
    library_free(&app.lib);
    library_free(&app.vlib);
    library_free(&app.plib);
    net_exit();
    video_exit();
    covers_exit();
    audio_exit();
    art_exit();
    ui_exit();
    romfsExit();
    return 0;
}
