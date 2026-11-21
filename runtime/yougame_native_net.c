#include "yougame_native_net.h"
#include "yougame_native.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#if defined(_WIN32) && defined(PIPECLEAN_YOUGAME_NATIVE)
#include "third_party/yougame/yougame.h"

typedef struct YouGameNativeNetCall {
    YouGameNativeNetReplyCallback callback;
    void *user;
    int connection_id;
    int opening;
    struct YouGameNativeNetCall *next;
} YouGameNativeNetCall;

static YouGameNativeNetEventCallback g_net_event_callback;
static void *g_net_event_user;
static int g_net_registered;
static YouGameNativeNetCall *g_net_pending_calls;

static int append_char(char *out, size_t capacity, size_t *used, char value)
{
    if (*used + 1 >= capacity) return 0;
    out[(*used)++] = value;
    out[*used] = 0;
    return 1;
}

static int append_text(char *out, size_t capacity, size_t *used, const char *text)
{
    size_t length;
    if (!text) return 0;
    length = strlen(text);
    if (length >= capacity || *used > capacity - length - 1) return 0;
    memcpy(out + *used, text, length);
    *used += length;
    out[*used] = 0;
    return 1;
}

/* JSON string writer: preserves UTF-8 bytes and escapes quotes, slashes and
 * control characters so caller data cannot break the broker request envelope. */
static int append_json_string(char *out, size_t capacity, size_t *used,
                              const char *value)
{
    const unsigned char *p = (const unsigned char *)(value ? value : "");
    static const char hex[] = "0123456789abcdef";
    if (!append_char(out, capacity, used, '"')) return 0;
    while (*p) {
        unsigned char c = *p++;
        if (c == '"' || c == '\\') {
            if (!append_char(out, capacity, used, '\\') ||
                !append_char(out, capacity, used, (char)c)) return 0;
        } else if (c < 0x20) {
            if (!append_text(out, capacity, used, "\\u00") ||
                !append_char(out, capacity, used, hex[c >> 4]) ||
                !append_char(out, capacity, used, hex[c & 15])) return 0;
        } else if (!append_char(out, capacity, used, (char)c)) {
            return 0;
        }
    }
    return append_char(out, capacity, used, '"');
}

static int append_optional_string(char *out, size_t capacity, size_t *used,
                                  const char *key, const char *value)
{
    if (!value || !value[0]) return 1;
    return append_char(out, capacity, used, ',') &&
           append_json_string(out, capacity, used, key) &&
           append_char(out, capacity, used, ':') &&
           append_json_string(out, capacity, used, value);
}

static void net_request_finished(void *opaque, long long request_id,
    const char *type, int ok, const char *code, const char *error,
    const char *result_json)
{
    YouGameNativeNetCall *call = (YouGameNativeNetCall *)opaque;
    int connection_id = call ? call->connection_id : 0;
    char result_code[64];
    const char *final_code = code ? code : "";
    const char *final_error = error ? error : "";

    YouGameNativeNetCall **link = &g_net_pending_calls;
    (void)type;
    if (!call) return;
    while (*link && *link != call) link = &(*link)->next;
    if (*link == call) *link = call->next;
    if (call->opening && ok) {
        double parsed = 0;
        if (!result_json ||
            !yg_json_number(result_json, "conn", &parsed) ||
            parsed < 1 || parsed > INT_MAX || parsed != (double)(int)parsed) {
            ok = 0;
            connection_id = 0;
            snprintf(result_code, sizeof result_code, "bad_result");
            final_code = result_code;
            final_error = "YouGame netOpen returned an invalid connection id";
        } else {
            connection_id = (int)parsed;
        }
    }
    if (call->callback)
        call->callback(call->user, request_id, ok, connection_id,
                       final_code, final_error);
    free(call);
}

static void net_push_received(void *user, const char *type, const char *json)
{
    YouGameNativeNetEvent event;
    char event_name[24];
    double connection = 0;
    (void)user;

    if (!type || strcmp(type, "net") != 0 || !json ||
        !yg_json_number(json, "conn", &connection) ||
        connection < 1 || connection > INT_MAX ||
        connection != (double)(int)connection ||
        yg_json_string(json, "event.type", event_name, sizeof event_name) < 0)
        return;

    if (!g_net_event_callback) return;
    if (strcmp(event_name, "open") != 0 &&
        strcmp(event_name, "message") != 0 &&
        strcmp(event_name, "error") != 0 &&
        strcmp(event_name, "close") != 0)
        return;

    event.connection_id = (int)connection;
    event.event = event_name;
    event.push_json = json;
    g_net_event_callback(g_net_event_user, &event);
}

static long long native_net_request(const char *type, const char *body,
    int connection_id, int opening,
    YouGameNativeNetReplyCallback callback, void *user)
{
    YouGameNativeNetCall *call = NULL;
    long long request_id;

    if (!yougame_native_ready()) return 0;
    if (callback) {
        call = (YouGameNativeNetCall *)calloc(1, sizeof *call);
        if (!call) return 0;
        call->callback = callback;
        call->user = user;
        call->connection_id = connection_id;
        call->opening = opening;
    }
    request_id = yougame_native_request(type, body, 15.0,
        callback ? net_request_finished : NULL, call);
    if (request_id <= 0) {
        free(call);
        return 0;
    }
    if (call) {
        call->next = g_net_pending_calls;
        g_net_pending_calls = call;
    }
    return request_id;
}

void yougame_native_net_init(void)
{
    if (g_net_registered) return;
    yougame_native_set_push_handler(net_push_received, NULL);
    g_net_registered = 1;
}

void yougame_native_net_shutdown(void)
{
    if (g_net_registered)
        yougame_native_set_push_handler(NULL, NULL);
    g_net_registered = 0;
    /* This is called after yougame_native_shutdown(), which guarantees that
     * the SDK will no longer invoke callbacks carrying these user pointers. */
    while (g_net_pending_calls) {
        YouGameNativeNetCall *call = g_net_pending_calls;
        g_net_pending_calls = call->next;
        free(call);
    }
    g_net_event_callback = NULL;
    g_net_event_user = NULL;
}

void yougame_native_net_set_event_callback(
    YouGameNativeNetEventCallback callback, void *user)
{
    g_net_event_callback = callback;
    g_net_event_user = user;
}

long long yougame_native_net_open(const char *ticket, const char *room,
    const char *pass, const char *name,
    YouGameNativeNetReplyCallback callback, void *user)
{
    char body[8192] = "{\"ticket\":";
    size_t used = strlen(body);
    if (!ticket || !ticket[0] ||
        !append_json_string(body, sizeof body, &used, ticket) ||
        !append_optional_string(body, sizeof body, &used, "room", room) ||
        !append_optional_string(body, sizeof body, &used, "pass", pass) ||
        !append_optional_string(body, sizeof body, &used, "name", name) ||
        !append_char(body, sizeof body, &used, '}'))
        return 0;
    if (!callback) return 0; /* Caller must receive the broker connection id. */
    return native_net_request("netOpen", body, 0, 1, callback, user);
}

long long yougame_native_net_send(int connection_id, const char *data,
    YouGameNativeNetReplyCallback callback, void *user)
{
    size_t length, capacity, used;
    char *body;
    long long request_id;
    if (connection_id <= 0 || !data) return 0;
    length = strlen(data);
    if (length > 16384) return 0;
    capacity = length * 6 + 64;
    body = (char *)malloc(capacity);
    if (!body) return 0;
    used = (size_t)snprintf(body, capacity, "{\"conn\":%d,\"data\":", connection_id);
    if (used >= capacity ||
        !append_json_string(body, capacity, &used, data) ||
        !append_char(body, capacity, &used, '}')) {
        free(body);
        return 0;
    }
    request_id = native_net_request("netSend", body, connection_id, 0,
                                    callback, user);
    free(body); /* yg_call serializes/copies the request before returning. */
    return request_id;
}

long long yougame_native_net_close(int connection_id, int code,
    const char *reason, YouGameNativeNetReplyCallback callback, void *user)
{
    char body[1024];
    size_t used;
    if (connection_id <= 0 ||
        !(code == 1000 || (code >= 3000 && code <= 4999)))
        return 0;
    used = (size_t)snprintf(body, sizeof body, "{\"conn\":%d,\"code\":%d",
                            connection_id, code);
    if (used >= sizeof body ||
        !append_optional_string(body, sizeof body, &used, "reason", reason) ||
        !append_char(body, sizeof body, &used, '}'))
        return 0;
    return native_net_request("netClose", body, connection_id, 0,
                              callback, user);
}

#else

void yougame_native_net_init(void) {}
void yougame_native_net_shutdown(void) {}
void yougame_native_net_set_event_callback(
    YouGameNativeNetEventCallback callback, void *user)
{ (void)callback; (void)user; }
long long yougame_native_net_open(const char *ticket, const char *room,
    const char *pass, const char *name,
    YouGameNativeNetReplyCallback callback, void *user)
{ (void)ticket; (void)room; (void)pass; (void)name; (void)callback; (void)user; return 0; }
long long yougame_native_net_send(int connection_id, const char *data,
    YouGameNativeNetReplyCallback callback, void *user)
{ (void)connection_id; (void)data; (void)callback; (void)user; return 0; }
long long yougame_native_net_close(int connection_id, int code,
    const char *reason, YouGameNativeNetReplyCallback callback, void *user)
{ (void)connection_id; (void)code; (void)reason; (void)callback; (void)user; return 0; }

#endif
