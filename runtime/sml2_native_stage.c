#include "sml2_native_stage.h"

#include <stddef.h>
#include <string.h>

static int rect_valid(const Sml2NativeRect *r)
{
    return r && r->right > r->left && r->bottom > r->top;
}



int sml2_native_stage_capture_sml2(
    Sml2NativeStage *stage, Sml2NativeMemoryRead read_memory, void *userdata,
    const uint8_t *collision_flags, size_t collision_flags_count,
    uint8_t *cells_out, size_t cells_capacity,
    uint8_t *block_definitions_out, size_t block_definitions_capacity,
    Sml2NativeStageTile tiles_out[SML2_NATIVE_SML2_BLOCK_COUNT])
{
    unsigned i;
    if (!stage || !read_memory || !cells_out || !block_definitions_out ||
        !tiles_out || cells_capacity < SML2_NATIVE_SML2_MAP_BYTES ||
        block_definitions_capacity < SML2_NATIVE_SML2_BLOCKDEF_BYTES)
        return -1;

    for (i = 0; i < SML2_NATIVE_SML2_MAP_BYTES; ++i)
        cells_out[i] = read_memory(userdata, (uint16_t)(0xB000u + i));
    for (i = 0; i < SML2_NATIVE_SML2_BLOCKDEF_BYTES; ++i)
        block_definitions_out[i] =
            read_memory(userdata, (uint16_t)(0xA600u + i));

    return sml2_native_stage_decode_sml2(stage,
        cells_out, SML2_NATIVE_SML2_MAP_BYTES,
        block_definitions_out, SML2_NATIVE_SML2_BLOCKDEF_BYTES,
        collision_flags, collision_flags_count,
        cells_out, cells_capacity, tiles_out);
}

int sml2_native_stage_decode_sml2(
    Sml2NativeStage *stage,
    const uint8_t *level_map, size_t level_map_size,
    const uint8_t *block_definitions, size_t block_definitions_size,
    const uint8_t *collision_flags, size_t collision_flags_count,
    uint8_t *cells_out, size_t cells_capacity,
    Sml2NativeStageTile tiles_out[SML2_NATIVE_SML2_BLOCK_COUNT])
{
    unsigned i;
    if (!stage || !level_map || !block_definitions || !cells_out ||
        !tiles_out || level_map_size < SML2_NATIVE_SML2_MAP_BYTES ||
        block_definitions_size < SML2_NATIVE_SML2_BLOCKDEF_BYTES ||
        cells_capacity < SML2_NATIVE_SML2_MAP_BYTES ||
        (collision_flags && collision_flags_count < SML2_NATIVE_SML2_BLOCK_COUNT) ||
        (!collision_flags && collision_flags_count != 0))
        return -1;

    /* Reject map flag bits instead of silently treating them as a block ID.
       SML2's ordinary gameplay map currently exposes 7-bit metatile indices. */
    for (i = 0; i < SML2_NATIVE_SML2_MAP_BYTES; ++i)
        if (level_map[i] >= SML2_NATIVE_SML2_BLOCK_COUNT) return -1;

    memmove(cells_out, level_map, SML2_NATIVE_SML2_MAP_BYTES);
    memset(tiles_out, 0,
           sizeof(Sml2NativeStageTile) * SML2_NATIVE_SML2_BLOCK_COUNT);
    for (i = 0; i < SML2_NATIVE_SML2_BLOCK_COUNT; ++i) {
        unsigned part;
        for (part = 0; part < 4; ++part)
            tiles_out[i].graphics[part] =
                block_definitions[i * 4u + part];
        if (collision_flags)
            tiles_out[i].collision_flags = collision_flags[i];
    }

    return sml2_native_stage_init(stage,
        SML2_NATIVE_SML2_STAGE_WIDTH_BLOCKS,
        SML2_NATIVE_SML2_STAGE_HEIGHT_BLOCKS,
        cells_out, tiles_out, SML2_NATIVE_SML2_BLOCK_COUNT,
        NULL, 0, NULL, 0, 1);
}

int sml2_native_stage_init(Sml2NativeStage *stage,
                          uint16_t width_blocks, uint16_t height_blocks,
                          const uint8_t *cells,
                          const Sml2NativeStageTile *tiles, uint16_t tile_count,
                          const Sml2NativeStageObject *objects, uint16_t object_count,
                          const Sml2NativeStageRoom *rooms, uint16_t room_count,
                          uint8_t solid_out_of_bounds)
{
    uint32_t cell_count, cell_i;
    uint16_t i;
    if (!stage || !width_blocks || !height_blocks ||\n        (uint32_t)width_blocks * 16u > 32767u ||\n        (uint32_t)height_blocks * 16u > 32767u || !cells ||
        !tiles || !tile_count || tile_count > SML2_NATIVE_STAGE_MAX_TILES ||
        (object_count && !objects) || (room_count && !rooms) ||
        room_count > SML2_NATIVE_STAGE_MAX_ROOMS ||
        solid_out_of_bounds > 1)
        return -1;

    cell_count = (uint32_t)width_blocks * height_blocks;
    for (cell_i = 0; cell_i < cell_count; ++cell_i)
        if (cells[cell_i] >= tile_count) return -1;

    for (i = 0; i < object_count; ++i) {
        const Sml2NativeStageObject *o = &objects[i];
        if (!o->width || !o->height || o->x < 0 || o->y < 0 ||
            (int64_t)o->x + o->width > (int64_t)width_blocks * 16 ||
            (int64_t)o->y + o->height > (int64_t)height_blocks * 16)
            return -1;
    }

    for (i = 0; i < room_count; ++i) {
        const Sml2NativeCameraRegion *r = &rooms[i].bounds;
        if (r->left < 0 || r->top < 0 || r->right <= r->left ||
            r->bottom <= r->top || r->right > (int32_t)width_blocks * 16 ||
            r->bottom > (int32_t)height_blocks * 16)
            return -1;
    }

    memset(stage, 0, sizeof(*stage));
    stage->width_blocks = width_blocks;
    stage->height_blocks = height_blocks;
    stage->cells = cells;
    stage->tiles = tiles;
    stage->tile_count = tile_count;
    stage->objects = objects;
    stage->object_count = object_count;
    stage->rooms = rooms;
    stage->room_count = room_count;
    stage->solid_out_of_bounds = solid_out_of_bounds;
    stage->collision_map.width_blocks = width_blocks;
    stage->collision_map.height_blocks = height_blocks;
    stage->collision_map.block_ids = cells;
    stage->collision_map.solid_out_of_bounds = solid_out_of_bounds;
    for (i = 0; i < tile_count; ++i) {
        if (tiles[i].collision_flags & SML2_NATIVE_STAGE_TILE_SOLID)
            stage->collision_map.block_flags[i] = SML2_NATIVE_BLOCK_SOLID;
    }
    return 0;
}

int sml2_native_stage_visit_draw_cells(const Sml2NativeStage *stage,
                                       const Sml2NativeBlockBounds *bounds,
                                       Sml2NativeStageDrawVisitor visitor,
                                       void *userdata)
{
    uint16_t x, y;
    if (!stage || !bounds || !visitor || !stage->cells || !stage->tiles ||
        bounds->left > bounds->right || bounds->top > bounds->bottom ||
        bounds->right > stage->width_blocks ||
        bounds->bottom > stage->height_blocks)
        return -1;

    for (y = bounds->top; y < bounds->bottom; ++y) {
        for (x = bounds->left; x < bounds->right; ++x) {
            Sml2NativeStageDrawCell cell;
            cell.block_x = x;
            cell.block_y = y;
            cell.world_x = (int32_t)x * 16;
            cell.world_y = (int32_t)y * 16;
            cell.tile_id = stage->cells[(uint32_t)y * stage->width_blocks + x];
            cell.tile = &stage->tiles[cell.tile_id];
            {
                int result = visitor(userdata, &cell);
                if (result) return result;
            }
        }
    }
    return 0;
}

int sml2_native_stage_visit_objects(const Sml2NativeStage *stage,
                                    const Sml2NativeRect *world_bounds,
                                    Sml2NativeStageObjectVisitor visitor,
                                    void *userdata)
{
    uint16_t i;
    if (!stage || !rect_valid(world_bounds) || !visitor ||
        (stage->object_count && !stage->objects))
        return -1;

    for (i = 0; i < stage->object_count; ++i) {
        const Sml2NativeStageObject *o = &stage->objects[i];
        int64_t right, bottom;
        int result;
        if (!(o->flags & SML2_NATIVE_STAGE_OBJECT_ENABLED)) continue;
        right = (int64_t)o->x + o->width;
        bottom = (int64_t)o->y + o->height;
        if (right <= world_bounds->left || o->x >= world_bounds->right ||
            bottom <= world_bounds->top || o->y >= world_bounds->bottom)
            continue;
        result = visitor(userdata, o);
        if (result) return result;
    }
    return 0;
}

int sml2_native_stage_get_room(const Sml2NativeStage *stage, uint16_t room_id,
                               Sml2NativeCameraRegion *region)
{
    uint16_t i;
    if (!stage || !region || (stage->room_count && !stage->rooms)) return -1;
    for (i = 0; i < stage->room_count; ++i) {
        if (stage->rooms[i].id == room_id) {
            *region = stage->rooms[i].bounds;
            return 0;
        }
    }
    return -1;
}
