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

/* Called from yg_update on the PipeClean main thread. result_json is callback-scoped. */
typedef void (*YouGameNativeReplyCallback)(void *user, long long request_id,
    const char *type, int ok, const char *code, const char *error,
    const char *result_json);
typedef void (*YouGameNativePushCallback)(void *user, const char *type,
    const char *json);
typedef void (*YouGameNativeLineCallback)(void *user, const char *line,
    size_t length);

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
/* Generic native broker bridge. All callbacks run during yougame_native_update(). */
long long yougame_native_request(const char *type, const char *body_json,
    double timeout_seconds, YouGameNativeReplyCallback callback, void *user);
void yougame_native_set_push_handler(YouGameNativePushCallback callback, void *user);
void yougame_native_set_line_handler(YouGameNativeLineCallback callback, void *user);
long long yougame_native_next_request_id(void);
int yougame_native_write_line(const char *line, size_t length);

/* True once when YouGame asks the native game to quit. */
int yougame_native_take_quit_request(void);

#ifdef __cplusplus
}
#endif
