#include "branding.h"
#include <SDL_image.h>
#include <stdio.h>
#include <string.h>

static SDL_Texture *icon_tex;
static SDL_Texture *logo_tex;

static SDL_Texture *load_brand_texture(SDL_Renderer *renderer, const char *name,
                                       const char *fallback, int *w_out, int *h_out)
{
    if (!renderer) return NULL;

    const char *paths[] = {name, fallback};
    char base[2048], p0[2048], p1[2048], p2[2048];
    const char *base_dir = SDL_GetBasePath();
    if (base_dir) snprintf(base, sizeof base, "%s", base_dir); else base[0] = 0;
    for (int i = 0; i < 2; i++) {
        if (!paths[i] || !paths[i][0]) continue;
        const char *try_paths[5] = { paths[i], NULL, NULL, NULL, NULL };
        if (base[0]) {
            snprintf(p0, sizeof p0, "%s%s", base, paths[i]);
            snprintf(p1, sizeof p1, "%s../%s", base, paths[i]);
            snprintf(p2, sizeof p2, "%s../../%s", base, paths[i]);
            try_paths[1] = p0; try_paths[2] = p1; try_paths[3] = p2;
        }
        for (int j = 0; j < 4; j++) {
            if (!try_paths[j]) continue;
            SDL_Surface *src = IMG_Load(try_paths[j]);
            if (!src) continue;

            SDL_Surface *s = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_RGBA32, 0);
            SDL_FreeSurface(src);
            if (!s) continue;
            SDL_Texture *t = SDL_CreateTextureFromSurface(renderer, s);
            if (w_out) *w_out = s->w;
            if (h_out) *h_out = s->h;
            SDL_FreeSurface(s);
            if (!t) continue;
            SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
            return t;
        }
    }
    return NULL;
}

int branding_init(SDL_Renderer *renderer, SDL_Window *window)
{
    (void)window;

    /*
     * Use the actual PipeClean SVG artwork already stored in the repository.
     * PNG fallbacks are retained for packaged builds that include them.
     */
    icon_tex = load_brand_texture(renderer,
                                  "assets/pipeclean-icon.svg",
                                  "assets/pipeclean-icon.png", NULL, NULL);
    logo_tex = load_brand_texture(renderer,
                                  "assets/pipeclean-logo.svg",
                                  "assets/pipeclean-logo.png", NULL, NULL);

    if (!icon_tex)
        fprintf(stderr, "PipeClean branding: couldn't load icon: %s\n", IMG_GetError());
    if (!logo_tex)
        fprintf(stderr, "PipeClean branding: couldn't load logo: %s\n", IMG_GetError());

    return icon_tex != NULL || logo_tex != NULL;
}

void branding_shutdown(void)
{
    if (icon_tex) SDL_DestroyTexture(icon_tex);
    if (logo_tex) SDL_DestroyTexture(logo_tex);
    icon_tex = NULL;
    logo_tex = NULL;
}

SDL_Texture *branding_icon_texture(void) { return icon_tex; }
SDL_Texture *branding_logo_texture(void) { return logo_tex; }
