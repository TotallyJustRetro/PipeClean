#pragma once

/*
 * PipeClean's independently authored native SML2 simulation foundation.
 *
 * Positions and velocities use signed 16.16 fixed-point pixels. The engine
 * advances exactly one deterministic simulation tick per sml2_native_step().
 * Collision geometry is supplied by the game-world layer; this keeps the
 * simulation independent from SDL, the Game Boy emulator, and rendering.
 *
 * This is a new gameplay backend foundation, not a complete SML2 replacement
 * yet. Movement tuning remains provisional until measured against the game.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SML2_NATIVE_MAX_PLAYERS 4
#define SML2_NATIVE_FP_SHIFT 16
#define SML2_NATIVE_FP_ONE ((int32_t)1 << SML2_NATIVE_FP_SHIFT)

enum {
    SML2_NATIVE_BUTTON_LEFT   = 1u << 0,
    SML2_NATIVE_BUTTON_RIGHT  = 1u << 1,
    SML2_NATIVE_BUTTON_DOWN   = 1u << 2,
    SML2_NATIVE_BUTTON_UP     = 1u << 3,
    SML2_NATIVE_BUTTON_JUMP   = 1u << 4,
    SML2_NATIVE_BUTTON_ACTION = 1u << 5,
    SML2_NATIVE_BUTTON_START  = 1u << 6,
    SML2_NATIVE_BUTTON_SELECT = 1u << 7
};

enum {
    SML2_NATIVE_COLLISION_NONE       = 0,
    SML2_NATIVE_COLLISION_SOLID      = 1u << 0,
    SML2_NATIVE_COLLISION_STANDABLE  = 1u << 1,
    SML2_NATIVE_COLLISION_HAZARD     = 1u << 2,
    SML2_NATIVE_COLLISION_COIN       = 1u << 3,
    SML2_NATIVE_COLLISION_EXIT       = 1u << 4,
    SML2_NATIVE_COLLISION_WATER      = 1u << 5,
    SML2_NATIVE_COLLISION_MOON       = 1u << 6,
    SML2_NATIVE_COLLISION_HIDDEN     = 1u << 7,
    SML2_NATIVE_COLLISION_BREAKABLE  = 1u << 8,
    SML2_NATIVE_COLLISION_PIPE       = 1u << 9,
    SML2_NATIVE_COLLISION_BOUNCE     = 1u << 10,
    SML2_NATIVE_COLLISION_ONE_WAY    = 1u << 11
};

/* Block-map flags use the same bit values as the collision result mask. */
enum {
    SML2_NATIVE_BLOCK_SOLID = SML2_NATIVE_COLLISION_SOLID
};

enum {
    SML2_NATIVE_WIDESCREEN_OFF = 0,
    SML2_NATIVE_WIDESCREEN_16_9 = 1,
    SML2_NATIVE_WIDESCREEN_21_9 = 2,
    SML2_NATIVE_WIDESCREEN_WINDOW = 3
};

enum {
    SML2_NATIVE_ZOOM_100 = 100,
    SML2_NATIVE_ZOOM_125 = 125,
    SML2_NATIVE_ZOOM_150 = 150,
    SML2_NATIVE_ZOOM_200 = 200
};

typedef struct {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} Sml2NativeRect;

/* Camera lock rectangle in world pixels; right/bottom are exclusive. */
typedef struct {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} Sml2NativeCameraRegion;

/* Data-only collision map. One cell is a 16×16 world-pixel block; block IDs
 * select flags below. Render tiles/graphics can be stored separately. */
typedef struct {
    uint16_t width_blocks;
    uint16_t height_blocks;
    const uint8_t *block_ids;
    uint16_t block_flags[256];
    uint8_t solid_out_of_bounds;
} Sml2NativeTileMap;

/* Half-open tile coordinates for a visible/prefetch stage rectangle. */
typedef struct {
    uint16_t left;
    uint16_t top;
    uint16_t right;
    uint16_t bottom;
} Sml2NativeBlockBounds;

typedef struct {
    uint8_t buttons;
} Sml2NativeInput;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t vx;
    int32_t vy;
    int32_t width;
    int32_t height;
    uint16_t invulnerability_ticks;
    uint8_t lives;
    uint8_t powerup;
    uint8_t active;
    uint8_t facing_right;
    uint8_t grounded;
    uint8_t jump_was_down;
} Sml2NativePlayer;

typedef struct {
    int32_t level_width;
    int32_t level_height;
    int32_t gravity;
    int32_t horizontal_acceleration;
    int32_t horizontal_friction;
    int32_t max_run_speed;
    int32_t jump_velocity;
    int32_t terminal_velocity;
    int32_t player_width;
    int32_t player_height;
    int32_t camera_padding;
    uint16_t window_width;
    uint16_t window_height;
    uint8_t widescreen_mode;
    uint8_t camera_zoom_percent;
    uint8_t max_camera_zoom_percent;
    uint8_t auto_zoom;
} Sml2NativeConfig;

typedef struct {
    Sml2NativeConfig config;
    Sml2NativePlayer players[SML2_NATIVE_MAX_PLAYERS];
    Sml2NativeCameraRegion camera_region;
    uint64_t tick;
    int32_t camera_x;
    int32_t camera_y;
    int32_t camera_view_width;
    int32_t camera_view_height;
    uint8_t camera_zoom_percent;
    uint8_t player_count;
} Sml2NativeWorld;

/* Return a bitmask of SML2_NATIVE_COLLISION_* flags for a world-space AABB.
 * Rect coordinates are signed 16.16 fixed-point pixels, right/bottom exclusive.
 * The callback must be deterministic and must not mutate the simulation. */
typedef uint32_t (*Sml2NativeCollisionQuery)(
    void *userdata, const Sml2NativeRect *rect);

/* Helpers for constructing fixed-point values without floating point. */
int32_t sml2_native_fp_from_int(int32_t pixels);
int32_t sml2_native_fp_to_int(int32_t value);

/* Calculate the logical world-pixel view dimensions for a display mode and
 * zoom preset. The window dimensions are used only in WINDOW mode. Returns
 * 0 on success and -1 for an invalid mode or zoom value. */
int sml2_native_view_size(uint8_t widescreen_mode, uint8_t zoom_percent,
                          uint16_t window_width, uint16_t window_height,
                          int32_t *view_width, int32_t *view_height);

/* Change view settings at runtime (for live display/window setting changes).
 * The camera is recomputed on the next simulation tick. */
int sml2_native_set_view(Sml2NativeWorld *world, uint8_t widescreen_mode,
                         uint8_t zoom_percent, uint16_t window_width,
                         uint16_t window_height, int auto_zoom);

/* Set the current camera-lock room. Bounds must lie inside the level.
 * Passing NULL restores the full-level camera region. */
int sml2_native_set_camera_region(Sml2NativeWorld *world,
                                  const Sml2NativeCameraRegion *region);

/* Shared camera helpers. World coordinates and returned view coordinates are
 * signed 16.16 fixed-point pixels. Rect visibility uses the full logical view,
 * so scenery, sprites, and enemy activation can share one widescreen boundary. */
int sml2_native_world_to_view(const Sml2NativeWorld *world,
                              int32_t world_x, int32_t world_y,
                              int32_t *view_x, int32_t *view_y);
/* Return the camera's world-space view rectangle, expanded by an optional
 * pixel margin. Use margin=0 for rendering/culling and a positive margin for
 * pre-activating actors near newly visible edges. Returns -1 on invalid input. */
int sml2_native_view_bounds(const Sml2NativeWorld *world, int32_t margin_pixels,
                            Sml2NativeRect *world_bounds);
/* Convert the camera view (plus an optional pixel margin) into a clamped
 * half-open range of 16x16 stage blocks suitable for tile rendering/prefetch.
 * Returns -1 for invalid inputs. */
int sml2_native_view_block_bounds(const Sml2NativeWorld *world,
                                  const Sml2NativeTileMap *map,
                                  int32_t margin_pixels,
                                  Sml2NativeBlockBounds *block_bounds);

int sml2_native_view_contains_rect(const Sml2NativeWorld *world,
                                   const Sml2NativeRect *world_rect);

/* Collision callback for Sml2NativeTileMap. Rectangles are half-open 16.16
 * world-pixel bounds; map block IDs are resolved through block_flags[]. */
uint32_t sml2_native_level_collision(void *userdata,
                                     const Sml2NativeRect *world_rect);

/* Original SML2 ROM world addressing. Coordinates are 16-bit pixel
 * positions; the game masks each high byte to 4 bits (12-bit wrap). The
 * returned cell coordinates are 16x16 metatile indices. Rows $30-$4D read
 * the WRAM echo at rows $10-$2D. Returns 0 for a mapped stage cell, 1 when
 * the row is beyond the known map/echo window, and -1 for invalid pointers. */
int sml2_native_rom_world_cell(uint16_t world_x, uint16_t world_y,
                               uint16_t *block_x, uint16_t *block_y);

/* Collision query using the original ROM's 12-bit coordinate wrap and WRAM
 * echo behavior. Requires the 256x48 SML2 gameplay map profile. */
uint32_t sml2_native_rom_level_collision(void *userdata,
                                         const Sml2NativeRect *world_rect);

/* Initialize with explicit movement/camera tuning. No ROM or game assets are
 * read by this layer. Returns 0 on success and -1 for invalid arguments. */
int sml2_native_init(Sml2NativeWorld *world, const Sml2NativeConfig *config);

/* Spawn or reset a player at integral world-pixel coordinates. */
int sml2_native_spawn_player(Sml2NativeWorld *world, unsigned player_index,
                             int32_t x_pixels, int32_t y_pixels,
                             uint8_t lives);

/* Advance one fixed simulation tick for all active players simultaneously.
 * Inputs are indexed by player number; a NULL input array means no buttons.
 * The collision callback may be NULL for an unobstructed test world. */
int sml2_native_step(Sml2NativeWorld *world,
                     const Sml2NativeInput inputs[SML2_NATIVE_MAX_PLAYERS],
                     Sml2NativeCollisionQuery collision_query,
                     void *collision_userdata);

#ifdef __cplusplus
}
#endif
