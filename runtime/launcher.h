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
int  launcher_current_game(void);
int  launcher_take_load_state(int game);
int  launcher_prepare(int game, char *err, size_t n);
void launcher_shutdown(void);
void launcher_set_renderer(SDL_Renderer *r);