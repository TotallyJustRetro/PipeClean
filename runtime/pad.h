#pragma once
#include <SDL.h>
#include <stdint.h>

void pad_init(void);
void pad_shutdown(void);
void pad_event(const SDL_Event *e);
int  pad_count(void);
const char *pad_name(int i);
int  pad_is_dualsense(int i);
int  pad_is_edge(int i);
int  pad_is_dualsense_instance(SDL_JoystickID which);
int  pad_touchpad_event(const SDL_Event *e, float *x, float *y, int *kind);
const char *pad_status(char *buf, size_t n);

void pad_poll(int game, uint8_t *buttons, uint8_t *dpad);
void pad_poll_player(int game, int player, uint8_t *buttons, uint8_t *dpad);
int  pad_capture(const SDL_Event *e);
int  pad_binding_down(int game, int action);
int  pad_binding_event(int game, int action, const SDL_Event *e);
const char *pad_code_name(int code, char *buf, size_t n);
const char *key_code_name(int key, char *buf, size_t n);

void pad_set_context(int game, int in_game);
void pad_set_screen_color(uint32_t rgb);
void pad_event_fx(int game, int ev);
void pad_frame(float dt);
void pad_rumble(int strength_pct, int ms);
void pad_flash(uint32_t rgb);
int  pad_has_light(void);