#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PIPECLEAN_YOUGAME_PLAYING = 1,
    PIPECLEAN_YOUGAME_PAUSED = 2,
    PIPECLEAN_YOUGAME_MENU = 3
};

/* Starts the native YouGame SDK when enabled in a Windows build. */
int yougame_native_init(void);
void yougame_native_update(void);
void yougame_native_shutdown(void);
void yougame_native_set_state(int state);
void yougame_native_relay_key(const char *key);
int yougame_native_connected(void);
int yougame_native_ready(void);
int yougame_native_started_by_app(void);
int yougame_native_player_state(void);
const char *yougame_native_player_name(void);
const char *yougame_native_player_id(void);
/* True once when YouGame asks the native game to quit. */
int yougame_native_take_quit_request(void);

#ifdef __cplusplus
}
#endif
