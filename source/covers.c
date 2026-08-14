#include "covers.h"
#include "library.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_image.h"

#define COV_TEX      128
#define COV_CACHE    10
#define COV_MAX_SIDE 2048
#define COVER_SIDECAR "_ultisound_cover.txt"

typedef struct {
    char      path[LIB_PATH_MAX];
    bool      used;
    bool      has;         /* a real cover was loaded */
    uint32_t  lru;
    C2D_Image img;
} CovEntry;

static CovEntry s_cache[COV_CACHE];
static uint32_t s_tick;

static inline u32 tile_index(u32 x, u32 y, u32 w) {
    return (((y >> 3) * (w >> 3) + (x >> 3)) << 6) +
           ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
            ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
}

void covers_init(void) {
    memset(s_cache, 0, sizeof(s_cache));
    s_tick = 0;
}

static void free_entry(CovEntry* e) {
    if (e->has && e->img.tex) {
        C3D_TexDelete((C3D_Tex*)e->img.tex);
        free((void*)e->img.tex);
        free((void*)e->img.subtex);
    }
    memset(e, 0, sizeof(*e));
}

void covers_exit(void) {
    for (int i = 0; i < COV_CACHE; ++i) free_entry(&s_cache[i]);
}

/* nearest-neighbour shrink to COV_TEX x COV_TEX RGBA */
static void shrink_to_tex(const unsigned char* src, int sw, int sh, unsigned char* dst) {
    for (int y = 0; y < COV_TEX; ++y) {
        int sy = (int)((int64_t)y * sh / COV_TEX);
        for (int x = 0; x < COV_TEX; ++x) {
            int sx = (int)((int64_t)x * sw / COV_TEX);
            const unsigned char* p = src + (sy * sw + sx) * 4;
            unsigned char* d = dst + (y * COV_TEX + x) * 4;
            d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = p[3];
        }
    }
}

static bool upload(CovEntry* e, const unsigned char* rgba) {
    C3D_Tex* tex = (C3D_Tex*)malloc(sizeof(C3D_Tex));
    Tex3DS_SubTexture* sub = (Tex3DS_SubTexture*)malloc(sizeof(Tex3DS_SubTexture));
    if (!tex || !sub) { free(tex); free(sub); return false; }
    if (!C3D_TexInit(tex, COV_TEX, COV_TEX, GPU_RGBA8)) { free(tex); free(sub); return false; }
    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);

    u32* d = (u32*)tex->data;
    for (int y = 0; y < COV_TEX; ++y)
        for (int x = 0; x < COV_TEX; ++x) {
            const unsigned char* p = rgba + (y * COV_TEX + x) * 4;
            d[tile_index((u32)x, (u32)y, COV_TEX)] =
                ((u32)p[3]) | ((u32)p[2] << 8) | ((u32)p[1] << 16) | ((u32)p[0] << 24);
        }
    GSPGPU_FlushDataCache(tex->data, tex->size);

    sub->width = COV_TEX; sub->height = COV_TEX;
    sub->left = 0.0f; sub->top = 1.0f; sub->right = 1.0f; sub->bottom = 0.0f;
    e->img.tex = tex; e->img.subtex = sub;
    return true;
}

static bool load_image(const char* path, CovEntry* e) {
    int w = 0, h = 0, comp = 0;
    if (stbi_info(path, &w, &h, &comp)) {
        if (w <= 0 || h <= 0 || w > COV_MAX_SIDE || h > COV_MAX_SIDE) return false;
    }
    unsigned char* pix = stbi_load(path, &w, &h, &comp, 4);
    if (!pix) return false;
    unsigned char* small = (unsigned char*)malloc((size_t)COV_TEX * COV_TEX * 4);
    if (!small) { stbi_image_free(pix); return false; }
    shrink_to_tex(pix, w, h, small);
    stbi_image_free(pix);
    bool ok = upload(e, small);
    free(small);
    return ok;
}

/* Resolve a folder's cover: sidecar (relative or absolute) or common names. */
static bool resolve_and_load(const char* folder, CovEntry* e) {
    char path[LIB_PATH_MAX];
    char name[LIB_NAME_MAX];

    char sc[LIB_PATH_MAX];
    snprintf(sc, sizeof(sc), "%s%s", folder, COVER_SIDECAR);
    FILE* f = fopen(sc, "rb");
    if (f) {
        if (fgets(name, sizeof(name), f)) {
            size_t len = strlen(name);
            while (len && (name[len-1]=='\n'||name[len-1]=='\r'||name[len-1]==' '||name[len-1]=='\t'))
                name[--len] = 0;
            if (len) {
                if (strchr(name, ':') || name[0] == '/') snprintf(path, sizeof(path), "%s", name);
                else snprintf(path, sizeof(path), "%s%s", folder, name);
                if (load_image(path, e)) { fclose(f); return true; }
            }
        }
        fclose(f);
    }

    static const char* cand[] = {
        "cover.png","cover.jpg","cover.jpeg","folder.jpg","folder.png",
        "album.png","album.jpg","front.jpg","front.png", NULL
    };
    for (int i = 0; cand[i]; ++i) {
        snprintf(path, sizeof(path), "%s%s", folder, cand[i]);
        if (load_image(path, e)) return true;
    }
    return false;
}

const C2D_Image* covers_get(const char* folderPath) {
    if (!folderPath || !folderPath[0]) return NULL;

    /* hit? */
    for (int i = 0; i < COV_CACHE; ++i) {
        if (s_cache[i].used && strcmp(s_cache[i].path, folderPath) == 0) {
            s_cache[i].lru = ++s_tick;
            return s_cache[i].has ? &s_cache[i].img : NULL;
        }
    }

    /* pick a slot: first free, else least-recently-used */
    int slot = -1;
    for (int i = 0; i < COV_CACHE; ++i) if (!s_cache[i].used) { slot = i; break; }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < COV_CACHE; ++i)
            if (s_cache[i].lru < s_cache[slot].lru) slot = i;
        free_entry(&s_cache[slot]);
    }

    CovEntry* e = &s_cache[slot];
    memset(e, 0, sizeof(*e));
    e->used = true;
    e->lru = ++s_tick;
    strncpy(e->path, folderPath, sizeof(e->path) - 1);
    e->has = resolve_and_load(folderPath, e);
    return e->has ? &e->img : NULL;
}
