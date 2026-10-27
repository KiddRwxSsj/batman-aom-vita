// the obb only has ATITC textures. vitaGL would decode them to rgba8888 (4-8x the memory),
// but an ATC block is almost a DXT block, so they get rewritten block by block and stay compressed

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "util.h"

#define ATC_RGB 0x8C92
#define ATC_RGBA_EXPLICIT 0x8C93
#define ATC_RGBA_INTERPOLATED 0x87EE
#define DXT1_RGB 0x83F0
#define DXT3_RGBA 0x83F2
#define DXT5_RGBA 0x83F3

static void unpack565(uint32_t c, int *rgb) {
    rgb[0] = (c >> 11) & 31;
    rgb[1] = (c >> 5) & 63;
    rgb[2] = c & 31;
    rgb[0] = (rgb[0] << 3) | (rgb[0] >> 2);
    rgb[1] = (rgb[1] << 2) | (rgb[1] >> 4);
    rgb[2] = (rgb[2] << 3) | (rgb[2] >> 2);
}

// atc color0 is r5 g5 b5 with the top bit as a mode flag, and green borrows blue's top bit
// as its low bit (that's how the reference decoder reads it). color1 is plain 565
static void color_block(const uint8_t *in, uint8_t *out) {
    uint32_t a0 = in[0] | (in[1] << 8), a1 = in[2] | (in[3] << 8);
    uint32_t idx = in[4] | (in[5] << 8) | (in[6] << 16) | ((uint32_t)in[7] << 24);
    uint32_t c0 = (((a0 >> 10) & 31) << 11) | (((a0 >> 4) & 63) << 5) | (a0 & 31);
    uint32_t c1 = a1;
    uint8_t map[4];

    if (!(a0 & 0x8000)) {
        // atc 0..3 = c0, 2/3 c0 + 1/3 c1, 1/3 c0 + 2/3 c1, c1. dxt 0..3 = e0, e1, 2/3 e0, 1/3 e0
        if (c0 > c1) {
            map[0] = 0; map[1] = 2; map[2] = 3; map[3] = 1;
        } else if (c0 < c1) {
            uint32_t t = c0; c0 = c1; c1 = t;
            map[0] = 1; map[1] = 3; map[2] = 2; map[3] = 0;
        } else {
            map[0] = map[1] = map[2] = map[3] = 0;
        }
    } else {
        // atc 0..3 = black, c0 - c1/4, c0, c1. no dxt mode has that, so take black and the
        // brightest colour as ends and snap each of the four onto the nearest dxt step
        int p[4][3], e[3];
        unpack565(c0, p[2]);
        unpack565(c1, p[3]);
        for (int k = 0; k < 3; k++) {
            p[0][k] = 0;
            p[1][k] = p[2][k] - p[3][k] / 4;
            if (p[1][k] < 0)
                p[1][k] = 0;
        }
        uint32_t hi = (p[2][0] + p[2][1] + p[2][2] >= p[3][0] + p[3][1] + p[3][2]) ? c0 : c1;
        unpack565(hi, e);
        if (!hi) {
            map[0] = map[1] = map[2] = map[3] = 0;
            c0 = c1 = 0;
        } else {
            // dxt steps with e0 = hi, e1 = black: 0 = hi, 1 = black, 2 = 2/3 hi, 3 = 1/3 hi
            static const int frac[4] = { 3, 0, 2, 1 };
            for (int i = 0; i < 4; i++) {
                int best = 0, best_d = 1 << 30;
                for (int s = 0; s < 4; s++) {
                    int d = 0;
                    for (int k = 0; k < 3; k++) {
                        int v = e[k] * frac[s] / 3 - p[i][k];
                        d += v * v;
                    }
                    if (d < best_d) {
                        best_d = d;
                        best = s;
                    }
                }
                map[i] = best;
            }
            c0 = hi;
            c1 = 0;
        }
    }

    uint32_t o = 0;
    for (int i = 0; i < 16; i++)
        o |= (uint32_t)map[(idx >> (i * 2)) & 3] << (i * 2);
    out[0] = c0;
    out[1] = c0 >> 8;
    out[2] = c1;
    out[3] = c1 >> 8;
    out[4] = o;
    out[5] = o >> 8;
    out[6] = o >> 16;
    out[7] = o >> 24;
}

void gl_compressed_tex_image_2d(GLenum target, GLint level, GLenum format, GLsizei w, GLsizei h, GLint border,
                                GLsizei size, const void *data) {
#ifdef GL_TRACE
    extern void gl_upload_log(const char *fmt, ...);
    GLint t = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &t);
    gl_upload_log("Compressed tex %d target %x lvl %d %dx%d fmt %x size %d data %d", t, target, level, w, h, format, size, data != NULL);
#endif
    GLenum dxt;
    int alpha;
    switch (format) {
    case ATC_RGB: dxt = DXT1_RGB; alpha = 0; break;
#ifdef VITA3K
    // vita3k dies on vitaGL's RAW128 swizzle transfer for dxt3/5, its own atc decoder is fine there
    case ATC_RGBA_EXPLICIT:
    case ATC_RGBA_INTERPOLATED:
        glCompressedTexImage2D(target, level, format, w, h, border, size, data);
        return;
#else
    case ATC_RGBA_EXPLICIT: dxt = DXT3_RGBA; alpha = 1; break;
    case ATC_RGBA_INTERPOLATED: dxt = DXT5_RGBA; alpha = 1; break;
#endif
    default:
        glCompressedTexImage2D(target, level, format, w, h, border, size, data);
        return;
    }
    // vitaGL swizzles dxt mips with a w/4 x h/4 transfer, which is 0 wide below 4x4.
    // dropping those levels just leaves a shorter mip chain
    if (level > 0 && (w < 4 || h < 4))
        return;
    if (!data) {
        glCompressedTexImage2D(target, level, dxt, w, h, border, size, NULL);
        return;
    }

    int blocks = ((w + 3) / 4) * ((h + 3) / 4);
    int bsize = alpha ? 16 : 8;
    if (size < blocks * bsize) {
        LOG("atc: %dx%d fmt %x size %d too small", w, h, format, size);
        blocks = size / bsize;
    }
    VLOG("atc: %dx%d lvl %d fmt %x size %d", w, h, level, format, size);
    uint8_t *out = malloc(blocks * bsize);
    const uint8_t *in = data;
    for (int b = 0; b < blocks; b++) {
        // the explicit and interpolated alpha halves are laid out exactly like dxt3/dxt5's
        if (alpha)
            memcpy(out + b * bsize, in + b * bsize, 8);
        color_block(in + b * bsize + (alpha ? 8 : 0), out + b * bsize + (alpha ? 8 : 0));
    }
    glCompressedTexImage2D(target, level, dxt, w, h, border, blocks * bsize, out);
    free(out);
}
