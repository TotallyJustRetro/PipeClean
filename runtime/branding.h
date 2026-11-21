#pragma once
#include <SDL.h>
#include "games.h"

/* PipeClean uses the supplied artwork files directly; no procedural redraw. */
int branding_init(SDL_Renderer *renderer, SDL_Window *window);
void branding_shutdown(void);
SDL_Texture *branding_icon_texture(void);
SDL_Texture *branding_logo_texture(void);

/* Optional per-game hero artwork. Missing artwork falls back to the game's accent UI. */
SDL_Texture *branding_game_banner(int game); /* assets/<id>-banner.webp or .png */
SDL_Texture *branding_game_logo(int game);   /* assets/<id>-logo.webp or .png */
