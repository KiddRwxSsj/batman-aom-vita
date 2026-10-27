// just enough OpenSL ES for UOpenSLAudioDevice: an engine, an output mix and pcm players fed
// through buffer queues, all mixed into one sceAudioOut port

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stddef.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
// the android header tags a few things with clang-only availability attributes
#define SL_API_DEPRECATED(level)
#include <opensles/OpenSLES.h>

#include "opensl.h"
#include "util.h"

#define OUT_RATE 48000
#define OUT_GRAIN 512
#define MAX_PLAYERS 48
#define MAX_QUEUE 16

static const struct SLInterfaceID_ iid_engine = { 1 }, iid_play = { 2 }, iid_volume = { 3 }, iid_bufferqueue = { 4 };
static const SLInterfaceID sl_iid_engine = &iid_engine, sl_iid_play = &iid_play,
                           sl_iid_volume = &iid_volume, sl_iid_bufferqueue = &iid_bufferqueue;

enum { KIND_ENGINE, KIND_MIX, KIND_PLAYER };

typedef struct {
    const struct SLObjectItf_ *obj;
    const struct SLEngineItf_ *engine;
    const struct SLPlayItf_ *play;
    const struct SLVolumeItf_ *volume;
    const struct SLBufferQueueItf_ *bq;
    int kind;
    int used;

    // players only
    int channels, rate, bits;
    SLuint32 state;
    float gain;
    int muted;
    struct { const uint8_t *data; uint32_t frames; } queue[MAX_QUEUE];
    int head, count;
    uint32_t played;            // buffers finished since the last Clear
    uint64_t pos;               // 32.32 frames into queue[head]
    uint64_t total_frames;
    slBufferQueueCallback cb;
    void *cb_ctx;
} sl_obj;

#define FROM(p, field) ((sl_obj *)((char *)(p) - offsetof(sl_obj, field)))

static sl_obj objs[MAX_PLAYERS + 2];
// lw mutex, the mixer is a plain kernel thread
static SceKernelLwMutexWork lock;
static int port = -1;

static SLresult ok(void) { return SL_RESULT_SUCCESS; }
static SLresult unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }

// object

static SLresult obj_realize(SLObjectItf self, SLboolean async) { return SL_RESULT_SUCCESS; }
static SLresult obj_resume(SLObjectItf self, SLboolean async) { return SL_RESULT_SUCCESS; }

static SLresult obj_get_state(SLObjectItf self, SLuint32 *state) {
    *state = SL_OBJECT_STATE_REALIZED;
    return SL_RESULT_SUCCESS;
}

static SLresult obj_get_interface(SLObjectItf self, const SLInterfaceID iid, void *out) {
    sl_obj *o = FROM(self, obj);
    void **itf = out;
    if (iid == sl_iid_engine && o->kind == KIND_ENGINE)
        *itf = &o->engine;
    else if (iid == sl_iid_play && o->kind == KIND_PLAYER)
        *itf = &o->play;
    else if (iid == sl_iid_volume && o->kind == KIND_PLAYER)
        *itf = &o->volume;
    else if (iid == sl_iid_bufferqueue && o->kind == KIND_PLAYER)
        *itf = &o->bq;
    else {
        LOG("opensl: GetInterface %p on kind %d unsupported", iid, o->kind);
        return SL_RESULT_FEATURE_UNSUPPORTED;
    }
    return SL_RESULT_SUCCESS;
}

static void obj_destroy(SLObjectItf self) {
    sl_obj *o = FROM(self, obj);
    sceKernelLockLwMutex(&lock, 1, NULL);
    o->used = 0;
    o->count = 0;
    o->state = SL_PLAYSTATE_STOPPED;
    sceKernelUnlockLwMutex(&lock, 1);
}

static void obj_abort(SLObjectItf self) {}

static const struct SLObjectItf_ obj_vt = {
    obj_realize,
    obj_resume,
    obj_get_state,
    obj_get_interface,
    (void *)unsupported,
    obj_abort,
    obj_destroy,
    (void *)ok,
    (void *)unsupported,
    (void *)ok,
};

static const struct SLEngineItf_ engine_vt;
static const struct SLPlayItf_ play_vt;
static const struct SLVolumeItf_ volume_vt;
static const struct SLBufferQueueItf_ bq_vt;

static sl_obj *obj_new(int kind) {
    sceKernelLockLwMutex(&lock, 1, NULL);
    for (int i = 0; i < (int)(sizeof(objs) / sizeof(objs[0])); i++) {
        sl_obj *o = &objs[i];
        if (o->used)
            continue;
        memset(o, 0, sizeof(*o));
        o->used = 1;
        o->kind = kind;
        o->obj = &obj_vt;
        o->engine = &engine_vt;
        o->play = &play_vt;
        o->volume = &volume_vt;
        o->bq = &bq_vt;
        o->state = SL_PLAYSTATE_STOPPED;
        o->gain = 1.0f;
        sceKernelUnlockLwMutex(&lock, 1);
        return o;
    }
    sceKernelUnlockLwMutex(&lock, 1);
    return NULL;
}

// engine

static SLresult engine_create_player(SLEngineItf self, SLObjectItf *out, SLDataSource *src, SLDataSink *sink,
                                     SLuint32 n, const SLInterfaceID *ids, const SLboolean *req) {
    SLuint32 locator = *(SLuint32 *)src->pLocator;
    SLDataFormat_PCM *fmt = src->pFormat;
    // 0x800007bd is android's simple buffer queue, same interface as the plain one
    if ((locator != SL_DATALOCATOR_BUFFERQUEUE && locator != 0x800007bd) || fmt->formatType != SL_DATAFORMAT_PCM) {
        LOG("opensl: player with locator %x format %x unsupported", (unsigned)locator, (unsigned)fmt->formatType);
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }
    sl_obj *o = obj_new(KIND_PLAYER);
    if (!o) {
        LOG("opensl: out of players");
        return SL_RESULT_MEMORY_FAILURE;
    }
    o->channels = fmt->numChannels;
    o->rate = fmt->samplesPerSec > 1000000 ? fmt->samplesPerSec / 1000 : fmt->samplesPerSec;
    o->bits = fmt->bitsPerSample;
    VLOG("opensl: player %d ch %d hz %d bits", o->channels, o->rate, o->bits);
    *out = (SLObjectItf)&o->obj;
    return SL_RESULT_SUCCESS;
}

static SLresult engine_create_mix(SLEngineItf self, SLObjectItf *out, SLuint32 n, const SLInterfaceID *ids,
                                  const SLboolean *req) {
    sl_obj *o = obj_new(KIND_MIX);
    if (!o)
        return SL_RESULT_MEMORY_FAILURE;
    *out = (SLObjectItf)&o->obj;
    return SL_RESULT_SUCCESS;
}

static const struct SLEngineItf_ engine_vt = {
    (void *)unsupported,
    (void *)unsupported,
    engine_create_player,
    (void *)unsupported,
    (void *)unsupported,
    (void *)unsupported,
    (void *)unsupported,
    engine_create_mix,
    (void *)unsupported,
    (void *)unsupported,
    (void *)unsupported,
    (void *)unsupported,
    (void *)unsupported,
    (void *)unsupported,
    (void *)unsupported,
};

// play

static SLresult play_set_state(SLPlayItf self, SLuint32 state) {
    sl_obj *o = FROM(self, play);
    sceKernelLockLwMutex(&lock, 1, NULL);
    o->state = state;
    // stopping rewinds whatever is queued
    if (state == SL_PLAYSTATE_STOPPED)
        o->pos = 0;
    sceKernelUnlockLwMutex(&lock, 1);
    return SL_RESULT_SUCCESS;
}

static SLresult play_get_state(SLPlayItf self, SLuint32 *state) {
    *state = FROM(self, play)->state;
    return SL_RESULT_SUCCESS;
}

static SLresult play_get_duration(SLPlayItf self, SLmillisecond *ms) {
    *ms = SL_TIME_UNKNOWN;
    return SL_RESULT_SUCCESS;
}

static SLresult play_get_position(SLPlayItf self, SLmillisecond *ms) {
    sl_obj *o = FROM(self, play);
    *ms = o->rate ? (SLmillisecond)(o->total_frames * 1000 / o->rate) : 0;
    return SL_RESULT_SUCCESS;
}

static const struct SLPlayItf_ play_vt = {
    play_set_state,
    play_get_state,
    play_get_duration,
    play_get_position,
    (void *)ok,
    (void *)ok,
    (void *)ok,
    (void *)ok,
    (void *)ok,
    (void *)ok,
    (void *)ok,
    (void *)ok,
};

// volume

static SLresult vol_set_level(SLVolumeItf self, SLmillibel mb) {
    FROM(self, volume)->gain = mb <= -9600 ? 0.0f : powf(10.0f, mb / 2000.0f);
    return SL_RESULT_SUCCESS;
}

static SLresult vol_get_level(SLVolumeItf self, SLmillibel *mb) {
    float g = FROM(self, volume)->gain;
    *mb = g > 0.0f ? (SLmillibel)(2000.0f * log10f(g)) : SL_MILLIBEL_MIN;
    return SL_RESULT_SUCCESS;
}

static SLresult vol_get_max(SLVolumeItf self, SLmillibel *mb) {
    *mb = 0;
    return SL_RESULT_SUCCESS;
}

static SLresult vol_set_mute(SLVolumeItf self, SLboolean mute) {
    FROM(self, volume)->muted = mute;
    return SL_RESULT_SUCCESS;
}

static SLresult vol_get_mute(SLVolumeItf self, SLboolean *mute) {
    *mute = FROM(self, volume)->muted;
    return SL_RESULT_SUCCESS;
}

static const struct SLVolumeItf_ volume_vt = {
    vol_set_level,
    vol_get_level,
    vol_get_max,
    vol_set_mute,
    vol_get_mute,
    (void *)ok,
    (void *)unsupported,
    (void *)ok,
    (void *)unsupported,
};

// buffer queue

static SLresult bq_enqueue(SLBufferQueueItf self, const void *buf, SLuint32 size) {
    sl_obj *o = FROM(self, bq);
    int frame = o->channels * (o->bits / 8);
    sceKernelLockLwMutex(&lock, 1, NULL);
    if (o->count >= MAX_QUEUE) {
        sceKernelUnlockLwMutex(&lock, 1);
        return SL_RESULT_BUFFER_INSUFFICIENT;
    }
    int i = (o->head + o->count) % MAX_QUEUE;
    o->queue[i].data = buf;
    o->queue[i].frames = frame ? size / frame : 0;
    o->count++;
    sceKernelUnlockLwMutex(&lock, 1);
    return SL_RESULT_SUCCESS;
}

static SLresult bq_clear(SLBufferQueueItf self) {
    sl_obj *o = FROM(self, bq);
    sceKernelLockLwMutex(&lock, 1, NULL);
    o->count = 0;
    o->pos = 0;
    o->played = 0;
    sceKernelUnlockLwMutex(&lock, 1);
    return SL_RESULT_SUCCESS;
}

static SLresult bq_get_state(SLBufferQueueItf self, SLBufferQueueState *st) {
    sl_obj *o = FROM(self, bq);
    st->count = o->count;
    st->playIndex = o->played;
    return SL_RESULT_SUCCESS;
}

static SLresult bq_register(SLBufferQueueItf self, slBufferQueueCallback cb, void *ctx) {
    sl_obj *o = FROM(self, bq);
    o->cb = cb;
    o->cb_ctx = ctx;
    return SL_RESULT_SUCCESS;
}

static const struct SLBufferQueueItf_ bq_vt = {
    bq_enqueue,
    bq_clear,
    bq_get_state,
    bq_register,
};

static SLresult sl_create_engine(SLObjectItf *out, SLuint32 n, const SLEngineOption *opts, SLuint32 ni,
                                 const SLInterfaceID *ids, const SLboolean *req) {
    sl_obj *o = obj_new(KIND_ENGINE);
    if (!o)
        return SL_RESULT_MEMORY_FAILURE;
    *out = (SLObjectItf)&o->obj;
    LOG("opensl: engine created");
    return SL_RESULT_SUCCESS;
}

// mixer

static inline int sample(const sl_obj *o, const uint8_t *data, uint32_t frame, int ch) {
    if (o->channels == 1)
        ch = 0;
    if (o->bits == 8)
        return ((int)data[frame * o->channels + ch] - 128) << 8;
    return ((const int16_t *)data)[frame * o->channels + ch];
}

static int mix_thread(SceSize args, void *argp) {
    static int32_t acc[OUT_GRAIN * 2];
    static int16_t out[2][OUT_GRAIN * 2];
    int buf = 0;
    while (1) {
        struct { slBufferQueueCallback cb; SLBufferQueueItf itf; void *ctx; } fire[MAX_PLAYERS * 2];
        int nfire = 0;
        memset(acc, 0, sizeof(acc));

        sceKernelLockLwMutex(&lock, 1, NULL);
        for (int p = 0; p < (int)(sizeof(objs) / sizeof(objs[0])); p++) {
            sl_obj *o = &objs[p];
            if (!o->used || o->kind != KIND_PLAYER || o->state != SL_PLAYSTATE_PLAYING || !o->count || !o->rate)
                continue;
            int gain = o->muted ? 0 : (int)(o->gain * 256.0f);
            uint64_t step = ((uint64_t)o->rate << 32) / OUT_RATE;
            for (int i = 0; i < OUT_GRAIN && o->count; i++) {
                uint32_t f = o->pos >> 32;
                if (f >= o->queue[o->head].frames) {
                    o->pos -= (uint64_t)o->queue[o->head].frames << 32;
                    o->head = (o->head + 1) % MAX_QUEUE;
                    o->count--;
                    o->played++;
                    if (o->cb && nfire < (int)(sizeof(fire) / sizeof(fire[0]))) {
                        fire[nfire].cb = o->cb;
                        fire[nfire].itf = (SLBufferQueueItf)&o->bq;
                        fire[nfire].ctx = o->cb_ctx;
                        nfire++;
                    }
                    i--;
                    continue;
                }
                const uint8_t *data = o->queue[o->head].data;
                uint32_t frames = o->queue[o->head].frames;
                uint32_t frac = (o->pos >> 16) & 0xffff;
                for (int ch = 0; ch < 2; ch++) {
                    int a = sample(o, data, f, ch);
                    int b = f + 1 < frames ? sample(o, data, f + 1, ch) : a;
                    acc[i * 2 + ch] += ((a + (((b - a) * (int)frac) >> 16)) * gain) >> 8;
                }
                o->pos += step;
                o->total_frames++;
            }
        }
        sceKernelUnlockLwMutex(&lock, 1);

        // the engine's callback enqueues the next chunk, so it runs with the lock dropped
        for (int i = 0; i < nfire; i++)
            fire[i].cb(fire[i].itf, fire[i].ctx);

        for (int i = 0; i < OUT_GRAIN * 2; i++) {
            int v = acc[i];
            out[buf][i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        }
        sceAudioOutOutput(port, out[buf]);
        buf ^= 1;
    }
    return 0;
}

void opensl_init(void) {
    sceKernelCreateLwMutex(&lock, "opensl", 0, 0, NULL);
    port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, OUT_GRAIN, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        LOG("opensl: sceAudioOutOpenPort failed %x", port);
        return;
    }
    SceUID t = sceKernelCreateThread("opensl_mix", mix_thread, 64, 0x10000, 0, 0, NULL);
    sceKernelStartThread(t, 0, NULL);
}

void *opensl_sym(const char *name) {
    if (!strcmp(name, "slCreateEngine"))
        return sl_create_engine;
    if (!strcmp(name, "SL_IID_ENGINE"))
        return (void *)&sl_iid_engine;
    if (!strcmp(name, "SL_IID_PLAY"))
        return (void *)&sl_iid_play;
    if (!strcmp(name, "SL_IID_VOLUME"))
        return (void *)&sl_iid_volume;
    if (!strcmp(name, "SL_IID_BUFFERQUEUE") || !strcmp(name, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE"))
        return (void *)&sl_iid_bufferqueue;
    return NULL;
}
