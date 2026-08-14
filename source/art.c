#include "art.h"
#include "library.h"

#include <citro2d.h>
#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "stb_image.h"

#define ART_TEX 128
#define COVER_SIDECAR "_ultisound_cover.txt"

static C2D_Image s_img;
static bool      s_has;
static char      s_folder[LIB_PATH_MAX];

void art_init(void) {
    memset(&s_img, 0, sizeof(s_img));
    s_has = false;
    s_folder[0] = 0;
}

static void art_free_current(void) {
    if (s_has) {
        if (s_img.tex) {
            C3D_TexDelete((C3D_Tex*)s_img.tex);
            free((void*)s_img.tex);
        }
        free((void*)s_img.subtex);
        memset(&s_img, 0, sizeof(s_img));
        s_has = false;
    }
}

void art_exit(void) {
    art_free_current();
}

/* 3DS textures are stored in 8x8 Z-order tiles. */
static inline u32 tile_index(u32 x, u32 y, u32 w) {
    return (((y >> 3) * (w >> 3) + (x >> 3)) << 6) +
           ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
            ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
}

static void resize_rgba(const unsigned char* src, int sw, int sh,
                        unsigned char* dst, int dw, int dh) {
    for (int y = 0; y < dh; ++y) {
        float fy = ((float)y + 0.5f) * (float)sh / (float)dh - 0.5f;
        int y0 = (int)floorf(fy);
        float wy = fy - (float)y0;
        int y1 = y0 + 1;
        if (y0 < 0) y0 = 0; if (y0 >= sh) y0 = sh - 1;
        if (y1 < 0) y1 = 0; if (y1 >= sh) y1 = sh - 1;

        for (int x = 0; x < dw; ++x) {
            float fx = ((float)x + 0.5f) * (float)sw / (float)dw - 0.5f;
            int x0 = (int)floorf(fx);
            float wx = fx - (float)x0;
            int x1 = x0 + 1;
            if (x0 < 0) x0 = 0; if (x0 >= sw) x0 = sw - 1;
            if (x1 < 0) x1 = 0; if (x1 >= sw) x1 = sw - 1;

            const unsigned char* p00 = src + (y0 * sw + x0) * 4;
            const unsigned char* p01 = src + (y0 * sw + x1) * 4;
            const unsigned char* p10 = src + (y1 * sw + x0) * 4;
            const unsigned char* p11 = src + (y1 * sw + x1) * 4;
            unsigned char* d = dst + (y * dw + x) * 4;
            for (int c = 0; c < 4; ++c) {
                float top = p00[c] * (1 - wx) + p01[c] * wx;
                float bot = p10[c] * (1 - wx) + p11[c] * wx;
                float v = top * (1 - wy) + bot * wy;
                if (v < 0) v = 0; if (v > 255) v = 255;
                d[c] = (unsigned char)(v + 0.5f);
            }
        }
    }
}

static bool upload_cover(const unsigned char* rgba128) {
    C3D_Tex* tex = (C3D_Tex*)malloc(sizeof(C3D_Tex));
    Tex3DS_SubTexture* sub = (Tex3DS_SubTexture*)malloc(sizeof(Tex3DS_SubTexture));
    if (!tex || !sub) { free(tex); free(sub); return false; }

    if (!C3D_TexInit(tex, ART_TEX, ART_TEX, GPU_RGBA8)) {
        free(tex); free(sub); return false;
    }
    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);

    u32* dst = (u32*)tex->data;
    for (int y = 0; y < ART_TEX; ++y) {
        for (int x = 0; x < ART_TEX; ++x) {
            const unsigned char* p = rgba128 + (y * ART_TEX + x) * 4;
            u32 idx = tile_index((u32)x, (u32)y, ART_TEX);
            dst[idx] = ((u32)p[3]) | ((u32)p[2] << 8) | ((u32)p[1] << 16) | ((u32)p[0] << 24);
        }
    }
    GSPGPU_FlushDataCache(tex->data, tex->size);

    sub->width = ART_TEX; sub->height = ART_TEX;
    sub->left = 0.0f; sub->top = 1.0f; sub->right = 1.0f; sub->bottom = 0.0f;

    s_img.tex = tex;
    s_img.subtex = sub;
    s_has = true;
    return true;
}

/* Largest source image we will fully decode. stb_image expands the whole
 * image to RGBA in RAM before we downscale it, so an unbounded cover (e.g. a
 * 4000x4000 JPEG ~= 64 MB) can exhaust memory. Cover art only needs to be
 * ~128px, so anything past this is skipped rather than risking an OOM. */
#define ART_MAX_SIDE 2048

static bool try_load_image(const char* path) {
    int w = 0, h = 0, comp = 0;

    /* Cheap header-only probe first: reject huge images before allocating. */
    if (stbi_info(path, &w, &h, &comp)) {
        if (w <= 0 || h <= 0 || w > ART_MAX_SIDE || h > ART_MAX_SIDE)
            return false;
    }

    unsigned char* pix = stbi_load(path, &w, &h, &comp, 4);
    if (!pix) return false;

    unsigned char* resized = (unsigned char*)malloc((size_t)ART_TEX * ART_TEX * 4);
    if (!resized) { stbi_image_free(pix); return false; }

    resize_rgba(pix, w, h, resized, ART_TEX, ART_TEX);
    stbi_image_free(pix);

    bool ok = upload_cover(resized);
    free(resized);
    return ok;
}

/* Read the cover sidecar (if present) and return the referenced filename. */
static bool read_sidecar(const char* folder, char* outName, size_t n) {
    char path[LIB_PATH_MAX];
    snprintf(path, sizeof(path), "%s%s", folder, COVER_SIDECAR);
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    if (!fgets(outName, (int)n, f)) { fclose(f); return false; }
    fclose(f);
    /* strip trailing newline/whitespace */
    size_t len = strlen(outName);
    while (len && (outName[len - 1] == '\n' || outName[len - 1] == '\r' ||
                   outName[len - 1] == ' '  || outName[len - 1] == '\t'))
        outName[--len] = 0;
    return len > 0;
}

static void load_cover_for(const char* folder) {
    char path[LIB_PATH_MAX];
    char name[LIB_NAME_MAX];

    /* 1. explicit sidecar chosen by the user. The stored value may be a bare
     *    filename (relative to the folder) or an absolute SD path so a cover
     *    can live anywhere on the card. */
    if (read_sidecar(folder, name, sizeof(name))) {
        if (strchr(name, ':') || name[0] == '/')
            snprintf(path, sizeof(path), "%s", name);
        else
            snprintf(path, sizeof(path), "%s%s", folder, name);
        if (try_load_image(path)) return;
    }

    /* 2. common cover filenames */
    static const char* candidates[] = {
        "cover.png", "cover.jpg", "cover.jpeg",
        "folder.jpg", "folder.png",
        "album.png", "album.jpg",
        "front.jpg", "front.png", NULL
    };
    for (int i = 0; candidates[i]; ++i) {
        snprintf(path, sizeof(path), "%s%s", folder, candidates[i]);
        if (try_load_image(path)) return;
    }
}

void art_set_folder(const char* folderPath) {
    if (!folderPath) return;
    if (strcmp(folderPath, s_folder) == 0) return;

    art_free_current();
    strncpy(s_folder, folderPath, sizeof(s_folder) - 1);
    s_folder[sizeof(s_folder) - 1] = 0;
    load_cover_for(s_folder);
}

void art_reload(void) {
    char folder[LIB_PATH_MAX];
    strncpy(folder, s_folder, sizeof(folder) - 1);
    folder[sizeof(folder) - 1] = 0;
    art_free_current();
    if (folder[0]) load_cover_for(folder);
}

bool art_available(void) { return s_has; }

bool art_draw(float x, float y, float size) {
    if (!s_has) return false;
    float scale = size / (float)ART_TEX;
    C2D_DrawImageAt(s_img, x, y, 0.5f, NULL, scale, scale);
    return true;
}

bool art_set_cover_for_folder(const char* folderPath, const char* imageName) {
    if (!folderPath || !imageName) return false;
    char path[LIB_PATH_MAX];
    snprintf(path, sizeof(path), "%s%s", folderPath, COVER_SIDECAR);
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fputs(imageName, f);
    fputc('\n', f);
    fclose(f);

    /* If this is the folder currently shown, reload immediately. */
    if (strcmp(folderPath, s_folder) == 0) art_reload();
    return true;
}
