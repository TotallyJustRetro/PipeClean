#include "sml2_native_stage.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    unsigned count;
    unsigned last_x;
    unsigned last_y;
} DrawLog;

static int record_draw(void *userdata, const Sml2NativeStageDrawCell *cell)
{
    DrawLog *log = (DrawLog *)userdata;
    assert(cell->tile != NULL);
    assert(cell->world_x == (int32_t)cell->block_x * 16);
    assert(cell->world_y == (int32_t)cell->block_y * 16);
    ++log->count;
    log->last_x = cell->block_x;
    log->last_y = cell->block_y;
    return 0;
}

static int stop_after_one(void *userdata, const Sml2NativeStageDrawCell *cell)
{
    (void)userdata;
    (void)cell;
    return 7;
}

static int record_object(void *userdata, const Sml2NativeStageObject *object)
{
    unsigned *count = (unsigned *)userdata;
    assert(object->flags & SML2_NATIVE_STAGE_OBJECT_ENABLED);
    ++*count;
    return 0;
}

int main(void)
{
    uint8_t cells[8 * 4] = {0};
    Sml2NativeStageTile tiles[2];
    Sml2NativeStageObject objects[3] = {
        {1, 10, 0, 16, 16, 8, 8, SML2_NATIVE_STAGE_OBJECT_ENABLED},
        {2, 11, 0, 100, 16, 8, 8, SML2_NATIVE_STAGE_OBJECT_ENABLED},
        {3, 12, 0, 32, 16, 8, 8, 0}
    };
    Sml2NativeStageRoom rooms[1] = {{4, {0, 0, 64, 64}}};
    Sml2NativeStage stage;
    Sml2NativeBlockBounds draw = {0, 0, 3, 2};
    Sml2NativeRect view = {0, 0, 64, 48};
    Sml2NativeCameraRegion room;
    DrawLog log = {0, 0, 0};
    unsigned object_count = 0;

    memset(tiles, 0, sizeof(tiles));
    tiles[1].graphics[0] = 101;
    tiles[1].graphics[1] = 102;
    tiles[1].graphics[2] = 103;
    tiles[1].graphics[3] = 104;
    tiles[1].collision_flags = SML2_NATIVE_STAGE_TILE_SOLID;
    cells[1] = 1;
    cells[2 * 8 + 2] = 1;

    assert(sml2_native_stage_init(&stage, 8, 4, cells, tiles, 2,
                                  objects, 3, rooms, 1, 1) == 0);
    assert(stage.collision_map.block_flags[1] == SML2_NATIVE_BLOCK_SOLID);
    assert(sml2_native_level_collision(&stage.collision_map,
             &(Sml2NativeRect){16 * SML2_NATIVE_FP_ONE,
                               0,
                               32 * SML2_NATIVE_FP_ONE,
                               16 * SML2_NATIVE_FP_ONE}) ==
           SML2_NATIVE_COLLISION_SOLID);

    assert(sml2_native_stage_visit_draw_cells(&stage, &draw,
                                               record_draw, &log) == 0);
    assert(log.count == 6);
    assert(log.last_x == 2 && log.last_y == 1);
    assert(sml2_native_stage_visit_draw_cells(&stage, &draw,
                                               stop_after_one, NULL) == 7);
    assert(sml2_native_stage_visit_objects(&stage, &view,
                                            record_object, &object_count) == 0);
    assert(object_count == 1);
    assert(sml2_native_stage_get_room(&stage, 4, &room) == 0);
    assert(room.right == 64 && room.bottom == 64);
    assert(sml2_native_stage_get_room(&stage, 99, &room) == -1);

    draw.right = 9;
    assert(sml2_native_stage_visit_draw_cells(&stage, &draw,
                                               record_draw, &log) == -1);
    cells[0] = 2;
    assert(sml2_native_stage_init(&stage, 8, 4, cells, tiles, 2,
                                  objects, 3, rooms, 1, 1) == -1);
    return 0;
}
