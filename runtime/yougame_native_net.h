#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int connection_id;
    const char *event;      /* open, message, error, or close */
    const char *push_json;  /* callback-scoped complete push object */
} YouGameNativeNetEvent;

typedef void (*YouGameNativeNetEventCallback)(void *user,
    const YouGameNativeNetEvent *event);
typedef void (*YouGameNativeNetReplyCallback)(void *user,
    long long request_id, int ok, int connection_id,
    const char *code, const char *error);

/* Register after native SDK initialization. Pushes arrive through
 * yougame_native_update(), on PipeClean's main thread. */
void yougame_native_net_init(void);
/* Call after yougame_native_shutdown() so pending callback contexts are safe to retire. */
void yougame_native_net_shutdown(void);
void yougame_native_net_set_event_callback(
    YouGameNativeNetEventCallback callback, void *user);

/* ticket is the opaque string returned by mpTicket. A queue match can require a
 * second open with the same ticket and matched room code. Broker pushes may
 * precede the netOpen reply, so retain early events until the connection id is known. */
long long yougame_native_net_open(const char *ticket, const char *room,
    const char *pass, const char *name,
    YouGameNativeNetReplyCallback callback, void *user);

/* Sends one UTF-8 text frame, normally {"t":"msg","d":...}; max 16 KiB.
 * Requests are asynchronous. A null reply callback is allowed for send/close. */
long long yougame_native_net_send(int connection_id, const char *data,
    YouGameNativeNetReplyCallback callback, void *user);

/* code must be 1000 or 3000-4999; reason is optional UTF-8, at most 123 bytes. */
long long yougame_native_net_close(int connection_id, int code,
    const char *reason, YouGameNativeNetReplyCallback callback, void *user);

#ifdef __cplusplus
}
#endif
