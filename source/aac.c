#include "aac.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"
#include "neaacdec.h"

#define ADTS_BUF_CAP   (64 * 1024)
#define ADTS_REFILL_AT (16 * 1024)

struct AacDecoder {
    FILE*          f;
    NeAACDecHandle dec;
    int            isMp4;

    /* decoded PCM held by faad (not owned); consumed across reads */
    const int16_t* pcm;
    uint32_t       pcmSamples;   /* total int16 samples available (frames*channels) */
    uint32_t       pcmPos;       /* samples already consumed */

    uint32_t channels;
    uint32_t sampleRate;
    uint32_t auFrames;           /* per-channel samples per access unit */
    uint64_t totalFrames;
    uint64_t curFrame;

    /* ---- MP4 container ---- */
    MP4D_demux_t mp4;
    int          track;
    uint32_t     sample;         /* next AU index */
    uint32_t     sampleCount;
    uint8_t*     aubuf;
    uint32_t     aucap;

    /* ---- raw ADTS ---- */
    uint8_t*  inbuf;
    uint32_t  inlen;
    int       eof;
    uint32_t* frameOffset;       /* file offset of each ADTS frame (for seeking) */
    uint32_t  frameCount;
};

/* --------------------------------------------------------------------- */

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

bool aac_is_supported_ext(const char* path) {
    return ext_is(path, "aac") || ext_is(path, "m4a") ||
           ext_is(path, "mp4") || ext_is(path, "m4b");
}

static int mp4_read_cb(int64_t offset, void* buffer, size_t size, void* token) {
    FILE* f = (FILE*)token;
    if (fseek(f, (long)offset, SEEK_SET) != 0) return 1;
    return fread(buffer, 1, size, f) == size ? 0 : 1;
}

static void configure_faad(AacDecoder* d) {
    NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(d->dec);
    cfg->outputFormat = FAAD_FMT_16BIT;
    cfg->downMatrix   = 1;   /* fold multichannel down to stereo */
    NeAACDecSetConfiguration(d->dec, cfg);
}

/* Scan a raw ADTS stream to count frames and record their file offsets so we
 * can seek. Cheap: it only reads 7-byte headers and hops by frame length. */
static void adts_prescan(AacDecoder* d) {
    long saved = ftell(d->f);
    fseek(d->f, 0, SEEK_SET);

    uint32_t cap = 4096;
    d->frameOffset = (uint32_t*)malloc(cap * sizeof(uint32_t));
    d->frameCount = 0;

    uint8_t hdr[7];
    long pos = 0;
    /* skip a possible ID3v2 tag */
    if (fread(hdr, 1, 3, d->f) == 3 && hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {
        uint8_t sz[7];
        if (fread(sz, 1, 7, d->f) == 7) {
            long tagsize = ((sz[3] & 0x7f) << 21) | ((sz[4] & 0x7f) << 14) |
                           ((sz[5] & 0x7f) << 7)  |  (sz[6] & 0x7f);
            pos = 10 + tagsize;
        }
    }

    fseek(d->f, pos, SEEK_SET);
    while (fread(hdr, 1, 7, d->f) == 7) {
        if (hdr[0] != 0xFF || (hdr[1] & 0xF0) != 0xF0) break; /* lost sync */
        uint32_t frameLen = ((hdr[3] & 0x03) << 11) | (hdr[4] << 3) | ((hdr[5] >> 5) & 0x07);
        if (frameLen < 7) break;
        if (d->frameCount >= cap) {
            cap *= 2;
            uint32_t* n = (uint32_t*)realloc(d->frameOffset, cap * sizeof(uint32_t));
            if (!n) break;
            d->frameOffset = n;
        }
        d->frameOffset[d->frameCount++] = (uint32_t)pos;
        pos += frameLen;
        fseek(d->f, pos, SEEK_SET);
    }

    fseek(d->f, saved, SEEK_SET);
}

static void adts_refill(AacDecoder* d) {
    if (d->eof) return;
    if (d->inlen >= ADTS_REFILL_AT) return;
    uint32_t space = ADTS_BUF_CAP - d->inlen;
    size_t got = fread(d->inbuf + d->inlen, 1, space, d->f);
    d->inlen += (uint32_t)got;
    if (got < space) d->eof = 1;
}

/* Decode the next access unit. Returns true if PCM was produced. */
static bool decode_next(AacDecoder* d) {
    NeAACDecFrameInfo info;

    if (d->isMp4) {
        int guard = 0;
        while (d->sample < d->sampleCount && guard++ < 64) {
            unsigned bytes = 0, ts = 0, dur = 0;
            MP4D_file_offset_t off =
                MP4D_frame_offset(&d->mp4, d->track, d->sample, &bytes, &ts, &dur);
            d->sample++;
            if (bytes == 0) continue;
            if (bytes > d->aucap) {
                uint8_t* n = (uint8_t*)realloc(d->aubuf, bytes);
                if (!n) return false;
                d->aubuf = n; d->aucap = bytes;
            }
            if (fseek(d->f, (long)off, SEEK_SET) != 0) continue;
            if (fread(d->aubuf, 1, bytes, d->f) != bytes) continue;

            memset(&info, 0, sizeof(info));
            void* p = NeAACDecDecode(d->dec, &info, d->aubuf, bytes);
            if (info.error != 0 || p == NULL || info.samples == 0) continue;

            d->pcm        = (const int16_t*)p;
            d->pcmSamples = (uint32_t)info.samples;
            d->pcmPos     = 0;
            d->channels   = info.channels ? info.channels : d->channels;
            d->sampleRate = info.samplerate ? (uint32_t)info.samplerate : d->sampleRate;
            if (d->auFrames == 0 && d->channels)
                d->auFrames = (uint32_t)info.samples / d->channels;
            return true;
        }
        return false;
    }

    /* raw ADTS */
    int guard = 0;
    while (guard++ < 128) {
        adts_refill(d);
        if (d->inlen == 0) return false;

        memset(&info, 0, sizeof(info));
        void* p = NeAACDecDecode(d->dec, &info, d->inbuf, d->inlen);
        uint32_t consumed = (uint32_t)info.bytesconsumed;
        if (consumed == 0 && info.error) consumed = 1; /* force progress on error */
        if (consumed > d->inlen) consumed = d->inlen;
        if (consumed) {
            memmove(d->inbuf, d->inbuf + consumed, d->inlen - consumed);
            d->inlen -= consumed;
        }
        if (info.error != 0 || p == NULL || info.samples == 0) {
            if (d->eof && d->inlen == 0) return false;
            continue;
        }
        d->pcm        = (const int16_t*)p;
        d->pcmSamples = (uint32_t)info.samples;
        d->pcmPos     = 0;
        d->channels   = info.channels ? info.channels : d->channels;
        d->sampleRate = info.samplerate ? (uint32_t)info.samplerate : d->sampleRate;
        if (d->auFrames == 0 && d->channels)
            d->auFrames = (uint32_t)info.samples / d->channels;
        return true;
    }
    return false;
}

/* --------------------------------------------------------------------- */

AacDecoder* aac_open(const char* path) {
    if (!path || !aac_is_supported_ext(path)) return NULL;

    AacDecoder* d = (AacDecoder*)calloc(1, sizeof(AacDecoder));
    if (!d) return NULL;

    d->f = fopen(path, "rb");
    if (!d->f) { free(d); return NULL; }

    d->dec = NeAACDecOpen();
    if (!d->dec) { fclose(d->f); free(d); return NULL; }
    configure_faad(d);

    d->isMp4 = !ext_is(path, "aac");

    if (d->isMp4) {
        fseek(d->f, 0, SEEK_END);
        long fsize = ftell(d->f);
        fseek(d->f, 0, SEEK_SET);

        if (!MP4D_open(&d->mp4, mp4_read_cb, d->f, (int64_t)fsize)) goto fail;

        d->track = -1;
        for (unsigned i = 0; i < d->mp4.track_count; ++i) {
            MP4D_track_t* t = &d->mp4.track[i];
            if (t->handler_type == MP4D_HANDLER_TYPE_SOUN &&
                t->object_type_indication == MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3) {
                d->track = (int)i;
                break;
            }
        }
        if (d->track < 0) goto fail;

        MP4D_track_t* t = &d->mp4.track[d->track];
        unsigned long sr = 0; unsigned char ch = 0;
        if (t->dsi && t->dsi_bytes > 0) {
            /* NeAACDecInit2 returns char; char is unsigned on ARM, so cast. */
            if ((signed char)NeAACDecInit2(d->dec, t->dsi, t->dsi_bytes, &sr, &ch) < 0)
                goto fail;
        }
        d->sampleCount = t->sample_count;
        d->sampleRate  = sr ? (uint32_t)sr : t->SampleDescription.audio.samplerate_hz;
        d->channels    = ch ? ch : 2;

        /* rough duration from the container until we learn auFrames */
        if (t->timescale) {
            double secs = ((double)t->duration_hi * 4294967296.0 + t->duration_lo) /
                          (double)t->timescale;
            d->totalFrames = (uint64_t)(secs * (double)(d->sampleRate ? d->sampleRate : 44100));
        }
    } else {
        d->inbuf = (uint8_t*)malloc(ADTS_BUF_CAP);
        if (!d->inbuf) goto fail;
        adts_prescan(d);

        adts_refill(d);
        unsigned long sr = 0; unsigned char ch = 0;
        long consumed = NeAACDecInit(d->dec, d->inbuf, d->inlen, &sr, &ch);
        if (consumed < 0) goto fail;
        if (consumed > 0 && (uint32_t)consumed <= d->inlen) {
            memmove(d->inbuf, d->inbuf + consumed, d->inlen - consumed);
            d->inlen -= (uint32_t)consumed;
        }
        d->sampleRate = sr ? (uint32_t)sr : 44100;
        d->channels   = ch ? ch : 2;
    }

    /* Prime one AU so sample rate / channels / auFrames are known before
     * the audio engine configures the DSP output rate. */
    if (!decode_next(d)) goto fail;

    if (d->auFrames) {
        if (d->isMp4)
            d->totalFrames = (uint64_t)d->sampleCount * d->auFrames;
        else if (d->frameCount)
            d->totalFrames = (uint64_t)d->frameCount * d->auFrames;
    }
    if (d->channels == 0) d->channels = 2;
    if (d->sampleRate == 0) d->sampleRate = 44100;
    d->curFrame = 0;
    return d;

fail:
    aac_close(d);
    return NULL;
}

void aac_close(AacDecoder* d) {
    if (!d) return;
    if (d->dec) NeAACDecClose(d->dec);
    if (d->isMp4) MP4D_close(&d->mp4);
    if (d->f) fclose(d->f);
    free(d->aubuf);
    free(d->inbuf);
    free(d->frameOffset);
    free(d);
}

size_t aac_read(AacDecoder* d, int16_t* out, size_t frames) {
    if (!d || frames == 0) return 0;
    size_t outFrames = 0;

    while (outFrames < frames) {
        if (d->pcm && d->pcmPos < d->pcmSamples) {
            uint32_t ch = d->channels ? d->channels : 2;
            uint32_t availFrames = (d->pcmSamples - d->pcmPos) / ch;
            if (availFrames == 0) { d->pcmPos = d->pcmSamples; continue; }
            uint32_t want = (uint32_t)(frames - outFrames);
            uint32_t take = availFrames < want ? availFrames : want;
            const int16_t* src = d->pcm + d->pcmPos;
            int16_t* dst = out + outFrames * 2;

            if (ch == 2) {
                memcpy(dst, src, (size_t)take * 2 * sizeof(int16_t));
            } else if (ch == 1) {
                for (uint32_t i = 0; i < take; ++i) {
                    int16_t s = src[i];
                    dst[i * 2] = s; dst[i * 2 + 1] = s;
                }
            } else {
                for (uint32_t i = 0; i < take; ++i) {
                    dst[i * 2]     = src[i * ch];
                    dst[i * 2 + 1] = src[i * ch + 1];
                }
            }
            d->pcmPos += take * ch;
            outFrames += take;
            d->curFrame += take;
            continue;
        }
        if (!decode_next(d)) break;
    }
    return outFrames;
}

uint32_t aac_sample_rate(const AacDecoder* d) { return d ? d->sampleRate : 0; }
uint32_t aac_channels(const AacDecoder* d)    { return d ? (d->channels > 2 ? 2 : d->channels) : 0; }
uint64_t aac_total_frames(const AacDecoder* d){ return d ? d->totalFrames : 0; }
uint64_t aac_cur_frame(const AacDecoder* d)   { return d ? d->curFrame : 0; }

bool aac_seek(AacDecoder* d, uint64_t frame) {
    if (!d || d->auFrames == 0) return false;
    uint32_t au = (uint32_t)(frame / d->auFrames);

    if (d->isMp4) {
        if (au >= d->sampleCount) au = d->sampleCount ? d->sampleCount - 1 : 0;
        d->sample = au;
    } else {
        if (d->frameCount == 0) return false;
        if (au >= d->frameCount) au = d->frameCount - 1;
        if (fseek(d->f, (long)d->frameOffset[au], SEEK_SET) != 0) return false;
        d->inlen = 0;
        d->eof = 0;
    }

    NeAACDecPostSeekReset(d->dec, 0);
    d->pcm = NULL;
    d->pcmSamples = d->pcmPos = 0;
    d->curFrame = (uint64_t)au * d->auFrames;
    return true;
}
