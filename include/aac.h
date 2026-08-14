#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AAC decoding for .aac (raw ADTS) and .m4a/.mp4/.m4b (MP4 container),
 * backed by faad2 (+ minimp4 for the container). Output is interleaved
 * stereo int16, matching the rest of the audio pipeline. */

typedef struct AacDecoder AacDecoder;

AacDecoder* aac_open(const char* path);
void        aac_close(AacDecoder* d);

size_t   aac_read(AacDecoder* d, int16_t* out, size_t frames);
uint32_t aac_sample_rate(const AacDecoder* d);
uint32_t aac_channels(const AacDecoder* d);
uint64_t aac_total_frames(const AacDecoder* d);
uint64_t aac_cur_frame(const AacDecoder* d);
bool     aac_seek(AacDecoder* d, uint64_t frame);

bool aac_is_supported_ext(const char* path);

#ifdef __cplusplus
}
#endif
