#pragma once

#include <SDL.h>
#include <stdint.h>
#include "emu.h"

/* Live multiplayer telemetry exists only in PipeClean Dev builds. The regular
 * launcher links the same API to no-op implementations. */
void dev_diag_begin(int game, int player_count, SDL_Window *game_window);
void dev_diag_event(const char *message);
void dev_diag_frame(const Frame *frame, int game, int player_count,
                    const uint8_t buttons[MAX_MP_PLAYERS],
                    const uint8_t dpad[MAX_MP_PLAYERS], int paused);
/* Called after the game UI is drawn and before the renderer presents. */
void dev_diag_capture_screen(SDL_Renderer *game_renderer);
void dev_diag_end(const char *reason);
int dev_diag_handle_event(const SDL_Event *event);
