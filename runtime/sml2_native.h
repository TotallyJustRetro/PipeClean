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
    SML2_NATIVE_COLLISION_NONE  = 0,
    SML2_NATIVE_COLLISION_SOLID = 1u << 0
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
