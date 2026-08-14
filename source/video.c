#include "video.h"

#include <3ds.h>
#include <citro2d.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "stb_image.h"
#include "minimp4.h"
#include "aac.h"

#define VID_TEX_W      512
#define VID_TEX_H      256
#define VID_AUDIO_CHN  1
#define VID_NBUF       2
#define VID_AUDIO_BUF  (128 * 1024)   /* bytes per audio wave buffer */

typedef struct {
    volatile bool running;
    volatile bool stopReq;
    volatile bool finished;
    volatile bool paused;

    Thread thread;
    char   path[1024];

    /* stream info parsed from the AVI header */
    long   moviStart;
    long   moviEnd;
    int    videoStream;
    int    audioStream;
    int    width, height;
    double fps;
    double duration;
    long   totalFrames;   /* video frames, from strh dwLength (0 if unknown) */

    volatile bool seekReq;
    volatile double seekFrac;
    double clockBase;     /* seconds; presentation time at last (re)start/seek */

    bool     hasAudio;
    uint32_t audioRate;
    int      audioChannels;
    int      audioBits;
    int      audioFmt;        /* WAVEFORMATEX tag; 1 = PCM */

    /* audio output */
    int16_t*    abuf[VID_NBUF];
    ndspWaveBuf awb[VID_NBUF];
    uint64_t    audioSubmitted;   /* total samples handed to the DSP */

    /* frame textures (double buffered) */
    C3D_Tex           tex[2];
    Tex3DS_SubTexture sub[2];
    C2D_Image         img[2];
    volatile int      front;      /* index currently safe to draw */
    volatile bool     hasFrame;
    int               dispW, dispH;

    /* wall-clock fallback timing */
    uint64_t startMs;
    uint64_t pauseStartMs;
    uint64_t pauseAccumMs;

    volatile double posSec;
    char   errmsg[80];
} Video;

static Video v;

static int mp4_read_cb(int64_t offset, void* buffer, size_t size, void* token) {
    FILE* f = (FILE*)token;
    if (fseek(f, (long)offset, SEEK_SET) != 0) return 1;
    return fread(buffer, 1, size, f) == (size_t)size ? 0 : 1;
}

static bool path_is_mp4(const char* p) {
    const char* dot = strrchr(p, '.');
    if (!dot) return false;
    return strcasecmp(dot, ".mp4") == 0 || strcasecmp(dot, ".mov") == 0 ||
           strcasecmp(dot, ".m4v") == 0;
}

/* ---- little-endian readers ---- */
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}
static bool fourcc(const uint8_t* p, const char* s) {
    return p[0] == s[0] && p[1] == s[1] && p[2] == s[2] && p[3] == s[3];
}

static inline u32 tile_index(u32 x, u32 y, u32 w) {
    return (((y >> 3) * (w >> 3) + (x >> 3)) << 6) +
           ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
            ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
}

/* ------------------------- initialization ------------------------- */

void video_init(void) {
    memset(&v, 0, sizeof(v));
    for (int i = 0; i < 2; ++i) {
        C3D_TexInit(&v.tex[i], VID_TEX_W, VID_TEX_H, GPU_RGBA8);
        C3D_TexSetFilter(&v.tex[i], GPU_LINEAR, GPU_LINEAR);
        v.sub[i].width = VID_TEX_W; v.sub[i].height = VID_TEX_H;
        v.sub[i].left = 0.0f; v.sub[i].top = 1.0f;
        v.sub[i].right = 1.0f; v.sub[i].bottom = 0.0f;
        v.img[i].tex = &v.tex[i];
        v.img[i].subtex = &v.sub[i];
    }
}

void video_exit(void) {
    video_stop();
    for (int i = 0; i < 2; ++i) C3D_TexDelete(&v.tex[i]);
    memset(&v, 0, sizeof(v));
}

/* --------------------------- AVI parsing --------------------------- */

/* Read the hdrl/strl chain to discover streams, size, fps and audio format. */
static bool parse_header(FILE* f) {
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12) return false;
    if (!fourcc(hdr, "RIFF") || !fourcc(hdr + 8, "AVI ")) return false;

    int streamCounter = 0;
    int curStreamType = 0; /* 1=video, 2=audio for the strl being parsed */
    v.videoStream = -1;
    v.audioStream = -1;
    v.fps = 0.0;

    uint8_t ch[8];
    while (fread(ch, 1, 8, f) == 8) {
        uint32_t size = rd32(ch + 4);

        if (fourcc(ch, "LIST")) {
            uint8_t lt[4];
            if (fread(lt, 1, 4, f) != 4) break;
            if (fourcc(lt, "movi")) {
                v.moviStart = ftell(f);
                v.moviEnd   = v.moviStart + (long)size - 4;
                /* skip past movi; frames are read later during playback */
                fseek(f, v.moviEnd, SEEK_SET);
            }
            /* For hdrl/strl/rec we descend by simply continuing to read the
             * inner chunks linearly (do not skip). */
            continue;
        }

        /* leaf chunk: read its payload into a small buffer when useful */
        long next = ftell(f) + (long)size + (size & 1);

        if (fourcc(ch, "strh")) {
            uint8_t b[64];
            uint32_t n = size < sizeof(b) ? size : sizeof(b);
            fread(b, 1, n, f);
            if (fourcc(b, "vids")) {
                curStreamType = 1;
                v.videoStream = streamCounter;
                uint32_t scale = rd32(b + 20), rate = rd32(b + 24);
                uint32_t length = rd32(b + 32);
                if (scale > 0) v.fps = (double)rate / (double)scale;
                v.totalFrames = (long)length;
                if (v.fps > 0) v.duration = (double)length / v.fps;
            } else if (fourcc(b, "auds")) {
                curStreamType = 2;
                v.audioStream = streamCounter;
            } else {
                curStreamType = 0;
            }
            streamCounter++;
        } else if (fourcc(ch, "strf")) {
            uint8_t b[64];
            uint32_t n = size < sizeof(b) ? size : sizeof(b);
            fread(b, 1, n, f);
            if (curStreamType == 1) {
                int w = (int)rd32(b + 4);
                int h = (int)rd32(b + 8);
                if (h < 0) h = -h;
                v.width = w; v.height = h;
            } else if (curStreamType == 2) {
                v.audioFmt      = rd16(b + 0);
                v.audioChannels = rd16(b + 2);
                v.audioRate     = rd32(b + 4);
                v.audioBits     = rd16(b + 14);
            }
        } else if (fourcc(ch, "avih")) {
            uint8_t b[64];
            uint32_t n = size < sizeof(b) ? size : sizeof(b);
            fread(b, 1, n, f);
            if (v.width == 0)  v.width  = (int)rd32(b + 32);
            if (v.height == 0) v.height = (int)rd32(b + 36);
        }

        fseek(f, next, SEEK_SET);
        if (v.moviStart && ftell(f) >= v.moviEnd) break;
    }

    if (v.videoStream < 0 || v.moviStart == 0) return false;
    if (v.fps <= 0.0) v.fps = 30.0;
    v.hasAudio = (v.audioStream >= 0 && v.audioFmt == 1 &&
                  v.audioBits == 16 && v.audioChannels >= 1 && v.audioRate > 0);
    return true;
}

/* --------------------------- frame upload -------------------------- */

/* Nearest-neighbour shrink of an RGBA image to fit within maxW x maxH. */
static void present_frame(const unsigned char* rgba, int w, int h) {
    int dw = w, dh = h;
    if (dw > 400 || dh > 240) {
        double s = 400.0 / dw;
        if (240.0 / dh < s) s = 240.0 / dh;
        dw = (int)(w * s); dh = (int)(h * s);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    }
    if (dw > VID_TEX_W) dw = VID_TEX_W;
    if (dh > VID_TEX_H) dh = VID_TEX_H;

    int back = v.front ^ 1;
    u32* dst = (u32*)v.tex[back].data;

    for (int y = 0; y < dh; ++y) {
        int sy = (dh == h) ? y : (int)((int64_t)y * h / dh);
        for (int x = 0; x < dw; ++x) {
            int sx = (dw == w) ? x : (int)((int64_t)x * w / dw);
            const unsigned char* p = rgba + (sy * w + sx) * 4;
            u32 idx = tile_index((u32)x, (u32)y, VID_TEX_W);
            dst[idx] = ((u32)p[3]) | ((u32)p[2] << 8) | ((u32)p[1] << 16) | ((u32)p[0] << 24);
        }
    }
    GSPGPU_FlushDataCache(v.tex[back].data, v.tex[back].size);

    v.sub[back].width  = (u16)dw;
    v.sub[back].height = (u16)dh;
    v.sub[back].left   = 0.0f;
    v.sub[back].top    = 1.0f;
    v.sub[back].right  = (float)dw / (float)VID_TEX_W;
    v.sub[back].bottom = 1.0f - (float)dh / (float)VID_TEX_H;

    v.dispW = dw; v.dispH = dh;
    v.front = back;
    v.hasFrame = true;
}

/* ----------------------------- audio ------------------------------ */

static uint64_t audio_queued_samples(void) {
    uint64_t q = 0;
    for (int i = 0; i < VID_NBUF; ++i) {
        if (v.awb[i].status != NDSP_WBUF_DONE && v.awb[i].status != NDSP_WBUF_FREE)
            q += v.awb[i].nsamples;
    }
    return q;
}

static double clock_now(void) {
    if (v.hasAudio) {
        uint64_t played = v.audioSubmitted - audio_queued_samples();
        return v.clockBase + (double)played / (double)v.audioRate;
    }
    uint64_t ms = osGetTime() - v.startMs - v.pauseAccumMs;
    return v.clockBase + (double)ms / 1000.0;
}

/* Submit one audio chunk (blocking until a wave buffer is free). */
static void submit_audio(FILE* f, uint32_t size) {
    if (!v.hasAudio || size == 0 || size > VID_AUDIO_BUF) { fseek(f, size, SEEK_CUR); return; }

    int slot = -1;
    while (v.running && !v.stopReq) {
        for (int i = 0; i < VID_NBUF; ++i) {
            if (v.awb[i].status == NDSP_WBUF_DONE || v.awb[i].status == NDSP_WBUF_FREE) {
                slot = i; break;
            }
        }
        if (slot >= 0) break;
        svcSleepThread(2 * 1000 * 1000);
    }
    if (slot < 0) { fseek(f, size, SEEK_CUR); return; }

    if (fread(v.abuf[slot], 1, size, f) != size) return;
    uint32_t frames = size / (uint32_t)(v.audioChannels * 2);
    v.awb[slot].nsamples = frames;
    DSP_FlushDataCache(v.abuf[slot], size);
    ndspChnWaveBufAdd(VID_AUDIO_CHN, &v.awb[slot]);
    v.audioSubmitted += frames;
}

/* --------------------------- MP4/MOV path -------------------------- */

/* Top up the NDSP video-audio channel from the AAC decoder. Returns true
 * once the decoder has reached EOF and nothing new was queued. */
static bool mp4_feed_audio(AacDecoder* ad, size_t framesPerBuf) {
    bool eof = true;
    for (int i = 0; i < VID_NBUF; ++i) {
        if (v.awb[i].status != NDSP_WBUF_DONE && v.awb[i].status != NDSP_WBUF_FREE) {
            eof = false; continue;
        }
        size_t got = aac_read(ad, v.abuf[i], framesPerBuf);
        if (got == 0) continue;
        eof = false;
        v.awb[i].nsamples = (u32)got;
        DSP_FlushDataCache(v.abuf[i], got * 2 * sizeof(int16_t));
        ndspChnWaveBufAdd(VID_AUDIO_CHN, &v.awb[i]);
        v.audioSubmitted += got;
    }
    return eof;
}

static void play_mp4(void) {
    FILE* f = fopen(v.path, "rb");
    if (!f) { v.running = false; v.finished = true; return; }
    fseek(f, 0, SEEK_END); long fsize = ftell(f); fseek(f, 0, SEEK_SET);

    MP4D_demux_t mp4; memset(&mp4, 0, sizeof(mp4));
    if (!MP4D_open(&mp4, mp4_read_cb, f, (int64_t)fsize)) {
        fclose(f); v.running = false; v.finished = true; return;
    }

    int vt = -1;
    for (unsigned i = 0; i < mp4.track_count; ++i)
        if (mp4.track[i].handler_type == MP4D_HANDLER_TYPE_VIDE) { vt = (int)i; break; }
    if (vt < 0) { MP4D_close(&mp4); fclose(f); v.running = false; v.finished = true; return; }

    MP4D_track_t* T = &mp4.track[vt];
    unsigned nsamp = T->sample_count;
    v.width  = (int)T->SampleDescription.video.width;
    v.height = (int)T->SampleDescription.video.height;
    v.totalFrames = (long)nsamp;

    unsigned bytes0 = 0, ts0 = 0, dur0 = 0;
    MP4D_file_offset_t off0 = MP4D_frame_offset(&mp4, (unsigned)vt, 0, &bytes0, &ts0, &dur0);
    v.fps = (T->timescale && dur0) ? (double)T->timescale / (double)dur0 : 30.0;
    if (v.fps <= 0.0) v.fps = 30.0;
    v.duration = nsamp ? (double)nsamp / v.fps : 0.0;

    /* Probe: the first sample must decode as JPEG (MJPEG); otherwise it is a
     * codec we cannot software-decode (H.264/HEVC). */
    bool okJpeg = false;
    if (bytes0 > 0) {
        uint8_t* buf = (uint8_t*)malloc(bytes0);
        if (buf && fseek(f, (long)off0, SEEK_SET) == 0 && fread(buf, 1, bytes0, f) == bytes0) {
            int w = 0, h = 0, c = 0;
            unsigned char* px = stbi_load_from_memory(buf, (int)bytes0, &w, &h, &c, 4);
            if (px) { okJpeg = true; present_frame(px, w, h); stbi_image_free(px); }
        }
        free(buf);
    }
    if (!okJpeg) {
        strncpy(v.errmsg, "This .mp4 isn't MJPEG (likely H.264). Convert to MJPEG.",
                sizeof(v.errmsg) - 1);
        MP4D_close(&mp4); fclose(f);
        while (v.running && !v.stopReq) svcSleepThread(50 * 1000 * 1000);
        v.running = false; v.finished = false; return;
    }

    /* audio track decoded through the existing AAC front-end (stereo s16) */
    AacDecoder* ad = aac_open(v.path);
    v.hasAudio = (ad != NULL);
    if (v.hasAudio) {
        v.audioRate = aac_sample_rate(ad);
        if (v.audioRate == 0) { aac_close(ad); ad = NULL; v.hasAudio = false; }
    }
    if (v.hasAudio) {
        v.audioChannels = 2; /* aac_read always returns interleaved stereo */
        ndspChnReset(VID_AUDIO_CHN);
        ndspChnSetInterp(VID_AUDIO_CHN, NDSP_INTERP_LINEAR);
        ndspChnSetRate(VID_AUDIO_CHN, (float)v.audioRate);
        ndspChnSetFormat(VID_AUDIO_CHN, NDSP_FORMAT_STEREO_PCM16);
        float mix[12] = {0}; mix[0] = mix[1] = 1.0f;
        ndspChnSetMix(VID_AUDIO_CHN, mix);
        for (int i = 0; i < VID_NBUF; ++i) {
            v.abuf[i] = (int16_t*)linearAlloc(VID_AUDIO_BUF);
            memset(&v.awb[i], 0, sizeof(ndspWaveBuf));
            v.awb[i].data_vaddr = v.abuf[i];
            v.awb[i].status = NDSP_WBUF_DONE;
        }
    }

    v.startMs = osGetTime(); v.pauseAccumMs = 0; v.audioSubmitted = 0; v.clockBase = 0.0;
    size_t framesPerBuf = VID_AUDIO_BUF / (2 * sizeof(int16_t));
    bool audioDone = false;
    long vframe = 0;

    while (v.running && !v.stopReq && vframe < (long)nsamp) {
        while (v.paused && v.running && !v.stopReq) svcSleepThread(10 * 1000 * 1000);
        if (!v.running || v.stopReq) break;

        if (v.seekReq) {
            long target = (long)(v.seekFrac * (double)nsamp);
            if (target < 0) target = 0;
            if (target >= (long)nsamp) target = (long)nsamp - 1;
            if (v.hasAudio) {
                ndspChnWaveBufClear(VID_AUDIO_CHN);
                for (int i = 0; i < VID_NBUF; ++i) v.awb[i].status = NDSP_WBUF_DONE;
                uint64_t aframe = (uint64_t)((double)target / v.fps * (double)v.audioRate);
                aac_seek(ad, aframe);
                audioDone = false;
            }
            v.audioSubmitted = 0;
            v.clockBase = v.fps > 0 ? (double)target / v.fps : 0.0;
            v.startMs = osGetTime(); v.pauseAccumMs = 0;
            vframe = target; v.seekReq = false;
        }

        if (v.hasAudio && !audioDone) audioDone = mp4_feed_audio(ad, framesPerBuf);

        unsigned bytes = 0, ts = 0, dur = 0;
        MP4D_file_offset_t off = MP4D_frame_offset(&mp4, (unsigned)vt, (unsigned)vframe, &bytes, &ts, &dur);
        uint8_t* buf = bytes ? (uint8_t*)malloc(bytes) : NULL;
        unsigned char* px = NULL; int w = 0, h = 0, c = 0;
        if (buf && fseek(f, (long)off, SEEK_SET) == 0 && fread(buf, 1, bytes, f) == bytes)
            px = stbi_load_from_memory(buf, (int)bytes, &w, &h, &c, 4);
        free(buf);

        double target = (double)vframe / v.fps;
        while (v.running && !v.stopReq) {
            if (v.paused) { svcSleepThread(10 * 1000 * 1000); continue; }
            if (v.hasAudio && !audioDone) audioDone = mp4_feed_audio(ad, framesPerBuf);
            /* once audio ends, hand the clock to the wall timer so video finishes */
            if (v.hasAudio && audioDone && audio_queued_samples() == 0) {
                v.clockBase += (double)(v.audioSubmitted - 0) / (double)v.audioRate;
                v.audioSubmitted = 0;
                v.hasAudio = false;
                v.startMs = osGetTime(); v.pauseAccumMs = 0;
            }
            if (clock_now() >= target - 0.005) break;
            svcSleepThread(2 * 1000 * 1000);
        }
        if (px) { present_frame(px, w, h); stbi_image_free(px); }
        v.posSec = clock_now();
        vframe++;
    }

    if (!v.stopReq)
        while (v.running && v.hasAudio && ndspChnIsPlaying(VID_AUDIO_CHN))
            svcSleepThread(20 * 1000 * 1000);

    if (ad) aac_close(ad);
    if (v.abuf[0] || v.abuf[1]) {
        ndspChnWaveBufClear(VID_AUDIO_CHN);
        for (int i = 0; i < VID_NBUF; ++i)
            if (v.abuf[i]) { linearFree(v.abuf[i]); v.abuf[i] = NULL; }
    }
    MP4D_close(&mp4); fclose(f);
    v.finished = !v.stopReq;
    v.running = false;
}

/* --------------------------- worker loop --------------------------- */

static void video_worker(void* arg) {
    (void)arg;
    if (path_is_mp4(v.path)) { play_mp4(); return; }

    FILE* f = fopen(v.path, "rb");
    if (!f) { v.running = false; v.finished = true; return; }
    if (!parse_header(f)) { fclose(f); v.running = false; v.finished = true; return; }

    if (v.hasAudio) {
        ndspChnReset(VID_AUDIO_CHN);
        ndspChnSetInterp(VID_AUDIO_CHN, NDSP_INTERP_LINEAR);
        ndspChnSetRate(VID_AUDIO_CHN, (float)v.audioRate);
        ndspChnSetFormat(VID_AUDIO_CHN,
            v.audioChannels >= 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
        float mix[12] = {0};
        mix[0] = mix[1] = 1.0f;
        ndspChnSetMix(VID_AUDIO_CHN, mix);
        for (int i = 0; i < VID_NBUF; ++i) {
            v.abuf[i] = (int16_t*)linearAlloc(VID_AUDIO_BUF);
            memset(&v.awb[i], 0, sizeof(ndspWaveBuf));
            v.awb[i].data_vaddr = v.abuf[i];
            v.awb[i].status = NDSP_WBUF_DONE;
        }
    }

    v.startMs = osGetTime();
    v.pauseAccumMs = 0;
    v.audioSubmitted = 0;
    v.clockBase = 0.0;
    fseek(f, v.moviStart, SEEK_SET);

    long vframe = 0;
    uint8_t ch[8];

    while (v.running && !v.stopReq && ftell(f) < v.moviEnd) {
        while (v.paused && v.running && !v.stopReq) svcSleepThread(10 * 1000 * 1000);
        if (!v.running || v.stopReq) break;

        /* ---- handle a pending seek by scanning to the target frame ---- */
        if (v.seekReq) {
            long total = v.totalFrames > 0 ? v.totalFrames
                       : (v.fps > 0 ? (long)(v.duration * v.fps) : 0);
            if (total > 0) {
                long target = (long)(v.seekFrac * (double)total);
                if (target < 0) target = 0;
                if (target >= total) target = total - 1;

                if (v.hasAudio) {
                    ndspChnWaveBufClear(VID_AUDIO_CHN);
                    for (int i = 0; i < VID_NBUF; ++i) v.awb[i].status = NDSP_WBUF_DONE;
                }
                v.audioSubmitted = 0;

                fseek(f, v.moviStart, SEEK_SET);
                long vf = 0;
                uint8_t sh[8];
                while (vf < target && ftell(f) < v.moviEnd) {
                    if (fread(sh, 1, 8, f) != 8) break;
                    uint32_t ssz = rd32(sh + 4); long sp = ssz & 1;
                    if (fourcc(sh, "LIST") || fourcc(sh, "RIFF")) { fseek(f, 4, SEEK_CUR); continue; }
                    int sNo = (sh[0] - '0') * 10 + (sh[1] - '0');
                    fseek(f, ssz + sp, SEEK_CUR);
                    if (sNo == v.videoStream) vf++;
                }
                vframe = target;
                v.clockBase = v.fps > 0 ? (double)target / v.fps : 0.0;
                v.startMs = osGetTime();
                v.pauseAccumMs = 0;
            }
            v.seekReq = false;
        }

        if (fread(ch, 1, 8, f) != 8) break;
        uint32_t size = rd32(ch + 4);
        long pad = size & 1;

        if (fourcc(ch, "LIST") || fourcc(ch, "RIFF")) {
            fseek(f, 4, SEEK_CUR); /* skip list type, descend */
            continue;
        }

        int streamNo = (ch[0] - '0') * 10 + (ch[1] - '0');

        if (streamNo == v.videoStream) {
            uint8_t* jpg = (uint8_t*)malloc(size);
            if (!jpg) { fseek(f, size + pad, SEEK_CUR); continue; }
            if (fread(jpg, 1, size, f) != size) { free(jpg); break; }
            if (pad) fseek(f, pad, SEEK_CUR);

            int w = 0, h = 0, comp = 0;
            unsigned char* pix = stbi_load_from_memory(jpg, (int)size, &w, &h, &comp, 4);
            free(jpg);

            /* pace to presentation time */
            double target = (double)vframe / v.fps;
            while (v.running && !v.stopReq) {
                if (v.paused) { svcSleepThread(10 * 1000 * 1000); continue; }
                if (clock_now() >= target - 0.005) break;
                svcSleepThread(2 * 1000 * 1000);
            }
            if (pix) {
                present_frame(pix, w, h);
                stbi_image_free(pix);
            }
            v.posSec = clock_now();
            vframe++;
        } else if (v.hasAudio && streamNo == v.audioStream) {
            submit_audio(f, size);
            if (pad) fseek(f, pad, SEEK_CUR);
        } else {
            fseek(f, size + pad, SEEK_CUR);
        }
    }

    /* let queued audio drain unless we were told to stop */
    if (!v.stopReq) {
        while (v.running && v.hasAudio && ndspChnIsPlaying(VID_AUDIO_CHN))
            svcSleepThread(20 * 1000 * 1000);
    }

    if (v.hasAudio) {
        ndspChnWaveBufClear(VID_AUDIO_CHN);
        for (int i = 0; i < VID_NBUF; ++i)
            if (v.abuf[i]) { linearFree(v.abuf[i]); v.abuf[i] = NULL; }
    }
    fclose(f);
    v.finished = !v.stopReq;
    v.running = false;
}

/* ----------------------------- public ----------------------------- */

bool video_play(const char* path) {
    video_stop();

    v.stopReq = false;
    v.finished = false;
    v.paused = false;
    v.hasFrame = false;
    v.front = 0;
    v.posSec = 0.0;
    v.width = v.height = 0;
    v.moviStart = v.moviEnd = 0;
    v.seekReq = false;
    v.seekFrac = 0.0;
    v.clockBase = 0.0;
    v.totalFrames = 0;
    v.errmsg[0] = 0;
    strncpy(v.path, path, sizeof(v.path) - 1);
    v.path[sizeof(v.path) - 1] = 0;

    v.running = true;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    v.thread = threadCreate(video_worker, NULL, 256 * 1024, prio - 1, -1, false);
    if (!v.thread) { v.running = false; return false; }
    return true;
}

void video_stop(void) {
    if (v.thread) {
        v.stopReq = true;
        v.paused = false;
        threadJoin(v.thread, UINT64_MAX);
        threadFree(v.thread);
        v.thread = NULL;
    }
    v.running = false;
    v.hasFrame = false;
}

bool video_is_playing(void)  { return v.running; }
bool video_is_finished(void) { return v.finished; }
bool video_is_paused(void)   { return v.paused; }

void video_toggle_pause(void) {
    if (!v.running) return;
    if (!v.paused) {
        v.pauseStartMs = osGetTime();
        if (v.hasAudio) ndspChnSetPaused(VID_AUDIO_CHN, true);
        v.paused = true;
    } else {
        v.pauseAccumMs += osGetTime() - v.pauseStartMs;
        if (v.hasAudio) ndspChnSetPaused(VID_AUDIO_CHN, false);
        v.paused = false;
    }
}

void video_seek_fraction(double frac) {
    if (!v.running) return;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    v.seekFrac = frac;
    v.seekReq = true;
}

double video_position_sec(void) { return v.posSec; }
double video_duration_sec(void) { return v.duration; }
int    video_width(void)        { return v.width; }
int    video_height(void)       { return v.height; }
bool   video_has_audio(void)    { return v.hasAudio; }
const char* video_message(void) { return v.errmsg[0] ? v.errmsg : NULL; }

bool video_draw(float x, float y, float boxW, float boxH) {
    if (!v.hasFrame) return false;
    int idx = v.front;
    float w = (float)v.sub[idx].width;
    float h = (float)v.sub[idx].height;
    if (w <= 0 || h <= 0) return false;

    float scale = boxW / w;
    if (boxH / h < scale) scale = boxH / h;
    float drawW = w * scale, drawH = h * scale;
    float dx = x + (boxW - drawW) / 2.0f;
    float dy = y + (boxH - drawH) / 2.0f;
    C2D_DrawImageAt(v.img[idx], dx, dy, 0.5f, NULL, scale, scale);
    return true;
}
