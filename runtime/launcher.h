#pragma once
#include <SDL.h>
typedef struct { int play; int quit; } LauncherResult;

void launcher_init(void);
void launcher_enter(void);
LauncherResult launcher_frame(float dt);
void launcher_event(const SDL_Event *e);
int  launcher_capturing(void);
void launcher_toast(const char *msg);
void launcher_drop(const char *path);

/* Android Companion Deck custom-layout bridge. Picker kinds are defined by the
 * launcher and results are routed back through the SDL event loop. */
void launcher_open_companion_layout_picker(int kind, int game);
void launcher_companion_layout_picker_result(int kind, int game, const char *path);
int  launcher_current_game(void);
int  launcher_take_load_state(int game);
int  launcher_prepare(int game, char *err, size_t n);
/* Stable per-profile key used to keep save states and battery saves separate. */
void launcher_game_file_id(int game, char *out, size_t n);
void launcher_shared_slot_paths(int game, int slot,
                                char *state_path, size_t state_size,
                                char *marker_path, size_t marker_size);
int launcher_shared_slot_is_saved(int game, int slot);
void launcher_shared_slot_name_get(int game, int slot, char *out, size_t out_size);
int launcher_shared_slot_name_set(int game, int slot, const char *name);
void launcher_shutdown(void);
void launcher_set_renderer(SDL_Renderer *r);
/* Companion dashboard and menu-music actions shared with the Android host. */
int launcher_preview_pixels(int game, const uint32_t **pixels, int *w, int *h);
void launcher_menu_music_toggle(void);
int launcher_music_notification_serial(void);
int launcher_music_notification_allowed(void);
void launcher_menu_music_step(int delta);
void launcher_menu_music_select(int index);