#ifndef GFX_H
#define GFX_H

#include <vitaGL.h>

#define SCREEN_W 960
#define SCREEN_H 544

extern int gfx_ready;
extern volatile int shot_req;

void gfx_init(void);
void gfx_swap(void);

const GLubyte *gl_get_string(GLenum name);
void gl_blend_color(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void gl_link_program(GLuint program);
void gl_shader_source(GLuint sh, GLsizei n, const GLchar *const *str, const GLint *len);
void gl_get_shader_precision_format(GLenum shader, GLenum precision, GLint *range, GLint *prec);
void *gl_map_buffer_oes(GLenum target, GLenum access);
GLboolean gl_unmap_buffer_oes(GLenum target);
void *egl_get_proc_address(const char *name);
void gl_compressed_tex_image_2d(GLenum target, GLint level, GLenum format, GLsizei w, GLsizei h, GLint border,
                                GLsizei size, const void *data);

#endif
