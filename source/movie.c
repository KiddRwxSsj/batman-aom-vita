// java plays StartVideo's mp4 (a stored entry in the obb) on a view over the gl surface while the
// engine keeps running. here sceAvPlayer decodes it and gfx_swap draws the frame over the engine's
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <psp2/avplayer.h>
#include <psp2/audioout.h>
#include <psp2/sysmodule.h>
#include <psp2/gxm.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>

#include "movie.h"
#include "gfx.h"
#include "java.h"
#include "loader.h"
#include "util.h"

#define FRAMES 4
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

static SceAvPlayerHandle player;
static SceUID obb_fd = -1;
static uint64_t mv_off, mv_len;
static int mv_loop;
static uint64_t mv_start_at;
// looping movies are loading screens, they can be switched off here
static const int skip_loops = 1;
static volatile int requested, active, finish_pending, audio_run, restart;
static SceUID audio_thid = -1;
static int have_frame;
static unsigned frame_w, frame_h;

static GLuint prog, tex[FRAMES];
static SceGxmTexture *gxm_tex[FRAMES];
static int tex_idx;
static GLint u_rect;

static void *cpu_alloc(void *p, uint32_t align, uint32_t size) {
    return memalign(align, size);
}

static void cpu_free(void *p, void *ptr) {
    free(ptr);
}

// the decoder only produced frames into its own uncached phycont blocks on the lockdown port,
// gfx_init leaves some phycont out of vitagl for this
static void *gpu_alloc(void *p, uint32_t align, uint32_t size) {
    LOG("movie: frame alloc %u", size);
    size = ALIGN(size, 1024 * 1024);
    SceUID blk = sceKernelAllocMemBlock("movie", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, size, NULL);
    if (blk < 0) {
        LOG("movie: frame alloc %u failed %08x", size, blk);
        return NULL;
    }
    void *res;
    sceKernelGetMemBlockBase(blk, &res);
    sceGxmMapMemory(res, size, SCE_GXM_MEMORY_ATTRIB_RW);
    return res;
}

static void gpu_free(void *p, void *ptr) {
    glFinish();
    SceUID blk = sceKernelFindMemBlockByAddr(ptr, 0);
    sceGxmUnmapMemory(ptr);
    sceKernelFreeMemBlock(blk);
}

static int f_open(void *p, const char *name) { return 0; }
static int f_close(void *p) { return 0; }
static uint64_t f_size(void *p) { return mv_len; }

static int f_read(void *p, uint8_t *buf, uint64_t pos, uint32_t len) {
    if (pos >= mv_len)
        return 0;
    if (pos + len > mv_len)
        len = mv_len - pos;
    return sceIoPread(obb_fd, buf, len, mv_off + pos);
}

static int audio_thread(SceSize args, void *argp) {
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, 1024, 48000, SCE_AUDIO_OUT_MODE_STEREO);
    while (audio_run) {
        SceAvPlayerFrameInfo frame;
        if (sceAvPlayerIsActive(player) && sceAvPlayerGetAudioData(player, &frame)) {
            sceAudioOutSetConfig(port, 1024, frame.details.audio.sampleRate,
                                 frame.details.audio.channelCount == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
            sceAudioOutOutput(port, frame.pData);
        } else {
            sceKernelDelayThread(1000);
        }
    }
    sceAudioOutReleasePort(port);
    return sceKernelExitThread(0);
}

static void close_player(void) {
    if (!active)
        return;
    active = 0;
    audio_run = 0;
    sceKernelWaitThreadEnd(audio_thid, NULL, NULL);
    sceKernelDeleteThread(audio_thid);
    sceAvPlayerStop(player);
    sceAvPlayerClose(player);
    have_frame = 0;
}

static int open_player(void) {
    static int module;
    if (!module) {
        int ret = sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
        LOG("movie: avplayer module %08x", ret);
        module = 1;
    }
    if (obb_fd < 0)
        obb_fd = sceIoOpen(OBB_PATH, SCE_O_RDONLY, 0);

    SceAvPlayerInitData init;
    memset(&init, 0, sizeof(init));
    init.memoryReplacement.allocate = cpu_alloc;
    init.memoryReplacement.deallocate = cpu_free;
    init.memoryReplacement.allocateTexture = gpu_alloc;
    init.memoryReplacement.deallocateTexture = gpu_free;
    init.fileReplacement.open = f_open;
    init.fileReplacement.close = f_close;
    init.fileReplacement.readOffset = f_read;
    init.fileReplacement.size = f_size;
    init.basePriority = 0xA0;
    init.numOutputVideoFrameBuffers = FRAMES;
    init.autoStart = 1;
    LOG("movie: init");
    player = sceAvPlayerInit(&init);
    // handles can look negative, the errors are 0x806axxxx
    if (((uint32_t)player & 0xffff0000) == 0x806a0000) {
        LOG("movie: sceAvPlayerInit %08x", player);
        return 0;
    }
    LOG("movie: player %08x", player);
    sceAvPlayerSetLooping(player, mv_loop);
    int ret = sceAvPlayerAddSource(player, "movie.mp4");
    LOG("movie: playing off %llx len %llx loop %d (%08x)", mv_off, mv_len, mv_loop, ret);
    active = 1;
    audio_run = 1;
    audio_thid = sceKernelCreateThread("movie_audio", audio_thread, 0x10000100 - 10, 0x4000, 0, 0, NULL);
    sceKernelStartThread(audio_thid, 0, NULL);
    return 1;
}

void movie_start(int64_t off, int64_t len, int loop, int delay) {
    LOG("StartVideo off %llx len %llx loop %d delay %d", off, len, loop, delay);
    // looping movies are loading screens (Load_ImageA, Mapload1) that run while a level streams in.
    // the decoder needs ~17MB of phycont the level wants too, playing one during a map load hung
    // the whole vita (hd22), so those report finished right away and the engine's own screen stays
    if (loop && skip_loops) {
        movie_stop();
        finish_pending = 1;
        return;
    }
    // a new movie replaces the old one, like java's StartVideo calling StopVideo first
    if (requested && active)
        restart = 1;
    mv_off = off;
    mv_len = len;
    mv_loop = loop;
    mv_start_at = uptime_ms() + (delay > 0 ? delay * 1000 : 0);
    requested = 1;
}

// java's StopVideo doesn't report MovieFinished, but the engine's movie layer eats all input until
// something does (its stop path can bail out early), so a stopped movie counts as finished here
void movie_stop(void) {
    LOG("StopVideo");
    if (requested)
        finish_pending = 1;
    requested = 0;
}

int movie_playing(void) {
    return requested;
}

// java reports the end from its own thread, the main loop stands in for it
void movie_tick(void) {
    if (finish_pending) {
        finish_pending = 0;
        NativeCallback_MovieFinished(&jni, java_thiz);
    }
}

static const char *vsh =
    "attribute vec2 pos;\n"
    "uniform vec4 rect;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "    uv = (pos - rect.xy) / rect.zw;\n"
    "    gl_Position = vec4(pos.x * 2.0 - 1.0, 1.0 - pos.y * 2.0, 0.0, 1.0);\n"
    "}\n";

static const char *fsh =
    "precision mediump float;\n"
    "uniform sampler2D tex;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0)\n"
    "        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "    else\n"
    "        gl_FragColor = texture2D(tex, uv);\n"
    "}\n";

// built in gfx_init, before the engine issues any gl of its own (hd29 built it mid-boot from
// another thread while the engine compiled shaders: crash, or garbage in the shader cache)
void movie_gl_init(void) {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(vs, 1, &vsh, NULL);
    glCompileShader(vs);
    glShaderSource(fs, 1, &fsh, NULL);
    glCompileShader(fs);
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "pos");
    glLinkProgram(prog);
    u_rect = glGetUniformLocation(prog, "rect");

    glGenTextures(FRAMES, tex);
    for (int i = 0; i < FRAMES; i++) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        gxm_tex[i] = vglGetGxmTexture(GL_TEXTURE_2D);
        vglFree(vglGetTexDataPointer(GL_TEXTURE_2D));
    }
}

// the engine shadows its gl state, so everything touched here goes back the way it was
static void draw_frame(void) {
    static const float quad[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    GLint old_prog, old_active, old_tex, old_abuf, old_fbo, vp[4], a0_on;
    GLboolean mask[4], en[5];
    static const GLenum caps[5] = { GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST };

    glGetIntegerv(GL_CURRENT_PROGRAM, &old_prog);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_abuf);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a0_on);
    for (int i = 0; i < 5; i++) {
        en[i] = glIsEnabled(caps[i]);
        glDisable(caps[i]);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, SCREEN_W, SCREEN_H);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(prog);
    // fit, keep the aspect, black bars around it
    float sw = 1.0f, sh = (float)SCREEN_W * frame_h / frame_w / SCREEN_H;
    if (sh > 1.0f) {
        sw = 1.0f / sh;
        sh = 1.0f;
    }
    glUniform4f(u_rect, (1.0f - sw) / 2, (1.0f - sh) / 2, sw, sh);
    glBindTexture(GL_TEXTURE_2D, tex[tex_idx]);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (!a0_on)
        glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, old_abuf);
    glBindTexture(GL_TEXTURE_2D, old_tex);
    glActiveTexture(old_active);
    glUseProgram(old_prog);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, old_fbo);
    for (int i = 0; i < 5; i++)
        if (en[i])
            glEnable(caps[i]);
}

static uint64_t opened_at;
static int frames_shown;

// decode side of a frame: opens/closes the player, picks up the newest frame.
// returns 1 when there is something to draw
static int movie_step(void) {
    if (!requested || restart) {
        restart = 0;
        close_player();
        if (!requested)
            return 0;
    }
    if (!active) {
        if (uptime_ms() < mv_start_at)
            return 0;
        LOG("movie: opening player");
        if (!open_player()) {
            requested = 0;
            finish_pending = 1;
            return 0;
        }
        opened_at = uptime_ms();
        frames_shown = 0;
    }

    if (sceAvPlayerIsActive(player)) {
        SceAvPlayerFrameInfo frame;
        if (sceAvPlayerGetVideoData(player, &frame)) {
            tex_idx = (tex_idx + 1) % FRAMES;
            frame_w = frame.details.video.width;
            frame_h = frame.details.video.height;
            sceGxmTextureInitLinear(gxm_tex[tex_idx], frame.pData, SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1, frame_w, frame_h, 0);
            sceGxmTextureSetMinFilter(gxm_tex[tex_idx], SCE_GXM_TEXTURE_FILTER_LINEAR);
            sceGxmTextureSetMagFilter(gxm_tex[tex_idx], SCE_GXM_TEXTURE_FILTER_LINEAR);
            have_frame = 1;
        }
    } else if (have_frame || uptime_ms() - opened_at > 5000) {
        LOG("movie: finished after %u ms, %d frames shown%s", (unsigned)(uptime_ms() - opened_at), frames_shown, have_frame ? "" : ", no frames");
        close_player();
        requested = 0;
        finish_pending = 1;
        return 0;
    }
    return have_frame;
}

// thread holding the engine's gl context (makeCurrent/unMakeCurrent, or whoever swaps)
static volatile SceUID gl_thread = -1;

void movie_gl_owner(int own) {
    static int logs;
    SceUID me = sceKernelGetThreadId();
    if (own)
        gl_thread = me;
    else if (gl_thread == me)
        gl_thread = -1;
    if (logs < 32) {
        logs++;
        LOG("gl context %s on thread %08x", own ? "made current" : "released", me);
    }
}

static uint64_t last_engine_swap, last_movie_swap;

// any new button press or a new touch skips (only checked while the engine waits on the movie)
static int skip_pressed(void) {
    static uint32_t old = ~0u;
    static int old_touch = 1;
    SceCtrlData pad;
    SceTouchData touch;
    uint32_t buttons = 0;
    int touching = 0;
    if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0)
        buttons = pad.buttons;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0)
        touching = touch.reportNum > 0;
    int pressed = (buttons & ~old) || (touching && !old_touch);
    old = buttons;
    old_touch = touching;
    return pressed;
}

// gfx_swap calls this on the render thread right before the buffers flip
void movie_draw(void) {
    gl_thread = sceKernelGetThreadId();
    last_engine_swap = uptime_ms();
    if (movie_step()) {
        draw_frame();
        frames_shown++;
    }
}

// the engine waits for fullscreen movies in an appSleep(0.1) loop on the render thread
// and draws nothing meanwhile (java shows the video on top). so while it sleeps there
// the movie gets the screen to itself
void app_sleep(float secs) {
    int us = (int)(secs * 1000000.0f);
    SceUID me = sceKernelGetThreadId();
    // only on the thread holding the context (so no engine gl can run meanwhile), while the
    // engine itself isn't presenting, and at most once per vblank: the render thread's idle
    // loop calls this back to back
    if (requested && me == gl_thread && uptime_ms() - last_engine_swap > 100) {
        if (!mv_loop && active && uptime_ms() - opened_at > 1000 && skip_pressed()) {
            LOG("movie: skipped");
            close_player();
            requested = 0;
            finish_pending = 1;
        } else if (uptime_ms() - last_movie_swap >= 16) {
            progress_ticks++;
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, SCREEN_W, SCREEN_H);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            if (movie_step()) {
                draw_frame();
                frames_shown++;
            }
            vglSwapBuffers(GL_FALSE);
            last_movie_swap = uptime_ms();
            if (us > 16000)
                us -= 16000;
        }
    }
    sceKernelDelayThread(us > 0 ? us : 0);
}
