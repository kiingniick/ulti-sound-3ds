#include "ui.h"
#include "audio.h"
#include "art.h"
#include "video.h"
#include "covers.h"

#include <citro2d.h>
#include <3ds.h>

#include <string.h>
#include <stdio.h>
#include <math.h>

#define TOP_W 400
#define TOP_H 240
#define BOT_W 320
#define BOT_H 240

#define TITLE_H 24
#define ROW_H   27
#define SCROLLBAR_W 4
#define TABBAR_H 36

/* Settings layout (shared by renderer + touch mapping) */
#define SET_SLIDER_X 16
#define SET_SLIDER_W (BOT_W - 32)
#define SET_SLIDER_Y (TITLE_H + 40)
#define SET_ROW1_Y   (TITLE_H + 64)

static C3D_RenderTarget* s_top;
static C3D_RenderTarget* s_bot;
static C2D_TextBuf s_buf;
static C2D_TextBuf s_meas;

/* iPod-inspired palette (recomputed for light/dark themes) */
static u32 clrBg, clrText, clrDim, clrSelTop, clrSelBot, clrSelText;
static u32 clrBarTop, clrBarBot, clrAccent, clrDivider, clrArtBg, clrArtBorder;
static u32 clrProgBg, clrKnob, clrWhite, clrHeaderText, clrSelRow;
static bool s_dark = false;

int ui_visible_rows(void) {
    return (BOT_H - TITLE_H - TABBAR_H) / ROW_H;
}

int ui_row_at_touch(int py) {
    if (py < TITLE_H) return -1;
    if (py >= BOT_H - TABBAR_H) return -1; /* tab bar area */
    int r = (py - TITLE_H) / ROW_H;
    if (r >= ui_visible_rows()) return -1;
    return r;
}

#define SEARCH_BTN_W 30
#define CFBTN_W      30

bool ui_touch_in_search_button(int px, int py) {
    return py < TITLE_H && px >= (BOT_W - SEARCH_BTN_W);
}

bool ui_touch_coverflow_button(int px, int py) {
    return py < TITLE_H && px < CFBTN_W;
}

int ui_touch_tab(int px, int py) {
    if (py < BOT_H - TABBAR_H) return -1;
    int t = px / (BOT_W / 3);
    if (t < 0) t = 0;
    if (t > 2) t = 2;
    return t;
}

int ui_settings_row_at_touch(int py) {
    if (py < TITLE_H || py >= BOT_H - TABBAR_H) return -1;
    int r = (py - TITLE_H) / ROW_H;
    if (r >= ui_visible_rows()) return -1;
    return r;
}

int ui_touch_dir(int px) {
    if (px < BOT_W * 0.38f) return -1;
    if (px > BOT_W * 0.62f) return +1;
    return 0;
}

/* Video transport bar geometry (bottom screen). */
#define VC_BAR_Y   96
#define VC_BTN_Y   150
#define VC_BTN_R   26

VideoCtl ui_video_hit(int px, int py, float* outFrac) {
    /* seek bar */
    int sx = 24, sw = BOT_W - 48;
    if (py >= VC_BAR_Y - 16 && py <= VC_BAR_Y + 16 && px >= sx && px <= sx + sw) {
        if (outFrac) {
            float f = (float)(px - sx) / (float)sw;
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            *outFrac = f;
        }
        return VC_SEEK;
    }
    /* play/pause (center) and stop (right) buttons */
    if (py >= VC_BTN_Y - VC_BTN_R && py <= VC_BTN_Y + VC_BTN_R) {
        if (px >= BOT_W / 2 - VC_BTN_R && px <= BOT_W / 2 + VC_BTN_R) return VC_PLAYPAUSE;
        if (px >= BOT_W - 70 - VC_BTN_R && px <= BOT_W - 70 + VC_BTN_R) return VC_STOP;
        if (px >= 70 - VC_BTN_R && px <= 70 + VC_BTN_R) return VC_BACK;
    }
    return VC_NONE;
}

/* ---- Cover Flow bottom-screen transport hit-testing ---- */
#define CF_BTN_Y   118
#define CF_BTN_R   26
#define CF_OPEN_R  34

CoverFlowCtl ui_coverflow_hit(int px, int py) {
    if (py >= BOT_H - 30) return CF_BACK;
    if (py >= CF_BTN_Y - CF_OPEN_R && py <= CF_BTN_Y + CF_OPEN_R) {
        if (px >= BOT_W / 2 - CF_OPEN_R && px <= BOT_W / 2 + CF_OPEN_R) return CF_OPEN;
        if (px <= 46 + CF_BTN_R)          return CF_PREV;
        if (px >= BOT_W - 46 - CF_BTN_R)  return CF_NEXT;
    }
    return CF_NONE;
}

/* ---- EQ editor geometry / hit-testing ---- */
#define EQ_AREA_X   20
#define EQ_TRACK_T  (TITLE_H + 26)
#define EQ_TRACK_B  (BOT_H - 54)

int ui_eq_hit(int px, int py, int* outDb) {
    int bands = audio_eq_band_count();
    float areaW = BOT_W - 2 * EQ_AREA_X;
    float colW  = areaW / bands;
    if (px < EQ_AREA_X || px > BOT_W - EQ_AREA_X) return -1;
    if (py < EQ_TRACK_T - 20 || py > EQ_TRACK_B + 20) return -1;  /* not the slider area */
    int band = (int)((px - EQ_AREA_X) / colW);
    if (band < 0) band = 0;
    if (band >= bands) band = bands - 1;
    if (outDb) {
        float t = (float)(EQ_TRACK_B - py) / (float)(EQ_TRACK_B - EQ_TRACK_T);
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        *outDb = (int)(-12.0f + t * 24.0f + 0.5f);
    }
    return band;
}

static void apply_theme(bool dark) {
    s_dark   = dark;
    clrWhite = C2D_Color32(255, 255, 255, 255);
    if (!dark) {
        clrBg         = C2D_Color32(244, 245, 248, 255);
        clrText       = C2D_Color32(24, 24, 28, 255);
        clrDim        = C2D_Color32(130, 132, 140, 255);
        clrSelTop     = C2D_Color32(92, 162, 255, 255);
        clrSelBot     = C2D_Color32(18, 92, 224, 255);
        clrSelText    = C2D_Color32(255, 255, 255, 255);
        clrBarTop     = C2D_Color32(252, 252, 254, 255);
        clrBarBot     = C2D_Color32(198, 201, 210, 255);
        clrAccent     = C2D_Color32(24, 108, 228, 255);
        clrDivider    = C2D_Color32(214, 216, 224, 255);
        clrArtBg      = C2D_Color32(222, 226, 234, 255);
        clrArtBorder  = C2D_Color32(180, 184, 196, 255);
        clrProgBg     = C2D_Color32(206, 209, 218, 255);
        clrKnob       = C2D_Color32(255, 255, 255, 255);
        clrHeaderText = C2D_Color32(40, 44, 52, 255);
        clrSelRow     = C2D_Color32(232, 240, 255, 255);
    } else {
        clrBg         = C2D_Color32(18, 19, 23, 255);
        clrText       = C2D_Color32(233, 235, 240, 255);
        clrDim        = C2D_Color32(140, 144, 154, 255);
        clrSelTop     = C2D_Color32(70, 140, 255, 255);
        clrSelBot     = C2D_Color32(26, 92, 220, 255);
        clrSelText    = C2D_Color32(255, 255, 255, 255);
        clrBarTop     = C2D_Color32(44, 46, 54, 255);
        clrBarBot     = C2D_Color32(26, 27, 32, 255);
        clrAccent     = C2D_Color32(74, 158, 255, 255);
        clrDivider    = C2D_Color32(52, 54, 62, 255);
        clrArtBg      = C2D_Color32(38, 40, 46, 255);
        clrArtBorder  = C2D_Color32(64, 66, 74, 255);
        clrProgBg     = C2D_Color32(58, 60, 68, 255);
        clrKnob       = C2D_Color32(240, 242, 248, 255);
        clrHeaderText = C2D_Color32(226, 228, 234, 255);
        clrSelRow     = C2D_Color32(36, 46, 66, 255);
    }
}

void ui_set_theme(bool dark) { apply_theme(dark); }

bool ui_init(void) {
    gfxInitDefault();
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) return false;
    if (!C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) return false;
    C2D_Prepare();

    s_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    s_bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    if (!s_top || !s_bot) return false;

    s_buf  = C2D_TextBufNew(16384);
    s_meas = C2D_TextBufNew(1024);
    if (!s_buf || !s_meas) return false;

    apply_theme(false);
    return true;
}

void ui_exit(void) {
    if (s_buf)  C2D_TextBufDelete(s_buf);
    if (s_meas) C2D_TextBufDelete(s_meas);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
}

/* ---- text helpers ---- */

static void text(const char* s, float x, float y, float scale, u32 color, u32 align) {
    if (!s || !*s) return;
    C2D_Text t;
    C2D_TextParse(&t, s_buf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor | align, x, y, 0.5f, scale, scale, color);
}

static float text_width(const char* s, float scale) {
    if (!s || !*s) return 0.0f;
    C2D_TextBufClear(s_meas);
    C2D_Text t;
    C2D_TextParse(&t, s_meas, s);
    float w = 0, h = 0;
    C2D_TextGetDimensions(&t, scale, scale, &w, &h);
    return w;
}

/* Draw text truncated with ".." to fit within maxw (left aligned). */
static void text_fit(const char* s, float x, float y, float scale, u32 color, float maxw) {
    if (!s || !*s) return;
    float w = text_width(s, scale);
    if (w <= maxw) { text(s, x, y, scale, color, 0); return; }

    char buf[300];
    int len = (int)strlen(s);
    if (len > (int)sizeof(buf) - 4) len = (int)sizeof(buf) - 4;
    int keep = (int)((float)len * (maxw / w)) - 1;
    if (keep < 1) keep = 1;
    while (keep > 1) {
        memcpy(buf, s, keep);
        buf[keep] = 0;
        strcat(buf, "..");
        if (text_width(buf, scale) <= maxw) break;
        keep -= 1;
    }
    if (keep <= 1) { buf[0] = s[0]; buf[1] = 0; }
    text(buf, x, y, scale, color, 0);
}

static void fmt_time(double sec, char* out, size_t n) {
    if (sec < 0 || isnan(sec)) sec = 0;
    int total = (int)(sec + 0.5);
    int m = total / 60;
    int s = total % 60;
    snprintf(out, n, "%d:%02d", m, s);
}

/* ---- icon helpers ---- */

static void draw_play_icon(float cx, float cy, float r, u32 color) {
    C2D_DrawTriangle(cx - r * 0.6f, cy - r,       color,
                     cx - r * 0.6f, cy + r,       color,
                     cx + r,        cy,           color, 0.5f);
}

static void draw_pause_icon(float cx, float cy, float r, u32 color) {
    float bw = r * 0.55f;
    C2D_DrawRectSolid(cx - r * 0.7f, cy - r, 0.5f, bw, r * 2, color);
    C2D_DrawRectSolid(cx + r * 0.15f, cy - r, 0.5f, bw, r * 2, color);
}

static void draw_note_icon(float cx, float cy, float scale, u32 color) {
    float r = 9.0f * scale;
    /* note heads */
    C2D_DrawCircleSolid(cx - 10 * scale, cy + 16 * scale, 0.5f, r, color);
    C2D_DrawCircleSolid(cx + 14 * scale, cy + 10 * scale, 0.5f, r, color);
    /* stems */
    C2D_DrawRectSolid(cx - 10 * scale + r - 2 * scale, cy - 22 * scale, 0.5f, 3.0f * scale, 40 * scale, color);
    C2D_DrawRectSolid(cx + 14 * scale + r - 2 * scale, cy - 28 * scale, 0.5f, 3.0f * scale, 40 * scale, color);
    /* beam */
    C2D_DrawRectSolid(cx - 10 * scale + r - 2 * scale, cy - 28 * scale, 0.5f, 28 * scale, 6 * scale, color);
}

/* Filled rounded rectangle (approximated with a cross + corner circles). */
static void draw_round_rect(float x, float y, float w, float h, float r, u32 color) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    C2D_DrawRectSolid(x + r, y,     0.5f, w - 2 * r, h,         color);
    C2D_DrawRectSolid(x,     y + r, 0.5f, w,         h - 2 * r, color);
    C2D_DrawCircleSolid(x + r,     y + r,     0.5f, r, color);
    C2D_DrawCircleSolid(x + w - r, y + r,     0.5f, r, color);
    C2D_DrawCircleSolid(x + r,     y + h - r, 0.5f, r, color);
    C2D_DrawCircleSolid(x + w - r, y + h - r, 0.5f, r, color);
}

static void draw_chevron(float x, float y, float h, u32 color) {
    float w = h * 0.55f;
    C2D_DrawTriangle(x,     y,         color,
                     x,     y + h,     color,
                     x + w, y + h / 2, color, 0.5f);
}

static void draw_magnifier(float cx, float cy, float r, u32 color) {
    /* ring + handle */
    C2D_DrawCircleSolid(cx, cy, 0.5f, r, color);
    C2D_DrawCircleSolid(cx, cy, 0.5f, r - 2.0f, clrBarTop);
    C2D_DrawRectSolid(cx + r * 0.5f, cy + r * 0.5f, 0.5f, r * 0.9f, 2.4f, color);
}

static void draw_speaker(float x, float y, float s, u32 color) {
    /* little speaker body + sound arc bars */
    C2D_DrawRectSolid(x, y + s * 0.3f, 0.5f, s * 0.4f, s * 0.4f, color);
    C2D_DrawTriangle(x + s * 0.4f, y + s * 0.5f, color,
                     x + s,        y,            color,
                     x + s,        y + s,        color, 0.5f);
    C2D_DrawRectSolid(x + s * 1.2f, y + s * 0.25f, 0.5f, s * 0.15f, s * 0.5f, color);
}

static void draw_film(float cx, float cy, float s, u32 color) {
    /* film strip: body + sprocket holes on both sides */
    float w = s * 1.6f, h = s;
    C2D_DrawRectSolid(cx - w / 2, cy - h / 2, 0.5f, w, h, color);
    u32 hole = clrBarTop;
    int n = 3;
    float hs = h * 0.18f;
    for (int i = 0; i < n; ++i) {
        float hx = cx - w / 2 + w * (0.2f + 0.3f * i);
        C2D_DrawRectSolid(hx, cy - h / 2 + 2, 0.5f, hs, hs, hole);
        C2D_DrawRectSolid(hx, cy + h / 2 - 2 - hs, 0.5f, hs, hs, hole);
    }
}

/* Cover Flow button icon: three overlapping "cards". */
static void draw_coverflow_icon(float cx, float cy, float s, u32 color) {
    u32 back = s_dark ? C2D_Color32(120, 122, 132, 255) : C2D_Color32(170, 174, 186, 255);
    C2D_DrawRectSolid(cx - s * 1.15f, cy - s * 0.7f, 0.5f, s * 0.9f, s * 1.4f, back);
    C2D_DrawRectSolid(cx + s * 0.25f, cy - s * 0.7f, 0.5f, s * 0.9f, s * 1.4f, back);
    C2D_DrawRectSolid(cx - s * 0.55f, cy - s * 0.95f, 0.5f, s * 1.1f, s * 1.9f, color);
}

static void draw_gear(float cx, float cy, float r, u32 color) {
    C2D_DrawCircleSolid(cx, cy, 0.5f, r, color);
    C2D_DrawCircleSolid(cx, cy, 0.5f, r * 0.45f, clrBarTop);
    for (int i = 0; i < 8; ++i) {
        float a = (float)i * 0.785398f;
        float tx = cx + cosf(a) * r;
        float ty = cy + sinf(a) * r;
        C2D_DrawRectSolid(tx - 1.6f, ty - 1.6f, 0.5f, 3.2f, 3.2f, color);
    }
}

/* Bottom-screen tab bar: Music / Movies / Settings. */
static void draw_tabbar(int active) {
    float y0 = BOT_H - TABBAR_H;
    float cw = BOT_W / 3.0f;

    /* base bar with a soft top edge */
    C2D_DrawRectangle(0, y0, 0.0f, BOT_W, TABBAR_H, clrBarTop, clrBarTop, clrBarBot, clrBarBot);
    C2D_DrawRectSolid(0, y0, 0.0f, BOT_W, 1, clrDivider);
    C2D_DrawRectSolid(0, y0 + 1, 0.0f, BOT_W, 1,
                      s_dark ? C2D_Color32(70, 72, 82, 90) : C2D_Color32(255, 255, 255, 150));

    const char* labels[3] = { "Music", "Movies", "Settings" };
    for (int i = 0; i < 3; ++i) {
        float x = i * cw;
        bool on = (i == active);
        float icx = x + cw / 2.0f;

        if (on) {
            /* rounded accent pill behind the active tab */
            float pw = cw - 12, ph = TABBAR_H - 8;
            draw_round_rect(x + 6, y0 + 4, pw, ph, ph / 2.0f, clrAccent);
        }
        u32 col = on ? clrWhite : clrDim;
        if (i == 0)      draw_note_icon(icx, y0 + 6, 0.30f, col);
        else if (i == 1) draw_film(icx, y0 + 11, 8, col);
        else             draw_gear(icx, y0 + 11, 6, col);
        text(labels[i], icx, y0 + 19, 0.40f, col, C2D_AlignCenter);
    }
}

/* ============================ Bottom screen ============================ */

static void render_browser(AppState* app) {
    C2D_TargetClear(s_bot, clrBg);
    C2D_SceneBegin(s_bot);

    /* title bar */
    C2D_DrawRectangle(0, 0, 0.0f, BOT_W, TITLE_H, clrBarTop, clrBarTop, clrBarBot, clrBarBot);
    C2D_DrawRectSolid(0, TITLE_H - 1, 0.0f, BOT_W, 1, clrDivider);

    /* title text = current folder name (or "Ulti-Sound" at root) */
    const char* title = "Ulti-Sound";
    char folderTitle[LIB_NAME_MAX];
    if (!library_at_root(&app->lib)) {
        char tmp[LIB_PATH_MAX];
        strncpy(tmp, app->lib.path, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = 0;
        size_t n = strlen(tmp);
        if (n && tmp[n - 1] == '/') tmp[--n] = 0;
        char* slash = strrchr(tmp, '/');
        strncpy(folderTitle, slash ? slash + 1 : tmp, sizeof(folderTitle) - 1);
        folderTitle[sizeof(folderTitle) - 1] = 0;
        title = folderTitle;
    }
    {
        /* center the (possibly truncated) title */
        char buf[LIB_NAME_MAX + 4];
        strncpy(buf, title, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        float w = text_width(buf, 0.6f);
        if (w > BOT_W - 60) w = BOT_W - 60;
        text(buf, BOT_W / 2.0f, 4, 0.6f, clrHeaderText, C2D_AlignCenter);
    }

    draw_magnifier(BOT_W - 15, TITLE_H / 2.0f, 6.5f, clrAccent);
    draw_coverflow_icon(15, TITLE_H / 2.0f, 6.0f, clrAccent);

    int visible = ui_visible_rows();
    int count = app->lib.count;

    if (count == 0) {
        text("(no music here)", BOT_W / 2.0f, TITLE_H + 40, 0.55f, clrDim, C2D_AlignCenter);
        text("Put audio in folders on your SD card.", BOT_W / 2.0f, TITLE_H + 66, 0.45f, clrDim, C2D_AlignCenter);
        draw_tabbar(TAB_MUSIC);
        return;
    }

    for (int row = 0; row < visible; ++row) {
        int idx = app->scroll + row;
        if (idx >= count) break;
        LibEntry* e = &app->lib.entries[idx];
        float y = TITLE_H + row * ROW_H;
        bool sel = (idx == app->selected);

        float listW = BOT_W - SCROLLBAR_W;
        if (sel) {
            C2D_DrawRectangle(0, y, 0.0f, listW, ROW_H, clrSelTop, clrSelTop, clrSelBot, clrSelBot);
        } else {
            C2D_DrawRectSolid(0, y + ROW_H - 1, 0.0f, listW, 1, clrDivider);
        }

        u32 tc = sel ? clrSelText : clrText;
        float textX = 10;

        /* now-playing marker */
        bool isPlaying = app->hasTrack &&
                         strcmp(app->plFolder, app->lib.path) == 0 &&
                         app->plIndex >= 0 && app->plIndex < app->plCount &&
                         strcmp(app->plNames[app->plIndex], e->name) == 0;
        if (isPlaying) {
            draw_speaker(6, y + ROW_H / 2.0f - 6, 12, sel ? clrSelText : clrAccent);
            textX = 26;
        }

        text_fit(e->name, textX, y + 6, 0.55f, tc, listW - textX - 18);

        if (e->isDir) {
            draw_chevron(listW - 14, y + ROW_H / 2.0f - 6, 12,
                         sel ? clrSelText : clrDim);
        }
    }

    /* scrollbar */
    if (count > visible) {
        float trackH = BOT_H - TITLE_H - TABBAR_H;
        float thumbH = trackH * ((float)visible / (float)count);
        if (thumbH < 16) thumbH = 16;
        float maxScroll = (float)(count - visible);
        float t = maxScroll > 0 ? (float)app->scroll / maxScroll : 0;
        float thumbY = TITLE_H + t * (trackH - thumbH);
        C2D_DrawRectSolid(BOT_W - SCROLLBAR_W, TITLE_H, 0.0f, SCROLLBAR_W, trackH, clrDivider);
        C2D_DrawRectSolid(BOT_W - SCROLLBAR_W, thumbY, 0.0f, SCROLLBAR_W, thumbH, clrAccent);
    }

    draw_tabbar(TAB_MUSIC);
}

static void render_search(AppState* app) {
    C2D_TargetClear(s_bot, clrBg);
    C2D_SceneBegin(s_bot);

    C2D_DrawRectangle(0, 0, 0.0f, BOT_W, TITLE_H, clrBarTop, clrBarTop, clrBarBot, clrBarBot);
    C2D_DrawRectSolid(0, TITLE_H - 1, 0.0f, BOT_W, 1, clrDivider);

    char title[128];
    snprintf(title, sizeof(title), "Search: %s", app->searchQuery);
    text_fit(title, 10, 4, 0.55f, clrHeaderText, BOT_W - 40);
    draw_magnifier(BOT_W - 15, TITLE_H / 2.0f, 6.5f, clrAccent);

    int visible = ui_visible_rows();
    if (app->srCount == 0) {
        text("No matches.", BOT_W / 2.0f, TITLE_H + 40, 0.55f, clrDim, C2D_AlignCenter);
        text("Press B to go back.", BOT_W / 2.0f, TITLE_H + 66, 0.45f, clrDim, C2D_AlignCenter);
        draw_tabbar(TAB_MUSIC);
        return;
    }

    for (int row = 0; row < visible; ++row) {
        int idx = app->srScroll + row;
        if (idx >= app->srCount) break;
        float y = TITLE_H + row * ROW_H;
        bool sel = (idx == app->srSelected);
        float listW = BOT_W - SCROLLBAR_W;

        if (sel) {
            C2D_DrawRectangle(0, y, 0.0f, listW, ROW_H, clrSelTop, clrSelTop, clrSelBot, clrSelBot);
        } else {
            C2D_DrawRectSolid(0, y + ROW_H - 1, 0.0f, listW, 1, clrDivider);
        }
        u32 tc = sel ? clrSelText : clrText;
        text_fit(app->srNames[idx], 10, y + 6, 0.55f, tc, listW - 28);
    }

    if (app->srCount > visible) {
        float trackH = BOT_H - TITLE_H - TABBAR_H;
        float thumbH = trackH * ((float)visible / (float)app->srCount);
        if (thumbH < 16) thumbH = 16;
        float maxScroll = (float)(app->srCount - visible);
        float t = maxScroll > 0 ? (float)app->srScroll / maxScroll : 0;
        float thumbY = TITLE_H + t * (trackH - thumbH);
        C2D_DrawRectSolid(BOT_W - SCROLLBAR_W, TITLE_H, 0.0f, SCROLLBAR_W, trackH, clrDivider);
        C2D_DrawRectSolid(BOT_W - SCROLLBAR_W, thumbY, 0.0f, SCROLLBAR_W, thumbH, clrAccent);
    }

    draw_tabbar(TAB_MUSIC);
}

/* ----- generic folder/list title bar helper ----- */
static void draw_list_title(const Library* lib, const char* rootTitle) {
    C2D_DrawRectangle(0, 0, 0.0f, BOT_W, TITLE_H, clrBarTop, clrBarTop, clrBarBot, clrBarBot);
    C2D_DrawRectSolid(0, TITLE_H - 1, 0.0f, BOT_W, 1, clrDivider);
    const char* title = rootTitle;
    char folderTitle[LIB_NAME_MAX];
    if (!library_at_root(lib)) {
        char tmp[LIB_PATH_MAX];
        strncpy(tmp, lib->path, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = 0;
        size_t n = strlen(tmp);
        if (n && tmp[n - 1] == '/') tmp[--n] = 0;
        char* slash = strrchr(tmp, '/');
        strncpy(folderTitle, slash ? slash + 1 : tmp, sizeof(folderTitle) - 1);
        folderTitle[sizeof(folderTitle) - 1] = 0;
        title = folderTitle;
    }
    text(title, BOT_W / 2.0f, 4, 0.6f, clrHeaderText, C2D_AlignCenter);
}

static void draw_list_scrollbar(int count, int visible, int scroll) {
    if (count <= visible) return;
    float trackH = BOT_H - TITLE_H - TABBAR_H;
    float thumbH = trackH * ((float)visible / (float)count);
    if (thumbH < 16) thumbH = 16;
    float maxScroll = (float)(count - visible);
    float t = maxScroll > 0 ? (float)scroll / maxScroll : 0;
    float thumbY = TITLE_H + t * (trackH - thumbH);
    C2D_DrawRectSolid(BOT_W - SCROLLBAR_W, TITLE_H, 0.0f, SCROLLBAR_W, trackH, clrDivider);
    C2D_DrawRectSolid(BOT_W - SCROLLBAR_W, thumbY, 0.0f, SCROLLBAR_W, thumbH, clrAccent);
}

/* ----- Movies tab: browse .avi (MJPEG) videos ----- */
static void render_movies(AppState* app) {
    C2D_TargetClear(s_bot, clrBg);
    C2D_SceneBegin(s_bot);
    draw_list_title(&app->vlib, "Movies");

    int visible = ui_visible_rows();
    int count = app->vlib.count;
    if (count == 0) {
        text("(no videos here)", BOT_W / 2.0f, TITLE_H + 40, 0.55f, clrDim, C2D_AlignCenter);
        text("Put MJPEG .avi files on your SD card.", BOT_W / 2.0f, TITLE_H + 64, 0.45f, clrDim, C2D_AlignCenter);
        text("Convert: ffmpeg -i in.mp4 -c:v mjpeg", BOT_W / 2.0f, TITLE_H + 86, 0.4f, clrDim, C2D_AlignCenter);
        text("-q:v 5 -c:a pcm_s16le out.avi", BOT_W / 2.0f, TITLE_H + 104, 0.4f, clrDim, C2D_AlignCenter);
        draw_tabbar(TAB_MOVIES);
        return;
    }

    float listW = BOT_W - SCROLLBAR_W;
    for (int row = 0; row < visible; ++row) {
        int idx = app->vscroll + row;
        if (idx >= count) break;
        LibEntry* e = &app->vlib.entries[idx];
        float y = TITLE_H + row * ROW_H;
        bool sel = (idx == app->vselected);
        if (sel) C2D_DrawRectangle(0, y, 0.0f, listW, ROW_H, clrSelTop, clrSelTop, clrSelBot, clrSelBot);
        else     C2D_DrawRectSolid(0, y + ROW_H - 1, 0.0f, listW, 1, clrDivider);

        u32 tc = sel ? clrSelText : clrText;
        float textX = 10;
        if (!e->isDir) { draw_film(14, y + ROW_H / 2.0f, 8, sel ? clrSelText : clrAccent); textX = 28; }
        text_fit(e->name, textX, y + 6, 0.55f, tc, listW - textX - 18);
        if (e->isDir) draw_chevron(listW - 14, y + ROW_H / 2.0f - 6, 12, sel ? clrSelText : clrDim);
    }
    draw_list_scrollbar(count, visible, app->vscroll);
    draw_tabbar(TAB_MOVIES);
}

/* ----- Settings tab ----- */

/* Fill label/value strings and (for sliders) a 0..1 fraction for a row. */
static void settings_row_info(AppState* app, int row, char* label, char* value,
                              bool* isSlider, bool* isChevron, float* frac) {
    *isSlider = false; *isChevron = false; *frac = 0.0f; value[0] = 0;
    switch (row) {
        case SET_THEME:
            strcpy(label, "Theme");
            snprintf(value, 24, "%s", app->darkMode ? "Dark" : "Light"); break;
        case SET_PREAMP:
            strcpy(label, "Pre-amp gain");
            snprintf(value, 24, "%d%%", app->preamp);
            *isSlider = true; *frac = app->preamp / 300.0f; break;
        case SET_EFFECT:
            strcpy(label, "Sound enhancement");
            snprintf(value, 24, "%s", audio_effect_name(audio_get_effect_preset())); break;
        case SET_EQ:
            strcpy(label, "Equalizer");
            snprintf(value, 24, "%s", audio_get_effect_preset() == audio_effect_count() - 1
                                      ? "Custom" : "Presets");
            *isChevron = true; break;
        case SET_WIDTH:
            strcpy(label, "Stereo width");
            snprintf(value, 24, "%d", audio_get_width());
            *isSlider = true; *frac = audio_get_width() / 100.0f; break;
        case SET_VIZ: {
            static const char* vn[] = { "Default", "Soundwaves", "Spectrum" };
            strcpy(label, "Visualizer");
            snprintf(value, 24, "%s", vn[app->vizMode % VIZ_COUNT]); break;
        }
        case SET_COVER:
            strcpy(label, "Set album cover from SD image");
            *isChevron = true; break;
        case SET_ONLINE_ART:
            strcpy(label, "Download art online (Wi-Fi)");
            *isChevron = true; break;
        default: label[0] = 0; break;
    }
}

static void render_settings(AppState* app) {
    C2D_TargetClear(s_bot, clrBg);
    C2D_SceneBegin(s_bot);

    C2D_DrawRectangle(0, 0, 0.0f, BOT_W, TITLE_H, clrBarTop, clrBarTop, clrBarBot, clrBarBot);
    C2D_DrawRectSolid(0, TITLE_H - 1, 0.0f, BOT_W, 1, clrDivider);
    text("Settings", BOT_W / 2.0f, 4, 0.6f, clrHeaderText, C2D_AlignCenter);

    int visible = ui_visible_rows();
    float listW = BOT_W - SCROLLBAR_W;

    for (int r = 0; r < visible; ++r) {
        int row = app->setScroll + r;
        if (row >= SET_COUNT) break;
        float y = TITLE_H + r * ROW_H;
        bool sel = (row == app->setSelected);

        if (sel) C2D_DrawRectSolid(0, y, 0.0f, listW, ROW_H, clrSelRow);
        else     C2D_DrawRectSolid(0, y + ROW_H - 1, 0.0f, listW, 1, clrDivider);

        char label[64], value[24];
        bool isSlider, isChevron; float frac;
        settings_row_info(app, row, label, value, &isSlider, &isChevron, &frac);

        text(label, 12, y + 6, 0.48f, clrText, 0);

        if (isSlider) {
            float bx = 150, bw = listW - bx - 46, by = y + ROW_H / 2.0f;
            C2D_DrawRectSolid(bx, by - 2, 0.0f, bw, 4, clrProgBg);
            C2D_DrawRectSolid(bx, by - 2, 0.0f, bw * frac, 4, clrAccent);
            C2D_DrawCircleSolid(bx + bw * frac, by, 0.0f, 5, clrAccent);
            text(value, listW - 8, y + 6, 0.44f, clrAccent, C2D_AlignRight);
        } else if (isChevron) {
            if (value[0]) text(value, listW - 26, y + 6, 0.44f, clrDim, C2D_AlignRight);
            draw_chevron(listW - 14, y + ROW_H / 2.0f - 6, 12, sel ? clrAccent : clrDim);
        } else {
            text(value, listW - 12, y + 6, 0.46f, clrAccent, C2D_AlignRight);
        }
    }

    draw_list_scrollbar(SET_COUNT, visible, app->setScroll);
    draw_tabbar(TAB_SETTINGS);
}

/* ----- Video transport controls (bottom screen during playback) ----- */
static void render_video_controls(AppState* app) {
    (void)app;
    C2D_TargetClear(s_bot, clrBg);
    C2D_SceneBegin(s_bot);

    C2D_DrawRectangle(0, 0, 0.0f, BOT_W, TITLE_H, clrBarTop, clrBarTop, clrBarBot, clrBarBot);
    C2D_DrawRectSolid(0, TITLE_H - 1, 0.0f, BOT_W, 1, clrDivider);
    text("Now Playing", BOT_W / 2.0f, 4, 0.6f, clrHeaderText, C2D_AlignCenter);

    if (video_has_audio())
        text("MJPEG video + audio", BOT_W / 2.0f, TITLE_H + 20, 0.44f, clrDim, C2D_AlignCenter);
    else
        text("MJPEG video (no audio track)", BOT_W / 2.0f, TITLE_H + 20, 0.44f, clrDim, C2D_AlignCenter);

    /* seek bar */
    double posS = video_position_sec(), durS = video_duration_sec();
    float sx = 24, sw = BOT_W - 48, sy = 96;
    float frac = durS > 0 ? (float)(posS / durS) : 0;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    C2D_DrawRectSolid(sx, sy, 0.0f, sw, 5, clrProgBg);
    C2D_DrawRectSolid(sx, sy, 0.0f, sw * frac, 5, clrAccent);
    C2D_DrawCircleSolid(sx + sw * frac, sy + 2.5f, 0.0f, 7, clrKnob);
    C2D_DrawCircleSolid(sx + sw * frac, sy + 2.5f, 0.0f, 6, clrAccent);
    char a[16], b[16];
    fmt_time(posS, a, sizeof(a)); fmt_time(durS, b, sizeof(b));
    text(a, sx, sy + 10, 0.42f, clrDim, 0);
    text(b, sx + sw, sy + 10, 0.42f, clrDim, C2D_AlignRight);

    /* buttons: back(<<list) | play/pause | stop */
    float by = 150;
    C2D_DrawCircleSolid(70, by, 0.0f, 24, clrProgBg);
    draw_chevron(70 - 3, by - 8, 16, clrText);  /* back-ish */
    text("List", 70, by + 26, 0.4f, clrDim, C2D_AlignCenter);

    C2D_DrawCircleSolid(BOT_W / 2.0f, by, 0.0f, 26, clrAccent);
    if (video_is_paused()) draw_play_icon(BOT_W / 2.0f + 1, by, 11, clrWhite);
    else                   draw_pause_icon(BOT_W / 2.0f, by, 10, clrWhite);

    C2D_DrawCircleSolid(BOT_W - 70, by, 0.0f, 24, clrProgBg);
    C2D_DrawRectSolid(BOT_W - 70 - 8, by - 8, 0.0f, 16, 16, clrText); /* stop square */
    text("Stop", BOT_W - 70, by + 26, 0.4f, clrDim, C2D_AlignCenter);

    text("Tap the bar to seek", BOT_W / 2.0f, BOT_H - 16, 0.4f, clrDim, C2D_AlignCenter);
}

/* ----- Album-cover image picker overlay ----- */
static void render_picker(AppState* app) {
    C2D_TargetClear(s_bot, clrBg);
    C2D_SceneBegin(s_bot);
    draw_list_title(&app->plib, "Choose cover image");

    int visible = ui_visible_rows();
    int count = app->plib.count;
    float listW = BOT_W - SCROLLBAR_W;
    if (count == 0) {
        text("(no images in this folder)", BOT_W / 2.0f, TITLE_H + 40, 0.5f, clrDim, C2D_AlignCenter);
    }
    for (int row = 0; row < visible; ++row) {
        int idx = app->pscroll + row;
        if (idx >= count) break;
        LibEntry* e = &app->plib.entries[idx];
        float y = TITLE_H + row * ROW_H;
        bool sel = (idx == app->pselected);
        if (sel) C2D_DrawRectangle(0, y, 0.0f, listW, ROW_H, clrSelTop, clrSelTop, clrSelBot, clrSelBot);
        else     C2D_DrawRectSolid(0, y + ROW_H - 1, 0.0f, listW, 1, clrDivider);
        u32 tc = sel ? clrSelText : clrText;
        text_fit(e->name, 12, y + 6, 0.55f, tc, listW - 30);
        if (e->isDir) draw_chevron(listW - 14, y + ROW_H / 2.0f - 6, 12, sel ? clrSelText : clrDim);
    }
    draw_list_scrollbar(count, visible, app->pscroll);

    /* footer instead of tab bar */
    float y0 = BOT_H - TABBAR_H;
    C2D_DrawRectangle(0, y0, 0.0f, BOT_W, TABBAR_H, clrBarTop, clrBarTop, clrBarBot, clrBarBot);
    C2D_DrawRectSolid(0, y0, 0.0f, BOT_W, 1, clrDivider);
    text("A: choose image    B: back", BOT_W / 2.0f, y0 + 8, 0.46f, clrDim, C2D_AlignCenter);
}

/* ============================ Top screen ============================ */

static void render_header(void) {
    C2D_DrawRectangle(0, 0, 0.0f, TOP_W, TITLE_H, clrBarTop, clrBarTop, clrBarBot, clrBarBot);
    C2D_DrawRectSolid(0, TITLE_H - 1, 0.0f, TOP_W, 1, clrDivider);
    text("Ulti-Sound", TOP_W / 2.0f, 4, 0.6f, clrHeaderText, C2D_AlignCenter);
}

static void render_volume(float x, float y, float w) {
    int vol = audio_get_volume();
    draw_speaker(x, y, 12, clrDim);
    float bx = x + 20;
    float bw = w - 20;
    C2D_DrawRectSolid(bx, y + 4, 0.0f, bw, 4, clrProgBg);
    C2D_DrawRectSolid(bx, y + 4, 0.0f, bw * (vol / 100.0f), 4, clrAccent);
}

/* Shared footer for the alternate visualizers: track name + progress bar. */
static void render_np_footer(AppState* app) {
    const char* name = (app->plIndex >= 0 && app->plIndex < app->plCount)
                        ? app->plNames[app->plIndex] : "";
    text_fit(name, 12, TITLE_H + 4, 0.5f, clrText, TOP_W - 24);

    double posS = audio_position_sec();
    double durS = audio_duration_sec();
    float px = 24, py = TOP_H - 24, pw = TOP_W - 48;
    float frac = durS > 0 ? (float)(posS / durS) : 0;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    C2D_DrawRectSolid(px, py, 0.0f, pw, 4, clrProgBg);
    C2D_DrawRectSolid(px, py, 0.0f, pw * frac, 4, clrAccent);

    char a[16], b[16];
    fmt_time(posS, a, sizeof(a));
    fmt_time(durS, b, sizeof(b));
    text(a, px, py + 6, 0.4f, clrDim, 0);
    text(b, px + pw, py + 6, 0.4f, clrDim, C2D_AlignRight);
}

/* In-place iterative radix-2 FFT (N must be a power of two). */
static void fft(float* re, float* im, int n) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = re[i]; re[i] = re[j]; re[j] = tr;
            float ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * 3.14159265358979f / (float)len;
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cwr = 1.0f, cwi = 0.0f;
            for (int k = 0; k < len / 2; ++k) {
                int a = i + k, b = i + k + len / 2;
                float vr = re[b] * cwr - im[b] * cwi;
                float vi = re[b] * cwi + im[b] * cwr;
                re[b] = re[a] - vr; im[b] = im[a] - vi;
                re[a] += vr;        im[a] += vi;
                float nwr = cwr * wr - cwi * wi;
                cwi = cwr * wi + cwi * wr;
                cwr = nwr;
            }
        }
    }
}

#define SPEC_N    512
#define SPEC_BARS 28

static void render_np_spectrum(AppState* app) {
    static float re[SPEC_N], im[SPEC_N];
    static float bars[SPEC_BARS] = { 0 };
    static float peaks[SPEC_BARS] = { 0 };

    int n = audio_get_wave(re, SPEC_N);
    for (int i = n; i < SPEC_N; ++i) re[i] = 0.0f;
    for (int i = 0; i < SPEC_N; ++i) {
        float w = 0.5f - 0.5f * cosf(2.0f * 3.14159265358979f * i / (SPEC_N - 1));
        re[i] *= w;
        im[i]  = 0.0f;
    }
    fft(re, im, SPEC_N);

    float baseY = TOP_H - 34.0f;
    float topY  = TITLE_H + 22.0f;
    float maxH  = baseY - topY;
    float areaX = 16.0f, areaW = TOP_W - 32.0f;
    float gap   = 3.0f;
    float bw    = (areaW - gap * (SPEC_BARS - 1)) / SPEC_BARS;

    /* log-spaced frequency bins across the lower half of the spectrum */
    int lo = 1, hi = SPEC_N / 2;
    for (int bIdx = 0; bIdx < SPEC_BARS; ++bIdx) {
        float f0 = (float)lo * powf((float)hi / lo, (float)bIdx / SPEC_BARS);
        float f1 = (float)lo * powf((float)hi / lo, (float)(bIdx + 1) / SPEC_BARS);
        int   k0 = (int)f0, k1 = (int)f1;
        if (k1 <= k0) k1 = k0 + 1;
        if (k1 > hi) k1 = hi;
        float mag = 0.0f;
        for (int k = k0; k < k1; ++k) {
            float m = sqrtf(re[k] * re[k] + im[k] * im[k]);
            if (m > mag) mag = m;
        }
        /* log compression + gentle high-frequency lift */
        float v = logf(1.0f + mag * (6.0f + bIdx * 0.5f)) * 0.34f;
        if (v > 1.0f) v = 1.0f;

        if (v > bars[bIdx]) bars[bIdx] = v;                 /* fast attack */
        else                bars[bIdx] += (v - bars[bIdx]) * 0.35f; /* smooth decay */

        if (bars[bIdx] >= peaks[bIdx]) peaks[bIdx] = bars[bIdx];
        else                           peaks[bIdx] -= 0.012f;
        if (peaks[bIdx] < 0) peaks[bIdx] = 0;
    }

    C2D_DrawRectSolid(areaX, baseY + 1, 0.0f, areaW, 1, clrDivider);
    for (int bIdx = 0; bIdx < SPEC_BARS; ++bIdx) {
        float h = bars[bIdx] * maxH;
        if (h < 2) h = 2;
        float x = areaX + bIdx * (bw + gap);
        u32 top = C2D_Color32(120, 190, 255, 255);
        C2D_DrawRectangle(x, baseY - h, 0.0f, bw, h, top, top, clrAccent, clrAccent);
        float pkY = baseY - peaks[bIdx] * maxH;
        C2D_DrawRectSolid(x, pkY - 2, 0.0f, bw, 2, clrText);
    }

    render_np_footer(app);
    text("Spectrum", TOP_W - 12, TOP_H - 40, 0.38f, clrDim, C2D_AlignRight);
}

static void render_np_waves(AppState* app) {
    static float wave[256];
    int n = audio_get_wave(wave, 256);

    float midY = TOP_H / 2.0f + 6;
    float amp  = 78.0f;
    float step = (float)TOP_W / (float)(n - 1);

    /* faint center line */
    C2D_DrawRectSolid(0, midY, 0.0f, TOP_W, 1, clrDivider);

    /* mirrored filled oscilloscope */
    for (int i = 0; i < n - 1; ++i) {
        float x0 = i * step,       x1 = (i + 1) * step;
        float y0 = midY - wave[i]     * amp;
        float y1 = midY - wave[i + 1] * amp;
        C2D_DrawLine(x0, y0, clrAccent, x1, y1, clrAccent, 2.0f, 0.5f);
        float ym0 = midY + wave[i]     * amp;
        float ym1 = midY + wave[i + 1] * amp;
        C2D_DrawLine(x0, ym0, clrSelTop, x1, ym1, clrSelTop, 1.4f, 0.5f);
    }

    render_np_footer(app);
    text("Soundwaves", TOP_W - 12, TOP_H - 40, 0.38f, clrDim, C2D_AlignRight);
}

static void render_np_default(AppState* app) {
    /* album art (per-folder cover) or placeholder */
    float ax = 24, ay = 44, asz = 132;
    C2D_DrawRectSolid(ax - 2, ay - 2, 0.0f, asz + 4, asz + 4, clrArtBorder);
    if (!art_draw(ax, ay, asz)) {
        C2D_DrawRectSolid(ax, ay, 0.0f, asz, asz, clrArtBg);
        draw_note_icon(ax + asz / 2.0f, ay + asz / 2.0f - 6, 1.5f, clrArtBorder);
    }

    /* artist picture badge in the cover's lower-right corner (if downloaded) */
    if (art_artist_available()) {
        float av = 42, avx = ax + asz - av + 4, avy = ay + asz - av + 4;
        C2D_DrawRectSolid(avx - 3, avy - 3, 0.0f, av + 6, av + 6, clrWhite);
        art_draw_artist(avx, avy, av);
    }

    /* track info */
    float tx = ax + asz + 20;
    float tw = TOP_W - tx - 16;
    const char* name = (app->plIndex >= 0 && app->plIndex < app->plCount)
                        ? app->plNames[app->plIndex] : "";
    text_fit(name, tx, 54, 0.62f, clrText, tw);
    text_fit(app->plFolderName, tx, 82, 0.5f, clrDim, tw);

    char info[96];
    snprintf(info, sizeof(info), "%s  %u Hz", audio_format_name(), (unsigned)audio_sample_rate());
    text(info, tx, 106, 0.44f, clrDim, 0);

    char pos[16];
    snprintf(pos, sizeof(pos), "%d / %d", app->plIndex + 1, app->plCount);
    text(pos, tx, 128, 0.44f, clrDim, 0);

    /* playback state + flags */
    float sy = 158;
    if (audio_is_paused())
        draw_pause_icon(tx + 8, sy + 8, 8, clrAccent);
    else if (audio_state() == AUDIO_PLAYING)
        draw_play_icon(tx + 8, sy + 8, 8, clrAccent);
    else
        draw_pause_icon(tx + 8, sy + 8, 8, clrDim);

    char flags[32];
    snprintf(flags, sizeof(flags), "%s%s",
             app->shuffle ? "Shuffle  " : "",
             app->repeat  ? "Repeat"   : "");
    if (flags[0]) text(flags, tx + 26, sy + 2, 0.42f, clrAccent, 0);

    /* progress scrubber */
    double posS = audio_position_sec();
    double durS = audio_duration_sec();
    float px = 24, py = 198, pw = TOP_W - 48;
    float frac = durS > 0 ? (float)(posS / durS) : 0;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;

    C2D_DrawRectSolid(px, py, 0.0f, pw, 5, clrProgBg);
    C2D_DrawRectSolid(px, py, 0.0f, pw * frac, 5, clrAccent);
    C2D_DrawCircleSolid(px + pw * frac, py + 2.5f, 0.0f, 7, clrKnob);
    C2D_DrawCircleSolid(px + pw * frac, py + 2.5f, 0.0f, 7, clrKnob);

    char a[16], b[16];
    fmt_time(posS, a, sizeof(a));
    fmt_time(durS, b, sizeof(b));
    text(a, px, py + 10, 0.42f, clrDim, 0);
    text(b, px + pw, py + 10, 0.42f, clrDim, C2D_AlignRight);

    /* volume, top-right of info column */
    render_volume(tx, 148 - 6, tw);
}

static void render_nowplaying(AppState* app) {
    C2D_TargetClear(s_top, clrBg);
    C2D_SceneBegin(s_top);
    render_header();

    if (!app->hasTrack) {
        draw_note_icon(TOP_W / 2.0f, 70, 1.6f, clrArtBorder);
        text("Ulti-Sound", TOP_W / 2.0f, 120, 1.0f, clrText, C2D_AlignCenter);
        text("Music Player for Nintendo 3DS", TOP_W / 2.0f, 152, 0.5f, clrDim, C2D_AlignCenter);
        text("Pick a song on the touch screen below.", TOP_W / 2.0f, 190, 0.45f, clrDim, C2D_AlignCenter);
        return;
    }

    switch (app->vizMode) {
        case VIZ_WAVES:    render_np_waves(app);    break;
        case VIZ_SPECTRUM: render_np_spectrum(app); break;
        default:           render_np_default(app);  break;
    }
}

static void render_video(AppState* app) {
    u32 black = C2D_Color32(0, 0, 0, 255);
    C2D_TargetClear(s_top, black);
    C2D_SceneBegin(s_top);

    const char* msg = video_message();
    if (msg) {
        text(msg, TOP_W / 2.0f, TOP_H / 2.0f - 18, 0.5f, clrWhite, C2D_AlignCenter);
        text("ffmpeg -i in.mp4 -c:v mjpeg -q:v 5 -c:a pcm_s16le out.avi",
             TOP_W / 2.0f, TOP_H / 2.0f + 6, 0.36f, clrDim, C2D_AlignCenter);
        text("Press B to go back.", TOP_W / 2.0f, TOP_H / 2.0f + 28, 0.4f, clrDim, C2D_AlignCenter);
        return;
    }
    if (!video_draw(0, 0, TOP_W, TOP_H)) {
        text("Loading video...", TOP_W / 2.0f, TOP_H / 2.0f - 8, 0.6f, clrWhite, C2D_AlignCenter);
    }

    /* transport overlay along the bottom */
    double posS = video_position_sec();
    double durS = video_duration_sec();
    float px = 16, py = TOP_H - 18, pw = TOP_W - 32;
    float frac = durS > 0 ? (float)(posS / durS) : 0;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    C2D_DrawRectSolid(px, py, 0.0f, pw, 4, C2D_Color32(255, 255, 255, 90));
    C2D_DrawRectSolid(px, py, 0.0f, pw * frac, 4, clrAccent);

    char a[16], b[16];
    fmt_time(posS, a, sizeof(a));
    fmt_time(durS, b, sizeof(b));
    text(a, px, py - 16, 0.42f, clrWhite, 0);
    text(b, px + pw, py - 16, 0.42f, clrWhite, C2D_AlignRight);

    if (video_is_paused())
        draw_pause_icon(TOP_W / 2.0f, 20, 9, clrWhite);

    if (app->statusTimer > 0.0f)
        text(app->status, TOP_W / 2.0f, 8, 0.44f, clrWhite, C2D_AlignCenter);
}

/* ===================== Cover Flow menu (iPod-style) ===================== */

/* Collect the folder entries of the music library (albums to flip through). */
static int cf_collect(AppState* app, int* dirs, int max) {
    int nd = 0;
    for (int i = 0; i < app->lib.count && nd < max; ++i)
        if (app->lib.entries[i].isDir) dirs[nd++] = i;
    return nd;
}

/* Draw one cover (with a fading floor reflection) centered at (cx). */
static void cf_draw_cover(const C2D_Image* img, const char* name,
                          float cx, float cy, float sz, bool center) {
    float x = cx - sz / 2.0f;
    float y = cy - sz / 2.0f;

    if (img && img->tex) {
        float iw = img->subtex ? img->subtex->width  : 128.0f;
        float ih = img->subtex ? img->subtex->height : 128.0f;
        float sc = sz / iw;
        float dh = ih * sc;
        C2D_DrawImageAt(*img, x, cy - dh / 2.0f, 0.5f, NULL, sc, sc);
        /* mirrored reflection below, faded into black */
        float yb = cy + dh / 2.0f;
        C2D_DrawImageAt(*img, x, yb + dh, 0.4f, NULL, sc, -sc * 0.9f);
        C2D_DrawRectangle(x, yb, 0.45f, sz, dh * 0.9f,
                          C2D_Color32(0, 0, 0, 150), C2D_Color32(0, 0, 0, 150),
                          C2D_Color32(0, 0, 0, 255), C2D_Color32(0, 0, 0, 255));
    } else {
        C2D_DrawRectSolid(x, y, 0.5f, sz, sz, C2D_Color32(46, 48, 56, 255));
        C2D_DrawRectSolid(x, y, 0.5f, sz, 2, C2D_Color32(90, 92, 100, 255));
        draw_note_icon(cx, cy - 6, sz / 90.0f, C2D_Color32(96, 98, 108, 255));
    }
    (void)name; (void)center;
}

static void render_coverflow_top(AppState* app) {
    u32 black = C2D_Color32(0, 0, 0, 255);
    C2D_TargetClear(s_top, black);
    C2D_SceneBegin(s_top);

    int dirs[256];
    int nd = cf_collect(app, dirs, 256);

    /* top status strip */
    text("Cover Flow", TOP_W / 2.0f, 6, 0.5f, clrWhite, C2D_AlignCenter);

    if (nd == 0) {
        text("No albums in this folder", TOP_W / 2.0f, TOP_H / 2.0f - 8, 0.5f,
             C2D_Color32(200, 200, 205, 255), C2D_AlignCenter);
        text("Press B to go back.", TOP_W / 2.0f, TOP_H / 2.0f + 18, 0.42f, clrDim, C2D_AlignCenter);
        return;
    }

    if (app->cfSel < 0) app->cfSel = 0;
    if (app->cfSel >= nd) app->cfSel = nd - 1;
    app->cfPos += ((float)app->cfSel - app->cfPos) * 0.22f;

    int   base = (int)(app->cfPos + 0.5f);
    float cx   = TOP_W / 2.0f, cy = 118;
    float spacing = 92.0f, csz = 132.0f, ssz = 92.0f;

    /* side covers first, center last (painter's order) */
    for (int pass = 0; pass < 2; ++pass) {
        for (int k = -3; k <= 3; ++k) {
            int idx = base + k;
            if (idx < 0 || idx >= nd) continue;
            bool isCenter = (idx == app->cfSel);
            if ((pass == 0) == isCenter) continue;

            char folder[LIB_PATH_MAX];
            snprintf(folder, sizeof(folder), "%s%s/", app->lib.path,
                     app->lib.entries[dirs[idx]].name);
            const C2D_Image* img = covers_get(folder);

            float pos  = (float)idx - app->cfPos;
            float dist = pos < 0 ? -pos : pos;
            if (dist > 3.2f) continue;
            float sz   = csz - (csz - ssz) * (dist > 1 ? 1 : dist);
            float ox   = pos * spacing;
            /* tuck side covers closer to the middle for the fan effect */
            if (pos > 0) ox -= spacing * 0.35f;
            if (pos < 0) ox += spacing * 0.35f;
            cf_draw_cover(img, app->lib.entries[dirs[idx]].name, cx + ox, cy, sz, isCenter);
        }
    }

    /* centered album title */
    text_fit(app->lib.entries[dirs[app->cfSel]].name, 20, TOP_H - 42,
             0.5f, clrWhite, TOP_W - 40);
    char cnt[32];
    snprintf(cnt, sizeof(cnt), "%d of %d", app->cfSel + 1, nd);
    text(cnt, TOP_W - 12, TOP_H - 20, 0.4f, clrDim, C2D_AlignRight);
}

static void render_coverflow_bottom(AppState* app) {
    C2D_TargetClear(s_bot, clrBg);
    C2D_SceneBegin(s_bot);

    C2D_DrawRectangle(0, 0, 0.0f, BOT_W, TITLE_H, clrBarTop, clrBarTop, clrBarBot, clrBarBot);
    C2D_DrawRectSolid(0, TITLE_H - 1, 0.0f, BOT_W, 1, clrDivider);
    text("Cover Flow", BOT_W / 2.0f, 4, 0.6f, clrHeaderText, C2D_AlignCenter);

    /* prev / open / next */
    C2D_DrawCircleSolid(46, CF_BTN_Y, 0.0f, CF_BTN_R, clrProgBg);
    C2D_DrawTriangle(46 + 5, CF_BTN_Y - 9, clrText,
                     46 + 5, CF_BTN_Y + 9, clrText,
                     46 - 7, CF_BTN_Y,     clrText, 0.5f);

    C2D_DrawCircleSolid(BOT_W - 46, CF_BTN_Y, 0.0f, CF_BTN_R, clrProgBg);
    C2D_DrawTriangle(BOT_W - 46 - 5, CF_BTN_Y - 9, clrText,
                     BOT_W - 46 - 5, CF_BTN_Y + 9, clrText,
                     BOT_W - 46 + 7, CF_BTN_Y,     clrText, 0.5f);

    C2D_DrawCircleSolid(BOT_W / 2.0f, CF_BTN_Y, 0.0f, CF_OPEN_R, clrAccent);
    draw_play_icon(BOT_W / 2.0f + 2, CF_BTN_Y, 13, clrWhite);
    text("Open album", BOT_W / 2.0f, CF_BTN_Y + CF_OPEN_R + 6, 0.4f, clrDim, C2D_AlignCenter);

    /* back bar */
    float y0 = BOT_H - 30;
    C2D_DrawRectangle(0, y0, 0.0f, BOT_W, 30, clrBarTop, clrBarTop, clrBarBot, clrBarBot);
    C2D_DrawRectSolid(0, y0, 0.0f, BOT_W, 1, clrDivider);
    text("Back to list  (B)", BOT_W / 2.0f, y0 + 7, 0.46f, clrDim, C2D_AlignCenter);
    (void)app;
}

/* ========================= Equalizer editor ========================= */

static void render_eq(AppState* app) {
    C2D_TargetClear(s_bot, clrBg);
    C2D_SceneBegin(s_bot);

    C2D_DrawRectangle(0, 0, 0.0f, BOT_W, TITLE_H, clrBarTop, clrBarTop, clrBarBot, clrBarBot);
    C2D_DrawRectSolid(0, TITLE_H - 1, 0.0f, BOT_W, 1, clrDivider);
    char title[48];
    snprintf(title, sizeof(title), "Equalizer  -  %s",
             audio_effect_name(audio_get_effect_preset()));
    text(title, BOT_W / 2.0f, 4, 0.56f, clrHeaderText, C2D_AlignCenter);

    int bands = audio_eq_band_count();
    float areaW = BOT_W - 2 * EQ_AREA_X;
    float colW  = areaW / bands;
    float trackT = EQ_TRACK_T, trackB = EQ_TRACK_B;
    float trackH = trackB - trackT;
    float midY   = trackT + trackH / 2.0f;

    /* 0 dB reference line */
    C2D_DrawRectSolid(EQ_AREA_X, midY, 0.0f, areaW, 1, clrDivider);

    for (int b = 0; b < bands; ++b) {
        float cx = EQ_AREA_X + colW * (b + 0.5f);
        bool sel = (b == app->eqBand);
        int db = audio_eq_get(b);
        float frac = (db + 12) / 24.0f;              /* 0..1 */
        float knobY = trackB - frac * trackH;

        /* track */
        C2D_DrawRectSolid(cx - 2, trackT, 0.0f, 4, trackH, clrProgBg);
        /* fill from 0 dB line to the knob */
        u32 fill = sel ? clrAccent : clrSelTop;
        if (knobY < midY) C2D_DrawRectSolid(cx - 2, knobY, 0.0f, 4, midY - knobY, fill);
        else              C2D_DrawRectSolid(cx - 2, midY,  0.0f, 4, knobY - midY, fill);
        /* knob */
        C2D_DrawCircleSolid(cx, knobY, 0.0f, sel ? 7 : 5, sel ? clrAccent : clrText);
        if (sel) C2D_DrawCircleSolid(cx, knobY, 0.0f, 3, clrWhite);

        /* labels */
        text(audio_eq_label(b), cx, trackB + 6, 0.4f, sel ? clrAccent : clrDim, C2D_AlignCenter);
        char v[16];
        snprintf(v, sizeof(v), "%+d", db);
        text(v, cx, trackT - 16, 0.4f, sel ? clrAccent : clrDim, C2D_AlignCenter);
    }

    float y0 = BOT_H - 26;
    C2D_DrawRectangle(0, y0, 0.0f, BOT_W, 26, clrBarTop, clrBarTop, clrBarBot, clrBarBot);
    C2D_DrawRectSolid(0, y0, 0.0f, BOT_W, 1, clrDivider);
    text("Up/Down: gain   L/R: preset   B: back", BOT_W / 2.0f, y0 + 6, 0.42f, clrDim, C2D_AlignCenter);
    (void)app;
}

void ui_render(AppState* app) {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TextBufClear(s_buf);

    /* top screen */
    if (app->videoActive)          render_video(app);
    else if (app->coverflowActive) render_coverflow_top(app);
    else                           render_nowplaying(app);

    /* bottom screen */
    if (app->videoActive) {
        render_video_controls(app);
    } else if (app->coverflowActive) {
        render_coverflow_bottom(app);
    } else if (app->eqActive) {
        render_eq(app);
    } else if (app->pickerActive) {
        render_picker(app);
    } else if (app->tab == TAB_MOVIES) {
        render_movies(app);
    } else if (app->tab == TAB_SETTINGS) {
        render_settings(app);
    } else { /* TAB_MUSIC */
        if (app->searchActive) render_search(app);
        else                   render_browser(app);
    }

    C3D_FrameEnd(0);
}
