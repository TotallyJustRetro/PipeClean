#include "sml2_native_capture.h"

#include <string.h>

#define SML2_MODE_ADDR 0xFF9Bu
#define SML2_BONUS_ADDR 0xA28Bu
#define SML2_CAMERA_X 0xFFCAu
#define SML2_CAMERA_Y 0xFFC8u
#define SML2_MAP_BASE 0xB000u
#define SML2_BLOCKDEF_BASE 0xA600u
#define SML2_NATIVE_SCREEN_WIDTH 160u
#define SML2_NATIVE_SCREEN_HEIGHT 144u
#define SML2_NATIVE_CAMERA_CENTRE_X 80
#define SML2_NATIVE_CAMERA_CENTRE_Y 72
#define SML2_NATIVE_HUD_HEIGHT 8u
#define SML2_NATIVE_SAMPLE_BOTTOM 136u
#define SML2_NATIVE_MAP_OFF 0x1800u
#define SML2_NATIVE_MAP_ALT_OFF 0x1C00u

static int align_to_scroll(int origin, uint8_t scroll)
{
    int delta = (int)(uint8_t)(scroll - (uint8_t)origin);
    if (delta > 127) delta -= 256;
    return origin + delta;
}

static unsigned read_word(Sml2NativeMemoryRead read_memory, void *userdata,
                          uint16_t address)
{
    unsigned lo = read_memory(userdata, address);
    unsigned hi = read_memory(userdata, (uint16_t)(address + 1u));
    return lo | (hi << 8);
}

static int capture_scene_valid(Sml2NativeMemoryRead read_memory,
                               void *userdata, uint8_t lcdc,
                               uint8_t wx, uint8_t wy)
{
    unsigned mode;
    if (lcdc != 0xE3u || wx != 7u || wy != 0x88u) return 0;
    mode = read_memory(userdata, SML2_MODE_ADDR);
    if (mode != 0x04u && mode != 0x08u && mode != 0x09u) return 0;
    return (read_memory(userdata, SML2_BONUS_ADDR) & 0xF0u) == 0;
}

static int validate_live_view(Sml2NativeSml2Capture *capture,
                              const uint8_t ppu_vram[0x2000],
                              unsigned min_samples,
                              unsigned min_match_percent,
                              unsigned camera_x, unsigned camera_y)
{
    unsigned first_x, first_y, map_off, sx, sy;
    int hits = 0, samples = 0;
    int origin_x = (int)camera_x - SML2_NATIVE_CAMERA_CENTRE_X;
    int origin_y = (int)camera_y - SML2_NATIVE_CAMERA_CENTRE_Y;

    origin_x = align_to_scroll(origin_x, capture->scx);
    origin_y = align_to_scroll(origin_y, capture->scy);
    capture->world_origin_x = origin_x;
    capture->world_origin_y = origin_y;

    first_x = (8u - (capture->scx & 7u)) & 7u;
    first_y = (8u - (capture->scy & 7u)) & 7u;
    map_off = (capture->lcdc & 0x08u)
        ? SML2_NATIVE_MAP_ALT_OFF : SML2_NATIVE_MAP_OFF;

    for (sy = first_y; sy < SML2_NATIVE_SAMPLE_BOTTOM; sy += 8u) {
        for (sx = first_x; sx < SML2_NATIVE_SCREEN_WIDTH; sx += 8u) {
            int world_x = origin_x + (int)sx;
            int world_y = origin_y + (int)sy;
            unsigned block_x, block_y, block_id, quadrant, expected_tile;
            unsigned map_index;
            if (world_x < 0 || world_y < 0 ||
                world_x >= SML2_NATIVE_SML2_STAGE_WIDTH_BLOCKS * 16 ||
                world_y >= SML2_NATIVE_SML2_STAGE_HEIGHT_BLOCKS * 16)
                continue;

            block_x = (unsigned)world_x >> 4;
            block_y = (unsigned)world_y >> 4;
            block_id = capture->cells[block_y *
                SML2_NATIVE_SML2_STAGE_WIDTH_BLOCKS + block_x];
            quadrant = (((unsigned)world_y >> 3) & 1u) * 2u |
                       (((unsigned)world_x >> 3) & 1u);
            expected_tile = capture->tiles[block_id].graphics[quadrant];
            map_index = map_off +
                ((((sy + capture->scy) >> 3) & 31u) * 32u) +
                (((sx + capture->scx) >> 3) & 31u);
            if (map_index >= 0x2000u) return 0;
            if (ppu_vram[map_index] == (uint8_t)expected_tile) hits++;
            samples++;
        }
    }

    capture->validation_samples = (unsigned)samples;
    capture->validation_matches = (unsigned)hits;
    if ((unsigned)samples < min_samples || samples <= 0) return 0;
    return (unsigned)(hits * 100) >=
           (unsigned)samples * min_match_percent;
}

int sml2_native_capture_live_sml2(
    Sml2NativeSml2Capture *capture,
    Sml2NativeMemoryRead read_memory, void *memory_userdata,
    const uint8_t ppu_vram[0x2000],
    uint8_t lcdc, uint8_t scx, uint8_t scy,
    uint8_t wx, uint8_t wy, uint8_t bgp,
    const uint8_t *collision_flags, size_t collision_flags_count,
    unsigned min_samples, unsigned min_match_percent)
{
    unsigned camera_x, camera_y;
    if (!capture || !read_memory || !ppu_vram ||
        min_samples == 0 || min_match_percent == 0 ||
        min_match_percent > 100 ||
        (collision_flags && collision_flags_count <
            SML2_NATIVE_SML2_BLOCK_COUNT) ||
        (!collision_flags && collision_flags_count != 0))
        return SML2_NATIVE_CAPTURE_INVALID;

    if (!capture_scene_valid(read_memory, memory_userdata, lcdc, wx, wy))
        return SML2_NATIVE_CAPTURE_WRONG_SCENE;

    if (sml2_native_stage_capture_sml2(
            &capture->stage, read_memory, memory_userdata,
            collision_flags, collision_flags_count,
            capture->cells, sizeof(capture->cells),
            capture->block_definitions, sizeof(capture->block_definitions),
            capture->tiles) != 0)
        return SML2_NATIVE_CAPTURE_BAD_STAGE;

    memcpy(capture->vram, ppu_vram, sizeof(capture->vram));
    capture->lcdc = lcdc;
    capture->scx = scx;
    capture->scy = scy;
    capture->bgp = bgp;
    capture->graphics.vram = capture->vram;
    capture->graphics.lcdc = lcdc;

    camera_x = read_word(read_memory, memory_userdata, SML2_CAMERA_X);
    camera_y = read_word(read_memory, memory_userdata, SML2_CAMERA_Y);
    if (!validate_live_view(capture, ppu_vram, min_samples,
                            min_match_percent, camera_x, camera_y))
        return SML2_NATIVE_CAPTURE_VIEW_MISMATCH;

    return SML2_NATIVE_CAPTURE_OK;
}

int sml2_native_apply_dmg_palette(uint8_t *pixels, size_t width, size_t height,
                                  size_t pitch, size_t capacity, uint8_t bgp)
{
    size_t y, x, required;
    if (!pixels || !width || !height || pitch < width ||
        pitch > (size_t)-1 / height)
        return -1;
    required = (height - 1u) * pitch + width;
    if (capacity < required) return -1;
    /* Validate the complete input first so malformed pixels never leave a
       half-converted framebuffer behind. */
    for (y = 0; y < height; ++y) {
        const uint8_t *row = pixels + y * pitch;
        for (x = 0; x < width; ++x)
            if (row[x] > 3u) return -1;
    }
    for (y = 0; y < height; ++y) {
        uint8_t *row = pixels + y * pitch;
        for (x = 0; x < width; ++x)
            row[x] = (uint8_t)((bgp >> (row[x] * 2u)) & 3u);
    }
    return 0;
}
