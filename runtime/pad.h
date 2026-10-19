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
int  pad_is_selected_instance(int game, SDL_JoystickID which);
int  pad_is_selected_dualsense_instance(int game, SDL_JoystickID which);
int  pad_device_identity(int device, char *buf, size_t n);
int  pad_assign_device(int game, int player, int device);
void pad_sync_assignments(void);
int  pad_touchpad_event(const SDL_Event *e, float *x, float *y, int *kind);
const char *pad_status(char *buf, size_t n);

void pad_poll(int game, uint8_t *buttons, uint8_t *dpad);
/* Virtual on-screen controls feed the same Game Boy input path as keys/pads. */
void pad_set_touch_input(int game, int player, uint8_t buttons, uint8_t dpad);
void pad_poll_player(int game, int player, uint8_t *buttons, uint8_t *dpad);
/* Launcher navigation does not require a per-game controller assignment. */
void pad_poll_launcher(float *x_axis, float *y_axis, int *confirm, int *back);
int  pad_capture(int device, const SDL_Event *e);
int  pad_binding_down(int game, int action);
int  pad_binding_event(int game, int action, const SDL_Event *e);
const char *pad_code_name_device(int device, int code, char *buf, size_t n);
const char *pad_code_name(int code, char *buf, size_t n);
const char *key_code_name(int key, char *buf, size_t n);

void pad_set_context(int game, int in_game);
void pad_set_screen_color(uint32_t rgb);
void pad_event_fx(int game, int ev);
/* Preview the full event-specific haptic/light cue even if its event toggle is off. */
void pad_event_preview(int game, int ev);
void pad_frame(float dt);
void pad_rumble(int strength_pct, int ms);
void pad_rumble_selected(int game, int strength_pct, int ms);
/* Android-only vibration routing/test controls; option 0 is Auto, 1 is phone, 2+ are controller slots. */
int  pad_android_haptic_option_count(void);
int  pad_android_haptic_option_target(int option);
int  pad_android_haptic_target_option(int target);
void pad_android_haptic_option_name(int option, char *buf, size_t n);
int  pad_android_haptic_target_available(int target);
int  pad_android_haptic_test(int game, int strength_pct, int ms);
void pad_flash(uint32_t rgb);
int  pad_has_light(void);