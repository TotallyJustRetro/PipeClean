#pragma once
#include <SDL.h>
unsigned char *image_load_rgba(const char *path, int *w, int *h);
void bg_init(SDL_Renderer *r);
void bg_shutdown(void);
int  bg_load(const char *path);
void bg_update(float dt);
SDL_Texture *bg_texture(int *w, int *h);
int  bg_frames(void);