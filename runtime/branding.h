#pragma once
#include <SDL.h>

/* PipeClean visual identity used by the native window and launcher header. */
int branding_init(SDL_Renderer *renderer, SDL_Window *window);
void branding_shutdown(void);
SDL_Texture *branding_icon_texture(void);
