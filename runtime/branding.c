#include "branding.h"
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

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

static void thick_line(uint32_t *p, int w, int h, int x0, int y0, int x1, int y1,
                       int r, uint32_t c)
{
    int dx = x1 - x0, dy = y1 - y0;
    int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    if (!steps) { disc(p, w, h, x0, y0, r, c); return; }
    for (int i = 0; i <= steps; i++) {
        int x = x0 + dx * i / steps, y = y0 + dy * i / steps;
        disc(p, w, h, x, y, r, c);
    }
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
    const int n = 96;
    uint32_t *p = (uint32_t *)calloc((size_t)n * n, sizeof *p);
    if (!p) return NULL;

    const uint32_t bg0 = rgba8(87, 54, 229, 255);
    const uint32_t bg1 = rgba8(68, 43, 176, 255);
    const uint32_t edge = rgba8(191, 150, 255, 255);
    const uint32_t dark = rgba8(8, 7, 36, 255);
    const uint32_t pipe = rgba8(103, 66, 232, 255);
    const uint32_t pipe2 = rgba8(125, 77, 240, 255);
    const uint32_t hi = rgba8(201, 181, 255, 255);
    const uint32_t white = rgba8(249, 250, 255, 255);
    const uint32_t water = rgba8(91, 213, 255, 255);
    const uint32_t water2 = rgba8(207, 249, 255, 255);

    /* Purple rounded-square badge. */
    rounded_box(p, n, n, 2, 2, 93, 93, 18, bg0);
    rounded_box(p, n, n, 4, 4, 91, 91, 16, bg1);
    rounded_box(p, n, n, 6, 6, 89, 89, 14, edge);

    /* Water splashes behind the pipe. */
    thick_line(p, n, n, 15, 30, 22, 18, 6, water);
    thick_line(p, n, n, 22, 18, 28, 11, 4, water);
    thick_line(p, n, n, 72, 17, 82, 12, 5, water);
    thick_line(p, n, n, 77, 25, 86, 21, 5, water);
    thick_line(p, n, n, 22, 82, 31, 86, 6, water);
    thick_line(p, n, n, 61, 83, 72, 79, 6, water);
    disc(p, n, n, 13, 52, 4, water);
    disc(p, n, n, 84, 57, 5, water);
    disc(p, n, n, 29, 12, 3, water2);
    disc(p, n, n, 80, 13, 3, water2);
    disc(p, n, n, 74, 88, 3, water2);
    disc(p, n, n, 16, 76, 2, water2);

    /*
     * The pipe is a bent, heavy cartoon tube. Draw the dark silhouette first,
     * then the purple body slightly inset so the black outline stays visible.
     */
    thick_line(p, n, n, 25, 63, 20, 53, 10, dark);
    thick_line(p, n, n, 20, 53, 29, 42, 10, dark);
    thick_line(p, n, n, 29, 42, 43, 34, 10, dark);
    thick_line(p, n, n, 43, 34, 61, 36, 10, dark);
    thick_line(p, n, n, 61, 36, 73, 43, 10, dark);

    thick_line(p, n, n, 25, 63, 20, 53, 7, pipe);
    thick_line(p, n, n, 20, 53, 29, 42, 7, pipe);
    thick_line(p, n, n, 29, 42, 43, 34, 7, pipe);
    thick_line(p, n, n, 43, 34, 61, 36, 7, pipe);
    thick_line(p, n, n, 61, 36, 73, 43, 7, pipe2);

    /* Lower purple shading and bright upper rim. */
    thick_line(p, n, n, 24, 65, 21, 55, 3, rgba8(53, 31, 150, 255));
    thick_line(p, n, n, 25, 48, 39, 39, 2, hi);
    thick_line(p, n, n, 41, 37, 59, 39, 2, hi);

    /* Open end of the pipe. */
    disc(p, n, n, 73, 43, 13, dark);
    disc(p, n, n, 73, 43, 9, rgba8(15, 11, 54, 255));
    thick_line(p, n, n, 65, 36, 69, 33, 2, hi);

    /* Two oversized expressive eyes. */
    disc(p, n, n, 46, 24, 12, dark);
    disc(p, n, n, 46, 24, 9, white);
    disc(p, n, n, 55, 24, 5, dark);
    disc(p, n, n, 42, 19, 2, white);

    disc(p, n, n, 36, 28, 11, dark);
    disc(p, n, n, 36, 28, 8, white);
    disc(p, n, n, 39, 28, 4, dark);
    disc(p, n, n, 33, 23, 2, white);

    /* Small cyan droplets in the negative spaces. */
    disc(p, n, n, 12, 30, 3, water2);
    disc(p, n, n, 83, 31, 3, water2);
    disc(p, n, n, 33, 86, 2, water2);

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
            SDL_SetTextureScaleMode(icon_tex, SDL_ScaleModeNearest);
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
