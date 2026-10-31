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


static void test_native_tilemap_collision(void)
{
    uint8_t blocks[32 * 16] = {0};
    Sml2NativeTileMap map;
    Sml2NativeRect rect;
    unsigned x;
    memset(&map, 0, sizeof(map));
    map.width_blocks = 32;
    map.height_blocks = 16;
    map.block_ids = blocks;
    map.block_flags[1] = SML2_NATIVE_BLOCK_SOLID;
    map.solid_out_of_bounds = 1;

    /* Build a floor at y=128 and one freestanding wall block. */
    for (x = 0; x < 32; ++x) blocks[8 * 32 + x] = 1;
    blocks[4 * 32 + 10] = 1;

    rect.left = sml2_native_fp_from_int(20);
    rect.top = sml2_native_fp_from_int(112);
    rect.right = sml2_native_fp_from_int(32);
    rect.bottom = sml2_native_fp_from_int(128);
    assert(sml2_native_level_collision(&map, &rect) ==
           SML2_NATIVE_COLLISION_NONE); /* touching the floor is not overlap */

    rect.bottom++;
    assert(sml2_native_level_collision(&map, &rect) ==
           SML2_NATIVE_COLLISION_SOLID);

    rect.left = sml2_native_fp_from_int(160);
    rect.top = sml2_native_fp_from_int(64);
    rect.right = sml2_native_fp_from_int(172);
    rect.bottom = sml2_native_fp_from_int(80);
    assert(sml2_native_level_collision(&map, &rect) ==
           SML2_NATIVE_COLLISION_SOLID); /* wall block */

    rect.left = sml2_native_fp_from_int(144);
    rect.right = sml2_native_fp_from_int(160);
    assert(sml2_native_level_collision(&map, &rect) ==
           SML2_NATIVE_COLLISION_NONE); /* right edge is exclusive */

    rect.left = sml2_native_fp_from_int(-1);
    rect.right = sml2_native_fp_from_int(8);
    rect.top = sml2_native_fp_from_int(16);
    rect.bottom = sml2_native_fp_from_int(24);
    assert(sml2_native_level_collision(&map, &rect) ==
           SML2_NATIVE_COLLISION_SOLID); /* explicit outside-level policy */

    map.solid_out_of_bounds = 0;
    assert(sml2_native_level_collision(&map, &rect) ==
           SML2_NATIVE_COLLISION_NONE);
}

static void test_sml2_rom_player_probes(void)
{
    uint8_t blocks[256 * 48] = {0};
    Sml2NativeTileMap map;
    uint8_t tile = 0xFF;

    memset(&map, 0, sizeof(map));
    map.width_blocks = 256;
    map.height_blocks = 48;
    map.block_ids = blocks;

    /* $1EFA subtracts (8,16) before selecting a 16x16 map cell. */
    blocks[1 * 256 + 0] = 0x7E;
    assert(sml2_native_rom_probe_tile(&map, 8, 32, &tile) == 0);
    assert(tile == 0x7E);
    assert(sml2_native_rom_probe_tile(&map, 8, 0, &tile) == 0);
    assert(tile == 0); /* outside the map/echo window reads zero */

    /* $1E3B: gated floor probe, x+12 first, x+20 only if needed. */
    blocks[2 * 256 + 0] = 0x48; /* coin is not a standable floor tile */
    blocks[2 * 256 + 1] = 0x37; /* second sample is standable */
    assert(sml2_native_rom_floor_probe(&map, 4, 4, &tile) == 1);
    assert(tile == 0x37);
    assert(sml2_native_rom_floor_probe(&map, 4, 10, &tile) == 0);

    /* $1DFE ceiling probe uses y+28 small / y+18 powered. */
    blocks[1 * 256 + 0] = 0x7E;
    assert(sml2_native_rom_ceiling_probe(&map, 4, 4, 0, &tile) == 1);
    assert(tile == 0x7E);
    blocks[1 * 256 + 0] = 0x48;
    assert(sml2_native_rom_ceiling_probe(&map, 4, 4, 0, &tile) == 0);

    /* $1E7C/$1E8D wall probes: powered Mario first checks y+20. */
    blocks[0 * 256 + 0] = 0x37;
    assert(sml2_native_rom_wall_probe(&map, 4, 4, 0, 1, 0, &tile) == 1);
    assert(tile == 0x37);
    blocks[0 * 256 + 0] = 0x48;
    blocks[1 * 256 + 0] = 0x36;
    assert(sml2_native_rom_wall_probe(&map, 4, 4, 0, 0, 0, &tile) == 1);
    assert(tile == 0x36);
    assert(sml2_native_rom_wall_probe(&map, 4, 4, 2, 0, 0, &tile) == -1);
}

static void test_sml2_rom_world_wrap_and_echo(void)
{
    uint8_t blocks[256 * 48] = {0};
    Sml2NativeTileMap map;
    Sml2NativeRect rect;
    uint16_t block_x, block_y;

    memset(&map, 0, sizeof(map));
    map.width_blocks = 256;
    map.height_blocks = 48;
    map.block_ids = blocks;
    map.block_flags[1] = SML2_NATIVE_COLLISION_HAZARD;
    map.block_flags[2] = SML2_NATIVE_COLLISION_SOLID;
    map.solid_out_of_bounds = 1;

    assert(sml2_native_rom_world_cell(0x1234u, 0x0300u,
                                     &block_x, &block_y) == 0);
    assert(block_x == 0x23u);
    assert(block_y == 0x10u);
    assert(sml2_native_rom_world_cell(0x1000u, 0x04D0u,
                                     &block_x, &block_y) == 0);
    assert(block_x == 0u);
    assert(block_y == 0x2Du);
    assert(sml2_native_rom_world_cell(0xFFFFu, 0x04E0u,
                                     &block_x, &block_y) == 1);
    assert(block_x == 0xFFu);
    assert(block_y == 0x4Eu);
    assert(sml2_native_rom_world_cell(0, 0, NULL, &block_y) == -1);

    /* A ROM coordinate at $1000 wraps back to column zero. */
    blocks[16 * 256] = 1;
    rect.left = sml2_native_fp_from_int(4096);
    rect.right = sml2_native_fp_from_int(4112);
    rect.top = sml2_native_fp_from_int(0x300);
    rect.bottom = sml2_native_fp_from_int(0x310);
    assert(sml2_native_rom_level_collision(&map, &rect) ==
           SML2_NATIVE_COLLISION_HAZARD);

    /* Rows $30-$4D read the $10-$2D WRAM echo. */
    blocks[0x2D * 256 + 255] = 2;
    rect.left = sml2_native_fp_from_int(-16);
    rect.right = sml2_native_fp_from_int(0);
    rect.top = sml2_native_fp_from_int(0x4D0);
    rect.bottom = sml2_native_fp_from_int(0x4E0);
    assert(sml2_native_rom_level_collision(&map, &rect) ==
           SML2_NATIVE_COLLISION_SOLID);

    /* The first row beyond the echo uses the explicit out-of-bounds policy. */
    rect.top = sml2_native_fp_from_int(0x4E0);
    rect.bottom = sml2_native_fp_from_int(0x4F0);
    assert(sml2_native_rom_level_collision(&map, &rect) ==
           SML2_NATIVE_COLLISION_SOLID);
    map.solid_out_of_bounds = 0;
    assert(sml2_native_rom_level_collision(&map, &rect) ==
           SML2_NATIVE_COLLISION_NONE);
}

int main(void)
{
    Sml2NativeConfig config = make_config();
    Sml2NativeWorld first, second, room_world;
    Sml2NativeInput no_input[SML2_NATIVE_MAX_PLAYERS] = {{0}};

    assert(sml2_native_fp_from_int(-12) == -786432);
    int32_t view_width, view_height;
    assert(sml2_native_fp_to_int(-786432) == -12);
    test_native_tilemap_collision();
    test_sml2_rom_world_wrap_and_echo();
    test_sml2_rom_player_probes();

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

    /* The 16:9 camera's extra world area is available to both drawing and
       actor activation; it is not merely a stretched 160-pixel image. */
    {
        Sml2NativeRect visible_extra, beyond_view, invalid_rect;
        int32_t screen_x, screen_y;
        visible_extra.left = sml2_native_fp_from_int(250);
        visible_extra.top = sml2_native_fp_from_int(80);
        visible_extra.right = sml2_native_fp_from_int(270);
        visible_extra.bottom = sml2_native_fp_from_int(104);
        beyond_view.left = sml2_native_fp_from_int(330);
        beyond_view.top = visible_extra.top;
        beyond_view.right = sml2_native_fp_from_int(350);
        beyond_view.bottom = visible_extra.bottom;
        invalid_rect = visible_extra;
        invalid_rect.right = invalid_rect.left;

        assert(sml2_native_view_contains_rect(&first, &visible_extra));
        assert(!sml2_native_view_contains_rect(&first, &beyond_view));
        assert(!sml2_native_view_contains_rect(&first, &invalid_rect));
        {
            Sml2NativeRect render_bounds, activation_bounds;
            assert(sml2_native_view_bounds(&first, 0, &render_bounds) == 0);
            assert(sml2_native_view_bounds(&first, 32, &activation_bounds) == 0);
            assert(render_bounds.left == first.camera_x);
            assert(render_bounds.right == first.camera_x +
                   sml2_native_fp_from_int(first.camera_view_width));
            assert(activation_bounds.left >= 0);
            assert(activation_bounds.top >= 0);
            assert(activation_bounds.right <=
                   sml2_native_fp_from_int(first.config.level_width));
            assert(activation_bounds.bottom <=
                   sml2_native_fp_from_int(first.config.level_height));
            /* An actor just beyond the visible edge is eligible for early
               activation; one farther away remains dormant. */
            assert((int64_t)beyond_view.left < activation_bounds.right);
            assert((int64_t)beyond_view.right > activation_bounds.left);
            assert(sml2_native_view_bounds(&first, -1, &activation_bounds) == -1);
        }
        {
            uint8_t empty_blocks[32 * 16] = {0};
            Sml2NativeTileMap stage;
            Sml2NativeBlockBounds draw_blocks, wake_blocks;
            memset(&stage, 0, sizeof(stage));
            stage.width_blocks = 32;
            stage.height_blocks = 16;
            stage.block_ids = empty_blocks;
            assert(sml2_native_view_block_bounds(&first, &stage, 0,
                                                 &draw_blocks) == 0);
            assert(sml2_native_view_block_bounds(&first, &stage, 32,
                                                 &wake_blocks) == 0);
            assert(draw_blocks.left < draw_blocks.right);
            assert(draw_blocks.top < draw_blocks.bottom);
            assert(wake_blocks.left <= draw_blocks.left);
            assert(wake_blocks.top <= draw_blocks.top);
            assert(wake_blocks.right >= draw_blocks.right);
            assert(wake_blocks.bottom >= draw_blocks.bottom);
            assert(wake_blocks.right <= stage.width_blocks);
            assert(wake_blocks.bottom <= stage.height_blocks);
            assert(sml2_native_view_block_bounds(&first, &stage, -1,
                                                 &wake_blocks) == -1);
        }
        assert(sml2_native_world_to_view(&first, visible_extra.left,
                                          visible_extra.top,
                                          &screen_x, &screen_y) == 0);
        assert(screen_x == visible_extra.left - first.camera_x);
        assert(screen_y == visible_extra.top - first.camera_y);
    }

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
    assert(first.camera_x == 0);
    assert(first.camera_y == 0);

    /* Both players remain independent and inside the level's camera bounds. */
    assert(first.players[0].active && first.players[1].active);
    assert(first.players[0].x != first.players[1].x);
    assert(first.camera_x <= sml2_native_fp_from_int(config.level_width));
    assert(first.camera_y <= sml2_native_fp_from_int(config.level_height));

    /* Invalid construction and player indices fail safely. */
    assert(sml2_native_init(&first, NULL) == -1);
    assert(sml2_native_spawn_player(&second, SML2_NATIVE_MAX_PLAYERS,
                                    0, 0, 3) == -1);

    /* A camera lock room constrains an enlarged view without changing the
       underlying level geometry. A view wider than the room is centred on it. */
    config = make_config();
    assert(sml2_native_init(&room_world, &config) == 0);
    assert(sml2_native_set_view(&room_world, SML2_NATIVE_WIDESCREEN_16_9,
                                SML2_NATIVE_ZOOM_125, 1920, 1080, 0) == 0);
    assert(sml2_native_spawn_player(&room_world, 0, 220, 104, 5) == 0);
    assert(sml2_native_step(&room_world, no_input, test_collision, NULL) == 0);
    {
        Sml2NativeCameraRegion room = {128, 0, 384, 256};
        assert(sml2_native_set_camera_region(&room_world, &room) == 0);
        assert(sml2_native_step(&room_world, no_input, test_collision, NULL) == 0);
        assert(room_world.camera_x == sml2_native_fp_from_int(96));
        room.right = 513;
        assert(sml2_native_set_camera_region(&room_world, &room) == -1);
        assert(sml2_native_set_camera_region(&room_world, NULL) == 0);
        assert(room_world.camera_region.left == 0);
        assert(room_world.camera_region.right == config.level_width);
    }
    return 0;
}
