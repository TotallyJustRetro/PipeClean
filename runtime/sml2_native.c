#include "sml2_native.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define FP_ONE ((int64_t)SML2_NATIVE_FP_ONE)

static int32_t clamp_i64(int64_t value)
{
    if (value > INT32_MAX) return INT32_MAX;
    if (value < INT32_MIN) return INT32_MIN;
    return (int32_t)value;
}

static int32_t clamp_i32(int32_t value, int32_t low, int32_t high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

int32_t sml2_native_fp_from_int(int32_t pixels)
{
    return clamp_i64((int64_t)pixels * FP_ONE);
}

int32_t sml2_native_fp_to_int(int32_t value)
{
    /* C integer division rounds toward zero, which is useful for negative
       coordinates near the left/top world boundary. */
    return (int32_t)((int64_t)value / FP_ONE);
}

static int zoom_valid(uint8_t zoom)
{
    return zoom == SML2_NATIVE_ZOOM_100 ||
           zoom == SML2_NATIVE_ZOOM_125 ||
           zoom == SML2_NATIVE_ZOOM_150 ||
           zoom == SML2_NATIVE_ZOOM_200;
}

int sml2_native_view_size(uint8_t mode, uint8_t zoom_percent,
                          uint16_t window_width, uint16_t window_height,
                          int32_t *view_width, int32_t *view_height)
{
    int64_t width, height, native_width;
    if (!view_width || !view_height || !zoom_valid(zoom_percent) ||
        mode > SML2_NATIVE_WIDESCREEN_WINDOW)
        return -1;

    height = (144LL * zoom_percent) / 100;
    native_width = (height * 10) / 9;
    width = native_width;

    switch (mode) {
    case SML2_NATIVE_WIDESCREEN_OFF:
        break;
    case SML2_NATIVE_WIDESCREEN_16_9:
        width = (height * 16) / 9;
        break;
    case SML2_NATIVE_WIDESCREEN_21_9:
        width = (height * 21) / 9;
        break;
    case SML2_NATIVE_WIDESCREEN_WINDOW:
        if (window_width != 0 && window_height != 0) {
            if ((int64_t)window_width * height >=
                (int64_t)window_height * native_width) {
                width = ((int64_t)window_width * height) / window_height;
            } else {
                width = native_width;
                height = ((int64_t)native_width * window_height) / window_width;
            }
        }
        break;
    default:
        return -1;
    }

    if (width < 1 || height < 1 || width > 32767 || height > 32767)
        return -1;
    *view_width = (int32_t)width;
    *view_height = (int32_t)height;
    return 0;
}

static int config_valid(const Sml2NativeConfig *c)
{
    int32_t view_width, view_height;
    return c &&
           c->level_width > 0 && c->level_width <= 32767 &&
           c->level_height > 0 && c->level_height <= 32767 &&
           c->gravity >= 0 &&
           c->horizontal_acceleration > 0 &&
           c->horizontal_friction >= 0 &&
           c->max_run_speed > 0 &&
           c->jump_velocity < 0 &&
           c->terminal_velocity > 0 &&
           c->player_width > 0 && c->player_width <= 32767 &&
           c->player_height > 0 && c->player_height <= 32767 &&
           c->camera_padding >= 0 && c->camera_padding <= 32767 &&
           zoom_valid(c->camera_zoom_percent) &&
           zoom_valid(c->max_camera_zoom_percent) &&
           c->max_camera_zoom_percent >= c->camera_zoom_percent &&
           c->auto_zoom <= 1 &&
           sml2_native_view_size(c->widescreen_mode, c->camera_zoom_percent,
                                 c->window_width, c->window_height,
                                 &view_width, &view_height) == 0 &&
           sml2_native_view_size(c->widescreen_mode, c->max_camera_zoom_percent,
                                 c->window_width, c->window_height,
                                 &view_width, &view_height) == 0;
}

int sml2_native_set_view(Sml2NativeWorld *world, uint8_t widescreen_mode,
                         uint8_t zoom_percent, uint16_t window_width,
                         uint16_t window_height, int auto_zoom)
{
    int32_t view_width, view_height;
    if (!world || widescreen_mode > SML2_NATIVE_WIDESCREEN_WINDOW ||
        !zoom_valid(zoom_percent) ||
        zoom_percent > world->config.max_camera_zoom_percent ||
        (auto_zoom != 0 && auto_zoom != 1))
        return -1;
    if (sml2_native_view_size(widescreen_mode, zoom_percent,
                              window_width, window_height,
                              &view_width, &view_height) != 0)
        return -1;
    world->config.widescreen_mode = widescreen_mode;
    world->config.camera_zoom_percent = zoom_percent;
    world->config.window_width = window_width;
    world->config.window_height = window_height;
    world->config.auto_zoom = (uint8_t)auto_zoom;
    return 0;
}

int sml2_native_init(Sml2NativeWorld *world, const Sml2NativeConfig *config)
{
    if (!world || !config_valid(config)) return -1;
    memset(world, 0, sizeof(*world));
    world->config = *config;
    world->camera_region.left = 0;
    world->camera_region.top = 0;
    world->camera_region.right = config->level_width;
    world->camera_region.bottom = config->level_height;
    if (sml2_native_view_size(config->widescreen_mode,
                              config->camera_zoom_percent,
                              config->window_width, config->window_height,
                              &world->camera_view_width,
                              &world->camera_view_height) != 0)
        return -1;
    world->camera_zoom_percent = config->camera_zoom_percent;
    return 0;
}

int sml2_native_set_camera_region(Sml2NativeWorld *world,
                                  const Sml2NativeCameraRegion *region)
{
    Sml2NativeCameraRegion next;
    if (!world) return -1;
    if (!region) {
        next.left = 0;
        next.top = 0;
        next.right = world->config.level_width;
        next.bottom = world->config.level_height;
    } else {
        next = *region;
    }
    if (next.left < 0 || next.top < 0 ||
        next.right <= next.left || next.bottom <= next.top ||
        next.right > world->config.level_width ||
        next.bottom > world->config.level_height)
        return -1;
    world->camera_region = next;
    return 0;
}

int sml2_native_spawn_player(Sml2NativeWorld *world, unsigned player_index,
                             int32_t x_pixels, int32_t y_pixels,
                             uint8_t lives)
{
    Sml2NativePlayer *p;
    if (!world || player_index >= SML2_NATIVE_MAX_PLAYERS) return -1;

    p = &world->players[player_index];
    memset(p, 0, sizeof(*p));
    p->x = sml2_native_fp_from_int(x_pixels);
    p->y = sml2_native_fp_from_int(y_pixels);
    p->width = sml2_native_fp_from_int(world->config.player_width);
    p->height = sml2_native_fp_from_int(world->config.player_height);
    p->lives = lives;
    p->active = 1;
    p->facing_right = 1;

    if (world->player_count <= player_index)
        world->player_count = (uint8_t)(player_index + 1u);
    return 0;
}

static Sml2NativeRect player_rect(const Sml2NativePlayer *p,
                                  int32_t x, int32_t y)
{
    Sml2NativeRect r;
    r.left = x;
    r.top = y;
    r.right = clamp_i64((int64_t)x + p->width);
    r.bottom = clamp_i64((int64_t)y + p->height);
    return r;
}

static int rect_is_solid(Sml2NativeCollisionQuery query, void *userdata,
                         const Sml2NativePlayer *p,
                         int32_t x, int32_t y)
{
    Sml2NativeRect rect;
    if (!query) return 0;
    rect = player_rect(p, x, y);
    return (query(userdata, &rect) & SML2_NATIVE_COLLISION_SOLID) != 0;
}

/* Move in <=1-pixel samples so ordinary frame velocities cannot tunnel
 * through one-pixel collision features. Larger scripted displacements are
 * still bounded to 4096 samples to prevent hostile input from stalling. */
static void move_horizontal(Sml2NativePlayer *p, int32_t delta,
                            Sml2NativeCollisionQuery query, void *userdata)
{
    int64_t magnitude = delta < 0 ? -(int64_t)delta : (int64_t)delta;
    int64_t steps = (magnitude + FP_ONE - 1) / FP_ONE;
    int32_t start = p->x;
    int32_t last = start;
    int64_t i;

    if (steps <= 0) return;
    if (steps > 4096) steps = 4096;

    for (i = 1; i <= steps; ++i) {
        int32_t candidate = clamp_i64((int64_t)start +
                                      ((int64_t)delta * i) / steps);
        if (rect_is_solid(query, userdata, p, candidate, p->y)) {
            p->x = last;
            p->vx = 0;
            return;
        }
        last = candidate;
    }
    p->x = last;
}

static void move_vertical(Sml2NativePlayer *p, int32_t delta,
                          Sml2NativeCollisionQuery query, void *userdata)
{
    int64_t magnitude = delta < 0 ? -(int64_t)delta : (int64_t)delta;
    int64_t steps = (magnitude + FP_ONE - 1) / FP_ONE;
    int32_t start = p->y;
    int32_t last = start;
    int64_t i;

    p->grounded = 0;
    if (steps <= 0) return;
    if (steps > 4096) steps = 4096;

    for (i = 1; i <= steps; ++i) {
        int32_t candidate = clamp_i64((int64_t)start +
                                      ((int64_t)delta * i) / steps);
        if (rect_is_solid(query, userdata, p, p->x, candidate)) {
            p->y = last;
            if (delta > 0) p->grounded = 1;
            p->vy = 0;
            return;
        }
        last = candidate;
    }
    p->y = last;
}

static void update_player(Sml2NativeWorld *world, Sml2NativePlayer *p,
                          const Sml2NativeInput *input,
                          Sml2NativeCollisionQuery query, void *userdata)
{
    uint8_t buttons = input ? input->buttons : 0;
    int left = (buttons & SML2_NATIVE_BUTTON_LEFT) != 0;
    int right = (buttons & SML2_NATIVE_BUTTON_RIGHT) != 0;
    int jump_down = (buttons & SML2_NATIVE_BUTTON_JUMP) != 0;
    int jump_pressed = jump_down && !p->jump_was_down;
    const Sml2NativeConfig *c = &world->config;

    if (!p->active) return;

    if (left != right) {
        int32_t accel = left ? -c->horizontal_acceleration
                             : c->horizontal_acceleration;
        p->vx = clamp_i64((int64_t)p->vx + accel);
        p->vx = clamp_i32(p->vx, -c->max_run_speed, c->max_run_speed);
        p->facing_right = (uint8_t)!left;
    } else if (p->vx > 0) {
        p->vx = p->vx > c->horizontal_friction
              ? p->vx - c->horizontal_friction : 0;
    } else if (p->vx < 0) {
        p->vx = p->vx < -c->horizontal_friction
              ? p->vx + c->horizontal_friction : 0;
    }

    if (jump_pressed && p->grounded)
        p->vy = c->jump_velocity;

    p->jump_was_down = (uint8_t)jump_down;

    move_horizontal(p, p->vx, query, userdata);

    if (p->vy < c->terminal_velocity)
        p->vy = clamp_i64((int64_t)p->vy + c->gravity);
    if (p->vy > c->terminal_velocity)
        p->vy = c->terminal_velocity;
    move_vertical(p, p->vy, query, userdata);

    if (p->invulnerability_ticks > 0)
        p->invulnerability_ticks--;
}

static int64_t floor_div_i64(int64_t value, int64_t divisor)
{
    int64_t quotient = value / divisor;
    int64_t remainder = value % divisor;
    if (remainder < 0) quotient--;
    return quotient;
}

int sml2_native_rom_world_cell(uint16_t world_x, uint16_t world_y,
                               uint16_t *block_x, uint16_t *block_y)
{
    uint16_t x, y;
    if (!block_x || !block_y) return -1;

    /* The original code keeps only the low nibble of each high byte. */
    x = (uint16_t)((world_x & 0x0FFFu) >> 4);
    y = (uint16_t)((world_y & 0x0FFFu) >> 4);

    /* $B000 + row*$100 enters the WRAM echo for rows $30-$4D.
       The echo aliases $C000-$DDFF, i.e. rows $10-$2D. */
    if (y >= 0x30u && y < 0x4Eu)
        y = (uint16_t)(y - 0x20u);

    *block_x = x;
    *block_y = y;
    return y < 48u ? 0 : 1;
}

int sml2_native_rom_probe_tile(const Sml2NativeTileMap *map,
                               uint16_t sample_x, uint16_t sample_y,
                               uint8_t *tile_id)
{
    uint16_t block_x, block_y;
    uint16_t lookup_x, lookup_y;
    int mapped;
    if (!map || !map->block_ids || !tile_id ||
        map->width_blocks != 256u || map->height_blocks != 48u)
        return -1;

    /* $1EFA: $B000 + ((y-16)>>4)*$100 + ((x-8)>>4). */
    lookup_x = (uint16_t)(sample_x - 8u);
    lookup_y = (uint16_t)(sample_y - 16u);
    mapped = sml2_native_rom_world_cell(lookup_x, lookup_y,
                                        &block_x, &block_y);
    if (mapped < 0) return -1;
    if (mapped > 0) {
        *tile_id = 0;
        return 0;
    }
    *tile_id = map->block_ids[(size_t)block_y * 256u + block_x];
    return 0;
}

int sml2_native_rom_floor_probe(const Sml2NativeTileMap *map,
                                uint16_t player_x, uint16_t player_y,
                                uint8_t *tile_id)
{
    uint8_t tile;
    if (!tile_id) return -1;
    /* $1E3B's fractional-Y gate: (y-4)&$0F must be below five. */
    if (((uint16_t)(player_y - 4u) & 0x0Fu) >= 5u) return 0;
    if (sml2_native_rom_probe_tile(map,
            (uint16_t)(player_x + 12u),
            (uint16_t)(player_y + 44u), &tile) != 0)
        return -1;
    if (tile < 0x48u) {
        *tile_id = tile;
        return 1;
    }
    if (sml2_native_rom_probe_tile(map,
            (uint16_t)(player_x + 20u),
            (uint16_t)(player_y + 44u), &tile) != 0)
        return -1;
    if (tile < 0x48u) {
        *tile_id = tile;
        return 1;
    }
    return 0;
}

int sml2_native_rom_ceiling_probe(const Sml2NativeTileMap *map,
                                  uint16_t player_x, uint16_t player_y,
                                  uint8_t powerup, uint8_t *tile_id)
{
    uint8_t tile;
    uint16_t dy = powerup ? 18u : 28u;
    if (!tile_id ||
        sml2_native_rom_probe_tile(map,
            (uint16_t)(player_x + 16u),
            (uint16_t)(player_y + dy), &tile) != 0)
        return -1;
    if (tile == 0x7Eu || tile < 0x38u) {
        *tile_id = tile;
        return 1;
    }
    return 0;
}

int sml2_native_rom_collect_coin(uint8_t *layout_cells, size_t cell_count,
                                 uint16_t sample_x, uint16_t sample_y,
                                 uint8_t sampled_tile,
                                 uint8_t *replacement_tile)
{
    uint16_t block_x, block_y;
    size_t index;
    uint8_t replacement;
    if (!layout_cells || !replacement_tile ||
        cell_count < (size_t)256u * 48u)
        return -1;
    if ((sampled_tile & 0xFEu) != 0x48u) return 0;

    if (sml2_native_rom_world_cell(
            (uint16_t)(sample_x - 8u),
            (uint16_t)(sample_y - 16u),
            &block_x, &block_y) != 0)
        return 0;
    index = (size_t)block_y * 256u + block_x;
    if (layout_cells[index] != sampled_tile) return 0;

    replacement = sampled_tile == 0x48u ? 0x60u : 0x7Fu;
    layout_cells[index] = replacement;
    *replacement_tile = replacement;
    return 1;
}

int sml2_native_rom_environment_probe(const Sml2NativeTileMap *map,
                                      uint16_t player_x, uint16_t player_y,
                                      uint8_t powerup, uint8_t ducking,
                                      uint8_t *environment_flags,
                                      uint8_t *sampled_tile)
{
    uint8_t tile, hazard_y;
    if (!environment_flags || !sampled_tile) return -1;
    *environment_flags = 0;

    /* $1C39/$1C4E: big Mario checks y+22 first, then y+28 on a miss. */
    hazard_y = (powerup && !ducking) ? 22u : 28u;
    if (sml2_native_rom_probe_tile(map,
            (uint16_t)(player_x + 16u),
            (uint16_t)(player_y + hazard_y), &tile) != 0)
        return -1;
    if ((tile & 0xFEu) == 0x50u) {
        *environment_flags = SML2_NATIVE_ROM_ENV_HAZARD;
        *sampled_tile = tile;
        return 0;
    }
    if (hazard_y == 22u) {
        if (sml2_native_rom_probe_tile(map,
                (uint16_t)(player_x + 16u),
                (uint16_t)(player_y + 28u), &tile) != 0)
            return -1;
        if ((tile & 0xFEu) == 0x50u) {
            *environment_flags = SML2_NATIVE_ROM_ENV_HAZARD;
            *sampled_tile = tile;
            return 0;
        }
    }

    /* The final y+28 sample selects water, exit, then moon physics. */
    if (sml2_native_rom_probe_tile(map,
            (uint16_t)(player_x + 16u),
            (uint16_t)(player_y + 28u), &tile) != 0)
        return -1;
    *sampled_tile = tile;
    if ((tile & 0xFCu) == 0x4Cu)
        *environment_flags = SML2_NATIVE_ROM_ENV_WATER;
    else if ((tile & 0xFEu) == 0x4Au)
        *environment_flags = SML2_NATIVE_ROM_ENV_EXIT;
    else if (tile >= 0x52u && tile <= 0x57u)
        *environment_flags = SML2_NATIVE_ROM_ENV_MOON;
    return 0;
}

int sml2_native_rom_wall_probe(const Sml2NativeTileMap *map,
                               uint16_t player_x, uint16_t player_y,
                               uint8_t probe_side, uint8_t powerup,
                               uint8_t ducking, uint8_t *tile_id)
{
    uint8_t tile;
    uint16_t probe_x;
    if (!tile_id || probe_side > 1u) return -1;
    probe_x = (uint16_t)(player_x + (probe_side ? 20u : 11u));

    if (powerup && !ducking) {
        if (sml2_native_rom_probe_tile(map, probe_x,
                (uint16_t)(player_y + 20u), &tile) != 0)
            return -1;
        if (tile < 0x38u) {
            *tile_id = tile;
            return 1;
        }
    }
    if (sml2_native_rom_probe_tile(map, probe_x,
            (uint16_t)(player_y + 32u), &tile) != 0)
        return -1;
    if (tile < 0x38u) {
        *tile_id = tile;
        return 1;
    }
    if (sml2_native_rom_probe_tile(map, probe_x,
            (uint16_t)(player_y + 40u), &tile) != 0)
        return -1;
    if (tile < 0x38u) {
        *tile_id = tile;
        return 1;
    }
    return 0;
}

static uint32_t native_map_cell_flags(const Sml2NativeTileMap *map,
                                      int64_t x, int64_t y,
                                      int solid_outside)
{
    size_t index;
    if (x < 0 || y < 0 ||
        x >= map->width_blocks || y >= map->height_blocks)
        return solid_outside ? SML2_NATIVE_COLLISION_SOLID
                             : SML2_NATIVE_COLLISION_NONE;
    index = (size_t)y * map->width_blocks + (size_t)x;
    return map->block_flags[map->block_ids[index]];
}

uint32_t sml2_native_rom_level_collision(void *userdata,
                                         const Sml2NativeRect *world_rect)
{
    const Sml2NativeTileMap *map = (const Sml2NativeTileMap *)userdata;
    const int64_t block_fp = 16LL * FP_ONE;
    int64_t first_x, last_x, first_y, last_y, x, y;
    uint32_t result = SML2_NATIVE_COLLISION_NONE;

    if (!map || !world_rect || !map->block_ids ||
        map->width_blocks != 256u || map->height_blocks != 48u ||
        world_rect->left >= world_rect->right ||
        world_rect->top >= world_rect->bottom)
        return SML2_NATIVE_COLLISION_SOLID;

    first_x = floor_div_i64(world_rect->left, block_fp);
    last_x = floor_div_i64((int64_t)world_rect->right - 1, block_fp);
    first_y = floor_div_i64(world_rect->top, block_fp);
    last_y = floor_div_i64((int64_t)world_rect->bottom - 1, block_fp);

    if (last_x - first_x > 256 || last_y - first_y > 256 ||
        (last_x - first_x + 1) * (last_y - first_y + 1) > 4096)
        return SML2_NATIVE_COLLISION_SOLID;

    for (y = first_y; y <= last_y; ++y) {
        /* Wrapping cell coordinates is equivalent to masking the 16-bit
           pixel high byte with $0F before selecting a 16-pixel cell. */
        uint16_t row = (uint16_t)((uint64_t)y & 0xFFu);
        if (row >= 0x30u && row < 0x4Eu)
            row = (uint16_t)(row - 0x20u);
        for (x = first_x; x <= last_x; ++x) {
            uint16_t column = (uint16_t)((uint64_t)x & 0xFFu);
            if (row >= map->height_blocks) {
                if (map->solid_out_of_bounds)
                    result |= SML2_NATIVE_COLLISION_SOLID;
                continue;
            }
            result |= native_map_cell_flags(map, column, row,
                                             map->solid_out_of_bounds);
        }
    }
    return result;
}

uint32_t sml2_native_level_collision(void *userdata,
                                     const Sml2NativeRect *world_rect)
{
    const Sml2NativeTileMap *map = (const Sml2NativeTileMap *)userdata;
    const int64_t block_fp = 16LL * FP_ONE;
    int64_t first_x, last_x, first_y, last_y;
    int64_t x, y;
    uint32_t result = SML2_NATIVE_COLLISION_NONE;

    if (!map || !world_rect || !map->block_ids ||
        map->width_blocks == 0 || map->height_blocks == 0 ||
        world_rect->left >= world_rect->right ||
        world_rect->top >= world_rect->bottom)
        return SML2_NATIVE_COLLISION_SOLID;

    first_x = floor_div_i64(world_rect->left, block_fp);
    last_x = floor_div_i64((int64_t)world_rect->right - 1, block_fp);
    first_y = floor_div_i64(world_rect->top, block_fp);
    last_y = floor_div_i64((int64_t)world_rect->bottom - 1, block_fp);

    /* Bound worst-case work for malformed/extreme rectangles. Normal player
       bounds touch only a handful of cells; enormous queries fail closed. */
    if (last_x - first_x > 256 || last_y - first_y > 256 ||
        (last_x - first_x + 1) * (last_y - first_y + 1) > 4096)
        return SML2_NATIVE_COLLISION_SOLID;

    for (y = first_y; y <= last_y; ++y) {
        for (x = first_x; x <= last_x; ++x) {
            uint8_t block_id;
            size_t index;
            if (x < 0 || y < 0 ||
                x >= map->width_blocks || y >= map->height_blocks) {
                if (map->solid_out_of_bounds)
                    result |= SML2_NATIVE_COLLISION_SOLID;
                continue;
            }
            index = (size_t)y * map->width_blocks + (size_t)x;
            block_id = map->block_ids[index];
            result |= map->block_flags[block_id];
        }
    }
    return result;
}

static int32_t camera_origin_axis(int32_t center_fp,
                                   int32_t view_pixels,
                                   int32_t world_pixels,
                                   int32_t region_start,
                                   int32_t region_end,
                                   int vertical_bias_pixels)
{
    int64_t view_fp = (int64_t)view_pixels * FP_ONE;
    int64_t world_fp = (int64_t)world_pixels * FP_ONE;
    int64_t lo_fp = (int64_t)region_start * FP_ONE;
    int64_t hi_fp = (int64_t)region_end * FP_ONE;
    int64_t origin = (int64_t)center_fp - view_fp / 2 -
                     (int64_t)vertical_bias_pixels * FP_ONE;

    if (lo_fp < 0) lo_fp = 0;
    if (hi_fp > world_fp) hi_fp = world_fp;
    if (hi_fp < lo_fp) hi_fp = lo_fp;

    if (hi_fp - lo_fp >= view_fp) {
        int64_t maximum = hi_fp - view_fp;
        if (origin < lo_fp) origin = lo_fp;
        if (origin > maximum) origin = maximum;
    } else {
        /* A view wider than its room is centred on that room, then clamped
           to the physical level. A view wider than the entire level starts
           at zero so it never generates negative world coordinates. */
        int64_t maximum = world_fp > view_fp ? world_fp - view_fp : 0;
        origin = (lo_fp + hi_fp - view_fp) / 2;
        if (origin < 0) origin = 0;
        if (origin > maximum) origin = maximum;
    }
    return clamp_i64(origin);
}

static void update_camera(Sml2NativeWorld *world)
{
    int32_t min_x = INT32_MAX, min_y = INT32_MAX;
    int32_t max_x = INT32_MIN, max_y = INT32_MIN;
    int found = 0;
    int32_t view_width = 160, view_height = 144;
    int32_t center_x = 0, center_y = 0;
    uint8_t selected_zoom = world->config.camera_zoom_percent;
    unsigned active_count = 0;
    unsigned i;

    for (i = 0; i < SML2_NATIVE_MAX_PLAYERS; ++i) {
        const Sml2NativePlayer *p = &world->players[i];
        int32_t right, bottom;
        if (!p->active) continue;
        right = clamp_i64((int64_t)p->x + p->width);
        bottom = clamp_i64((int64_t)p->y + p->height);
        if (p->x < min_x) min_x = p->x;
        if (p->y < min_y) min_y = p->y;
        if (right > max_x) max_x = right;
        if (bottom > max_y) max_y = bottom;
        found = 1;
        active_count++;
    }

    if (!found) {
        (void)sml2_native_view_size(world->config.widescreen_mode,
                                    world->config.camera_zoom_percent,
                                    world->config.window_width,
                                    world->config.window_height,
                                    &world->camera_view_width,
                                    &world->camera_view_height);
        world->camera_zoom_percent = world->config.camera_zoom_percent;
        world->camera_x = 0;
        world->camera_y = 0;
        return;
    }

    center_x = clamp_i64(((int64_t)min_x + max_x) / 2);
    center_y = clamp_i64(((int64_t)min_y + max_y) / 2);

    /* Auto zoom selects the least zoomed-out preset that frames every active
       player plus the configured breathing room. The display aspect ratio is
       preserved at every step; zoom is never implemented by stretching. */
    if (world->config.auto_zoom && active_count > 1) {
        int32_t required_width = clamp_i64(
            ((int64_t)max_x - min_x) / FP_ONE +
            2LL * world->config.camera_padding);
        int32_t required_height = clamp_i64(
            ((int64_t)max_y - min_y) / FP_ONE +
            2LL * world->config.camera_padding);
        static const uint8_t zoom_steps[] = {
            SML2_NATIVE_ZOOM_100, SML2_NATIVE_ZOOM_125,
            SML2_NATIVE_ZOOM_150, SML2_NATIVE_ZOOM_200
        };
        unsigned z;
        for (z = 0; z < sizeof(zoom_steps) / sizeof(zoom_steps[0]); ++z) {
            int32_t candidate_width, candidate_height;
            uint8_t zoom = zoom_steps[z];
            if (zoom < world->config.camera_zoom_percent ||
                zoom > world->config.max_camera_zoom_percent)
                continue;
            if (sml2_native_view_size(world->config.widescreen_mode, zoom,
                                      world->config.window_width,
                                      world->config.window_height,
                                      &candidate_width, &candidate_height) != 0)
                continue;
            selected_zoom = zoom;
            view_width = candidate_width;
            view_height = candidate_height;
            if (candidate_width >= required_width &&
                candidate_height >= required_height)
                break;
        }
    }

    if (sml2_native_view_size(world->config.widescreen_mode, selected_zoom,
                              world->config.window_width,
                              world->config.window_height,
                              &view_width, &view_height) != 0)
        return;

    world->camera_view_width = view_width;
    world->camera_view_height = view_height;
    world->camera_zoom_percent = selected_zoom;

    /* Match the reference view's level framing: when zoomed out, reveal
       three quarters of the added vertical space above the native playfield
       and one quarter below. This is a presentation bias, not physics. */
    {
        int32_t level_bias = view_height > 144
                           ? (view_height - 144) / 4 : 0;
        world->camera_x = camera_origin_axis(center_x, view_width,
                                             world->config.level_width,
                                             world->camera_region.left,
                                             world->camera_region.right, 0);
        world->camera_y = camera_origin_axis(center_y, view_height,
                                             world->config.level_height,
                                             world->camera_region.top,
                                             world->camera_region.bottom,
                                             level_bias);
    }
}

int sml2_native_world_to_view(const Sml2NativeWorld *world,
                              int32_t world_x, int32_t world_y,
                              int32_t *view_x, int32_t *view_y)
{
    if (!world || !view_x || !view_y ||
        world->camera_view_width <= 0 || world->camera_view_height <= 0)
        return -1;
    *view_x = clamp_i64((int64_t)world_x - world->camera_x);
    *view_y = clamp_i64((int64_t)world_y - world->camera_y);
    return 0;
}

int sml2_native_view_bounds(const Sml2NativeWorld *world,
                            int32_t margin_pixels,
                            Sml2NativeRect *world_bounds)
{
    int64_t margin_fp;
    int64_t left, top, right, bottom;
    if (!world || !world_bounds || margin_pixels < 0 ||
        world->camera_view_width <= 0 || world->camera_view_height <= 0)
        return -1;

    margin_fp = (int64_t)margin_pixels * FP_ONE;
    left = (int64_t)world->camera_x - margin_fp;
    top = (int64_t)world->camera_y - margin_fp;
    right = (int64_t)world->camera_x +
            (int64_t)world->camera_view_width * FP_ONE + margin_fp;
    bottom = (int64_t)world->camera_y +
             (int64_t)world->camera_view_height * FP_ONE + margin_fp;

    /* Keep the query within the physical level. The view itself may extend
       beyond a small room, but never asks the stage renderer for invalid
       world coordinates. */
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > (int64_t)world->config.level_width * FP_ONE)
        right = (int64_t)world->config.level_width * FP_ONE;
    if (bottom > (int64_t)world->config.level_height * FP_ONE)
        bottom = (int64_t)world->config.level_height * FP_ONE;

    world_bounds->left = clamp_i64(left);
    world_bounds->top = clamp_i64(top);
    world_bounds->right = clamp_i64(right);
    world_bounds->bottom = clamp_i64(bottom);
    return 0;
}

int sml2_native_view_block_bounds(const Sml2NativeWorld *world,
                                  const Sml2NativeTileMap *map,
                                  int32_t margin_pixels,
                                  Sml2NativeBlockBounds *block_bounds)
{
    Sml2NativeRect pixels;
    int64_t left, top, right, bottom;
    if (!map || !block_bounds || map->width_blocks == 0 ||
        map->height_blocks == 0 ||
        sml2_native_view_bounds(world, margin_pixels, &pixels) != 0)
        return -1;

    /* Camera bounds are half-open. Floor the origin and ceil the far edge
       so partially visible edge tiles are never omitted. */
    left = pixels.left / (16LL * FP_ONE);
    top = pixels.top / (16LL * FP_ONE);
    right = (pixels.right + 16LL * FP_ONE - 1) / (16LL * FP_ONE);
    bottom = (pixels.bottom + 16LL * FP_ONE - 1) / (16LL * FP_ONE);

    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > map->width_blocks) right = map->width_blocks;
    if (bottom > map->height_blocks) bottom = map->height_blocks;
    if (left > map->width_blocks) left = map->width_blocks;
    if (top > map->height_blocks) top = map->height_blocks;
    if (right < left) right = left;
    if (bottom < top) bottom = top;

    block_bounds->left = (uint16_t)left;
    block_bounds->top = (uint16_t)top;
    block_bounds->right = (uint16_t)right;
    block_bounds->bottom = (uint16_t)bottom;
    return 0;
}

int sml2_native_view_contains_rect(const Sml2NativeWorld *world,
                                   const Sml2NativeRect *world_rect)
{
    Sml2NativeRect bounds;
    if (!world_rect ||
        world_rect->left >= world_rect->right ||
        world_rect->top >= world_rect->bottom ||
        sml2_native_view_bounds(world, 0, &bounds) != 0)
        return 0;

    /* Half-open rectangles: touching an edge alone is not visible. */
    return (int64_t)world_rect->right > bounds.left &&
           (int64_t)world_rect->left < bounds.right &&
           (int64_t)world_rect->bottom > bounds.top &&
           (int64_t)world_rect->top < bounds.bottom;
}

static int rom_collision_sample(const Sml2NativeTileMap *map,
                                  int32_t pixel_x, int32_t pixel_y)
{
    uint8_t tile = 0;
    if (sml2_native_rom_probe_tile(map,
            (uint16_t)(pixel_x + 8),
            (uint16_t)(pixel_y + 16), &tile) != 0)
        return 1;
    return tile < 0x38u || tile == 0x7Eu;
}

static uint32_t rom_player_collision(void *userdata,
                                     const Sml2NativeRect *rect)
{
    const Sml2NativeTileMap *map = (const Sml2NativeTileMap *)userdata;
    int32_t left, top, right, bottom;
    if (!map || !rect || rect->left >= rect->right ||
        rect->top >= rect->bottom)
        return SML2_NATIVE_COLLISION_SOLID;

    left = sml2_native_fp_to_int(rect->left);
    top = sml2_native_fp_to_int(rect->top);
    right = sml2_native_fp_to_int(rect->right - 1);
    bottom = sml2_native_fp_to_int(rect->bottom - 1);
    if (rom_collision_sample(map, left, top) ||
        rom_collision_sample(map, right, top) ||
        rom_collision_sample(map, left, bottom) ||
        rom_collision_sample(map, right, bottom))
        return SML2_NATIVE_COLLISION_SOLID;
    return SML2_NATIVE_COLLISION_NONE;
}

int sml2_native_rom_step(Sml2NativeWorld *world,
                         const Sml2NativeInput inputs[SML2_NATIVE_MAX_PLAYERS],
                         const Sml2NativeTileMap *map)
{
    unsigned i;
    int exit_phase = 0;
    if (!world || !map || !map->block_ids ||
        map->width_blocks != 256u || map->height_blocks != 48u)
        return -1;

    /* State $18 replaces the in-level update after the bell arm. VBlank keeps
       the fade timer running, but player movement, probes, and camera updates
       stop until the 192-frame tail reaches the hand-off boundary. */
    for (i = 0; i < SML2_NATIVE_MAX_PLAYERS; ++i) {
        if (world->players[i].active && world->players[i].exit_requested) {
            exit_phase = 1;
            break;
        }
    }
    if (exit_phase) {
        for (i = 0; i < SML2_NATIVE_MAX_PLAYERS; ++i) {
            Sml2NativePlayer *p = &world->players[i];
            if (!p->active || !p->exit_requested || p->exit_completed)
                continue;
            if (p->exit_fade_ticks > 0u)
                p->exit_fade_ticks--;
            if (p->exit_fade_ticks == 0u)
                p->exit_completed = 1u;
        }
        world->tick++;
        return 0;
    }

    if (sml2_native_step(world, inputs, rom_player_collision,
                         (void *)map) != 0)
        return -1;

    /* The source probes run against the post-movement player position. They
       are deliberately separate from the broad AABB query: their asymmetric
       offsets and powered/small-player heights are gameplay-significant. */
    for (i = 0; i < SML2_NATIVE_MAX_PLAYERS; ++i) {
        Sml2NativePlayer *p = &world->players[i];
        uint16_t x, y;
        uint8_t tile;
        int hit;
        if (!p->active) continue;
        x = (uint16_t)sml2_native_fp_to_int(p->x);
        y = (uint16_t)sml2_native_fp_to_int(p->y);

        if (p->vy < 0) {
            hit = sml2_native_rom_ceiling_probe(map, x, y,
                                                p->powerup, &tile);
            if (hit > 0) {
                p->vy = 0;
            } else if (hit < 0) {
                return -1;
            }
        }
        if (p->vx < 0) {
            hit = sml2_native_rom_wall_probe(map, x, y, 0,
                                             p->powerup, 0, &tile);
            if (hit > 0) p->vx = 0;
            else if (hit < 0) return -1;
        } else if (p->vx > 0) {
            hit = sml2_native_rom_wall_probe(map, x, y, 1,
                                             p->powerup, 0, &tile);
            if (hit > 0) p->vx = 0;
            else if (hit < 0) return -1;
        }
        if (p->vy >= 0) {
            hit = sml2_native_rom_floor_probe(map, x, y, &tile);
            if (hit > 0) {
                p->grounded = 1;
                p->vy = 0;
                /* $134F/$1366 landing alignment: the ROM nudges Y upward
                   by one pixel per landing frame until (y-4)&$0F is zero. */
                if (((uint16_t)(y - 4u) & 0x0Fu) != 0)
                    p->y = clamp_i64((int64_t)p->y - FP_ONE);
            } else if (hit < 0) {
                return -1;
            }
        }

        /* Keep the original environment classification attached to each
           player's native state so later physics can react without rescanning
           a different or duplicated world. Ducking is not modelled yet. */
        if (sml2_native_rom_environment_probe(
                map, x, y, p->powerup, 0,
                &p->environment_flags, &p->environment_tile) != 0)
            return -1;

        /* $1CBA-$1D01: the exit tile arms the level-clear state once.
           The actual fade/state-machine hand-off is advanced by the runtime. */
        if (!p->exit_requested &&
            (p->environment_flags & SML2_NATIVE_ROM_ENV_EXIT) != 0) {
            p->exit_requested = 1u;
            p->exit_tile = p->environment_tile;
            p->music_request = 3u;
            p->sound_request = 0u;
            p->exit_fade_ticks = 192u;
        }
    }
    return 0;
}

/* The ROM's coin counter uses a BCD low byte and a single high digit. */
static void rom_coin_credit(Sml2NativePlayer *player)
{
    uint8_t ones = (uint8_t)((player->coins_low & 0x0Fu) + 1u);
    uint8_t tens = (uint8_t)(player->coins_low >> 4);
    if (ones > 9u) {
        ones = (uint8_t)(ones - 10u);
        tens++;
    }
    player->coins_low = tens > 9u
                      ? 0u : (uint8_t)((tens << 4) | ones);
    if (player->coins_low == 0u) {
        player->coins_high++;
        if (player->coins_high >= 0x0Au) {
            player->coins_low = 0x99u;
            player->coins_high = 0x09u;
        }
    }
}

/* Mirrors the ordered side-probe call: each non-solid sample is offered to
   $1D75 before the next Y position is read. A solid sample ends the probe. */
static int rom_collect_side_probe(Sml2NativePlayer *player,
                                  const Sml2NativeTileMap *map,
                                  uint8_t *layout_cells, size_t cell_count,
                                  uint16_t player_x, uint16_t player_y,
                                  uint8_t side)
{
    uint16_t probe_x = (uint16_t)(player_x + (side ? 20u : 11u));
    uint16_t sample_y[3];
    unsigned count = 0, i;
    uint8_t tile, replacement;
    int collected;

    if (player->powerup) sample_y[count++] = (uint16_t)(player_y + 20u);
    sample_y[count++] = (uint16_t)(player_y + 32u);
    sample_y[count++] = (uint16_t)(player_y + 40u);

    for (i = 0; i < count; ++i) {
        if (sml2_native_rom_probe_tile(map, probe_x, sample_y[i], &tile) != 0)
            return -1;
        if (tile < 0x38u)
            return 1; /* This side probe hit a wall; ROM skips the other side. */
        if ((tile & 0xFEu) != 0x48u)
            continue;
        collected = sml2_native_rom_collect_coin(
            layout_cells, cell_count, probe_x, sample_y[i], tile, &replacement);
        if (collected < 0) return -1;
        if (collected > 0) {
            rom_coin_credit(player);
            player->sound_request = 7u;
        }
    }
    return 0;
}

int sml2_native_rom_step_mutable(
    Sml2NativeWorld *world,
    const Sml2NativeInput inputs[SML2_NATIVE_MAX_PLAYERS],
    const Sml2NativeTileMap *map,
    uint8_t *layout_cells, size_t cell_count)
{
    unsigned i;
    if (!world || !map || !map->block_ids || !layout_cells ||
        map->block_ids != layout_cells ||
        map->width_blocks != 256u || map->height_blocks != 48u ||
        cell_count < 256u * 48u)
        return -1;

    /* The mutable wrapper normally performs coin touches before the ROM
       movement step. Once state $18 is active, bypass those probes too. */
    for (i = 0; i < SML2_NATIVE_MAX_PLAYERS; ++i) {
        if (world->players[i].active && world->players[i].exit_requested)
            return sml2_native_rom_step(world, inputs, map);
    }

    /* $0F71 calls the left and right wall probes before horizontal integration.
       Each probe can collect coins even when it finds no wall. The right probe
       is an else-if: a solid left-side hit suppresses it for this frame. */
    for (i = 0; i < SML2_NATIVE_MAX_PLAYERS; ++i) {
        Sml2NativePlayer *player = &world->players[i];
        uint16_t x, y;
        int left_hit;
        if (!player->active) continue;
        x = (uint16_t)sml2_native_fp_to_int(player->x);
        y = (uint16_t)sml2_native_fp_to_int(player->y);
        left_hit = rom_collect_side_probe(player, map, layout_cells,
                                          cell_count, x, y, 0u);
        if (left_hit < 0) return -1;
        if (left_hit == 0 &&
            rom_collect_side_probe(player, map, layout_cells, cell_count,
                                   x, y, 1u) < 0)
            return -1;
    }

    return sml2_native_rom_step(world, inputs, map);
}

int sml2_native_step(Sml2NativeWorld *world,
                     const Sml2NativeInput inputs[SML2_NATIVE_MAX_PLAYERS],
                     Sml2NativeCollisionQuery collision_query,
                     void *collision_userdata)
{
    unsigned i;
    if (!world || !config_valid(&world->config)) return -1;

    /* Every active player receives one update against the same world query
       and the same simulation tick. No second game-world clone is created. */
    for (i = 0; i < SML2_NATIVE_MAX_PLAYERS; ++i)
        update_player(world, &world->players[i],
                      inputs ? &inputs[i] : NULL,
                      collision_query, collision_userdata);

    update_camera(world);
    world->tick++;
    return 0;
}
