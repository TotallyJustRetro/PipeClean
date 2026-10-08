#pragma once
#include <SDL.h>

/* PipeClean uses the supplied artwork files directly; no procedural redraw. */
int branding_init(SDL_Renderer *renderer, SDL_Window *window);
void branding_shutdown(void);
SDL_Texture *branding_icon_texture(void);
SDL_Texture *branding_logo_texture(void);
