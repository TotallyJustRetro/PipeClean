#pragma once

#include <SDL.h>
#include "settings.h"
#include "emu.h"

/* Desktop-only second-window simulator. Android uses the native Presentation. */
void companion_sim_sync(const Settings *settings, SDL_Window *main_window);
void companion_sim_set_state(int game, const Frame *frame, int paused, int selected_slot);
void companion_sim_render(const Settings *settings, SDL_Window *main_window);
int companion_sim_handle_event(const SDL_Event *event, Settings *settings);
void companion_sim_shutdown(void);
