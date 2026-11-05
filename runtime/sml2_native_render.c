#include "sml2_native_render.h"

#include <string.h>

static int64_t render_floor_div(int64_t value, int64_t divisor)
{
    int64_t quotient = value / divisor;
    int64_t remainder = value % divisor;
    if (remainder < 0) --quotient;
    return quotient;
}

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
        int64_t world_y64 = render_floor_div(
            (int64_t)world->camera_y + (int64_t)y * SML2_NATIVE_FP_ONE,
            SML2_NATIVE_FP_ONE);
        int32_t world_y;
        memset(row, clear_color, (size_t)width);

        if (world_y64 < 0 || world_y64 >= stage_height) continue;
        world_y = (int32_t)world_y64;
        for (x = 0; x < width; ++x) {
            int64_t world_x64 = render_floor_div(
                (int64_t)world->camera_x + (int64_t)x * SML2_NATIVE_FP_ONE,
                SML2_NATIVE_FP_ONE);
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
            graphics_id = (uint8_t)stage->tiles[block_id].graphics[quadrant];
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
