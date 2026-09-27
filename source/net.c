#include "net.h"

#include <3ds.h>
#include <string.h>
#include <stdlib.h>

#define NET_MAX_REDIRECTS 5
#define NET_MAX_BYTES     (6 * 1024 * 1024)   /* hard cap on a download */

static bool s_ready = false;

bool net_init(void) {
    if (s_ready) return true;
    if (R_FAILED(httpcInit(0))) return false;   /* 0 = default POST buffer */
    s_ready = true;
    return true;
}

void net_exit(void) {
    if (s_ready) { httpcExit(); s_ready = false; }
}

bool net_available(void) {
    /* Signal-strength bars (0..3) from the shared config; >0 means we're
     * associated with an access point. Not a guarantee of internet, but a
     * good, cheap gate before attempting a request. */
    return osGetWifiStrength() > 0;
}

static Result begin_get(const char* url, httpcContext* ctx) {
    Result r = httpcOpenContext(ctx, HTTPC_METHOD_GET, url, 1);
    if (R_FAILED(r)) return r;
    httpcSetSSLOpt(ctx, SSLCOPT_DisableVerify);  /* no CA bundle on the SD */
    httpcSetKeepAlive(ctx, HTTPC_KEEPALIVE_ENABLED);
    httpcAddRequestHeaderField(ctx, "User-Agent", "ulti-sound-3ds/1.0");
    httpcAddRequestHeaderField(ctx, "Connection", "Keep-Alive");
    return httpcBeginRequest(ctx);
}

int net_http_get(const char* url, unsigned char** outBuf, size_t* outLen) {
    if (!outBuf || !outLen) return -1;
    *outBuf = NULL; *outLen = 0;
    if (!s_ready && !net_init()) return -1;

    char cur[1024];
    strncpy(cur, url, sizeof(cur) - 1);
    cur[sizeof(cur) - 1] = 0;

    for (int hop = 0; hop < NET_MAX_REDIRECTS; ++hop) {
        httpcContext ctx;
        if (R_FAILED(begin_get(cur, &ctx))) return -1;

        u32 status = 0;
        if (R_FAILED(httpcGetResponseStatusCode(&ctx, &status))) {
            httpcCloseContext(&ctx); return -1;
        }

        if (status >= 301 && status <= 308) {
            char loc[1024];
            if (R_FAILED(httpcGetResponseHeader(&ctx, "Location", loc, sizeof(loc)))) {
                httpcCloseContext(&ctx); return -1;
            }
            httpcCloseContext(&ctx);
            strncpy(cur, loc, sizeof(cur) - 1);
            cur[sizeof(cur) - 1] = 0;
            continue;
        }

        if (status != 200) { httpcCloseContext(&ctx); return (int)status; }

        u32 hinted = 0;
        httpcGetDownloadSizeState(&ctx, NULL, &hinted);
        size_t cap = hinted ? (size_t)hinted : 65536;
        if (cap > NET_MAX_BYTES) cap = NET_MAX_BYTES;
        unsigned char* buf = (unsigned char*)malloc(cap);
        if (!buf) { httpcCloseContext(&ctx); return -1; }

        size_t len = 0;
        Result rr;
        do {
            if (len == cap) {
                if (cap >= NET_MAX_BYTES) break;
                size_t ncap = cap * 2;
                if (ncap > NET_MAX_BYTES) ncap = NET_MAX_BYTES;
                unsigned char* nb = (unsigned char*)realloc(buf, ncap);
                if (!nb) { free(buf); httpcCloseContext(&ctx); return -1; }
                buf = nb; cap = ncap;
            }
            u32 got = 0;
            rr = httpcDownloadData(&ctx, buf + len, cap - len, &got);
            len += got;
        } while (rr == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING && len < NET_MAX_BYTES);

        httpcCloseContext(&ctx);

        if (R_FAILED(rr) && rr != (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) {
            free(buf); return -1;
        }
        *outBuf = buf; *outLen = len;
        return 0;
    }
    return -1;   /* too many redirects */
}
