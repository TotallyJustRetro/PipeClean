#pragma once

#include "sml2_native.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SML2_NATIVE_STAGE_MAX_TILES 256
#define SML2_NATIVE_STAGE_MAX_ROOMS 64

enum {
    SML2_NATIVE_STAGE_TILE_SOLID = SML2_NATIVE_COLLISION_SOLID,
    SML2_NATIVE_STAGE_TILE_STANDABLE = SML2_NATIVE_COLLISION_STANDABLE,
    SML2_NATIVE_STAGE_TILE_HAZARD = SML2_NATIVE_COLLISION_HAZARD,
    SML2_NATIVE_STAGE_TILE_COIN = SML2_NATIVE_COLLISION_COIN,
    SML2_NATIVE_STAGE_TILE_EXIT = SML2_NATIVE_COLLISION_EXIT,
    SML2_NATIVE_STAGE_TILE_WATER = SML2_NATIVE_COLLISION_WATER,
    SML2_NATIVE_STAGE_TILE_MOON = SML2_NATIVE_COLLISION_MOON,
    SML2_NATIVE_STAGE_TILE_HIDDEN = SML2_NATIVE_COLLISION_HIDDEN,
    SML2_NATIVE_STAGE_TILE_BREAKABLE = SML2_NATIVE_COLLISION_BREAKABLE,
    SML2_NATIVE_STAGE_TILE_PIPE = SML2_NATIVE_COLLISION_PIPE,
    SML2_NATIVE_STAGE_TILE_BOUNCE = SML2_NATIVE_COLLISION_BOUNCE,
    SML2_NATIVE_STAGE_TILE_ONE_WAY = SML2_NATIVE_COLLISION_ONE_WAY
};

enum {
    SML2_NATIVE_MATERIAL_SOLID = 1,
    SML2_NATIVE_MATERIAL_SLOPE,
    SML2_NATIVE_MATERIAL_COIN,
    SML2_NATIVE_MATERIAL_EXIT,
    SML2_NATIVE_MATERIAL_WATER,
    SML2_NATIVE_MATERIAL_HAZARD,
    SML2_NATIVE_MATERIAL_MOON,
    SML2_NATIVE_MATERIAL_DECOR,
    SML2_NATIVE_MATERIAL_HIDDEN_BLOCK
};

enum {
    SML2_NATIVE_STAGE_OBJECT_ENABLED = 1u << 0,
    SML2_NATIVE_STAGE_OBJECT_UNIQUE = 1u << 1,
    SML2_NATIVE_STAGE_OBJECT_PERSISTENT = 1u << 2,
    SML2_NATIVE_STAGE_OBJECT_BOSS = 1u << 3,
    SML2_NATIVE_STAGE_OBJECT_PROJECTILE = 1u << 4
};

/* One 16x16 metatile rendered as four 8x8 graphics cells. Graphics IDs are
 * renderer-owned indices; this module never assumes a particular SDL/PPU. */
typedef struct {
    uint16_t graphics[4]; /* top-left, top-right, bottom-left, bottom-right */
    uint16_t collision_flags;
    uint8_t material;
} Sml2NativeStageTile;

typedef struct {
    uint16_t id;
    uint16_t kind;
    uint16_t subtype;
    int32_t x;
    int32_t y;
    uint16_t width;
    uint16_t height;
    uint8_t flags;
} Sml2NativeStageObject;

typedef struct {
    uint16_t id;
    Sml2NativeCameraRegion bounds;
} Sml2NativeStageRoom;

typedef struct {
    uint16_t width_blocks;
    uint16_t height_blocks;
    const uint8_t *cells;
    const Sml2NativeStageTile *tiles;
    uint16_t tile_count;
    const Sml2NativeStageObject *objects;
    uint16_t object_count;
    const Sml2NativeStageRoom *rooms;
    uint16_t room_count;
    uint8_t solid_out_of_bounds;
    Sml2NativeTileMap collision_map;
} Sml2NativeStage;

typedef struct {
    uint16_t block_x;
    uint16_t block_y;
    int32_t world_x;
    int32_t world_y;
    uint8_t tile_id;
    const Sml2NativeStageTile *tile;
} Sml2NativeStageDrawCell;

typedef int (*Sml2NativeStageDrawVisitor)(
    void *userdata, const Sml2NativeStageDrawCell *cell);
typedef int (*Sml2NativeStageObjectVisitor)(
    void *userdata, const Sml2NativeStageObject *object);

/* Validate immutable extracted stage data and prepare its collision adapter.
 * All arrays remain caller-owned and must outlive the stage. */
int sml2_native_stage_init(Sml2NativeStage *stage,
                          uint16_t width_blocks, uint16_t height_blocks,
                          const uint8_t *cells,
                          const Sml2NativeStageTile *tiles, uint16_t tile_count,
                          const Sml2NativeStageObject *objects, uint16_t object_count,
                          const Sml2NativeStageRoom *rooms, uint16_t room_count,
                          uint8_t solid_out_of_bounds);

/* Return the exact cells in a half-open block range. A visitor may return
 * nonzero to stop traversal; the function returns that value unchanged. */
int sml2_native_stage_visit_draw_cells(const Sml2NativeStage *stage,
                                       const Sml2NativeBlockBounds *bounds,
                                       Sml2NativeStageDrawVisitor visitor,
                                       void *userdata);

/* Visit enabled object spawn records whose world-space rectangles intersect
 * the requested half-open bounds. No objects are copied or mutated. */
int sml2_native_stage_visit_objects(const Sml2NativeStage *stage,
                                    const Sml2NativeRect *world_bounds,
                                    Sml2NativeStageObjectVisitor visitor,
                                    void *userdata);

/* SML2 gameplay stages use a 256x48 grid of 16x16 blocks. The live
 * decompressed map and four-8x8-tile block definitions can be copied into
 * caller-owned buffers, then decoded here without emulator dependencies. */
#define SML2_NATIVE_SML2_STAGE_WIDTH_BLOCKS 256
#define SML2_NATIVE_SML2_STAGE_HEIGHT_BLOCKS 48
#define SML2_NATIVE_SML2_BLOCK_COUNT 128
#define SML2_NATIVE_SML2_MAP_BYTES (256u * 48u)
#define SML2_NATIVE_SML2_BLOCKDEF_BYTES (128u * 4u)

typedef uint8_t (*Sml2NativeMemoryRead)(void *userdata, uint16_t address);

/* Capture the currently decompressed SML2 gameplay stage through a read-only
 * memory callback. This snapshots $B000-$DFFF and $A600-$A7FF into caller-owned
 * buffers, then initializes the native stage from that stable snapshot. */
int sml2_native_stage_capture_sml2(
    Sml2NativeStage *stage, Sml2NativeMemoryRead read_memory, void *userdata,
    const uint16_t *collision_flags, size_t collision_flags_count,
    uint8_t *cells_out, size_t cells_capacity,
    uint8_t *block_definitions_out, size_t block_definitions_capacity,
    Sml2NativeStageTile tiles_out[SML2_NATIVE_SML2_BLOCK_COUNT]);

/* SML2's global metatile-ID ranges classify terrain independent of level.
 * These helpers include special IDs for pipes, breakables, bounce blocks,
 * and the hidden block; slope geometry still requires its dedicated profile. */
uint8_t sml2_native_sml2_material(uint8_t metatile_id);
uint16_t sml2_native_sml2_collision_flags(uint8_t metatile_id);

int sml2_native_stage_decode_sml2(
    Sml2NativeStage *stage,
    const uint8_t *level_map, size_t level_map_size,
    const uint8_t *block_definitions, size_t block_definitions_size,
    const uint8_t *collision_flags, size_t collision_flags_count,
    uint8_t *cells_out, size_t cells_capacity,
    Sml2NativeStageTile tiles_out[SML2_NATIVE_SML2_BLOCK_COUNT]);

/* Resolve a room ID to camera bounds. Returns -1 when the room is absent. */
int sml2_native_stage_get_room(const Sml2NativeStage *stage, uint16_t room_id,
                               Sml2NativeCameraRegion *region);

#ifdef __cplusplus
}
#endif
