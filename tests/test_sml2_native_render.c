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
    world.camera_x = 32 * SML2_NATIVE_FP_ONE;
    world.camera_y = 16 * SML2_NATIVE_FP_ONE;
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
    world.camera_x = 100 * SML2_NATIVE_FP_ONE;
    assert(sml2_native_render_stage(&stage, &world, read_test_tile, &calls,
                                    pixels, 256, sizeof(pixels), 2) == 0);
    assert(pixels[220] == 2);

    /* Enlarged views may extend beyond the left/top level edge. Those pixels
       are cleared while valid world pixels remain correctly sampled. */
    world.camera_x = -8 * SML2_NATIVE_FP_ONE;
    world.camera_y = -4 * SML2_NATIVE_FP_ONE;
    assert(sml2_native_render_stage(&stage, &world, read_test_tile, &calls,
                                    pixels, 256, sizeof(pixels), 2) == 0);
    assert(pixels[0] == 2);
    assert(pixels[8 * 256 + 8] == 1);

    /* Fixed-point camera sampling floors fractional and negative origins. */
    world.camera_x = 32 * SML2_NATIVE_FP_ONE +
                     SML2_NATIVE_FP_ONE / 2;
    world.camera_y = 16 * SML2_NATIVE_FP_ONE +
                     SML2_NATIVE_FP_ONE / 2;
    assert(sml2_native_render_stage(&stage, &world, read_test_tile, &calls,
                                    pixels, 256, sizeof(pixels), 2) == 0);
    assert(pixels[0] == 1);
    assert(pixels[8] == 2);
    world.camera_x = -SML2_NATIVE_FP_ONE / 2;
    world.camera_y = -SML2_NATIVE_FP_ONE / 2;
    assert(sml2_native_render_stage(&stage, &world, read_test_tile, &calls,
                                    pixels, 256, sizeof(pixels), 2) == 0);
    assert(pixels[0] == 2);
    assert(pixels[256 + 1] == 1);

    /* A short pitch/capacity or an invalid palette index fails safely. */
    assert(sml2_native_render_stage(&stage, &world, read_test_tile, &calls,
                                    pixels, 255, sizeof(pixels), 0) == -1);
    for (i = 0; i < sizeof(pixels); ++i) pixels[i] = 0;
    assert(sml2_native_render_stage(&stage, &world, read_test_tile, &calls,
                                    pixels, 256, 100, 0) == -1);
    return 0;
}
