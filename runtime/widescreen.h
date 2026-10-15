#pragma once
/* Widescreen: how much extra picture a game can show, and the small in-memory ROM patches
 * that keep enemies and level data correct in the extra space. The ROM file is never changed. */
#include <stdint.h>
#include "games.h"

typedef struct {
    int16_t x, y;
    uint8_t tile, attr;
} WideSml2Sprite;

/* pixels to add left and right for a slider value (0..100) */
void wide_dims(int game, int pct, int *left, int *right);
/* Variant-aware sizing/install hooks: the Wario DX profile needs its own tested bounds. */
void wide_dims_profile(int game, int pct, int wario_dx, int *left, int *right);
int  wide_install_profile(int game, int left, int right, int wario_dx);
int  wide_is_wario_dx_profile(const char *title, const char *path);
/* patch the loaded ROM for these dimensions. 1 = ok / nothing needed, 0 = this ROM doesn't match (use the normal screen) */
int  wide_install(int game, int left, int right);
/* Wario Land II activation-bound diagnostics (active only with its widescreen profile). */
extern int wide_wl2_trace_active;
void wide_wl2_trace_instruction(uint16_t pc);
void wide_wl2_trace_frame(const uint8_t *oam_snapshot);
/* Read the full 8-bit Wario Land II 16x16 block ID from its swizzled level map. */
int wide_wl2_block_at(int world_x, int world_y, uint8_t *block);
/* Read a 4-tile definition from the verified ROM metatile table. */
int wide_wl2_block_tile(int block, int part, uint8_t *tile);
void wide_wl2_trace_finish(void);
void wide_wl2_trace_set_path(const char *path);
/* Interpreter hook used by SML2 entity activation. Returns 1 when it handled op. */
int wide_intercept_sml2(uint8_t op);
/* Override SML2 enemy-spawn scan bounds at their verified bank-2 read sites. */
uint8_t wide_read_sml2(uint16_t address, uint8_t value);
/* Clear per-frame SML2 widescreen scanner state after a save-state/rewind load. */
void wide_state_reset(void);
/* Live block-map helpers used to build seamless Super Mario Land 2 margins. */
int wide_sml2_tile_at(int world_x, int world_y, uint8_t *tile);
/* Scene check and map-edge-aware margin allocation for the wider Homeworld canvas. */
int wide_sml2_homeworld_active(int lcdc, int wx, int wy);
void wide_sml2_homeworld_dims(int screen_left, int extra_total,
                              int *left, int *right);
void wide_sml2_homeworld_dims_from_camera(int scx, int extra_total,
                                          int *left, int *right);
/* Full-map Homeworld overview source and live camera origin. */
int wide_sml2_homeworld_tile_at(int world_x, int world_y, uint8_t *tile);
int wide_sml2_homeworld_camera(int scx, int scy, int *camera_x, int *camera_y);
void wide_sml2_prepare_frame(int left, int right, int scx, int scy,
                             int lcdc, int wx, int wy, const uint8_t *tilemap);
int wide_sml2_margin_tile(int screen_x, int screen_y, uint8_t *tile,
                          int *world_x, int *world_y);
uint8_t wide_sml2_read_tap(uint16_t address, uint8_t value);
void wide_sml2_snapshot_actors(void);
const WideSml2Sprite *wide_sml2_actor_sprites(int *count);
int wide_sml2_world_origin(int *world_x, int *world_y);
