#pragma once
#include <stddef.h>

enum {
    NETPLAY_OFFLINE = 0,
    NETPLAY_CONNECTING,
    NETPLAY_HOSTING,
    NETPLAY_CONNECTED,
    NETPLAY_ERROR
};

typedef struct {
    int state;
    char status[128];
    char code[16];
    char peer[32];
} NetplayInfo;

/* A relay address is a hostname or IPv4 address followed by :port. */
void netplay_init(void);
void netplay_shutdown(void);
int netplay_host(const char *endpoint, const char *name);
int netplay_join(const char *endpoint, const char *code, const char *name);
void netplay_disconnect(void);
void netplay_get_info(NetplayInfo *out);
