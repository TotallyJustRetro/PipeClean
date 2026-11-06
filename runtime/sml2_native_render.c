#include "sml2_native_render.h"

#include <string.h>

int sml2_native_read_gb_tile_pixels(
    void *userdata, uint8_t graphics_id, uint8_t pixels_out[64])
{
    const Sml2NativeGbGraphics *graphics =
        (const Sml2NativeGbGraphics *)userdata;
    int address;
    unsigned y, x;

    if (!graphics || !graphics->vram || !pixels_out) return -1;
    address = (graphics->lcdc & 0x10)
        ? (int)graphics_id * 16
        : 0x1000 + (int)(int8_t)graphics_id * 16;
    if (address < 0 || address + 16 > 0x2000) return -1;

    for (y = 0; y < 8; ++y) {
        uint8_t low = graphics->vram[address + (int)y * 2];
        uint8_t high = graphics->vram[address + (int)y * 2 + 1];
        for (x = 0; x < 8; ++x) {
            unsigned bit = 7u - x;
            pixels_out[y * 8u + x] =
                (uint8_t)((((high >> bit) & 1u) << 1) |
                          ((low >> bit) & 1u));
        }
    }
    return 0;
}

int sml2_native_draw_sprite(
    uint8_t *pixels, size_t pitch, size_t width, size_t height,
    const Sml2NativeSprite *sprite,
    Sml2NativeReadTilePixels read_tile, void *userdata)
{
    uint8_t tile_pixels[2][64];
    unsigned sprite_height, tile_count, tile_base;
    unsigned tile_index, i, x, y;
    size_t required;

    if (!pixels || !sprite || !read_tile || width == 0 || height == 0 ||
        width > 32767u || height > 32767u || pitch < width ||
        pitch > (size_t)-1 / height)
        return -1;
    required = (height - 1u) * pitch + width;
    if (required < width || (height - 1u) * pitch > (size_t)-1 - width)
        return -1;
    for (i = 0; i < 4; ++i)
        if (sprite->palette[i] > 3) return -1;

    sprite_height = (sprite->flags & SML2_NATIVE_SPRITE_8X16) ? 16u : 8u;
    tile_count = sprite_height / 8u;
    tile_base = sprite->tile_id;
    if (tile_count == 2u) tile_base &= ~1u;

    for (tile_index = 0; tile_index < tile_count; ++tile_index) {
        if (read_tile(userdata, (uint8_t)(tile_base + tile_index),
                      tile_pixels[tile_index]) != 0)
            return -1;
        for (i = 0; i < 64; ++i)
            if (tile_pixels[tile_index][i] > 3) return -1;
    }

    for (y = 0; y < sprite_height; ++y) {
        int64_t dest_y = (int64_t)sprite->y + y;
        unsigned source_y = (sprite->flags & SML2_NATIVE_SPRITE_FLIP_Y)
                          ? sprite_height - 1u - y : y;
        unsigned source_tile = source_y >> 3;
        unsigned tile_y = source_y & 7u;
        uint8_t *row;
        if (dest_y < 0 || dest_y >= (int64_t)height) continue;
        row = pixels + (size_t)dest_y * pitch;
        for (x = 0; x < 8; ++x) {
            int64_t dest_x = (int64_t)sprite->x + x;
            unsigned source_x = (sprite->flags & SML2_NATIVE_SPRITE_FLIP_X)
                              ? 7u - x : x;
            uint8_t color;
            if (dest_x < 0 || dest_x >= (int64_t)width) continue;
            color = tile_pixels[source_tile][tile_y * 8u + source_x];
            if (color != 0)
                row[(size_t)dest_x] = sprite->palette[color];
        }
    }
    return 0;
}

int sml2_native_render_stage(
    const Sml2NativeStage *stage, const Sml2NativeWorld *world,
    Sml2NativeReadTilePixels read_tile, void *userdata,
    uint8_t *pixels, size_t pitch, size_t pixels_capacity,
    uint8_t clear_color)
{
    uint8_t tile_cache[256][64];
    uint8_t tile_loaded[256] = {0};
    size_t required;
    int32_t width, height;
    int32_t x, y;
    int32_t stage_width, stage_height;

    if (!stage || !world || !read_tile || !pixels || !stage->cells ||
        !stage->tiles || !stage->width_blocks || !stage->height_blocks ||
        !stage->tile_count || clear_color > 3 ||
        world->camera_view_width <= 0 || world->camera_view_height <= 0)
        return -1;

    width = world->camera_view_width;
    height = world->camera_view_height;
    if (width > 32767 || height > 32767 || pitch < (size_t)width ||
        pitch > (size_t)-1 / (size_t)height)
        return -1;
    required = (size_t)(height - 1) * pitch + (size_t)width;
    if (pixels_capacity < required) return -1;

    stage_width = (int32_t)stage->width_blocks * 16;
    stage_height = (int32_t)stage->height_blocks * 16;
    for (y = 0; y < height; ++y) {
        uint8_t *row = pixels + (size_t)y * pitch;
        int64_t world_y64 = (int64_t)world->camera_y + y;
        int32_t world_y;
        memset(row, clear_color, (size_t)width);

        if (world_y64 < 0 || world_y64 >= stage_height) continue;
        world_y = (int32_t)world_y64;
        for (x = 0; x < width; ++x) {
            int64_t world_x64 = (int64_t)world->camera_x + x;
            int32_t world_x;
            uint32_t cell_index;
            uint8_t block_id, quadrant, graphics_id;
            unsigned tile_x, tile_y;
            if (world_x64 < 0 || world_x64 >= stage_width) continue;
            world_x = (int32_t)world_x64;

            cell_index = (uint32_t)(world_y >> 4) * stage->width_blocks +
                         (uint32_t)(world_x >> 4);
            block_id = stage->cells[cell_index];
            if (block_id >= stage->tile_count) return -1;

            quadrant = (uint8_t)(((world_y & 8) ? 2 : 0) |
                                 ((world_x & 8) ? 1 : 0));
            graphics_id = stage->tiles[block_id].graphics[quadrant];
            if (!tile_loaded[graphics_id]) {
                unsigned p;
                if (read_tile(userdata, graphics_id, tile_cache[graphics_id]) != 0)
                    return -1;
                for (p = 0; p < 64; ++p)
                    if (tile_cache[graphics_id][p] > 3) return -1;
                tile_loaded[graphics_id] = 1;
            }

            tile_x = (unsigned)world_x & 7u;
            tile_y = (unsigned)world_y & 7u;
            row[x] = tile_cache[graphics_id][tile_y * 8u + tile_x];
        }
    }
    return 0;
}
