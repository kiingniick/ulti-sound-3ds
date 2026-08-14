#include "library.h"
#include "decoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

static int name_has_ext(const char* name, const char* ext) {
    const char* dot = strrchr(name, '.');
    if (!dot) return 0;
    return strcasecmp(dot + 1, ext) == 0;
}

bool library_is_image(const char* name) {
    return name_has_ext(name, "png")  || name_has_ext(name, "jpg") ||
           name_has_ext(name, "jpeg") || name_has_ext(name, "bmp");
}

bool library_is_video(const char* name) {
    return name_has_ext(name, "avi")  || name_has_ext(name, "mjpeg") ||
           name_has_ext(name, "mjpg") || name_has_ext(name, "mp4")   ||
           name_has_ext(name, "mov")  || name_has_ext(name, "m4v");
}

static void ensure_trailing_slash(char* p) {
    size_t n = strlen(p);
    if (n == 0) { strcpy(p, "/"); return; }
    if (p[n - 1] != '/') { p[n] = '/'; p[n + 1] = 0; }
}

void library_init(Library* lib, const char* root) {
    memset(lib, 0, sizeof(*lib));
    strncpy(lib->root, root, LIB_PATH_MAX - 2);
    ensure_trailing_slash(lib->root);
    strcpy(lib->path, lib->root);
}

void library_free(Library* lib) {
    free(lib->entries);
    lib->entries = NULL;
    lib->count = lib->cap = 0;
}

void library_set_filter(Library* lib, LibFilter filter) {
    lib->filter = filter;
}

static bool push_entry(Library* lib, const char* name, bool isDir) {
    if (lib->count >= lib->cap) {
        int ncap = lib->cap ? lib->cap * 2 : 64;
        LibEntry* n = (LibEntry*)realloc(lib->entries, ncap * sizeof(LibEntry));
        if (!n) return false;
        lib->entries = n;
        lib->cap = ncap;
    }
    LibEntry* e = &lib->entries[lib->count++];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, LIB_NAME_MAX - 1);
    e->isDir = isDir;
    e->isAudio = !isDir && decoder_is_supported(name);
    e->isImage = !isDir && library_is_image(name);
    e->isVideo = !isDir && library_is_video(name);
    return true;
}

static int entry_cmp(const void* a, const void* b) {
    const LibEntry* ea = (const LibEntry*)a;
    const LibEntry* eb = (const LibEntry*)b;
    if (ea->isDir != eb->isDir) return ea->isDir ? -1 : 1; /* folders first */
    return strcasecmp(ea->name, eb->name);
}

bool library_open(Library* lib, const char* path) {
    char full[LIB_PATH_MAX];
    strncpy(full, path, LIB_PATH_MAX - 2);
    full[LIB_PATH_MAX - 2] = 0;
    ensure_trailing_slash(full);

    DIR* dir = opendir(full);
    if (!dir) return false;

    lib->count = 0;
    strcpy(lib->path, full);

    struct dirent* de;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        bool isDir = false;
        if (de->d_type == DT_DIR) {
            isDir = true;
        } else if (de->d_type == DT_UNKNOWN) {
            char child[LIB_PATH_MAX];
            snprintf(child, sizeof(child), "%s%s", full, de->d_name);
            struct stat st;
            if (stat(child, &st) == 0) isDir = S_ISDIR(st.st_mode);
        }

        if (!isDir) {
            bool keep;
            if (lib->filter == LIBF_VIDEO)
                keep = library_is_video(de->d_name);
            else if (lib->filter == LIBF_IMAGE)
                keep = library_is_image(de->d_name);
            else
                keep = decoder_is_supported(de->d_name) || library_is_image(de->d_name);
            if (!keep) continue; /* hide files that don't match this tab */
        }

        push_entry(lib, de->d_name, isDir);
    }
    closedir(dir);

    if (lib->count > 0)
        qsort(lib->entries, lib->count, sizeof(LibEntry), entry_cmp);
    return true;
}

bool library_at_root(const Library* lib) {
    return strcmp(lib->path, lib->root) == 0;
}

void library_entry_path(const Library* lib, int index, char* out, size_t outsz) {
    if (index < 0 || index >= lib->count) { if (outsz) out[0] = 0; return; }
    snprintf(out, outsz, "%s%s", lib->path, lib->entries[index].name);
}

bool library_enter(Library* lib, int index) {
    if (index < 0 || index >= lib->count) return false;
    if (!lib->entries[index].isDir) return false;
    char child[LIB_PATH_MAX];
    library_entry_path(lib, index, child, sizeof(child));
    return library_open(lib, child);
}

bool library_up(Library* lib) {
    if (library_at_root(lib)) return false;

    char parent[LIB_PATH_MAX];
    strcpy(parent, lib->path);
    size_t n = strlen(parent);
    if (n > 0 && parent[n - 1] == '/') parent[--n] = 0; /* drop trailing slash */
    char* slash = strrchr(parent, '/');
    if (!slash) return false;
    slash[1] = 0; /* keep the slash */

    /* Never climb above the configured root. */
    if (strlen(parent) < strlen(lib->root)) strcpy(parent, lib->root);
    return library_open(lib, parent);
}

int library_audio_count(const Library* lib) {
    int c = 0;
    for (int i = 0; i < lib->count; ++i)
        if (lib->entries[i].isAudio) ++c;
    return c;
}

int library_audio_index_of(const Library* lib, int entryIndex) {
    if (entryIndex < 0 || entryIndex >= lib->count) return -1;
    if (!lib->entries[entryIndex].isAudio) return -1;
    int idx = 0;
    for (int i = 0; i < entryIndex; ++i)
        if (lib->entries[i].isAudio) ++idx;
    return idx;
}

int library_entry_of_audio(const Library* lib, int audioIndex) {
    int idx = 0;
    for (int i = 0; i < lib->count; ++i) {
        if (lib->entries[i].isAudio) {
            if (idx == audioIndex) return i;
            ++idx;
        }
    }
    return -1;
}
