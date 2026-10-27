// java's song player: the engine hands over an offset/length of an mp3 inside the obb.
// songs loop, a new song waits for the old one to fade out (UpdateSongPlayer drives the fades)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/audioout.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include <minimp3/minimp3.h>

#include "audio.h"
#include "loader.h"
#include "util.h"

#define GRAIN 1024
#define READ_CHUNK (32 * 1024)

typedef struct {
    int active;
    char name[64];
    int64_t off, len;
    float fade_in, fade_out;
} song_req;

static SceKernelLwMutexWork lock;
static song_req cur, pending;
static float song_volume = 1.0f;   // what the engine asked for
static float fade = 1.0f;          // fade multiplier on top
static int fading_in, fading_out;
static volatile int restart;       // decoder must (re)open cur
static int port = -1, port_rate;

static void lock_on(void) { sceKernelLockLwMutex(&lock, 1, NULL); }
static void lock_off(void) { sceKernelUnlockLwMutex(&lock, 1); }

void audio_play_song(int64_t off, int64_t len, const char *name, float fade_in, float fade_out) {
    LOG("PlaySong %s fade %.2f/%.2f", name, fade_in, fade_out);
    lock_on();
    if (cur.active) {
        if ((strcmp(name, cur.name) || pending.active) && strcmp(name, pending.name)) {
            pending.active = 1;
            snprintf(pending.name, sizeof(pending.name), "%s", name);
            pending.off = off;
            pending.len = len;
            pending.fade_in = fade_in;
            pending.fade_out = fade_out;
            fading_out = 1;
        }
    } else {
        cur.active = 1;
        snprintf(cur.name, sizeof(cur.name), "%s", name);
        cur.off = off;
        cur.len = len;
        cur.fade_in = fade_in;
        cur.fade_out = fade_out;
        fading_in = fade_in > 0.0f;
        fade = fading_in ? 0.0f : 1.0f;
        restart = 1;
    }
    lock_off();
}

void audio_stop_song(void) {
    LOG("StopSong");
    lock_on();
    cur.active = 0;
    cur.name[0] = 0;
    pending.active = 0;
    pending.name[0] = 0;
    fading_in = fading_out = 0;
    restart = 1;
    lock_off();
}

void audio_song_volume(float volume) {
    if (volume >= 0.0f)
        song_volume = volume;
}

void audio_update_song(float dt) {
    lock_on();
    if (fading_out) {
        fade = cur.fade_out > 0.0f ? fade - dt / cur.fade_out : 0.0f;
        if (fade <= 0.0f) {
            fading_out = 0;
            cur = pending;
            pending.active = 0;
            pending.name[0] = 0;
            fading_in = cur.fade_in > 0.0f;
            fade = fading_in ? 0.0f : 1.0f;
            restart = 1;
        }
    } else if (fading_in) {
        fade = fade + dt / cur.fade_in;
        if (fade >= 1.0f) {
            fade = 1.0f;
            fading_in = 0;
        }
    }
    lock_off();
}

static void set_rate(int rate) {
    if (rate == port_rate)
        return;
    if (port < 0)
        port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, rate, SCE_AUDIO_OUT_MODE_STEREO);
    else
        sceAudioOutSetConfig(port, GRAIN, rate, SCE_AUDIO_OUT_MODE_STEREO);
    port_rate = rate;
}

static int song_thread(SceSize args, void *argp) {
    static mp3dec_t dec;
    static uint8_t in[READ_CHUNK * 2];
    static int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    static int16_t out[GRAIN * 2];
    SceUID fd = sceIoOpen(OBB_PATH, SCE_O_RDONLY, 0);
    int64_t base = 0, len = 0, pos = 0;
    int in_len = 0, out_len = 0, playing = 0;

    while (1) {
        if (restart) {
            lock_on();
            restart = 0;
            playing = cur.active;
            base = cur.off;
            len = cur.len;
            lock_off();
            pos = 0;
            in_len = 0;
            out_len = 0;
            mp3dec_init(&dec);
        }
        if (!playing || fd < 0) {
            sceKernelDelayThread(10000);
            continue;
        }

        if (in_len < READ_CHUNK && pos < len) {
            int n = len - pos < READ_CHUNK ? (int)(len - pos) : READ_CHUNK;
            n = sceIoPread(fd, in + in_len, n, base + pos);
            if (n > 0) {
                in_len += n;
                pos += n;
            }
        }

        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&dec, in, in_len, pcm, &info);
        if (info.frame_bytes) {
            memmove(in, in + info.frame_bytes, in_len - info.frame_bytes);
            in_len -= info.frame_bytes;
        }
        if (!info.frame_bytes || (!samples && !in_len)) {
            // end of the track (or junk at the end), loop it
            if (pos >= len) {
                pos = 0;
                in_len = 0;
                mp3dec_init(&dec);
            }
            continue;
        }
        if (!samples)
            continue;
        set_rate(info.hz);

        float g = song_volume * fade;
        int vol = (int)(g * 32768.0f);
        for (int i = 0; i < samples; i++) {
            int l = pcm[i * info.channels], r = pcm[i * info.channels + (info.channels > 1)];
            out[out_len * 2] = (l * vol) >> 15;
            out[out_len * 2 + 1] = (r * vol) >> 15;
            if (++out_len == GRAIN) {
                sceAudioOutOutput(port, out);
                out_len = 0;
            }
        }
    }
    return 0;
}

void audio_init(void) {
    sceKernelCreateLwMutex(&lock, "song", 0, 0, NULL);
    SceUID t = sceKernelCreateThread("song", song_thread, 65, 0x10000, 0, 0, NULL);
    sceKernelStartThread(t, 0, NULL);
}

// java's SoundPool path, the engine runs UOpenSLAudioDevice instead
int audio_load_sound(const char *path) {
    LOG("LoadSoundFile %s (unsupported)", path);
    return 0;
}

void audio_unload_sound(int id) {}
int audio_play_sound(int id, int loop) { return 0; }
void audio_stop_sound(int stream) {}
void audio_set_volume(int stream, float volume) {}
