#include "sml2_native_render.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static int read_test_tile(void *userdata, uint8_t tile_id, uint8_t out[64])
{
    unsigned i;
    unsigned *calls = (unsigned *)userdata;
    (*calls)++;
    for (i = 0; i < 64; ++i) out[i] = (uint8_t)(tile_id & 3u);
    return 0;
}

static int read_sprite_tile(void *userdata, uint8_t tile_id, uint8_t out[64])
{
    unsigned *calls = (unsigned *)userdata;
    unsigned i;
    ++*calls;
    for (i = 0; i < 64; ++i)
        out[i] = (uint8_t)(tile_id == 4 ? 1 : 2);
    if (tile_id == 4) out[2] = 0; /* transparent source pixel */
    return 0;
}

static Sml2NativeConfig make_config(void)
{
    Sml2NativeConfig c;
    memset(&c, 0, sizeof(c));
    c.level_width = 320;
    c.level_height = 192;
    c.gravity = 0x3000;
    c.horizontal_acceleration = 0x4000;
    c.horizontal_friction = 0x3000;
    c.max_run_speed = 0x28000;
    c.jump_velocity = -0x50000;
    c.terminal_velocity = 0x60000;
    c.player_width = 12;
    c.player_height = 16;
    c.camera_padding = 24;
    c.window_width = 1920;
    c.window_height = 1080;
    c.widescreen_mode = SML2_NATIVE_WIDESCREEN_16_9;
    c.camera_zoom_percent = SML2_NATIVE_ZOOM_100;
    c.max_camera_zoom_percent = SML2_NATIVE_ZOOM_200;
    return c;
}

int main(void)
{
    Sml2NativeConfig config = make_config();
    Sml2NativeWorld world;
    Sml2NativeStage stage;
    Sml2NativeStageTile tiles[1];
    uint8_t cells[20 * 12] = {0};
    uint8_t pixels[256 * 144];
    unsigned calls = 0;
    uint8_t vram[0x2000] = {0};
    uint8_t decoded[64];
    Sml2NativeGbGraphics gb;
    unsigned i;

    /* Decode both Game Boy tile-addressing modes from actual bitplanes. */
    gb.vram = vram;
    gb.lcdc = 0x10;
    vram[8 * 16] = 0x80;
    vram[8 * 16 + 1] = 0x40;
    assert(sml2_native_read_gb_tile_pixels(&gb, 8, decoded) == 0);
    assert(decoded[0] == 1 && decoded[1] == 2 && decoded[2] == 0);
    gb.lcdc = 0x00;
    vram[0x802] = 0x01;
    vram[0x803] = 0x80;
    assert(sml2_native_read_gb_tile_pixels(&gb, 0x80, decoded) == 0);
    assert(decoded[8] == 2 && decoded[15] == 1);
    assert(sml2_native_read_gb_tile_pixels(NULL, 0, decoded) == -1);

    memset(&tiles, 0, sizeof(tiles));
    tiles[0].graphics[0] = 1;
    tiles[0].graphics[1] = 2;
    tiles[0].graphics[2] = 3;
    tiles[0].graphics[3] = 0;
    assert(sml2_native_stage_init(&stage, 20, 12, cells, tiles, 1,
                                  NULL, 0, NULL, 0, 1) == 0);
    assert(sml2_native_init(&world, &config) == 0);

    /* Move the logical camera into the stage. 16:9 is a true 256-pixel
       world viewport, not a stretched 160-pixel image. */
    world.camera_x = 32;
    world.camera_y = 16;
    assert(world.camera_view_width == 256);
    assert(world.camera_view_height == 144);
    assert(sml2_native_render_stage(&stage, &world, read_test_tile, &calls,
                                    pixels, 256, sizeof(pixels), 2) == 0);

    assert(pixels[0] == 1);                 /* top-left 8x8 graphics tile */
    assert(pixels[8] == 2);                 /* top-right */
    assert(pixels[8 * 256] == 3);           /* bottom-left */
    assert(pixels[8 * 256 + 8] == 0);       /* bottom-right */
    assert(pixels[16] == 1);                /* next 16x16 block */
    assert(pixels[143 * 256 + 255] == 0);   /* right/bottom visible edge */
    assert(calls == 4);                     /* each graphics ID decoded once */

    /* Pixels beyond the 320x192 stage use the explicit clear palette index. */
    world.camera_x = 100;
    assert(sml2_native_render_stage(&stage, &world, read_test_tile, &calls,
                                    pixels, 256, sizeof(pixels), 2) == 0);
    assert(pixels[220] == 2);

    /* Enlarged views may extend beyond the left/top level edge. Those pixels
       are cleared while valid world pixels remain correctly sampled. */
    world.camera_x = -8;
    world.camera_y = -4;
    assert(sml2_native_render_stage(&stage, &world, read_test_tile, &calls,
                                    pixels, 256, sizeof(pixels), 2) == 0);
    assert(pixels[0] == 2);
    assert(pixels[8 * 256 + 8] == 1);

    /* Native OBJ composition supports 8x16 tile pairs, flips, palette maps,
       transparency, and clipping without stretching the source Game Boy art. */
    {
        uint8_t sprite_frame[8 * 16] = {0};
        Sml2NativeSprite sprite;
        unsigned sprite_calls = 0;
        memset(&sprite, 0, sizeof(sprite));
        sprite.x = -2;
        sprite.y = 0;
        sprite.tile_id = 5; /* hardware 8x16 mode aligns this to tile 4 */
        sprite.flags = SML2_NATIVE_SPRITE_8X16 |
                       SML2_NATIVE_SPRITE_FLIP_Y;
        sprite.palette[0] = 0;
        sprite.palette[1] = 3;
        sprite.palette[2] = 2;
        sprite.palette[3] = 1;
        assert(sml2_native_draw_sprite(sprite_frame, 8, 8, 16, &sprite,
                    read_sprite_tile, &sprite_calls) == 0);
        assert(sprite_calls == 2);
        assert(sprite_frame[0] == 2);       /* top half uses second tile */
        assert(sprite_frame[7] == 0);       /* right side is clipped */
        assert(sprite_frame[8 * 8] == 3);   /* bottom half uses first tile */
        assert(sprite_frame[15 * 8 + 0] == 0); /* color zero stays transparent */
        assert(sprite_frame[15 * 8 + 1] == 3);
        assert(sprite_frame[15 * 8 + 5] == 3);
        assert(sml2_native_draw_sprite(NULL, 8, 8, 16, &sprite,
                    read_sprite_tile, &sprite_calls) == -1);
        sprite.palette[1] = 4;
        assert(sml2_native_draw_sprite(sprite_frame, 8, 8, 16, &sprite,
                    read_sprite_tile, &sprite_calls) == -1);
    }

    /* A short pitch/capacity or an invalid palette index fails safely. */
    assert(sml2_native_render_stage(&stage, &world, read_test_tile, &calls,
                                    pixels, 255, sizeof(pixels), 0) == -1);
    for (i = 0; i < sizeof(pixels); ++i) pixels[i] = 0;
    assert(sml2_native_render_stage(&stage, &world, read_test_tile, &calls,
                                    pixels, 256, 100, 0) == -1);
    return 0;
}
