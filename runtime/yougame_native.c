#include "yougame_native.h"

#if defined(_WIN32) && defined(PIPECLEAN_YOUGAME_NATIVE)

#define YOUGAME_IMPLEMENTATION
#include "third_party/yougame/yougame.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef PIPECLEAN_YOUGAME_SLUG
#define PIPECLEAN_YOUGAME_SLUG ""
#endif

static int g_yougame_initialized;
static volatile LONG g_yougame_quit_requested;

typedef struct YouGamePendingCall {
    long long request_id;
    YouGameNativeReplyCallback callback;
    void *user;
    struct YouGamePendingCall *next;
} YouGamePendingCall;

static YouGamePendingCall *g_yougame_pending_calls;
static YouGameNativePushCallback g_yougame_push_callback;
static void *g_yougame_push_user;
static YouGameNativeLineCallback g_yougame_line_callback;
static void *g_yougame_line_user;

static void yougame_log(void *user, int level, const char *message)
{
    (void)user;
    if (!message || !message[0]) return;
    fprintf(stderr, "[PipeClean/YouGame] %s%s\n",
            level == YG_LOG_WARN ? "warning: " : "", message);
}

static void yougame_ready(void *user)
{
    (void)user;
    fprintf(stderr, "[PipeClean/YouGame] Native SDK handshake completed.\n");
}

static void yougame_player_arrived(void *user)
{
    (void)user;
    fprintf(stderr, "[PipeClean/YouGame] Player identity is available.\n");
}

static void yougame_quit(void *user)
{
    (void)user;
    InterlockedExchange(&g_yougame_quit_requested, 1);
}

static void yougame_push(void *user, const char *type, const char *json)
{
    (void)user;
    if (g_yougame_push_callback)
        g_yougame_push_callback(g_yougame_push_user, type ? type : "",
                                json ? json : "");
}

static void yougame_line(void *user, const char *line, size_t len)
{
    (void)user;
    if (g_yougame_line_callback)
        g_yougame_line_callback(g_yougame_line_user, line ? line : "", len);
}

static void yougame_call_finished(void *user, const yg_result *result)
{
    YouGamePendingCall *pending = (YouGamePendingCall *)user;
    YouGamePendingCall **link = &g_yougame_pending_calls;
    if (!pending) return;

    while (*link && *link != pending) link = &(*link)->next;
    if (*link == pending) *link = pending->next;

    if (pending->callback && result) {
        pending->callback(pending->user, result->id ? result->id : pending->request_id,
            result->type ? result->type : "", result->ok,
            result->code ? result->code : "", result->error ? result->error : "",
            result->result_json ? result->result_json : "");
    }
    free(pending);
}

int yougame_native_init(void)
{
    yg_config config = {0};
    const char *slug = getenv("YOUGAME_GAME");

    if (!slug || !slug[0]) slug = PIPECLEAN_YOUGAME_SLUG;
    config.game_slug = (slug && slug[0]) ? slug : NULL;
    config.game_version = "0.1.0";
    config.engine = "PipeClean";
    config.log = yougame_log;
    config.on_ready = yougame_ready;
    config.on_player = yougame_player_arrived;
    config.on_quit = yougame_quit;
    config.on_push = yougame_push;
    config.on_line = yougame_line;

    if (yg_init(&config) != 0) return 0;
    g_yougame_initialized = 1;
    (void)yg_set_game_state(YG_GAME_MENU);
    return 1;
}

void yougame_native_update(void)
{
    if (g_yougame_initialized) yg_update();
}

void yougame_native_shutdown(void)
{
    if (!g_yougame_initialized) return;
    yg_shutdown();
    g_yougame_initialized = 0;

    /* The SDK deliberately drops outstanding callbacks during shutdown. */
    while (g_yougame_pending_calls) {
        YouGamePendingCall *pending = g_yougame_pending_calls;
        g_yougame_pending_calls = pending->next;
        free(pending);
    }
    g_yougame_push_callback = NULL;
    g_yougame_push_user = NULL;
    g_yougame_line_callback = NULL;
    g_yougame_line_user = NULL;
}

long long yougame_native_request(const char *type, const char *body_json,
    double timeout_seconds, YouGameNativeReplyCallback callback, void *user)
{
    YouGamePendingCall *pending = NULL;
    long long request_id;

    if (!g_yougame_initialized || !type || !type[0]) return 0;
    if (!callback)
        return yg_call(type, body_json, timeout_seconds, NULL, NULL);

    pending = (YouGamePendingCall *)calloc(1, sizeof *pending);
    if (!pending) return 0;
    pending->callback = callback;
    pending->user = user;

    request_id = yg_call(type, body_json, timeout_seconds,
                         yougame_call_finished, pending);
    if (request_id <= 0) {
        free(pending);
        return 0;
    }
    pending->request_id = request_id;
    pending->next = g_yougame_pending_calls;
    g_yougame_pending_calls = pending;
    return request_id;
}

void yougame_native_set_push_handler(YouGameNativePushCallback callback, void *user)
{
    g_yougame_push_callback = callback;
    g_yougame_push_user = user;
}

void yougame_native_set_line_handler(YouGameNativeLineCallback callback, void *user)
{
    g_yougame_line_callback = callback;
    g_yougame_line_user = user;
}

long long yougame_native_next_request_id(void)
{
    return g_yougame_initialized ? yg_next_id() : 0;
}

int yougame_native_write_line(const char *line, size_t length)
{
    if (!g_yougame_initialized || !line || length == 0) return -1;
    return yg_write_line(line, length);
}

void yougame_native_set_state(int state)
{
    if (!g_yougame_initialized) return;
    if (state == PIPECLEAN_YOUGAME_PLAYING)
        (void)yg_set_game_state(YG_GAME_PLAYING);
    else if (state == PIPECLEAN_YOUGAME_PAUSED)
        (void)yg_set_game_state(YG_GAME_PAUSED);
    else
        (void)yg_set_game_state(YG_GAME_MENU);
}

void yougame_native_relay_key(const char *key)
{
    if (g_yougame_initialized && key && key[0])
        yg_relay_key(key);
}

int yougame_native_connected(void)
{
    return g_yougame_initialized && yg_connected();
}

int yougame_native_ready(void)
{
    return g_yougame_initialized && yg_ready();
}

int yougame_native_started_by_app(void)
{
    return g_yougame_initialized && yg_started_by() == YG_STARTED_BY_APP;
}

int yougame_native_player_state(void)
{
    return g_yougame_initialized ? (int)yg_player_status() : (int)YG_PLAYER_UNKNOWN;
}

const char *yougame_native_player_name(void)
{
    return g_yougame_initialized ? yg_player_name() : "";
}

const char *yougame_native_player_id(void)
{
    return g_yougame_initialized ? yg_player_id() : "";
}

int yougame_native_take_quit_request(void)
{
    return InterlockedExchange(&g_yougame_quit_requested, 0) != 0;
}

#else

int yougame_native_init(void) { return 0; }
void yougame_native_update(void) {}
void yougame_native_shutdown(void) {}
long long yougame_native_request(const char *type, const char *body_json,
    double timeout_seconds, YouGameNativeReplyCallback callback, void *user)
{ (void)type; (void)body_json; (void)timeout_seconds; (void)callback; (void)user; return 0; }
void yougame_native_set_push_handler(YouGameNativePushCallback callback, void *user)
{ (void)callback; (void)user; }
void yougame_native_set_line_handler(YouGameNativeLineCallback callback, void *user)
{ (void)callback; (void)user; }
long long yougame_native_next_request_id(void) { return 0; }
int yougame_native_write_line(const char *line, size_t length)
{ (void)line; (void)length; return -1; }
void yougame_native_set_state(int state) { (void)state; }
void yougame_native_relay_key(const char *key) { (void)key; }
int yougame_native_connected(void) { return 0; }
int yougame_native_ready(void) { return 0; }
int yougame_native_started_by_app(void) { return 0; }
int yougame_native_player_state(void) { return 0; }
const char *yougame_native_player_name(void) { return ""; }
const char *yougame_native_player_id(void) { return ""; }
int yougame_native_take_quit_request(void) { return 0; }

#endif
