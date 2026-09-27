#pragma once

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up / tear down the HTTP client (3DS httpc service). Safe to call
 * repeatedly; returns false if the service could not be started. */
bool net_init(void);
void net_exit(void);

/* Rough "is the console online" check (associated to a Wi-Fi AP). */
bool net_available(void);

/* HTTP(S) GET with redirect following. On success returns 0 and hands back a
 * malloc()'d buffer in *outBuf (caller frees) of *outLen bytes. Non-zero
 * return means failure (negative for transport errors, or the HTTP status
 * code for non-2xx responses). */
int net_http_get(const char* url, unsigned char** outBuf, size_t* outLen);

#ifdef __cplusplus
}
#endif
