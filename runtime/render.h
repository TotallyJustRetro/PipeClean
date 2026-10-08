#pragma once
#include <SDL.h>
#include "emu.h"
void render_init(SDL_Renderer *r);
void render_shutdown(void);
void render_reset(void);
void render_build(const Frame *f, int game, int live);
void render_fit(int W, int H, int aspect, int scaling, SDL_Rect *out);
void render_draw(const SDL_Rect *r, int scaling);
uint32_t render_avg_color(void);
void frame_from_ppu(Frame *f);
int render_width(void);