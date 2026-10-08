#include "branding.h"
#include "settings.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static SDL_Texture *icon_tex;

static uint32_t rgba8(int r, int g, int b, int a)
{
    return ((uint32_t)(r & 255) << 24) | ((uint32_t)(g & 255) << 16) |
           ((uint32_t)(b & 255) << 8) | (uint32_t)(a & 255);
}

static void put_px(uint32_t *px, int w, int h, int x, int y, uint32_t c)
{
    if ((unsigned)x < (unsigned)w && (unsigned)y < (unsigned)h) px[y * w + x] = c;
}

static void draw_pipe_logo(uint32_t *px, int w, int h)
{
    const int cx = w / 2, cy = h / 2;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++)
        px[y * w + x] = rgba8(24, 27, 57, 255);

    /* Rounded-square frame. */
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int dx = x < 8 ? 8 - x : (x >= w - 8 ? x - (w - 9) : 0);
        int dy = y < 8 ? 8 - y : (y >= h - 8 ? y - (h - 9) : 0);
        if (dx || dy) {
            float rr = sqrtf((float)(dx * dx + dy * dy));
            if (rr > 8.2f) continue;
        }
        int edge = (x < 4 || x >= w - 4 || y < 4 || y >= h - 4);
        if (edge) put_px(px, w, h, x, y, rgba8(92, 78, 224, 255));
    }

    /* Gradient pipe ring. */
    for (int y = 8; y < h - 8; y++) for (int x = 8; x < w - 8; x++) {
        float dx = (x - cx) / 22.0f, dy = (y - cy) / 22.0f;
        float rr = sqrtf(dx * dx + dy * dy);
        if (rr < 0.70f || rr > 1.15f) continue;
        if (y > cy - 1 && y < cy + 13) continue;
        int t = (int)(((float)x / (float)(w - 1)) * 255.0f);
        int r = 50 + t * 75 / 255;
        int g = 228 - t * 125 / 255;
        int b = 240 + t * 15 / 255;
        put_px(px, w, h, x, y, rgba8(r, g, b, 255));
    }

    /* Horizontal caps. */
    for (int y = cy - 7; y <= cy + 7; y++)
        for (int x = 9; x < 18; x++)
            put_px(px, w, h, x, y, rgba8(62, 138, 255, 255));
    for (int y = cy - 7; y <= cy + 7; y++)
        for (int x = w - 18; x < w - 9; x++)
            put_px(px, w, h, x, y, rgba8(103, 73, 250, 255));

    /* Dark center and four cyan blocks. */
    for (int y = cy - 12; y <= cy + 12; y++)
        for (int x = cx - 17; x <= cx + 17; x++)
            put_px(px, w, h, x, y, rgba8(9, 13, 31, 255));
    for (int gy = -1; gy <= 1; gy += 2)
        for (int gx = -1; gx <= 1; gx += 2)
            for (int y = cy + gy * 8 - 4; y <= cy + gy * 8 + 4; y++)
                for (int x = cx + gx * 8 - 4; x <= cx + gx * 8 + 4; x++)
                    put_px(px, w, h, x, y, rgba8(77, 233, 241, 255));

    /* Pixel sparks. */
    uint32_t cyan = rgba8(63, 226, 238, 255), purple = rgba8(125, 70, 255, 255);
    put_px(px, w, h, 6, 49, cyan); put_px(px, w, h, 10, 45, cyan); put_px(px, w, h, 4, 54, cyan);
    put_px(px, w, h, w - 8, 13, purple); put_px(px, w, h, w - 4, 17, purple); put_px(px, w, h, w - 10, 9, purple);
}

static SDL_Surface *make_icon_surface(int size)
{
    uint32_t *px = (uint32_t *)calloc((size_t)size * size, sizeof *px);
    if (!px) return NULL;
    draw_pipe_logo(px, size, size);
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(
        px, size, size, 32, size * (int)sizeof(uint32_t), SDL_PIXELFORMAT_RGBA8888);
    if (!s) { free(px); return NULL; }

    /* Surface owns the pixel buffer through its userdata only indirectly, so
     * copy once into SDL-owned storage before freeing our temporary buffer. */
    SDL_Surface *copy = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_RGBA8888, 0);
    SDL_FreeSurface(s);
    free(px);
    return copy;
}

int branding_init(SDL_Renderer *renderer, SDL_Window *window)
{
    SDL_Surface *s = make_icon_surface(64);
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
