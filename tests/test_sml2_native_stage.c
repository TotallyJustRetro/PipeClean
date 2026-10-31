#include "sml2_native_stage.h"

#include <assert.h>
#include <stdlib.h>
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

static uint8_t fake_sml2_memory(void *userdata, uint16_t address)
{
    const uint8_t *map = (const uint8_t *)userdata;
    if (address >= 0xB000u && address < 0xB000u + SML2_NATIVE_SML2_MAP_BYTES)
        return map[address - 0xB000u];
    if (address >= 0xA600u && address < 0xA600u + SML2_NATIVE_SML2_BLOCKDEF_BYTES)
        return (uint8_t)(address - 0xA600u);
    return 0;
}

static void test_sml2_collision_ranges(void)
{
    uint16_t flags;
    assert(sml2_native_sml2_material(0x00) == SML2_NATIVE_MATERIAL_SOLID);
    assert(sml2_native_sml2_collision_flags(0x00) &
           SML2_NATIVE_STAGE_TILE_SOLID);
    assert(sml2_native_sml2_collision_flags(0x37) &
           SML2_NATIVE_STAGE_TILE_STANDABLE);
    flags = sml2_native_sml2_collision_flags(0x38);
    assert(!(flags & SML2_NATIVE_STAGE_TILE_SOLID));
    assert(flags & SML2_NATIVE_STAGE_TILE_STANDABLE);
    assert(flags & SML2_NATIVE_STAGE_TILE_ONE_WAY);
    assert(sml2_native_sml2_collision_flags(0x48) &
           SML2_NATIVE_STAGE_TILE_COIN);
    assert(sml2_native_sml2_collision_flags(0x4A) &
           SML2_NATIVE_STAGE_TILE_EXIT);
    assert(sml2_native_sml2_collision_flags(0x4C) &
           SML2_NATIVE_STAGE_TILE_WATER);
    assert(sml2_native_sml2_collision_flags(0x50) &
           SML2_NATIVE_STAGE_TILE_HAZARD);
    assert(sml2_native_sml2_collision_flags(0x52) &
           SML2_NATIVE_STAGE_TILE_MOON);
    assert(sml2_native_sml2_collision_flags(0x04) &
           SML2_NATIVE_STAGE_TILE_BOUNCE);
    assert(sml2_native_sml2_collision_flags(0x05) &
           SML2_NATIVE_STAGE_TILE_PIPE);
    assert(sml2_native_sml2_collision_flags(0x06) &
           SML2_NATIVE_STAGE_TILE_BREAKABLE);
    assert(sml2_native_sml2_collision_flags(0x7E) &
           SML2_NATIVE_STAGE_TILE_HIDDEN);
}

static void test_sml2_layout_decode(void)
{
    uint8_t *map = (uint8_t *)calloc(SML2_NATIVE_SML2_MAP_BYTES, 1);
    uint8_t defs[SML2_NATIVE_SML2_BLOCKDEF_BYTES];
    uint16_t flags[SML2_NATIVE_SML2_BLOCK_COUNT] = {0};
    Sml2NativeStageTile *tiles = (Sml2NativeStageTile *)calloc(
        SML2_NATIVE_SML2_BLOCK_COUNT, sizeof(*tiles));
    Sml2NativeStage stage;
    assert(map && tiles);
    for (unsigned i = 0; i < sizeof defs; ++i) defs[i] = (uint8_t)i;
    map[7] = 3;
    flags[3] = SML2_NATIVE_STAGE_TILE_SOLID;
    assert(sml2_native_stage_decode_sml2(&stage, map,
        SML2_NATIVE_SML2_MAP_BYTES, defs, sizeof defs, flags, SML2_NATIVE_SML2_BLOCK_COUNT,
        map, SML2_NATIVE_SML2_MAP_BYTES, tiles) == 0);
    assert(stage.width_blocks == 256 && stage.height_blocks == 48);
    assert(tiles[3].graphics[0] == 12 && tiles[3].graphics[3] == 15);
    assert(stage.collision_map.block_flags[3] == SML2_NATIVE_BLOCK_SOLID);
    map[8] = 0;
    {
        uint8_t *captured_cells = (uint8_t *)calloc(SML2_NATIVE_SML2_MAP_BYTES, 1);
        uint8_t captured_defs[SML2_NATIVE_SML2_BLOCKDEF_BYTES];
        Sml2NativeStageTile captured_tiles[SML2_NATIVE_SML2_BLOCK_COUNT];
        assert(captured_cells);
        assert(sml2_native_stage_capture_sml2(&stage, fake_sml2_memory, map,
            flags, sizeof flags, captured_cells, SML2_NATIVE_SML2_MAP_BYTES,
            captured_defs, sizeof captured_defs, captured_tiles) == 0);
        assert(captured_tiles[3].graphics[0] == 12);
        assert(captured_tiles[3].graphics[3] == 15);
        free(captured_cells);
    }
    map[8] = 0x80;
    assert(sml2_native_stage_decode_sml2(&stage, map,
        SML2_NATIVE_SML2_MAP_BYTES, defs, sizeof defs, flags, sizeof flags,
        map, SML2_NATIVE_SML2_MAP_BYTES, tiles) == -1);
    free(tiles);
    free(map);
}

int main(void)
{
    test_sml2_collision_ranges();
    test_sml2_layout_decode();
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
    tiles[1].collision_flags = SML2_NATIVE_STAGE_TILE_SOLID |
        SML2_NATIVE_STAGE_TILE_HAZARD | SML2_NATIVE_STAGE_TILE_PIPE;
    cells[1] = 1;
    cells[2 * 8 + 2] = 1;

    assert(sml2_native_stage_init(&stage, 8, 4, cells, tiles, 2,
                                  objects, 3, rooms, 1, 1) == 0);
    assert(stage.collision_map.block_flags[1] == tiles[1].collision_flags);
    assert(sml2_native_level_collision(&stage.collision_map,
             &(Sml2NativeRect){16 * SML2_NATIVE_FP_ONE,
                               0,
                               32 * SML2_NATIVE_FP_ONE,
                               16 * SML2_NATIVE_FP_ONE}) ==
           (SML2_NATIVE_COLLISION_SOLID |
            SML2_NATIVE_COLLISION_HAZARD |
            SML2_NATIVE_COLLISION_PIPE));

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
