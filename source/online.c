#include "online.h"
#include "net.h"
#include "art.h"
#include "library.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARTIST_SIDECAR "_ultisound_artist.jpg"
#define COVER_FILE     "_ultisound_cover.jpg"

/* Percent-encode `in` into `out` for use in a URL query value. */
static void url_encode(const char* in, char* out, size_t n) {
    static const char* hex = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p && o + 4 < n; ++p) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = 0;
}

/* Pull the string value of "key":"..." out of a JSON blob, unescaping \/ and
 * \\. Returns true if found. Takes the first occurrence (first album entry). */
static bool json_extract(const char* json, const char* key, char* out, size_t n) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char* p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < n) {
        if (*p == '\\' && (p[1] == '/' || p[1] == '\\')) { out[o++] = p[1]; p += 2; }
        else out[o++] = *p++;
    }
    out[o] = 0;
    return o > 0;
}

/* Download `url` and write the bytes to `path`. */
static bool download_to_file(const char* url, const char* path) {
    unsigned char* buf = NULL;
    size_t len = 0;
    if (net_http_get(url, &buf, &len) != 0 || !buf || len == 0) {
        free(buf);
        return false;
    }
    FILE* f = fopen(path, "wb");
    if (!f) { free(buf); return false; }
    size_t wrote = fwrite(buf, 1, len, f);
    fclose(f);
    free(buf);
    return wrote == len;
}

const char* online_result_str(OnlineResult r) {
    switch (r) {
        case ONLINE_OK:       return "Art downloaded";
        case ONLINE_OFFLINE:  return "No Wi-Fi connection";
        case ONLINE_NOTFOUND: return "No online match found";
        case ONLINE_NETERR:   return "Network error";
        case ONLINE_SAVEERR:  return "Could not save to SD";
    }
    return "Unknown error";
}

OnlineResult online_fetch_art(const char* folder, const char* artist,
                              const char* album, bool wantArtist) {
    if (!folder || !album || !album[0]) return ONLINE_NOTFOUND;
    if (!net_available())               return ONLINE_OFFLINE;
    if (!net_init())                    return ONLINE_NETERR;

    /* Build the search query: "artist album" when we have an artist, else the
     * album title alone (compilations). */
    char query[512];
    if (artist && artist[0])
        snprintf(query, sizeof(query), "%s %s", artist, album);
    else
        snprintf(query, sizeof(query), "%s", album);

    char enc[1024];
    url_encode(query, enc, sizeof(enc));

    char url[1200];
    snprintf(url, sizeof(url), "https://api.deezer.com/search/album?limit=1&q=%s", enc);

    unsigned char* json = NULL;
    size_t jlen = 0;
    if (net_http_get(url, &json, &jlen) != 0 || !json) return ONLINE_NETERR;

    /* NUL-terminate for string parsing. */
    unsigned char* jbuf = (unsigned char*)realloc(json, jlen + 1);
    if (!jbuf) { free(json); return ONLINE_NETERR; }
    json = jbuf;
    json[jlen] = 0;

    char coverUrl[512]  = {0};
    char artistUrl[512] = {0};
    bool haveCover  = json_extract((const char*)json, "cover_xl", coverUrl, sizeof(coverUrl))
                   || json_extract((const char*)json, "cover_big", coverUrl, sizeof(coverUrl));
    bool haveArtist = json_extract((const char*)json, "picture_xl", artistUrl, sizeof(artistUrl))
                   || json_extract((const char*)json, "picture_big", artistUrl, sizeof(artistUrl));
    free(json);

    if (!haveCover) return ONLINE_NOTFOUND;

    /* Save + apply the album cover. */
    char coverPath[LIB_PATH_MAX];
    snprintf(coverPath, sizeof(coverPath), "%s%s", folder, COVER_FILE);
    if (!download_to_file(coverUrl, coverPath)) return ONLINE_SAVEERR;
    art_set_cover_for_folder(folder, COVER_FILE);   /* reloads if current */

    /* Optionally save + apply the artist picture. */
    if (wantArtist && haveArtist) {
        char artistPath[LIB_PATH_MAX];
        snprintf(artistPath, sizeof(artistPath), "%s%s", folder, ARTIST_SIDECAR);
        if (download_to_file(artistUrl, artistPath))
            art_reload_artist(folder);
    }

    return ONLINE_OK;
}
