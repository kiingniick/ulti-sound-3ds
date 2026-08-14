#include "decoder.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "dr_wav.h"
#include "dr_mp3.h"
#include "dr_flac.h"
#include "aac.h"

/* Pull in only the stb_vorbis declarations here; the implementation lives
 * in its own translation unit (source/stb_vorbis.c). */
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

struct Decoder {
    DecoderType type;
    uint32_t    sampleRate;
    uint32_t    channels;
    uint64_t    totalFrames;
    uint64_t    curFrame;

    /* format-specific handles */
    drwav        wav;
    drmp3        mp3;
    drflac*      flac;
    stb_vorbis*  vorbis;
    AacDecoder*  aac;

    /* scratch buffer used when the source is not already stereo */
    int16_t* scratch;
    size_t   scratchFrames; /* capacity in frames */
};

static const char* ext_of(const char* path) {
    const char* dot = strrchr(path, '.');
    return dot ? dot + 1 : "";
}

static int ext_is(const char* path, const char* want) {
    const char* e = ext_of(path);
    while (*e && *want) {
        if (tolower((unsigned char)*e) != tolower((unsigned char)*want)) return 0;
        ++e; ++want;
    }
    return *e == 0 && *want == 0;
}

bool decoder_is_supported(const char* path) {
    return ext_is(path, "wav") || ext_is(path, "mp3") ||
           ext_is(path, "flac") ||
           ext_is(path, "ogg") || ext_is(path, "oga") ||
           aac_is_supported_ext(path);
}

static bool ensure_scratch(Decoder* d, size_t frames) {
    if (d->channels <= 2) return true; /* stereo path decodes in place */
    if (d->scratchFrames >= frames && d->scratch) return true;
    size_t needed = frames * d->channels;
    int16_t* p = (int16_t*)realloc(d->scratch, needed * sizeof(int16_t));
    if (!p) return false;
    d->scratch = p;
    d->scratchFrames = frames;
    return true;
}

Decoder* decoder_open(const char* path) {
    if (!path || !decoder_is_supported(path)) return NULL;

    Decoder* d = (Decoder*)calloc(1, sizeof(Decoder));
    if (!d) return NULL;

    if (ext_is(path, "wav")) {
        if (!drwav_init_file(&d->wav, path, NULL)) { free(d); return NULL; }
        d->type        = DEC_WAV;
        d->sampleRate  = d->wav.sampleRate;
        d->channels    = d->wav.channels;
        d->totalFrames = d->wav.totalPCMFrameCount;
    } else if (ext_is(path, "mp3")) {
        if (!drmp3_init_file(&d->mp3, path, NULL)) { free(d); return NULL; }
        d->type        = DEC_MP3;
        d->sampleRate  = d->mp3.sampleRate;
        d->channels    = d->mp3.channels;
        d->totalFrames = drmp3_get_pcm_frame_count(&d->mp3);
    } else if (ext_is(path, "flac")) {
        d->flac = drflac_open_file(path, NULL);
        if (!d->flac) { free(d); return NULL; }
        d->type        = DEC_FLAC;
        d->sampleRate  = d->flac->sampleRate;
        d->channels    = d->flac->channels;
        d->totalFrames = d->flac->totalPCMFrameCount;
    } else if (ext_is(path, "ogg") || ext_is(path, "oga")) {
        int err = 0;
        d->vorbis = stb_vorbis_open_filename(path, &err, NULL);
        if (!d->vorbis) { free(d); return NULL; }
        stb_vorbis_info info = stb_vorbis_get_info(d->vorbis);
        d->type        = DEC_VORBIS;
        d->sampleRate  = info.sample_rate;
        d->channels    = info.channels;
        d->totalFrames = stb_vorbis_stream_length_in_samples(d->vorbis);
    } else { /* aac / m4a / mp4 / m4b */
        d->aac = aac_open(path);
        if (!d->aac) { free(d); return NULL; }
        d->type        = DEC_AAC;
        d->sampleRate  = aac_sample_rate(d->aac);
        d->channels    = 2; /* aac_read always yields stereo */
        d->totalFrames = aac_total_frames(d->aac);
    }

    if (d->channels == 0) d->channels = 2;
    if (d->sampleRate == 0) d->sampleRate = 44100;
    d->curFrame = 0;
    return d;
}

void decoder_close(Decoder* d) {
    if (!d) return;
    switch (d->type) {
        case DEC_WAV:    drwav_uninit(&d->wav); break;
        case DEC_MP3:    drmp3_uninit(&d->mp3); break;
        case DEC_FLAC:   if (d->flac) drflac_close(d->flac); break;
        case DEC_VORBIS: if (d->vorbis) stb_vorbis_close(d->vorbis); break;
        case DEC_AAC:    if (d->aac) aac_close(d->aac); break;
        default: break;
    }
    free(d->scratch);
    free(d);
}

/* Expand/contract an interleaved buffer of `srcCh` channels into stereo. */
static void to_stereo(const int16_t* src, int16_t* dst, size_t frames, uint32_t srcCh) {
    if (srcCh == 2) {
        memcpy(dst, src, frames * 2 * sizeof(int16_t));
        return;
    }
    if (srcCh == 1) {
        for (size_t i = 0; i < frames; ++i) {
            int16_t s = src[i];
            dst[i * 2 + 0] = s;
            dst[i * 2 + 1] = s;
        }
        return;
    }
    /* More than 2 channels: keep the first two (front L/R). */
    for (size_t i = 0; i < frames; ++i) {
        dst[i * 2 + 0] = src[i * srcCh + 0];
        dst[i * 2 + 1] = src[i * srcCh + 1];
    }
}

size_t decoder_read(Decoder* d, int16_t* out, size_t frames) {
    if (!d || frames == 0) return 0;

    size_t got = 0;

    if (d->type == DEC_AAC) {
        got = aac_read(d->aac, out, frames);
        d->curFrame = aac_cur_frame(d->aac);
        return got;
    }

    if (d->channels == 2) {
        /* Decode straight into the caller's stereo buffer. */
        switch (d->type) {
            case DEC_WAV:  got = (size_t)drwav_read_pcm_frames_s16(&d->wav, frames, out); break;
            case DEC_MP3:  got = (size_t)drmp3_read_pcm_frames_s16(&d->mp3, frames, out); break;
            case DEC_FLAC: got = (size_t)drflac_read_pcm_frames_s16(d->flac, frames, out); break;
            case DEC_VORBIS: {
                int n = stb_vorbis_get_samples_short_interleaved(d->vorbis, 2, out, (int)(frames * 2));
                got = (size_t)(n < 0 ? 0 : n);
                break;
            }
            default: break;
        }
    } else {
        if (!ensure_scratch(d, frames)) return 0;
        int16_t* tmp = d->scratch;
        switch (d->type) {
            case DEC_WAV:  got = (size_t)drwav_read_pcm_frames_s16(&d->wav, frames, tmp); break;
            case DEC_MP3:  got = (size_t)drmp3_read_pcm_frames_s16(&d->mp3, frames, tmp); break;
            case DEC_FLAC: got = (size_t)drflac_read_pcm_frames_s16(d->flac, frames, tmp); break;
            case DEC_VORBIS: {
                int n = stb_vorbis_get_samples_short_interleaved(
                            d->vorbis, (int)d->channels, tmp,
                            (int)(frames * d->channels));
                got = (size_t)(n < 0 ? 0 : n);
                break;
            }
            default: break;
        }
        to_stereo(tmp, out, got, d->channels);
    }

    d->curFrame += got;
    return got;
}

uint32_t decoder_sample_rate(const Decoder* d) { return d ? d->sampleRate : 0; }
uint32_t decoder_channels(const Decoder* d)    { return d ? d->channels : 0; }
uint64_t decoder_total_frames(const Decoder* d){ return d ? d->totalFrames : 0; }
uint64_t decoder_cur_frame(const Decoder* d) {
    if (!d) return 0;
    if (d->type == DEC_AAC) return aac_cur_frame(d->aac);
    return d->curFrame;
}
DecoderType decoder_type(const Decoder* d)     { return d ? d->type : DEC_NONE; }

bool decoder_seek(Decoder* d, uint64_t frame) {
    if (!d) return false;
    bool ok = false;
    switch (d->type) {
        case DEC_WAV:    ok = drwav_seek_to_pcm_frame(&d->wav, frame); break;
        case DEC_MP3:    ok = drmp3_seek_to_pcm_frame(&d->mp3, frame); break;
        case DEC_FLAC:   ok = drflac_seek_to_pcm_frame(d->flac, frame); break;
        case DEC_VORBIS: ok = stb_vorbis_seek(d->vorbis, (unsigned int)frame) != 0; break;
        case DEC_AAC:    ok = aac_seek(d->aac, frame); break;
        default: break;
    }
    if (ok && d->type != DEC_AAC) d->curFrame = frame;
    return ok;
}

const char* decoder_type_name(const Decoder* d) {
    if (!d) return "-";
    switch (d->type) {
        case DEC_WAV:    return "WAV";
        case DEC_MP3:    return "MP3";
        case DEC_FLAC:   return "FLAC";
        case DEC_VORBIS: return "OGG";
        case DEC_AAC:    return "AAC";
        default:         return "-";
    }
}
