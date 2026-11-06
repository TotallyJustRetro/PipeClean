#include "sml2_native_actor.h"

#include <assert.h>
#include <string.h>

static int update_actor(void *userdata, Sml2NativeActor *actor, uint64_t tick)
{
    unsigned *updates = (unsigned *)userdata;
    ++*updates;
    actor->x += SML2_NATIVE_FP_ONE;
    assert(tick == *updates - 1);
    return 0;
}

typedef struct {
    unsigned count;
    uint16_t last_id;
    Sml2NativeRect last_bounds;
} DrawCapture;

static int capture_actor_draw(void *userdata, const Sml2NativeActorDraw *draw)
{
    DrawCapture *capture = (DrawCapture *)userdata;
    ++capture->count;
    capture->last_id = draw->actor->id;
    capture->last_bounds = draw->view_bounds;
    return 0;
}

int main(void)
{
    uint8_t cells[8 * 4] = {0};
    Sml2NativeStageTile tiles[1] = {{{0, 0, 0, 0}, 0, 0}};
    Sml2NativeStageObject objects[2] = {
        {1, 10, 2, 16, 16, 8, 8,
         SML2_NATIVE_STAGE_OBJECT_ENABLED | SML2_NATIVE_STAGE_OBJECT_UNIQUE},
        {2, 11, 0, 120, 16, 8, 8, SML2_NATIVE_STAGE_OBJECT_ENABLED | SML2_NATIVE_STAGE_OBJECT_PERSISTENT}
    };
    Sml2NativeStage stage;
    Sml2NativeActorWorld world;
    Sml2NativeRect near = {0, 0, 64 * SML2_NATIVE_FP_ONE, 48 * SML2_NATIVE_FP_ONE};
    Sml2NativeRect far = {96 * SML2_NATIVE_FP_ONE, 0,
                          160 * SML2_NATIVE_FP_ONE, 48 * SML2_NATIVE_FP_ONE};
    unsigned updates = 0;

    assert(sml2_native_stage_init(&stage, 8, 4, cells, tiles, 1,
                                  objects, 2, NULL, 0, 1) == 0);
    sml2_native_actors_init(&world);
    assert(sml2_native_actors_sync(&world, &stage, &near) == 0);
    assert(world.count == 1);
    assert(sml2_native_actor_find(&world, 1)->state == SML2_NATIVE_ACTOR_AWAKE);
    assert(sml2_native_actors_step(&world, update_actor, &updates) == 0);
    assert(updates == 1 && world.tick == 1);
    assert(sml2_native_actor_damage(&world, 1, 1) == 1);
    assert(sml2_native_actor_find(&world, 1)->state == SML2_NATIVE_ACTOR_DEFEATED);

    assert(sml2_native_actors_sync(&world, &stage, &far) == 0);
    assert(world.count == 2);
    assert(sml2_native_actor_find(&world, 1)->state == SML2_NATIVE_ACTOR_DEFEATED);
    assert(sml2_native_actor_find(&world, 2)->state == SML2_NATIVE_ACTOR_AWAKE);
    assert(sml2_native_actor_find(&world, 2)->flags & SML2_NATIVE_ACTOR_PERSISTENT);
    assert(sml2_native_actors_step(&world, update_actor, &updates) == 0);
    assert(updates == 2 && world.tick == 2);
    assert(sml2_native_actor_damage(&world, 99, 1) == -1);
    {
        Sml2NativeWorld camera;
        Sml2NativeConfig config;
        DrawCapture capture;
        memset(&config, 0, sizeof(config));
        config.level_width = 128;
        config.level_height = 64;
        config.gravity = 0x3000;
        config.horizontal_acceleration = 0x4000;
        config.horizontal_friction = 0x3000;
        config.max_run_speed = 0x28000;
        config.jump_velocity = -0x50000;
        config.terminal_velocity = 0x60000;
        config.player_width = 12;
        config.player_height = 16;
        config.camera_padding = 8;
        config.window_width = 160;
        config.window_height = 144;
        config.widescreen_mode = SML2_NATIVE_WIDESCREEN_OFF;
        config.camera_zoom_percent = SML2_NATIVE_ZOOM_100;
        config.max_camera_zoom_percent = SML2_NATIVE_ZOOM_100;
        assert(sml2_native_init(&camera, &config) == 0);
        /* The defeated-unique persistence assertion above is complete; make
           the actor visible again to exercise the independent draw query. */
        world.actors[0].state = SML2_NATIVE_ACTOR_AWAKE;
        camera.camera_view_width = 64;
        camera.camera_view_height = 48;
        camera.camera_x = 0;
        camera.camera_y = 0;
        memset(&capture, 0, sizeof(capture));
        assert(sml2_native_actors_visit_visible(&world, &camera,
                    capture_actor_draw, &capture) == 0);
        assert(capture.count == 1 && capture.last_id == 1);
        assert(capture.last_bounds.left == 17 * SML2_NATIVE_FP_ONE);
        assert(capture.last_bounds.top == 16 * SML2_NATIVE_FP_ONE);
        assert(capture.last_bounds.right == 25 * SML2_NATIVE_FP_ONE);
        camera.camera_x = 64 * SML2_NATIVE_FP_ONE;
        memset(&capture, 0, sizeof(capture));
        assert(sml2_native_actors_visit_visible(&world, &camera,
                    capture_actor_draw, &capture) == 0);
        assert(capture.count == 1 && capture.last_id == 2);
        assert(capture.last_bounds.left == 57 * SML2_NATIVE_FP_ONE);
        assert(sml2_native_actors_visit_visible(NULL, &camera,
                    capture_actor_draw, &capture) == -1);
        assert(sml2_native_actors_visit_visible(&world, &camera,
                    NULL, &capture) == -1);
    }

    {
        /* The logical camera now drives actor activation: objects beyond the
           old 160-pixel edge materialize only when the selected view exposes
           them. Zoom-out expands the activation frontier again. */
        uint8_t wide_cells[256 * 48] = {0};
        Sml2NativeStageTile wide_tiles[1] = {{{0, 0, 0, 0}, 0, 0}};
        Sml2NativeStageObject wide_objects[3] = {
            {20, 1, 0, 200, 16, 8, 8, SML2_NATIVE_STAGE_OBJECT_ENABLED},
            {21, 1, 0, 300, 16, 8, 8, SML2_NATIVE_STAGE_OBJECT_ENABLED},
            {22, 1, 0, 400, 16, 8, 8, SML2_NATIVE_STAGE_OBJECT_ENABLED}
        };
        Sml2NativeStage wide_stage;
        Sml2NativeActorWorld narrow_actors, wide_actors, cinema_actors;
        Sml2NativeWorld narrow_camera, wide_camera, cinema_camera;
        Sml2NativeConfig view;
        assert(sml2_native_stage_init(&wide_stage, 256, 48, wide_cells,
                    wide_tiles, 1, wide_objects, 3, NULL, 0, 1) == 0);
        memset(&view, 0, sizeof(view));
        view.level_width = 4096;
        view.level_height = 768;
        view.gravity = 0x3000;
        view.horizontal_acceleration = 0x4000;
        view.horizontal_friction = 0x3000;
        view.max_run_speed = 0x28000;
        view.jump_velocity = -0x50000;
        view.terminal_velocity = 0x60000;
        view.player_width = 12;
        view.player_height = 16;
        view.camera_padding = 8;
        view.window_width = 160;
        view.window_height = 144;
        view.camera_zoom_percent = SML2_NATIVE_ZOOM_100;
        view.max_camera_zoom_percent = SML2_NATIVE_ZOOM_200;

        view.widescreen_mode = SML2_NATIVE_WIDESCREEN_OFF;
        assert(sml2_native_init(&narrow_camera, &view) == 0);
        sml2_native_actors_init(&narrow_actors);
        assert(narrow_camera.camera_view_width == 160);
        assert(sml2_native_actors_sync_camera(&narrow_actors, &wide_stage,
                    &narrow_camera, 0) == 0);
        assert(narrow_actors.count == 0);

        view.widescreen_mode = SML2_NATIVE_WIDESCREEN_16_9;
        assert(sml2_native_init(&wide_camera, &view) == 0);
        sml2_native_actors_init(&wide_actors);
        assert(wide_camera.camera_view_width == 256);
        assert(sml2_native_actors_sync_camera(&wide_actors, &wide_stage,
                    &wide_camera, 0) == 0);
        assert(wide_actors.count == 1);
        assert(sml2_native_actor_find(&wide_actors, 20) != NULL);
        assert(sml2_native_actor_find(&wide_actors, 21) == NULL);

        view.widescreen_mode = SML2_NATIVE_WIDESCREEN_21_9;
        view.camera_zoom_percent = SML2_NATIVE_ZOOM_100;
        assert(sml2_native_init(&cinema_camera, &view) == 0);
        sml2_native_actors_init(&cinema_actors);
        assert(cinema_camera.camera_view_width == 336);
        assert(sml2_native_actors_sync_camera(&cinema_actors, &wide_stage,
                    &cinema_camera, 0) == 0);
        assert(cinema_actors.count == 2);
        assert(sml2_native_actor_find(&cinema_actors, 20) != NULL);
        assert(sml2_native_actor_find(&cinema_actors, 21) != NULL);
        assert(sml2_native_actor_find(&cinema_actors, 22) == NULL);

        view.camera_zoom_percent = SML2_NATIVE_ZOOM_200;
        assert(sml2_native_init(&cinema_camera, &view) == 0);
        sml2_native_actors_init(&cinema_actors);
        assert(cinema_camera.camera_view_width == 672);
        assert(sml2_native_actors_sync_camera(&cinema_actors, &wide_stage,
                    &cinema_camera, 0) == 0);
        assert(cinema_actors.count == 3);
        assert(sml2_native_actor_find(&cinema_actors, 22) != NULL);

        assert(sml2_native_actors_sync_camera(&cinema_actors, &wide_stage,
                    &cinema_camera, -1) == -1);
    }
    return 0;
}
