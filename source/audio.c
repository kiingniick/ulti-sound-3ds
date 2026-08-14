#include "audio.h"
#include "decoder.h"

#include <3ds.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#define NUM_BUFFERS   2
#define BUF_FRAMES    8192            /* stereo frames per NDSP wave buffer */
#define NDSP_CHANNEL  0
#define VIZ_LEN       512             /* mono samples exposed to visualizers */
#define EQ_BANDS      5               /* graphic equalizer bands */

static const float kEqFreq[EQ_BANDS] = { 60.0f, 230.0f, 910.0f, 3600.0f, 14000.0f };

/* Direct-form-I biquad with independent L/R state (ch 0/1). */
typedef struct {
    float b0, b1, b2, a1, a2;
    float x1[2], x2[2], y1[2], y2[2];
} BiQuad;

static void bq_reset(BiQuad* b) {
    for (int c = 0; c < 2; ++c) { b->x1[c] = b->x2[c] = b->y1[c] = b->y2[c] = 0.0f; }
}
static void bq_identity(BiQuad* b) {
    b->b0 = 1.0f; b->b1 = b->b2 = b->a1 = b->a2 = 0.0f; bq_reset(b);
}
static inline float bq_proc(BiQuad* b, int c, float x) {
    float y = b->b0 * x + b->b1 * b->x1[c] + b->b2 * b->x2[c]
              - b->a1 * b->y1[c] - b->a2 * b->y2[c];
    b->x2[c] = b->x1[c]; b->x1[c] = x;
    b->y2[c] = b->y1[c]; b->y1[c] = y;
    return y;
}

/* RBJ cookbook peaking EQ filter at f0 with the given dB gain and Q. */
static void bq_peaking(BiQuad* b, float fs, float f0, float dbGain, float Q) {
    if (dbGain == 0.0f || fs <= 0.0f) { bq_identity(b); return; }
    float A  = powf(10.0f, dbGain / 40.0f);
    float w0 = 2.0f * 3.14159265358979f * f0 / fs;
    float cw = cosf(w0), sw = sinf(w0);
    float alpha = sw / (2.0f * Q);
    float a0 = 1.0f + alpha / A;
    b->b0 = (1.0f + alpha * A) / a0;
    b->b1 = (-2.0f * cw)       / a0;
    b->b2 = (1.0f - alpha * A) / a0;
    b->a1 = (-2.0f * cw)       / a0;
    b->a2 = (1.0f - alpha / A) / a0;
}

typedef struct {
    volatile AudioState state;
    volatile bool       finished;      /* set when track drains naturally */
    volatile bool       finishedLatch; /* consumed by audio_track_finished */
    volatile bool       decoderEnded;  /* decoder returned EOF, drain remaining */

    Decoder* dec;
    uint32_t sampleRate;
    uint64_t totalFrames;

    int16_t*    buffers;               /* linear PCM storage for all wave bufs */
    ndspWaveBuf wbuf[NUM_BUFFERS];

    LightLock lock;
    Thread    thread;
    volatile bool threadRun;

    int   volume;   /* 0..100 */
    int   preamp;   /* 0..300 (%), extra gain applied on top of volume */
    float mix[12];

    /* ---- audio enhancement (DSP) ---- */
    int    effPreset;      /* 0..AUDIO_EFFECT_COUNT-1 */
    int    eqGain[EQ_BANDS];  /* -12..+12 dB per band */
    int    width;          /* 0..100 (50 = normal, <50 narrower, >50 wider) */
    BiQuad eq[EQ_BANDS];
    volatile bool effDirty;

    /* ---- visualizer sample tap ---- */
    int16_t vizBuf[VIZ_LEN];
    volatile int vizReady;

    /* Deferred open: decoder_open() (which primes faad2 for AAC) must run on
     * this thread's large stack, never on the small main-thread stack. */
    volatile bool openReq;
    volatile bool openDone;
    volatile bool openOk;
    char          openPath[1024];
} Audio;

static Audio g;

static void apply_volume_locked(void) {
    /* Pre-amp multiplies the volume; values >100% amplify (may clip loudly). */
    float v = ((float)g.volume / 100.0f) * ((float)g.preamp / 100.0f);
    if (v < 0.0f) v = 0.0f;
    if (v > 4.0f) v = 4.0f;
    for (int i = 0; i < 12; ++i) g.mix[i] = 0.0f;
    g.mix[0] = v; /* front left  */
    g.mix[1] = v; /* front right */
    ndspChnSetMix(NDSP_CHANNEL, g.mix);
}

/* ---- audio enhancement presets (5-band graphic EQ gains + stereo width) ---- */
typedef struct { const char* name; int gain[EQ_BANDS]; int width; } EffPreset;
static const EffPreset kPresets[] = {
    /* name               60  230  910  3.6k 14k   width */
    { "Off",              {  0,  0,  0,  0,  0 }, 50 },
    { "Bass Boost",       {  8,  6,  1,  0,  0 }, 50 },
    { "Treble",           {  0,  0,  0,  5,  7 }, 50 },
    { "Vocal Clarity",    { -3, -1,  4,  3,  0 }, 40 },
    { "Virtual Surround", {  3,  2,  0,  2,  4 }, 82 },
    { "Loudness",         {  6,  3,  0,  3,  6 }, 56 },
    { "Rock",             {  5,  3, -1,  3,  5 }, 55 },
    { "Custom",           {  0,  0,  0,  0,  0 }, 50 },
};
#define EFF_COUNT ((int)(sizeof(kPresets) / sizeof(kPresets[0])))
#define EFF_CUSTOM (EFF_COUNT - 1)

static void recompute_effects_locked(void) {
    float fs = g.sampleRate ? (float)g.sampleRate : 44100.0f;
    for (int i = 0; i < EQ_BANDS; ++i)
        bq_peaking(&g.eq[i], fs, kEqFreq[i], (float)g.eqGain[i], 1.1f);
    g.effDirty = false;
}

/* True when no processing is needed (bypass for the cleanest signal path). */
static bool effects_bypassed(void) {
    if (g.width != 50) return false;
    for (int i = 0; i < EQ_BANDS; ++i) if (g.eqGain[i] != 0) return false;
    return true;
}

/* In-place multiband EQ + mid/side stereo width on interleaved s16. */
static void apply_effects(int16_t* pcm, size_t frames) {
    if (effects_bypassed()) return;
    if (g.effDirty) recompute_effects_locked();
    float sideGain = (float)g.width / 50.0f;   /* 0..2 */

    for (size_t i = 0; i < frames; ++i) {
        float l = pcm[2 * i]     / 32768.0f;
        float r = pcm[2 * i + 1] / 32768.0f;

        for (int b = 0; b < EQ_BANDS; ++b) {
            l = bq_proc(&g.eq[b], 0, l);
            r = bq_proc(&g.eq[b], 1, r);
        }

        if (sideGain != 1.0f) {
            float mid  = (l + r) * 0.5f;
            float side = (l - r) * 0.5f * sideGain;
            l = mid + side;
            r = mid - side;
        }

        if (l >  1.0f) l =  1.0f;
        if (l < -1.0f) l = -1.0f;
        if (r >  1.0f) r =  1.0f;
        if (r < -1.0f) r = -1.0f;
        pcm[2 * i]     = (int16_t)(l * 32767.0f);
        pcm[2 * i + 1] = (int16_t)(r * 32767.0f);
    }
}

/* Copy the most recent contiguous mono window for the visualizers/FFT. */
static void viz_capture(const int16_t* pcm, size_t frames) {
    if (frames == 0) return;
    size_t start = frames >= VIZ_LEN ? frames - VIZ_LEN : 0;
    size_t n = frames >= VIZ_LEN ? VIZ_LEN : frames;
    for (size_t i = 0; i < n; ++i) {
        int v = (pcm[2 * (start + i)] + pcm[2 * (start + i) + 1]) / 2;
        g.vizBuf[i] = (int16_t)v;
    }
    for (size_t i = n; i < VIZ_LEN; ++i) g.vizBuf[i] = 0;
    g.vizReady = 1;
}

static void reset_wavebufs_locked(void) {
    ndspChnWaveBufClear(NDSP_CHANNEL);
    for (int i = 0; i < NUM_BUFFERS; ++i) {
        memset(&g.wbuf[i], 0, sizeof(ndspWaveBuf));
        g.wbuf[i].data_vaddr = &g.buffers[i * BUF_FRAMES * 2];
        g.wbuf[i].nsamples   = 0;
        g.wbuf[i].status     = NDSP_WBUF_DONE; /* mark as free to fill */
    }
}

/* Fill any free/finished wave buffers from the decoder. Assumes lock held. */
static void pump_locked(void) {
    if (!g.dec || g.state != AUDIO_PLAYING) return;

    for (int i = 0; i < NUM_BUFFERS; ++i) {
        if (g.wbuf[i].status != NDSP_WBUF_DONE && g.wbuf[i].status != NDSP_WBUF_FREE)
            continue;
        if (g.decoderEnded) continue;

        int16_t* dst = (int16_t*)g.wbuf[i].data_vaddr;
        size_t got = decoder_read(g.dec, dst, BUF_FRAMES);
        if (got == 0) {
            g.decoderEnded = true;
            continue;
        }
        apply_effects(dst, got);
        viz_capture(dst, got);
        g.wbuf[i].nsamples = (u32)got;
        DSP_FlushDataCache(dst, got * 2 * sizeof(int16_t));
        ndspChnWaveBufAdd(NDSP_CHANNEL, &g.wbuf[i]);
    }

    if (g.decoderEnded && !ndspChnIsPlaying(NDSP_CHANNEL)) {
        g.state    = AUDIO_STOPPED;
        g.finished = true;
    }
}

/* Open a decoder and install it as the current track. Runs ON the audio
 * thread so the (potentially very stack-hungry) faad2 AAC decode happens on
 * this thread's large stack rather than the small main-thread stack. */
static void handle_open_request(void) {
    bool doOpen = false;
    char path[1024];

    LightLock_Lock(&g.lock);
    if (g.openReq) {
        g.openReq = false;
        doOpen = true;
        strncpy(path, g.openPath, sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
    }
    LightLock_Unlock(&g.lock);

    if (!doOpen) return;

    Decoder* nd = decoder_open(path); /* heavy work, no lock held */

    LightLock_Lock(&g.lock);
    ndspChnWaveBufClear(NDSP_CHANNEL);
    if (g.dec) decoder_close(g.dec);
    if (nd) {
        g.dec           = nd;
        g.sampleRate    = decoder_sample_rate(nd);
        g.totalFrames   = decoder_total_frames(nd);
        g.decoderEnded  = false;
        g.finished      = false;
        g.finishedLatch = false;
        ndspChnSetRate(NDSP_CHANNEL, (float)g.sampleRate);
        for (int i = 0; i < EQ_BANDS; ++i) bq_reset(&g.eq[i]);
        recompute_effects_locked();
        reset_wavebufs_locked();
        apply_volume_locked();
        g.state  = AUDIO_PLAYING;
        pump_locked();
        g.openOk = true;
    } else {
        g.dec    = NULL;
        g.state  = AUDIO_STOPPED;
        g.openOk = false;
    }
    g.openDone = true;
    LightLock_Unlock(&g.lock);
}

static void audio_thread(void* arg) {
    (void)arg;
    while (g.threadRun) {
        handle_open_request();

        LightLock_Lock(&g.lock);
        if (g.state == AUDIO_PLAYING) {
            pump_locked();
            if (g.finished && !g.finishedLatch) {
                g.finishedLatch = true;
            }
        }
        LightLock_Unlock(&g.lock);
        svcSleepThread(5 * 1000 * 1000); /* 5 ms */
    }
}

bool audio_init(void) {
    memset(&g, 0, sizeof(g));
    g.volume = 80;
    g.preamp = 100;
    g.effPreset = 0;
    g.width = 50;
    for (int i = 0; i < EQ_BANDS; ++i) { g.eqGain[i] = 0; bq_identity(&g.eq[i]); }
    g.effDirty = true;

    if (R_FAILED(ndspInit())) return false;

    g.buffers = (int16_t*)linearAlloc(NUM_BUFFERS * BUF_FRAMES * 2 * sizeof(int16_t));
    if (!g.buffers) { ndspExit(); return false; }

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(NDSP_CHANNEL);
    ndspChnSetInterp(NDSP_CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetRate(NDSP_CHANNEL, 44100.0f);
    ndspChnSetFormat(NDSP_CHANNEL, NDSP_FORMAT_STEREO_PCM16);

    LightLock_Init(&g.lock);
    apply_volume_locked();
    reset_wavebufs_locked();

    g.state    = AUDIO_STOPPED;
    g.threadRun = true;

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    /* Slightly higher priority than main so buffers refill promptly.
     * The stack must be generous: the faad2 AAC decoder (esp. HE-AAC SBR/PS)
     * has a deep call chain with large on-stack arrays. 32 KB overflowed. */
    g.thread = threadCreate(audio_thread, NULL, 512 * 1024, prio - 1, -1, false);
    if (!g.thread) {
        g.threadRun = false;
        linearFree(g.buffers);
        ndspExit();
        return false;
    }
    return true;
}

void audio_exit(void) {
    g.threadRun = false;
    if (g.thread) {
        threadJoin(g.thread, UINT64_MAX);
        threadFree(g.thread);
        g.thread = NULL;
    }
    LightLock_Lock(&g.lock);
    ndspChnWaveBufClear(NDSP_CHANNEL);
    if (g.dec) { decoder_close(g.dec); g.dec = NULL; }
    LightLock_Unlock(&g.lock);

    if (g.buffers) { linearFree(g.buffers); g.buffers = NULL; }
    ndspExit();
}

bool audio_play(const char* path) {
    /* Hand the open off to the audio thread (large stack) and wait for it.
     * This keeps faad2's deep, stack-hungry AAC decode off the main thread,
     * whose ~32 KB stack (fixed by the 3dsx loader) would overflow. */
    LightLock_Lock(&g.lock);
    ndspChnWaveBufClear(NDSP_CHANNEL);
    g.state    = AUDIO_STOPPED;   /* stop the old track immediately */
    strncpy(g.openPath, path, sizeof(g.openPath) - 1);
    g.openPath[sizeof(g.openPath) - 1] = 0;
    g.openReq  = true;
    g.openDone = false;
    g.openOk   = false;
    LightLock_Unlock(&g.lock);

    /* Wait for the worker to finish opening (bounded, ~10 s safety timeout). */
    for (int i = 0; i < 10000 && g.threadRun; ++i) {
        if (g.openDone) break;
        svcSleepThread(1 * 1000 * 1000); /* 1 ms */
    }
    return g.openOk;
}

void audio_stop(void) {
    LightLock_Lock(&g.lock);
    ndspChnWaveBufClear(NDSP_CHANNEL);
    if (g.dec) { decoder_close(g.dec); g.dec = NULL; }
    g.state = AUDIO_STOPPED;
    g.decoderEnded = false;
    LightLock_Unlock(&g.lock);
}

void audio_set_paused(bool paused) {
    LightLock_Lock(&g.lock);
    if (g.dec) {
        if (paused && g.state == AUDIO_PLAYING) {
            g.state = AUDIO_PAUSED;
            ndspChnSetPaused(NDSP_CHANNEL, true);
        } else if (!paused && g.state == AUDIO_PAUSED) {
            g.state = AUDIO_PLAYING;
            ndspChnSetPaused(NDSP_CHANNEL, false);
        }
    }
    LightLock_Unlock(&g.lock);
}

void audio_toggle_pause(void) {
    audio_set_paused(g.state == AUDIO_PLAYING);
}

AudioState audio_state(void) { return g.state; }
bool audio_is_paused(void)   { return g.state == AUDIO_PAUSED; }

bool audio_track_finished(void) {
    LightLock_Lock(&g.lock);
    bool f = g.finishedLatch;
    g.finishedLatch = false;
    if (f) g.finished = false;
    LightLock_Unlock(&g.lock);
    return f;
}

double audio_position_sec(void) {
    if (!g.dec || g.sampleRate == 0) return 0.0;
    return (double)decoder_cur_frame(g.dec) / (double)g.sampleRate;
}

double audio_duration_sec(void) {
    if (g.sampleRate == 0) return 0.0;
    return (double)g.totalFrames / (double)g.sampleRate;
}

uint32_t audio_sample_rate(void) { return g.sampleRate; }

const char* audio_format_name(void) {
    return g.dec ? decoder_type_name(g.dec) : "-";
}

void audio_seek_fraction(double frac) {
    if (!g.dec || g.totalFrames == 0) return;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    uint64_t target = (uint64_t)(frac * (double)g.totalFrames);

    LightLock_Lock(&g.lock);
    ndspChnWaveBufClear(NDSP_CHANNEL);
    /* decoder_seek only repositions (cheap, no heavy decode), so it is safe on
     * the main thread. The actual refill (decoder_read) is left to the audio
     * thread's next pump, keeping faad2's decode off the small main stack. */
    if (decoder_seek(g.dec, target)) {
        g.decoderEnded = false;
        reset_wavebufs_locked();
        if (g.state == AUDIO_PAUSED) {
            g.state = AUDIO_PLAYING;
            ndspChnSetPaused(NDSP_CHANNEL, false);
        }
    }
    LightLock_Unlock(&g.lock);
}

void audio_seek_relative(double delta_sec) {
    double dur = audio_duration_sec();
    if (dur <= 0.0) return;
    double pos = audio_position_sec() + delta_sec;
    audio_seek_fraction(pos / dur);
}

void audio_set_volume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    LightLock_Lock(&g.lock);
    g.volume = vol;
    apply_volume_locked();
    LightLock_Unlock(&g.lock);
}

int audio_get_volume(void) { return g.volume; }

void audio_volume_step(int delta) {
    audio_set_volume(g.volume + delta);
}

void audio_set_preamp(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 300) percent = 300;
    LightLock_Lock(&g.lock);
    g.preamp = percent;
    apply_volume_locked();
    LightLock_Unlock(&g.lock);
}

int audio_get_preamp(void) { return g.preamp; }

/* -------------------- audio enhancement (DSP) -------------------- */

int         audio_effect_count(void)         { return EFF_COUNT; }
const char* audio_effect_name(int preset) {
    if (preset < 0 || preset >= EFF_COUNT) return "-";
    return kPresets[preset].name;
}
int audio_get_effect_preset(void) { return g.effPreset; }
int audio_get_width(void)         { return g.width; }

void audio_set_effect_preset(int preset) {
    if (preset < 0 || preset >= EFF_COUNT) return;
    LightLock_Lock(&g.lock);
    g.effPreset = preset;
    if (preset != EFF_CUSTOM) {
        for (int i = 0; i < EQ_BANDS; ++i) g.eqGain[i] = kPresets[preset].gain[i];
        g.width = kPresets[preset].width;
    }
    g.effDirty = true;
    recompute_effects_locked();
    LightLock_Unlock(&g.lock);
}

static void set_custom_locked(void) { g.effPreset = EFF_CUSTOM; g.effDirty = true; recompute_effects_locked(); }

/* ---- multiband EQ ---- */
int         audio_eq_band_count(void) { return EQ_BANDS; }
int         audio_eq_get(int band) {
    if (band < 0 || band >= EQ_BANDS) return 0;
    return g.eqGain[band];
}
const char* audio_eq_label(int band) {
    static const char* labels[EQ_BANDS] = { "60", "230", "910", "3.6k", "14k" };
    if (band < 0 || band >= EQ_BANDS) return "-";
    return labels[band];
}
void audio_eq_set(int band, int db) {
    if (band < 0 || band >= EQ_BANDS) return;
    if (db < -12) db = -12;
    if (db > 12)  db = 12;
    LightLock_Lock(&g.lock); g.eqGain[band] = db; set_custom_locked(); LightLock_Unlock(&g.lock);
}

void audio_set_width(int w) {
    if (w < 0)   w = 0;
    if (w > 100) w = 100;
    LightLock_Lock(&g.lock); g.width = w; set_custom_locked(); LightLock_Unlock(&g.lock);
}

/* -------------------------- visualizer -------------------------- */

int audio_get_wave(float* out, int max) {
    if (!out || max <= 0) return 0;
    int n = max < VIZ_LEN ? max : VIZ_LEN;
    LightLock_Lock(&g.lock);
    bool ready = g.vizReady && g.state == AUDIO_PLAYING;
    for (int i = 0; i < n; ++i)
        out[i] = ready ? (float)g.vizBuf[i] / 32768.0f : 0.0f;
    LightLock_Unlock(&g.lock);
    return n;
}
