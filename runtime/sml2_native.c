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

static int config_valid(const Sml2NativeConfig *c)
{
    return c &&
           c->level_width > 0 &&
           c->level_height > 0 &&
           c->gravity >= 0 &&
           c->horizontal_acceleration > 0 &&
           c->horizontal_friction >= 0 &&
           c->max_run_speed > 0 &&
           c->jump_velocity < 0 &&
           c->terminal_velocity > 0 &&
           c->player_width > 0 &&
           c->player_height > 0 &&
           c->camera_padding >= 0 &&
           c->base_view_width > 0 &&
           c->max_view_width >= c->base_view_width &&
           c->view_height > 0;
}

int sml2_native_init(Sml2NativeWorld *world, const Sml2NativeConfig *config)
{
    if (!world || !config_valid(config)) return -1;
    memset(world, 0, sizeof(*world));
    world->config = *config;
    world->camera_view_width = config->base_view_width;
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

static void update_camera(Sml2NativeWorld *world)
{
    int32_t min_x = INT32_MAX, min_y = INT32_MAX;
    int32_t max_x = INT32_MIN, max_y = INT32_MIN;
    int found = 0;
    int32_t view_width, view_height, center_x, center_y;
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
    }

    if (!found) {
        world->camera_x = 0;
        world->camera_y = 0;
        world->camera_view_width = world->config.base_view_width;
        return;
    }

    view_width = clamp_i64(((int64_t)max_x - min_x) / FP_ONE +
                           2LL * world->config.camera_padding);
    view_width = clamp_i32(view_width, world->config.base_view_width,
                           world->config.max_view_width);
    view_height = world->config.view_height;
    center_x = clamp_i64(((int64_t)min_x + max_x) / 2);
    center_y = clamp_i64(((int64_t)min_y + max_y) / 2);

    world->camera_view_width = view_width;
    world->camera_x = center_x - sml2_native_fp_from_int(view_width / 2);
    world->camera_y = center_y - sml2_native_fp_from_int(view_height / 2);

    {
        int32_t max_camera_x = sml2_native_fp_from_int(
            world->config.level_width > view_width
            ? world->config.level_width - view_width : 0);
        int32_t max_camera_y = sml2_native_fp_from_int(
            world->config.level_height > view_height
            ? world->config.level_height - view_height : 0);
        world->camera_x = clamp_i32(world->camera_x, 0, max_camera_x);
        world->camera_y = clamp_i32(world->camera_y, 0, max_camera_y);
    }
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
