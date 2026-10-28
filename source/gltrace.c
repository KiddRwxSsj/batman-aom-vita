// GL_TRACE builds: the frame after a screenshot logs every program, uniform upload, attribute
// layout and draw, and dumps the shader sources of the programs it used

#ifdef GL_TRACE
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <psp2/io/fcntl.h>
#include <psp2/gxm.h>
#include <stdlib.h>
#include <string.h>
#include <so_util/so_util.h>

#include "gfx.h"
#include "util.h"

int gl_trace_on;

#define MAX_IDS 4096

static char *tbuf;
static size_t tlen;
#define TBUF_SIZE (4 * 1024 * 1024)

static void T(const char *fmt, ...) {
    if (!tbuf && !(tbuf = malloc(TBUF_SIZE)))
        return;
    if (tlen > TBUF_SIZE - 512)
        return;
    va_list ap;
    va_start(ap, fmt);
    tlen += vsnprintf(tbuf + tlen, TBUF_SIZE - tlen, fmt, ap);
    va_end(ap);
    tbuf[tlen++] = '\n';
}

// every texture upload/delete since boot (newest kept if it fills), saved next to the frame trace
static char *ubuf;
static size_t ulen;
#define UBUF_SIZE (2 * 1024 * 1024)

void gl_upload_log(const char *fmt, ...) {
    if (!ubuf && !(ubuf = malloc(UBUF_SIZE)))
        return;
    if (ulen > UBUF_SIZE - 256)
        ulen = 0;
    va_list ap;
    va_start(ap, fmt);
    ulen += vsnprintf(ubuf + ulen, UBUF_SIZE - ulen, fmt, ap);
    va_end(ap);
    ubuf[ulen++] = '\n';
}

static GLint bound_tex(void) {
    GLint t = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &t);
    return t;
}

static void t_tex_image_2d(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const void *data) {
    gl_upload_log("TexImage tex %d target %x lvl %d %dx%d ifmt %x fmt %x type %x data %d", bound_tex(), target, level, w, h, ifmt, fmt, type, data != NULL);
    glTexImage2D(target, level, ifmt, w, h, border, fmt, type, data);
}

static void t_delete_textures(GLsizei n, const GLuint *t) {
    for (int i = 0; i < n; i++)
        gl_upload_log("DeleteTexture %u", t[i]);
    glDeleteTextures(n, t);
}

// traced frames also save every bound 2d texture's level 0 as it sits in gpu memory (swizzled)
static char tex_dumped[MAX_IDS];

static void dump_texture(GLuint t) {
    if (t >= MAX_IDS || tex_dumped[t])
        return;
    tex_dumped[t] = 1;
    SceGxmTexture *g = vglGetGxmTexture(GL_TEXTURE_2D);
    void *data = vglGetTexDataPointer(GL_TEXTURE_2D);
    if (!g || !data)
        return;
    unsigned w = sceGxmTextureGetWidth(g), h = sceGxmTextureGetHeight(g);
    unsigned fmt = sceGxmTextureGetFormat(g), mips = sceGxmTextureGetMipmapCount(g);
    T("  tex %u %ux%u fmt %08x mips %u data %p", t, w, h, fmt, mips, data);
    unsigned size = w * h; // dxt5/3 size, dxt1 and 32 bit formats get more or less than needed
    if (size > 4 * 1024 * 1024)
        size = 4 * 1024 * 1024;
    char path[128];
    snprintf(path, sizeof(path), DATA_PATH "trace/tex_%u_%ux%u_%08x.bin", t, w, h, fmt);
    file_save(path, data, size);
}

static void t_bind_texture(GLenum target, GLuint t) {
    glBindTexture(target, t);
    if (gl_trace_on) {
        T("BindTexture %x %u", target, t);
        if (target == GL_TEXTURE_2D && t)
            dump_texture(t);
    }
}

static void t_tex_parameteri(GLenum target, GLenum p, GLint v) {
    gl_upload_log("TexParameteri tex %d %x %x", bound_tex(), p, v);
    glTexParameteri(target, p, v);
}

// gfx_swap calls this after the traced frame
void gl_trace_flush(void) {
    if (ulen)
        file_save(DATA_PATH "trace/uploads.txt", ubuf, ulen);
    memset(tex_dumped, 0, sizeof(tex_dumped));
    if (!tlen)
        return;
    file_save(DATA_PATH "trace/frame.txt", tbuf, tlen);
    LOG("gl trace saved, %u bytes", (unsigned)tlen);
    tlen = 0;
}

static char *sources[MAX_IDS];
static GLuint prog_shaders[MAX_IDS][2];
static char dumped[MAX_IDS];
static struct { GLuint prog; GLint loc; char name[40]; } locs[16384];
static int nlocs;

static const char *loc_name(GLint loc) {
    for (int i = nlocs - 1; i >= 0; i--)
        if (locs[i].loc == loc)
            return locs[i].name;
    return "?";
}

static void t_shader_source(GLuint sh, GLsizei n, const GLchar *const *str, const GLint *len) {
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
    if (sh < MAX_IDS) {
        free(sources[sh]);
        sources[sh] = s;
    } else {
        free(s);
    }
    gl_shader_source(sh, n, str, len);
}

static void t_attach(GLuint prog, GLuint sh) {
    if (prog < MAX_IDS)
        prog_shaders[prog][prog_shaders[prog][0] ? 1 : 0] = sh;
    glAttachShader(prog, sh);
}

static GLint t_get_uniform_location(GLuint prog, const GLchar *name) {
    GLint l = glGetUniformLocation(prog, name);
    if (l != -1 && nlocs < (int)(sizeof(locs) / sizeof(locs[0]))) {
        locs[nlocs].prog = prog;
        locs[nlocs].loc = l;
        snprintf(locs[nlocs].name, sizeof(locs[nlocs].name), "%s", name);
        nlocs++;
    }
    return l;
}

static void dump_program(GLuint prog) {
    if (prog >= MAX_IDS || dumped[prog])
        return;
    dumped[prog] = 1;
    for (int i = 0; i < nlocs; i++)
        if (locs[i].prog == prog && locs[i].loc <= -1000)
            T("  attrib %s = %d", locs[i].name, -1000 - locs[i].loc);
    for (int k = 0; k < 2; k++) {
        GLuint sh = prog_shaders[prog][k];
        if (sh && sh < MAX_IDS && sources[sh]) {
            char path[128];
            snprintf(path, sizeof(path), DATA_PATH "trace/p%u_s%u.glsl", prog, sh);
            file_save(path, sources[sh], strlen(sources[sh]));
        }
    }
}

// gfxdebug.txt in the data folder, reread every 2s: "skip 1 2 3" hides those programs' draws,
// "only 4 5" draws nothing else. lets a single program be compared against gfx_replay.py
static char prog_mode[MAX_IDS]; // 0 draw, 1 hidden
static GLuint cur_prog;

void gl_debug_poll(void) {
    static uint64_t last;
    if (uptime_ms() - last < 2000)
        return;
    last = uptime_ms();
    char buf[1024];
    SceUID fd = sceIoOpen(DATA_PATH "gfxdebug.txt", SCE_O_RDONLY, 0);
    int n = fd >= 0 ? sceIoRead(fd, buf, sizeof(buf) - 1) : 0;
    if (fd >= 0)
        sceIoClose(fd);
    buf[n > 0 ? n : 0] = 0;
    static char old[1024];
    if (!strcmp(old, buf))
        return;
    strcpy(old, buf);
    memset(prog_mode, 0, sizeof(prog_mode));
    char *p = buf;
    int only = !strncmp(p, "only", 4);
    if (only)
        memset(prog_mode, 1, sizeof(prog_mode));
    if (only || !strncmp(p, "skip", 4)) {
        p += 4;
        for (char *e; ; p = e) {
            long v = strtol(p, &e, 10);
            if (e == p)
                break;
            if (v > 0 && v < MAX_IDS)
                prog_mode[v] = !only;
        }
    }
    LOG("gfxdebug: %s", buf[0] ? buf : "(none)");
}

static void t_use_program(GLuint prog) {
    cur_prog = prog;
    if (gl_trace_on) {
        T("UseProgram %u", prog);
        dump_program(prog);
    }
    glUseProgram(prog);
}

static void t_uniform4fv(GLint loc, GLsizei count, const GLfloat *v) {
    if (gl_trace_on) {
        T("Uniform4fv %s count %d", loc_name(loc), count);
        for (int i = 0; i < count && i < 32; i++)
            T("  u %d %g %g %g %g", i, v[i * 4], v[i * 4 + 1], v[i * 4 + 2], v[i * 4 + 3]);
    }
    glUniform4fv(loc, count, v);
}

static void t_uniform_matrix4fv(GLint loc, GLsizei count, GLboolean tr, const GLfloat *v) {
    if (gl_trace_on)
        T("UniformMatrix4fv %s count %d tr %d", loc_name(loc), count, tr);
    glUniformMatrix4fv(loc, count, tr, v);
}

static void t_uniform3fv(GLint loc, GLsizei count, const GLfloat *v) {
    if (gl_trace_on)
        T("Uniform3fv %s count %d", loc_name(loc), count);
    glUniform3fv(loc, count, v);
}

static void t_uniform1fv(GLint loc, GLsizei count, const GLfloat *v) {
    if (gl_trace_on)
        T("Uniform1fv %s count %d", loc_name(loc), count);
    glUniform1fv(loc, count, v);
}

// bindings and layouts are tracked all the time so a traced draw can dump its vertices
static GLuint bound_array, bound_elem;
static struct { GLint size; GLenum type; GLboolean norm; GLsizei stride; const void *p; GLuint buf; } attr[8];

static void t_vertex_attrib_pointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *p) {
    if (i < 8) {
        attr[i].size = size;
        attr[i].type = type;
        attr[i].norm = norm;
        attr[i].stride = stride;
        attr[i].p = p;
        attr[i].buf = bound_array;
    }
    if (gl_trace_on)
        T("VertexAttribPointer %u size %d type %x norm %d stride %d ptr %p", i, size, type, norm, stride, p);
    glVertexAttribPointer(i, size, type, norm, stride, p);
}

// vitaGL buffer names are pointers to its vbo struct, whose first field is the storage
static const uint8_t *buf_data(GLuint buf, const void *off) {
    return buf ? *(uint8_t **)buf + (uintptr_t)off : (const uint8_t *)off;
}

static void dump_vertex(int a, int v) {
    const uint8_t *src = buf_data(attr[a].buf, attr[a].p) + v * attr[a].stride;
    char line[128];
    int n = snprintf(line, sizeof(line), "  v%d %d:", a, v);
    for (int c = 0; c < attr[a].size && c < 4; c++) {
        switch (attr[a].type) {
        case GL_FLOAT: n += snprintf(line + n, sizeof(line) - n, " %g", ((const float *)src)[c]); break;
        case GL_SHORT: n += snprintf(line + n, sizeof(line) - n, " %d", ((const int16_t *)src)[c]); break;
        case GL_UNSIGNED_SHORT: n += snprintf(line + n, sizeof(line) - n, " %u", ((const uint16_t *)src)[c]); break;
        case GL_UNSIGNED_BYTE: n += snprintf(line + n, sizeof(line) - n, " %u", src[c]); break;
        case GL_BYTE: n += snprintf(line + n, sizeof(line) - n, " %d", ((const int8_t *)src)[c]); break;
        default: n += snprintf(line + n, sizeof(line) - n, " ?"); break;
        }
    }
    T("%s", line);
}

static void dump_draw(GLsizei count, GLenum type, const void *idx) {
    if (type != GL_UNSIGNED_SHORT)
        return;
    const uint16_t *ind = (const uint16_t *)buf_data(bound_elem, idx);
    int maxi = 0;
    char line[160];
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (ind[i] > maxi)
            maxi = ind[i];
        if (i >= 96)
            continue;
        n += snprintf(line + n, sizeof(line) - n, " %u", ind[i]);
        if (n > 120 || i == count - 1 || i == 95) {
            T("  i%s", line);
            n = 0;
        }
    }
    GLint prog = 0, enabled;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    for (int a = 0; a < 4; a++) {
        glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
        if (!enabled)
            continue;
        for (int v = 0; v <= maxi && v < 64; v++)
            dump_vertex(a, v);
    }
}

static void t_draw_elements(GLenum mode, GLsizei count, GLenum type, const void *idx) {
    if (gl_trace_on) {
        T("DrawElements mode %x count %d type %x off %p", mode, count, type, idx);
        dump_draw(count, type, idx);
    }
    if (cur_prog < MAX_IDS && prog_mode[cur_prog])
        return;
    glDrawElements(mode, count, type, idx);
}

static void t_draw_arrays(GLenum mode, GLint first, GLsizei count) {
    if (gl_trace_on)
        T("DrawArrays mode %x first %d count %d", mode, first, count);
    if (cur_prog < MAX_IDS && prog_mode[cur_prog])
        return;
    glDrawArrays(mode, first, count);
}

static GLint t_get_attrib_location(GLuint prog, const GLchar *name) {
    GLint l = glGetAttribLocation(prog, name);
    if (nlocs < (int)(sizeof(locs) / sizeof(locs[0]))) {
        locs[nlocs].prog = prog;
        locs[nlocs].loc = -1000 - l;
        snprintf(locs[nlocs].name, sizeof(locs[nlocs].name), "%s", name);
        nlocs++;
    }
    return l;
}

static void t_bind_buffer(GLenum target, GLuint buf) {
    if (target == GL_ARRAY_BUFFER)
        bound_array = buf;
    else if (target == GL_ELEMENT_ARRAY_BUFFER)
        bound_elem = buf;
    if (gl_trace_on)
        T("BindBuffer %x %u", target, buf);
    glBindBuffer(target, buf);
}

static void t_buffer_data(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
    if (gl_trace_on)
        T("BufferData %x size %d data %p usage %x", target, (int)size, data, usage);
    glBufferData(target, size, data, usage);
}

static void t_buffer_sub_data(GLenum target, GLintptr off, GLsizeiptr size, const void *data) {
    if (gl_trace_on)
        T("BufferSubData %x off %d size %d", target, (int)off, (int)size);
    glBufferSubData(target, off, size, data);
}

static void *t_map_buffer(GLenum target, GLenum access) {
    extern uint32_t vgl_map_orphans;
    uint32_t before = vgl_map_orphans;
    void *p = gl_map_buffer_oes(target, access);
    if (gl_trace_on)
        T("MapBuffer %x -> %p%s", target, p, vgl_map_orphans != before ? " (orphaned)" : "");
    return p;
}

static GLboolean t_unmap_buffer(GLenum target) {
    if (gl_trace_on)
        T("UnmapBuffer %x", target);
    return gl_unmap_buffer_oes(target);
}

so_default_dynlib gl_trace_imports[] = {
    { "glTexImage2D", (uintptr_t)&t_tex_image_2d },
    { "glDeleteTextures", (uintptr_t)&t_delete_textures },
    { "glBindTexture", (uintptr_t)&t_bind_texture },
    { "glTexParameteri", (uintptr_t)&t_tex_parameteri },
    { "glBindBuffer", (uintptr_t)&t_bind_buffer },
    { "glBufferData", (uintptr_t)&t_buffer_data },
    { "glBufferSubData", (uintptr_t)&t_buffer_sub_data },
    { "glMapBufferOES", (uintptr_t)&t_map_buffer },
    { "glUnmapBufferOES", (uintptr_t)&t_unmap_buffer },
    { "glShaderSource", (uintptr_t)&t_shader_source },
    { "glAttachShader", (uintptr_t)&t_attach },
    { "glGetUniformLocation", (uintptr_t)&t_get_uniform_location },
    { "glUseProgram", (uintptr_t)&t_use_program },
    { "glUniform4fv", (uintptr_t)&t_uniform4fv },
    { "glUniform3fv", (uintptr_t)&t_uniform3fv },
    { "glUniform1fv", (uintptr_t)&t_uniform1fv },
    { "glUniformMatrix4fv", (uintptr_t)&t_uniform_matrix4fv },
    { "glVertexAttribPointer", (uintptr_t)&t_vertex_attrib_pointer },
    { "glDrawElements", (uintptr_t)&t_draw_elements },
    { "glDrawArrays", (uintptr_t)&t_draw_arrays },
    { "glGetAttribLocation", (uintptr_t)&t_get_attrib_location },
};

int gl_trace_imports_size = sizeof(gl_trace_imports);
#endif
