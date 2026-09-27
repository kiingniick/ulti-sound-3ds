#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ONLINE_OK = 0,
    ONLINE_OFFLINE,     /* no Wi-Fi / not associated to an AP */
    ONLINE_NOTFOUND,    /* the lookup returned no match */
    ONLINE_NETERR,      /* transport / HTTP error */
    ONLINE_SAVEERR      /* could not write the image to the SD card */
} OnlineResult;

/* Look an album up online (Deezer's open API) and download + apply its cover
 * art to `folder`. When `wantArtist` is true and `artist` is a real name, the
 * artist's profile picture is downloaded too and saved alongside the cover.
 *
 * `artist` may be NULL/empty (e.g. for compilations) -- then only the album
 * title is used for the query and no artist picture is fetched. */
OnlineResult online_fetch_art(const char* folder, const char* artist,
                              const char* album, bool wantArtist);

/* Human-readable message for a result code. */
const char* online_result_str(OnlineResult r);

#ifdef __cplusplus
}
#endif
