#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIB_NAME_MAX 256
#define LIB_PATH_MAX 1024

typedef enum {
    LIBF_AUDIO = 0,   /* show audio files + images */
    LIBF_VIDEO,       /* show video files */
    LIBF_IMAGE        /* show image files only */
} LibFilter;

typedef struct {
    char name[LIB_NAME_MAX];
    bool isDir;
    bool isAudio;
    bool isImage;
    bool isVideo;
} LibEntry;

/* True if the filename has an image extension we can load as album art. */
bool library_is_image(const char* name);
/* True if the filename is a video container we can play (MJPEG AVI). */
bool library_is_video(const char* name);

typedef struct {
    char      path[LIB_PATH_MAX]; /* always ends with '/' */
    char      root[LIB_PATH_MAX];
    LibEntry* entries;
    int       count;
    int       cap;
    LibFilter filter;             /* which file kinds to list */
} Library;

void library_init(Library* lib, const char* root);
void library_free(Library* lib);
void library_set_filter(Library* lib, LibFilter filter);

/* (Re)read the directory at `path` into the entry list. */
bool library_open(Library* lib, const char* path);

/* Navigate. Returns true if the view changed. */
bool library_enter(Library* lib, int index); /* descend into a folder */
bool library_up(Library* lib);               /* go to parent (clamped to root) */
bool library_at_root(const Library* lib);

/* Absolute path of an entry. */
void library_entry_path(const Library* lib, int index, char* out, size_t outsz);

/* Number of audio files in the current view, and mapping helpers used to
 * build a "now playing" queue from the current folder. */
int  library_audio_count(const Library* lib);
int  library_audio_index_of(const Library* lib, int entryIndex); /* -1 if not audio */
int  library_entry_of_audio(const Library* lib, int audioIndex); /* entry index */

#ifdef __cplusplus
}
#endif
