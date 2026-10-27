#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

void audio_init(void);
int audio_load_sound(const char *path);
void audio_unload_sound(int id);
int audio_play_sound(int id, int loop);
void audio_stop_sound(int stream);
void audio_set_volume(int stream, float volume);
void audio_play_song(int64_t off, int64_t len, const char *name, float fade_in, float fade_out);
void audio_stop_song(void);
void audio_song_volume(float volume);
void audio_update_song(float dt);

#endif
