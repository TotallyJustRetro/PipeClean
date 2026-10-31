#include "sml2_native.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static uint32_t test_collision(void *userdata, const Sml2NativeRect *r)
{
    const int32_t floor_y = sml2_native_fp_from_int(120);
    const int32_t wall_left = sml2_native_fp_from_int(300);
    const int32_t wall_right = sml2_native_fp_from_int(316);
    (void)userdata;

    if (r->bottom > floor_y && r->top < floor_y)
        return SML2_NATIVE_COLLISION_SOLID;
    if (r->right > wall_left && r->left < wall_right &&
        r->bottom > sml2_native_fp_from_int(80) &&
        r->top < sml2_native_fp_from_int(120))
        return SML2_NATIVE_COLLISION_SOLID;
    return SML2_NATIVE_COLLISION_NONE;
}

static Sml2NativeConfig make_config(void)
{
    Sml2NativeConfig c;
    memset(&c, 0, sizeof(c));
    c.level_width = 512;
    c.level_height = 256;
    c.gravity = 0x3000;
    c.horizontal_acceleration = 0x4000;
    c.horizontal_friction = 0x3000;
    c.max_run_speed = 0x28000;
    c.jump_velocity = -0x50000;
    c.terminal_velocity = 0x60000;
    c.player_width = 12;
    c.player_height = 16;
    c.camera_padding = 24;
    c.base_view_width = 160;
    c.max_view_width = 320;
    c.view_height = 144;
    return c;
}

static void run_script(Sml2NativeWorld *world)
{
    Sml2NativeInput inputs[SML2_NATIVE_MAX_PLAYERS];
    unsigned frame;
    memset(inputs, 0, sizeof(inputs));

    for (frame = 0; frame < 12; ++frame) {
        inputs[0].buttons = SML2_NATIVE_BUTTON_RIGHT;
        inputs[1].buttons = SML2_NATIVE_BUTTON_LEFT;
        assert(sml2_native_step(world, inputs, test_collision, NULL) == 0);
    }

    /* Jump is edge-triggered; holding the button does not restart the jump. */
    inputs[0].buttons = SML2_NATIVE_BUTTON_RIGHT | SML2_NATIVE_BUTTON_JUMP;
    inputs[1].buttons = SML2_NATIVE_BUTTON_LEFT;
    assert(sml2_native_step(world, inputs, test_collision, NULL) == 0);
    for (frame = 0; frame < 14; ++frame)
        assert(sml2_native_step(world, inputs, test_collision, NULL) == 0);

    inputs[0].buttons = SML2_NATIVE_BUTTON_RIGHT;
    for (frame = 0; frame < 110; ++frame)
        assert(sml2_native_step(world, inputs, test_collision, NULL) == 0);

    memset(inputs, 0, sizeof(inputs));
    for (frame = 0; frame < 3; ++frame)
        assert(sml2_native_step(world, inputs, test_collision, NULL) == 0);
}

int main(void)
{
    Sml2NativeConfig config = make_config();
    Sml2NativeWorld first, second;
    Sml2NativeInput no_input[SML2_NATIVE_MAX_PLAYERS] = {{0}};

    assert(sml2_native_fp_from_int(-12) == -786432);
    assert(sml2_native_fp_to_int(-786432) == -12);

    assert(sml2_native_init(&first, &config) == 0);
    assert(sml2_native_init(&second, &config) == 0);
    assert(sml2_native_spawn_player(&first, 0, 40, 104, 5) == 0);
    assert(sml2_native_spawn_player(&first, 1, 250, 104, 5) == 0);
    assert(sml2_native_spawn_player(&second, 0, 40, 104, 5) == 0);
    assert(sml2_native_spawn_player(&second, 1, 250, 104, 5) == 0);

    /* The shared camera expands to keep both independently moving players in view. */
    assert(sml2_native_step(&first, no_input, test_collision, NULL) == 0);
    assert(first.players[0].grounded);
    assert(first.players[1].grounded);
    assert(first.camera_view_width > config.base_view_width);
    assert(first.camera_view_width <= config.max_view_width);

    run_script(&first);
    run_script(&second);
    assert(first.tick == second.tick);
    assert(first.players[0].x == second.players[0].x);
    assert(first.players[0].y == second.players[0].y);
    assert(first.players[0].vx == second.players[0].vx);
    assert(first.players[0].vy == second.players[0].vy);
    assert(first.players[1].x == second.players[1].x);
    assert(first.players[1].y == second.players[1].y);
    assert(first.camera_x == second.camera_x);
    assert(first.camera_y == second.camera_y);
    assert(first.camera_view_width == second.camera_view_width);

    /* Both players remain independent and inside the level's camera bounds. */
    assert(first.players[0].active && first.players[1].active);
    assert(first.players[0].x != first.players[1].x);
    assert(first.camera_x >= 0);
    assert(first.camera_x <= sml2_native_fp_from_int(
        config.level_width - first.camera_view_width));

    /* Invalid construction and player indices fail safely. */
    assert(sml2_native_init(&first, NULL) == -1);
    assert(sml2_native_spawn_player(&second, SML2_NATIVE_MAX_PLAYERS,
                                    0, 0, 3) == -1);
    return 0;
}
