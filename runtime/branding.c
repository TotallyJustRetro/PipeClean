#include "branding.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>

static SDL_Texture *icon_tex;

static int icon_path(char *out, size_t n)
{
    snprintf(out, n, "%sassets/pipeclean-icon.bmp", settings_dir());
    FILE *f = fopen(out, "rb");
    if (f) {
        fclose(f);
        return 1;
    }
    snprintf(out, n, "assets/pipeclean-icon.bmp");
    f = fopen(out, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int branding_init(SDL_Renderer *renderer, SDL_Window *window)
{
    char path[1200];
    if (!icon_path(path, sizeof path)) return 0;

    SDL_Surface *s = SDL_LoadBMP(path);
    if (!s) {
        fprintf(stderr, "PipeClean branding: couldn't load %s: %s\n", path, SDL_GetError());
        return 0;
    }

    SDL_SetColorKey(s, SDL_TRUE, SDL_MapRGB(s->format, 0, 0, 0));
    if (window) SDL_SetWindowIcon(window, s);

    if (renderer) {
        icon_tex = SDL_CreateTextureFromSurface(renderer, s);
        if (icon_tex) {
            SDL_SetTextureBlendMode(icon_tex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(icon_tex, SDL_ScaleModeLinear);
        }
    }

    SDL_FreeSurface(s);
    return icon_tex != NULL;
}

void branding_shutdown(void)
{
    if (icon_tex) SDL_DestroyTexture(icon_tex);
    icon_tex = NULL;
}

SDL_Texture *branding_icon_texture(void)
{
    return icon_tex;
}
