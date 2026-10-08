#include "branding.h"
#include <stdint.h>
#include <stdlib.h>

static SDL_Texture *icon_tex;

static uint32_t rgba8(int r, int g, int b, int a)
{
    return ((uint32_t)(r & 255) << 24) | ((uint32_t)(g & 255) << 16) |
           ((uint32_t)(b & 255) << 8) | (uint32_t)(a & 255);
}

static void put_px(uint32_t *p, int w, int h, int x, int y, uint32_t c)
{
    if ((unsigned)x < (unsigned)w && (unsigned)y < (unsigned)h) p[y * w + x] = c;
}

static void disc(uint32_t *p, int w, int h, int cx, int cy, int r, uint32_t c)
{
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r) put_px(p, w, h, cx + x, cy + y, c);
}

static void rounded_box(uint32_t *p, int w, int h, int x0, int y0, int x1, int y1,
                        int r, uint32_t c)
{
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            int dx = x < x0 + r ? x0 + r - x : (x > x1 - r ? x - (x1 - r) : 0);
            int dy = y < y0 + r ? y0 + r - y : (y > y1 - r ? y - (y1 - r) : 0);
            if (!dx || !dy || dx * dx + dy * dy <= r * r)
                put_px(p, w, h, x, y, c);
        }
}

static SDL_Surface *make_icon_surface(void)
{
    const int n = 64;
    uint32_t *p = (uint32_t *)calloc((size_t)n * n, sizeof *p);
    if (!p) return NULL;

    const uint32_t bg = rgba8(73, 49, 180, 255);
    const uint32_t edge = rgba8(183, 139, 255, 255);
    const uint32_t pipe = rgba8(99, 61, 224, 255);
    const uint32_t pipe_hi = rgba8(168, 117, 255, 255);
    const uint32_t dark = rgba8(16, 13, 45, 255);
    const uint32_t white = rgba8(248, 249, 255, 255);
    const uint32_t water = rgba8(101, 220, 255, 255);
    const uint32_t shine = rgba8(224, 251, 255, 255);

    rounded_box(p, n, n, 2, 2, 61, 61, 12, bg);
    rounded_box(p, n, n, 3, 3, 60, 60, 11, edge);
    rounded_box(p, n, n, 8, 26, 50, 53, 16, dark);
    rounded_box(p, n, n, 11, 28, 48, 51, 14, pipe);
    rounded_box(p, n, n, 14, 30, 43, 36, 7, pipe_hi);

    disc(p, n, n, 35, 22, 10, dark);
    disc(p, n, n, 35, 22, 8, white);
    disc(p, n, n, 38, 22, 3, dark);
    disc(p, n, n, 31, 18, 2, white);

    disc(p, n, n, 47, 36, 10, dark);
    disc(p, n, n, 47, 36, 6, rgba8(9, 8, 28, 255));
    disc(p, n, n, 43, 31, 2, pipe_hi);

    disc(p, n, n, 9, 13, 4, water);
    disc(p, n, n, 13, 9, 3, water);
    disc(p, n, n, 55, 14, 4, water);
    disc(p, n, n, 53, 9, 2, water);
    disc(p, n, n, 13, 53, 4, water);
    disc(p, n, n, 19, 56, 3, water);
    disc(p, n, n, 54, 51, 4, water);
    disc(p, n, n, 49, 56, 2, water);
    disc(p, n, n, 9, 14, 1, shine);
    disc(p, n, n, 55, 13, 1, shine);
    disc(p, n, n, 13, 52, 1, shine);

    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(
        p, n, n, 32, n * (int)sizeof(uint32_t), SDL_PIXELFORMAT_RGBA8888);
    if (!s) { free(p); return NULL; }
    SDL_Surface *copy = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_RGBA8888, 0);
    SDL_FreeSurface(s);
    free(p);
    return copy;
}

int branding_init(SDL_Renderer *renderer, SDL_Window *window)
{
    SDL_Surface *s = make_icon_surface();
    if (!s) return 0;
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
