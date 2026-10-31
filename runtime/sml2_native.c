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
