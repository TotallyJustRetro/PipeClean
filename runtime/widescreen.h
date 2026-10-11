#pragma once
/* Widescreen: how much extra picture a game can show, and the small in-memory ROM patches
 * that keep enemies and level data correct in the extra space. The ROM file is never changed. */
#include "games.h"

/* pixels to add left and right for a slider value (0..100) */
void wide_dims(int game, int pct, int *left, int *right);
/* patch the loaded ROM for these dimensions. 1 = ok / nothing needed, 0 = this ROM doesn't match (use the normal screen) */
int  wide_install(int game, int left, int right);
/* Interpreter hook used by SML2 entity activation. Returns 1 when it handled op. */
int wide_intercept_sml2(uint8_t op);
/* Override SML2 enemy-spawn scan bounds at their verified bank-2 read sites. */
uint8_t wide_read_sml2(uint16_t address, uint8_t value);
/* Clear per-frame SML2 widescreen scanner state after a save-state/rewind load. */
void wide_state_reset(void);
/* Live block-map helpers used to build seamless Super Mario Land 2 margins. */
int wide_sml2_tile_at(int world_x, int world_y, uint8_t *tile);
void wide_sml2_prepare_frame(int left, int right, int scx, int scy,
                             int lcdc, int wx, int wy);
int wide_sml2_margin_tile(int screen_x, int screen_y, uint8_t *tile,
                          int *world_x, int *world_y);
