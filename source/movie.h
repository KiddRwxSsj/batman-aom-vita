#ifndef MOVIE_H
#define MOVIE_H

#include <stdint.h>

void movie_start(int64_t off, int64_t len, int loop, int delay);
void movie_stop(void);
int movie_playing(void);
void movie_tick(void);
void movie_draw(void);
void movie_gl_init(void);
void movie_gl_owner(int own);

#endif
