#include "branding.h"
#include "bgimg.h"
#include <stdlib.h>
#include <string.h>

static SDL_Texture *icon_tex;
static SDL_Texture *logo_tex;

static SDL_Texture *load_brand_texture(SDL_Renderer *renderer, const char *name, int *w_out, int *h_out)
{
    if (!renderer) return NULL;

    const char *paths[] = {
        name,
        "./assets/pipeclean-icon.png",
        "../assets/pipeclean-icon.png"
    };
    const char *logo_paths[] = {
        name,
        "./assets/pipeclean-logo.png",
        "../assets/pipeclean-logo.png"
    };
    const char *const *list = strstr(name, "logo") ? logo_paths : paths;

    for (int i = 0; i < 3; i++) {
        int w = 0, h = 0;
        unsigned char *px = image_load_rgba(list[i], &w, &h);
        if (!px) continue;

        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(
            px, w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32);
        if (!s) {
            free(px);
            continue;
        }

        SDL_Texture *t = SDL_CreateTextureFromSurface(renderer, s);
        SDL_FreeSurface(s);
        free(px);
        if (!t) continue;

        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
        if (w_out) *w_out = w;
        if (h_out) *h_out = h;
        return t;
    }
    return NULL;
}

int branding_init(SDL_Renderer *renderer, SDL_Window *window)
{
    (void)window;

    /*
     * These are the supplied PipeClean artwork files themselves.
     * Do not redraw or substitute the identity with SDL primitives.
     */
    icon_tex = load_brand_texture(renderer, "assets/pipeclean-icon.png", NULL, NULL);
    logo_tex = load_brand_texture(renderer, "assets/pipeclean-logo.png", NULL, NULL);

    /*
     * On Windows the embedded .ico resource in pipeclean.rc remains the
     * native title-bar/taskbar icon. The launcher artwork is the supplied PNG.
     */
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
