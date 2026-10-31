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

    if (r->left < 0 || r->right > sml2_native_fp_from_int(512))
        return SML2_NATIVE_COLLISION_SOLID;
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
    c.window_width = 1920;
    c.window_height = 1080;
    c.widescreen_mode = SML2_NATIVE_WIDESCREEN_16_9;
    c.camera_zoom_percent = SML2_NATIVE_ZOOM_100;
    c.max_camera_zoom_percent = SML2_NATIVE_ZOOM_200;
    c.auto_zoom = 1;
    return c;
}

static void run_script(Sml2NativeWorld *world)
{
    Sml2NativeInput inputs[SML2_NATIVE_MAX_PLAYERS];
    int32_t ground_y = world->players[0].y;
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
    assert(world->players[0].y < ground_y);
    for (frame = 0; frame < 14; ++frame)
        assert(sml2_native_step(world, inputs, test_collision, NULL) == 0);

    inputs[0].buttons = SML2_NATIVE_BUTTON_RIGHT;
    for (frame = 0; frame < 110; ++frame)
        assert(sml2_native_step(world, inputs, test_collision, NULL) == 0);

    memset(inputs, 0, sizeof(inputs));
    for (frame = 0; frame < 3; ++frame)
        assert(sml2_native_step(world, inputs, test_collision, NULL) == 0);

    assert(world->players[0].grounded);
    {
        int32_t landing_error = world->players[0].y - ground_y;
        if (landing_error < 0) landing_error = -landing_error;
        assert(landing_error <= SML2_NATIVE_FP_ONE);
    }
    assert(world->players[0].x <= sml2_native_fp_from_int(288));
    assert(world->players[1].x >= 0);
}

int main(void)
{
    Sml2NativeConfig config = make_config();
    Sml2NativeWorld first, second;
    Sml2NativeInput no_input[SML2_NATIVE_MAX_PLAYERS] = {{0}};

    assert(sml2_native_fp_from_int(-12) == -786432);
    int32_t view_width, view_height;
    assert(sml2_native_fp_to_int(-786432) == -12);

    /* Match the original project's supported aspect ratios and zoom presets. */
    assert(sml2_native_view_size(SML2_NATIVE_WIDESCREEN_OFF, 100, 1920, 1080,
                                 &view_width, &view_height) == 0);
    assert(view_width == 160 && view_height == 144);
    assert(sml2_native_view_size(SML2_NATIVE_WIDESCREEN_16_9, 100, 1920, 1080,
                                 &view_width, &view_height) == 0);
    assert(view_width == 256 && view_height == 144);
    assert(sml2_native_view_size(SML2_NATIVE_WIDESCREEN_16_9, 150, 1920, 1080,
                                 &view_width, &view_height) == 0);
    assert(view_width == 384 && view_height == 216);
    assert(sml2_native_view_size(SML2_NATIVE_WIDESCREEN_21_9, 200, 2520, 1080,
                                 &view_width, &view_height) == 0);
    assert(view_width == 672 && view_height == 288);
    assert(sml2_native_view_size(SML2_NATIVE_WIDESCREEN_WINDOW, 100, 1920, 1080,
                                 &view_width, &view_height) == 0);
    assert(view_width == 256 && view_height == 144);
    assert(sml2_native_view_size(SML2_NATIVE_WIDESCREEN_WINDOW, 100, 480, 960,
                                 &view_width, &view_height) == 0);
    assert(view_width == 160 && view_height == 320);
    assert(sml2_native_view_size(SML2_NATIVE_WIDESCREEN_WINDOW, 200, 480, 960,
                                 &view_width, &view_height) == 0);
    assert(view_width == 320 && view_height == 640);
    assert(sml2_native_view_size(99, 100, 1920, 1080,
                                 &view_width, &view_height) == -1);

    assert(sml2_native_init(&first, &config) == 0);
    assert(sml2_native_init(&second, &config) == 0);
    assert(sml2_native_spawn_player(&first, 0, 40, 104, 5) == 0);
    assert(sml2_native_spawn_player(&first, 1, 250, 104, 5) == 0);
    assert(sml2_native_spawn_player(&second, 0, 40, 104, 5) == 0);
    assert(sml2_native_spawn_player(&second, 1, 250, 104, 5) == 0);

    /* The shared camera expands to keep both independently moving players in view. */
    assert(sml2_native_step(&first, no_input, test_collision, NULL) == 0);
    assert(sml2_native_step(&second, no_input, test_collision, NULL) == 0);
    assert(first.players[0].grounded);
    assert(first.players[1].grounded);
    assert(first.camera_view_width == 320);
    assert(first.camera_view_height == 180);
    assert(first.camera_zoom_percent == SML2_NATIVE_ZOOM_125);

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
    assert(first.camera_view_height == second.camera_view_height);
    assert(first.camera_zoom_percent == second.camera_zoom_percent);

    /* Switching the view mode live recomputes the camera's logical surface. */
    assert(sml2_native_set_view(&first, SML2_NATIVE_WIDESCREEN_21_9,
                                SML2_NATIVE_ZOOM_100, 2520, 1080, 0) == 0);
    assert(sml2_native_step(&first, no_input, test_collision, NULL) == 0);
    assert(first.camera_view_width == 336);
    assert(first.camera_view_height == 144);
    assert(first.camera_zoom_percent == SML2_NATIVE_ZOOM_100);

    /* Tall window mode preserves the native horizontal field and grows vertically. */
    assert(sml2_native_set_view(&first, SML2_NATIVE_WIDESCREEN_WINDOW,
                                SML2_NATIVE_ZOOM_100, 480, 960, 0) == 0);
    assert(sml2_native_step(&first, no_input, test_collision, NULL) == 0);
    assert(first.camera_view_width == 160);
    assert(first.camera_view_height == 320);

    /* A view larger than a small level centres around the level instead of
       clipping to its top-left edge or overflowing world coordinates. */
    first.config.level_width = 128;
    first.config.level_height = 100;
    assert(sml2_native_set_view(&first, SML2_NATIVE_WIDESCREEN_21_9,
                                SML2_NATIVE_ZOOM_200, 2520, 1080, 0) == 0);
    assert(sml2_native_step(&first, no_input, test_collision, NULL) == 0);
    assert(first.camera_view_width == 672);
    assert(first.camera_view_height == 288);
    assert(first.camera_x == sml2_native_fp_from_int(-272));
    assert(first.camera_y == sml2_native_fp_from_int(-94));

    /* Both players remain independent and inside the level's camera bounds. */
    assert(first.players[0].active && first.players[1].active);
    assert(first.players[0].x != first.players[1].x);
    assert(first.camera_x <= sml2_native_fp_from_int(config.level_width));
    assert(first.camera_y <= sml2_native_fp_from_int(config.level_height));

    /* Invalid construction and player indices fail safely. */
    assert(sml2_native_init(&first, NULL) == -1);
    assert(sml2_native_spawn_player(&second, SML2_NATIVE_MAX_PLAYERS,
                                    0, 0, 3) == -1);
    return 0;
}
