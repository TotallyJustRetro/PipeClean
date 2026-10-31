#pragma once

#include "sml2_native_stage.h"

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

/* Render the native stage at the world's logical camera size. The output is
 * one byte per pixel (Game Boy palette index 0..3); pitch is measured in
 * bytes and may exceed the logical width. Pixels outside the stage are filled
 * with clear_color. This routine is SDL-independent and does not scale or
 * stretch a 160x144 hardware frame. */
int sml2_native_render_stage(
    const Sml2NativeStage *stage, const Sml2NativeWorld *world,
    Sml2NativeReadTilePixels read_tile, void *userdata,
    uint8_t *pixels, size_t pitch, size_t pixels_capacity,
    uint8_t clear_color);
