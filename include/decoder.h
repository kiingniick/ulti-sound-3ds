#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DEC_NONE = 0,
    DEC_WAV,
    DEC_MP3,
    DEC_FLAC,
    DEC_VORBIS,
    DEC_AAC
} DecoderType;

typedef struct Decoder Decoder;

/* Open an audio file. Returns NULL on failure or unsupported format. */
Decoder* decoder_open(const char* path);
void decoder_close(Decoder* d);

/* Decode up to `frames` interleaved stereo (2ch) int16 frames into `out`.
 * Always outputs stereo regardless of the source channel layout.
 * Returns the number of frames actually written (0 signals end of stream). */
size_t decoder_read(Decoder* d, int16_t* out, size_t frames);

uint32_t decoder_sample_rate(const Decoder* d);
uint32_t decoder_channels(const Decoder* d);   /* original channel count */
uint64_t decoder_total_frames(const Decoder* d);
uint64_t decoder_cur_frame(const Decoder* d);
bool     decoder_seek(Decoder* d, uint64_t frame);

DecoderType decoder_type(const Decoder* d);
const char* decoder_type_name(const Decoder* d);

/* Returns true if the file extension is a format we can decode. */
bool decoder_is_supported(const char* path);

#ifdef __cplusplus
}
#endif
