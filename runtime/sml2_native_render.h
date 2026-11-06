#pragma once

#include "sml2_native_actor.h"

#include <stddef.h>
#include <stdint.h>

/* Supplies one decoded 8x8 Game Boy graphics tile as 64 palette indices.
 * Each pixel must be in the range 0..3. The renderer caches each graphics ID
 * once per frame, so callers may decode from ROM data or a native asset cache. */
typedef int (*Sml2NativeReadTilePixels)(
    void *userdata, uint8_t graphics_id, uint8_t pixels_out[64]);

/* Optional Game Boy VRAM adapter for original SML2 graphics. lcdc supplies
 * LCDC.4 so signed/unsigned tile addressing matches the game's BG tile mode. */
typedef struct Sml2NativeGbGraphics {
    const uint8_t *vram; /* 0x2000 bytes */
    uint8_t lcdc;
} Sml2NativeGbGraphics;

int sml2_native_read_gb_tile_pixels(
    void *userdata, uint8_t graphics_id, uint8_t pixels_out[64]);

enum {
    SML2_NATIVE_SPRITE_FLIP_X = 1u << 0,
    SML2_NATIVE_SPRITE_FLIP_Y = 1u << 1,
    SML2_NATIVE_SPRITE_8X16 = 1u << 2
};

/* One native sprite in logical view-pixel coordinates. tile_id is the Game
 * Boy OBJ tile number; 8x16 sprites use its even-aligned pair. Palette maps
 * source color indices 0..3 to framebuffer indices. Color zero is transparent. */
typedef struct {
    int32_t x;
    int32_t y;
    uint8_t tile_id;
    uint8_t flags;
    uint8_t palette[4];
} Sml2NativeSprite;

/* Composite one 8x8 or 8x16 OBJ into an indexed logical framebuffer. Call in
 * OAM order to preserve caller-defined overlap priority. Clipping is applied
 * at the viewport edges; tile decoding is cached for the duration of the draw. */
int sml2_native_draw_sprite(
    uint8_t *pixels, size_t pitch, size_t width, size_t height,
    const Sml2NativeSprite *sprite,
    Sml2NativeReadTilePixels read_tile, void *userdata);

/* Render the native stage at the world's logical camera size. The output is
 * one byte per pixel (Game Boy palette index 0..3); pitch is measured in
 * bytes and may exceed the logical width. Pixels outside the stage are filled
 * with clear_color. This routine is SDL-independent and does not scale or
 * stretch a 160x144 hardware frame. */

/* Resolve one visible native actor to its current OBJ pose. Return 0 to draw,
 * a positive value to skip an actor with no visible sprite, or -1 on error.
 * The resolver owns game-specific animation selection; tick is deterministic. */
typedef int (*Sml2NativeResolveActorSprite)(
    void *userdata, const Sml2NativeActorDraw *actor_draw, uint64_t tick,
    Sml2NativeSprite *sprite);

/* Compose one complete native scene: stage first, then visible awake actors in
 * stable actor-slot order. Sprite positions are logical view pixels. This is
 * the backend-neutral composition seam; callers supply authentic animation
 * selection and graphics decoding separately. */
int sml2_native_render_scene(
    const Sml2NativeStage *stage, const Sml2NativeActorWorld *actors,
    const Sml2NativeWorld *world, uint64_t tick,
    Sml2NativeResolveActorSprite resolve_sprite, void *sprite_userdata,
    Sml2NativeReadTilePixels read_tile, void *tile_userdata,
    uint8_t *pixels, size_t pitch, size_t pixels_capacity,
    uint8_t clear_color);

int sml2_native_render_stage(
    const Sml2NativeStage *stage, const Sml2NativeWorld *world,
    Sml2NativeReadTilePixels read_tile, void *userdata,
    uint8_t *pixels, size_t pitch, size_t pixels_capacity,
    uint8_t clear_color);
