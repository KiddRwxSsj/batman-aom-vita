#include "gfx.h"
#include "movie.h"
#include "util.h"
#include "bionic.h"

#include <malloc.h>
#include <stdlib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include <stdio.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>

int gfx_ready;

// the engine picks CookedAndroid_<format> off this list, and the obb only ships ATITC.
// atc.c turns those into DXT on upload, nothing that would send it looking for DXT/PVRTC/ETC data is listed
static const char *extensions =
    "GL_AMD_compressed_ATC_texture "
    "GL_ATI_texture_compression_atitc "
    "GL_OES_depth24 "
    "GL_OES_packed_depth_stencil "
    "GL_OES_rgb8_rgba8 "
    "GL_OES_texture_npot "
    "GL_OES_mapbuffer";

#ifdef DEBUG_SHOTS
// hang hunt: no frame, file read or movie frame for a minute means something is stuck. crash on
// purpose so the coredump has every thread's registers and stack
static int watchdog_thread(SceSize args, void *argp) {
    unsigned last = progress_ticks;
    int idle = 0;
    while (1) {
        sceKernelDelayThread(5 * 1000 * 1000);
        if (progress_ticks != last) {
            last = progress_ticks;
            idle = 0;
        } else if ((idle += 5) >= 60) {
            LOG("watchdog: no progress for 60 s, crashing for a dump");
            sceKernelDelayThread(500 * 1000);
            *(volatile int *)0 = 0;
        }
    }
    return 0;
}
#endif

void gfx_init(void) {
    if (gfx_ready)
        return;
#ifdef DEBUG_SHOTS
    sceKernelStartThread(sceKernelCreateThread("watchdog", watchdog_thread, 64, 0x2000, 0, 0, NULL), 0, NULL);
#endif
    if (!file_exists("ur0:data/libshacccg.suprx") && !file_exists("ur0:data/external/libshacccg.suprx"))
        fatal_error("libshacccg.suprx is missing.\nInstall it with ShaRKBR33D first.");

    // shaders get translated and compiled at link time, the gxp cache skips that on later boots
    mkpath(DATA_PATH "shader_cache/nohalf/");
    vglSetShaderCachePath(DATA_PATH "shader_cache/");
    vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
    // vitaGL's pools are free memory minus these thresholds, the 56MB of user ram left over
    // is where bionic.c puts the engine's big allocations, 20MB of phycont is for the movie decoder
    vglInitWithCustomThreshold(0, SCREEN_W, SCREEN_H, 56 * 1024 * 1024, 0, 20 * 1024 * 1024, 0x8C6000, SCE_GXM_MULTISAMPLE_NONE);
    gfx_ready = 1;
    LOG("vitaGL up");
    movie_gl_init();
}

#ifdef DEBUG_SHOTS
// time inside vglSwapBuffers (blocks when the gpu is behind) and between swaps
static uint64_t swap_us;
static uint32_t worst_swap_us, worst_frame_us;
static int slow_frames;

static void debug_frame(void) {
    static int frames;
    static uint64_t last;
    frames++;
    uint64_t now = uptime_ms();
    if (now - last < 5000)
        return;
    struct mallinfo mi = mallinfo();
    SceKernelFreeMemorySizeInfo fm = { sizeof(fm) };
    sceKernelGetFreeMemorySize(&fm);
    extern uint32_t vgl_map_orphans, vgl_map_orphan_bytes;
    LOG("%d fps | fds %d | map orphans %u (%uKB) | heap %uMB used %uMB arena | big %u/%uMB | sys user %uMB phycont %uMB cdram %uMB | vgl ram %uMB vram %uMB",
        frames * 1000 / (int)(now - last), open_fds, vgl_map_orphans, vgl_map_orphan_bytes >> 10, mi.uordblks >> 20, mi.arena >> 20, big_count, big_bytes >> 20,
        fm.size_user >> 20, fm.size_phycont >> 20, fm.size_cdram >> 20,
        (unsigned)(vglMemFree(VGL_MEM_RAM) >> 20), (unsigned)(vglMemFree(VGL_MEM_VRAM) >> 20));
    extern void pthread_track_dump(void), cpu_usage_dump(uint64_t);
    pthread_track_dump();
    cpu_usage_dump((now - last) * 1000);
    if (frames)
        LOG("frame: avg %u.%u ms, worst %u ms, swap avg %u.%u ms worst %u ms, %d over 17 ms",
            (unsigned)((now - last) * 10 / frames / 10), (unsigned)((now - last) * 10 / frames % 10), worst_frame_us / 1000,
            (unsigned)(swap_us / frames / 1000), (unsigned)(swap_us / frames / 100 % 10), worst_swap_us / 1000, slow_frames);
    swap_us = worst_swap_us = worst_frame_us = slow_frames = 0;
    frames = 0;
    vgl_map_orphans = vgl_map_orphan_bytes = 0;
    last = now;
}

// press select and the next frame lands in shot.raw (rgba, bottom-up)
volatile int shot_req;

#ifdef GL_TRACE
static int trace_frames;
#endif

static void shot_on_request(void) {
    static uint8_t pixels[SCREEN_W * SCREEN_H * 4];
    if (!shot_req)
        return;
    shot_req = 0;
#ifdef GL_TRACE
    extern int gl_trace_on;
    trace_frames = 1;
    mkpath(DATA_PATH "trace/");
#endif
    glReadPixels(0, 0, SCREEN_W, SCREEN_H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    file_save(DATA_PATH "shot.raw", pixels, sizeof(pixels));
    LOG("shot saved");
}
#endif

void gfx_swap(void) {
    progress_ticks++;
    swap_ticks++;
    movie_draw();
#ifdef DEBUG_SHOTS
    debug_frame();
#ifdef GL_TRACE
    extern int gl_trace_on;
    extern void gl_trace_flush(void), gl_debug_poll(void);
    gl_debug_poll();
    if (gl_trace_on)
        gl_trace_flush();
    gl_trace_on = trace_frames > 0;
    if (trace_frames > 0)
        trace_frames--;
#endif
    shot_on_request();
    static uint64_t prev;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    if (prev) {
        uint32_t ft = (uint32_t)(t0 - prev);
        if (ft > worst_frame_us)
            worst_frame_us = ft;
        if (ft > 17000)
            slow_frames++;
    }
    prev = t0;
    vglSwapBuffers(GL_FALSE);
    uint32_t st = (uint32_t)(sceKernelGetProcessTimeWide() - t0);
    swap_us += st;
    if (st > worst_swap_us)
        worst_swap_us = st;
#else
    vglSwapBuffers(GL_FALSE);
#endif
}

const GLubyte *gl_get_string(GLenum name) {
    if (name == GL_EXTENSIONS)
        return (const GLubyte *)extensions;
    return glGetString(name);
}

// gxm has no constant blend colour
void gl_blend_color(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
}

static void gl_validate_program(GLuint program) {
}

void gl_get_shader_precision_format(GLenum shader, GLenum precision, GLint *range, GLint *prec) {
    range[0] = 127;
    range[1] = 127;
    *prec = 23;
}

void *gl_map_buffer_oes(GLenum target, GLenum access) {
    return glMapBuffer(target, access);
}

GLboolean gl_unmap_buffer_oes(GLenum target) {
    return glUnmapBuffer(target);
}

// fragment shaders that got the half rewrite keep their original source here until they link,
// a half version that doesn't compile (cg overloads can get ambiguous) relinks with this one
#define MAX_HALF 64
static struct { GLuint sh; char *orig; uint64_t hash; } half_src[MAX_HALF];

static uint64_t src_hash(const char *s) {
    uint64_t h = 0xcbf29ce484222325ull;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 0x100000001b3ull;
    return h;
}

static void nohalf_path(char *out, uint64_t hash) {
    sprintf(out, DATA_PATH "shader_cache/nohalf/%016llX", hash);
}

// vertex shader sources stay around until their program links too, so the fallback can rebuild the
// pair from fresh shader objects instead of re-sourcing the one that failed
static struct { GLuint sh; char *src; } vert_src[MAX_HALF];
static int vert_next;

static void keep_vert_src(GLuint sh, const char *src) {
    int slot = -1;
    for (int i = 0; i < MAX_HALF && slot < 0; i++)
        if (vert_src[i].src && vert_src[i].sh == sh)
            slot = i;
    for (int i = 0; i < MAX_HALF && slot < 0; i++)
        if (!vert_src[i].src)
            slot = i;
    if (slot < 0)
        slot = vert_next++ % MAX_HALF;
    free(vert_src[slot].src);
    vert_src[slot].sh = sh;
    vert_src[slot].src = strdup(src);
}

static void attached(GLuint program, int *h, int *v) {
    GLuint sh[2];
    GLsizei n = 0;
    *h = *v = -1;
    glGetAttachedShaders(program, 2, &n, sh);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < MAX_HALF; j++) {
            if (half_src[j].orig && half_src[j].sh == sh[i])
                *h = j;
            if (vert_src[j].src && vert_src[j].sh == sh[i])
                *v = j;
        }
}

static int half_fallback(GLuint program) {
    int h, v;
    attached(program, &h, &v);
    if (h < 0)
        return 0;
    char path[128];
    nohalf_path(path, half_src[h].hash);
    file_save(path, "", 0);
#ifdef DEBUG_SHOTS
    mkpath(DATA_PATH "halffail/");
    sprintf(path, DATA_PATH "halffail/p%u_f.glsl", program);
    file_save(path, half_src[h].orig, strlen(half_src[h].orig));
    if (v >= 0) {
        sprintf(path, DATA_PATH "halffail/p%u_v.glsl", program);
        file_save(path, vert_src[v].src, strlen(vert_src[v].src));
    }
#endif
    const GLchar *one = half_src[h].orig;
    if (v < 0) {
        LOG("program %u: no vertex source kept, relinking in place", program);
        glShaderSource(half_src[h].sh, 1, &one, NULL);
    } else {
        // fresh objects, the same path a later boot takes once the nohalf marker exists
        GLuint nv = glCreateShader(GL_VERTEX_SHADER), nf = glCreateShader(GL_FRAGMENT_SHADER);
        const GLchar *vs = vert_src[v].src;
        glShaderSource(nv, 1, &vs, NULL);
        glCompileShader(nv);
        glShaderSource(nf, 1, &one, NULL);
        glCompileShader(nf);
        glAttachShader(program, nv);
        glAttachShader(program, nf);
        // the program holds them now, they go away with it
        glDeleteShader(nv);
        glDeleteShader(nf);
        free(vert_src[v].src);
        vert_src[v].src = NULL;
    }
    free(half_src[h].orig);
    half_src[h].orig = NULL;
    return 1;
}

static void half_done(GLuint program) {
    int h, v;
    attached(program, &h, &v);
    if (h >= 0) {
        free(half_src[h].orig);
        half_src[h].orig = NULL;
    }
    if (v >= 0) {
        free(vert_src[v].src);
        vert_src[v].src = NULL;
    }
}

// first boot compiles every shader through libshacccg, this shows it's still going
void gl_link_program(GLuint program) {
    static int links, fallbacks;
    uint64_t t = uptime_ms();
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok && half_fallback(program)) {
        LOG("program %u: half version failed, relinking in fp32 (%d so far)", program, ++fallbacks);
        glLinkProgram(program);
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
    }
    if (!ok)
        LOG("program %u failed to link", program);
    half_done(program);
    progress_ticks++;
    if (++links % 25 == 0 || uptime_ms() - t > 1000)
        LOG("%d program links, last took %u ms", links, (unsigned)(uptime_ms() - t));
}

static int is_ident(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// gfx feeds 2 component GL_SHORT positions into "vec4 Position" and relies on gl's z = 0, w = 1
// (dot(Position, Transform2D[n]) carries the translation in w). gxm doesn't fill those in for
// integer formats, so in gfx vertex shaders every Position inside main() becomes vec4(Position.xy, 0, 1)
static char *patch_gfx_position(const char *src) {
    if (!strstr(src, "gl_Position") || !(strstr(src, "Transform2D") || strstr(src, "BatchFloat") || strstr(src, "BatchMatrix")))
        return NULL;
    const char *body = NULL;
    for (const char *p = strstr(src, "void main("); p; p = strstr(p + 1, "void main("))
        body = p;
    if (!body)
        return NULL;
    static const char rep[] = "vec4(Position.xy,0.0,1.0)";
    size_t len = strlen(src);
    char *out = malloc(len + 64 * (sizeof(rep) - 1) + 1), *o = out;
    memcpy(o, src, body - src);
    o += body - src;
    int n = 0;
    for (const char *p = body; *p;) {
        if (n < 64 && !strncmp(p, "Position", 8) && !is_ident(p[8]) && (p == src || !is_ident(p[-1]))) {
            memcpy(o, rep, sizeof(rep) - 1);
            o += sizeof(rep) - 1;
            p += 8;
            n++;
        } else {
            *o++ = *p++;
        }
    }
    *o = 0;
    return out;
}

// ue3's distance field font shader calls smoothstep(0.5 - w, 0.5 + w, d) with w = 0. glsl leaves equal
// edges undefined, android gpus treat it as a step, the vita returns 1 and every glyph is a solid box
static const char safe_smoothstep[] =
    "// vgl_ss 2\n" // changes the cache hash so old caches with these shaders get rebuilt
    "float vgl_ss(float a, float b, float x) { float d = b - a; d = abs(d) < 0.00001 ? 0.00001 : d;"
    " float t = clamp((x - a) / d, 0.0, 1.0); return t * t * (3.0 - 2.0 * t); }\n"
    "vec2 vgl_ss(vec2 a, vec2 b, vec2 x) { return vec2(vgl_ss(a.x, b.x, x.x), vgl_ss(a.y, b.y, x.y)); }\n"
    "vec3 vgl_ss(vec3 a, vec3 b, vec3 x) { return vec3(vgl_ss(a.xy, b.xy, x.xy), vgl_ss(a.z, b.z, x.z)); }\n"
    "vec4 vgl_ss(vec4 a, vec4 b, vec4 x) { return vec4(vgl_ss(a.xy, b.xy, x.xy), vgl_ss(a.zw, b.zw, x.zw)); }\n"
    "vec2 vgl_ss(float a, float b, vec2 x) { return vgl_ss(vec2(a), vec2(b), x); }\n"
    "vec3 vgl_ss(float a, float b, vec3 x) { return vgl_ss(vec3(a), vec3(b), x); }\n"
    "vec4 vgl_ss(float a, float b, vec4 x) { return vgl_ss(vec4(a), vec4(b), x); }\n";

static char *patch_smoothstep(const char *src) {
    if (!strstr(src, "smoothstep("))
        return NULL;
    const char *body = NULL;
    for (const char *p = strstr(src, "void main("); p; p = strstr(p + 1, "void main("))
        body = p;
    if (!body)
        return NULL;
    size_t len = strlen(src);
    char *out = malloc(len + sizeof(safe_smoothstep) + 64), *o = out;
    memcpy(o, src, body - src);
    o += body - src;
    memcpy(o, safe_smoothstep, sizeof(safe_smoothstep) - 1);
    o += sizeof(safe_smoothstep) - 1;
    for (const char *p = body; *p;) {
        if (!strncmp(p, "smoothstep(", 11) && !is_ident(p[-1])) {
            // same length, so the buffer size above holds
            memcpy(o, "vgl_ss(", 7);
            o += 7;
            p += 11;
        } else {
            *o++ = *p++;
        }
    }
    *o = 0;
    return out;
}

// vitagl's translator drops lowp/mediump, so every fragment shader ran in fp32. the usse does half
// a lot faster and ue3 marks nearly all colour math lowp. locals inside main() only: varyings and
// uniforms keep their types (the translator parses those), highp (uvs) stays fp32
static char *patch_half(const char *src) {
    if (!strstr(src, "gl_FragColor"))
        return NULL;
    const char *body = NULL;
    for (const char *p = strstr(src, "void main("); p; p = strstr(p + 1, "void main("))
        body = p;
    if (!body)
        return NULL;
    size_t len = strlen(src);
    // half is shorter than float/vec, the output never grows
    char *out = malloc(len + 1), *o = out;
    memcpy(o, src, body - src);
    o += body - src;
    int n = 0;
    for (const char *p = body; *p;) {
        int q = !strncmp(p, "lowp", 4) ? 4 : !strncmp(p, "mediump", 7) ? 7 : 0;
        if (q && !is_ident(p[-1]) && !is_ident(p[q])) {
            const char *t = p + q;
            while (*t == ' ' || *t == '\t')
                t++;
            if (!strncmp(t, "float", 5) && !is_ident(t[5])) {
                memcpy(o, "half", 4);
                o += 4;
                p = t + 5;
                n++;
                continue;
            }
            if (!strncmp(t, "vec", 3) && t[3] >= '2' && t[3] <= '4' && !is_ident(t[4])) {
                memcpy(o, "half", 4);
                o[4] = t[3];
                o += 5;
                p = t + 4;
                n++;
                continue;
            }
        }
        *o++ = *p++;
    }
    *o = 0;
    if (!n) {
        free(out);
        return NULL;
    }
    return out;
}

void gl_shader_source(GLuint sh, GLsizei n, const GLchar *const *str, const GLint *len) {
    size_t total = 0;
    for (int i = 0; i < n; i++)
        total += len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
    char *s = malloc(total + 1), *p = s;
    for (int i = 0; i < n; i++) {
        size_t l = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
        memcpy(p, str[i], l);
        p += l;
    }
    *p = 0;
    char *patched = patch_gfx_position(s);
    char *ss = patch_smoothstep(patched ? patched : s);
    const GLchar *cur = ss ? ss : patched ? patched : s;
    char *hf = NULL;
    uint64_t hash = src_hash(cur);
    char path[128];
    nohalf_path(path, hash);
    // distance field text (the only shaders with smoothstep) never goes half: the shadow/glow
    // variants don't compile that way, and text gains nothing from it
    if (!ss && !file_exists(path))
        hf = patch_half(cur);
    // remember the fp32 source in case the half one doesn't compile at link, no free slot = no half
    int slot = -1;
    for (int i = 0; hf && i < MAX_HALF && slot < 0; i++)
        if (!half_src[i].orig || half_src[i].sh == sh)
            slot = i;
    if (slot >= 0) {
        free(half_src[slot].orig);
        half_src[slot].sh = sh;
        half_src[slot].orig = strdup(cur);
        half_src[slot].hash = hash;
    } else if (hf) {
        free(hf);
        hf = NULL;
    }
    GLint type = 0;
    glGetShaderiv(sh, GL_SHADER_TYPE, &type);
    if (type == GL_VERTEX_SHADER)
        keep_vert_src(sh, cur);
    const GLchar *one = hf ? hf : cur;
    glShaderSource(sh, 1, &one, NULL);
    free(hf);
    free(ss);
    free(patched);
    free(s);
}

static const struct { const char *name; void *fn; } gl_extra[] = {
    { "glValidateProgram", gl_validate_program },
    { "glGetShaderPrecisionFormat", gl_get_shader_precision_format },
    { "glMapBufferOES", gl_map_buffer_oes },
    { "glUnmapBufferOES", gl_unmap_buffer_oes },
    { "glBlendColor", gl_blend_color },
    { "glGetString", gl_get_string },
    { "glLinkProgram", gl_link_program },
    { "glShaderSource", gl_shader_source },
    { "glCompressedTexImage2D", gl_compressed_tex_image_2d },
};

void *egl_get_proc_address(const char *name) {
    for (int i = 0; i < (int)(sizeof(gl_extra) / sizeof(gl_extra[0])); i++)
        if (!strcmp(name, gl_extra[i].name))
            return gl_extra[i].fn;
    void *p = vglGetProcAddress(name);
    LOG("eglGetProcAddress %s -> %p", name, p);
    return p;
}
