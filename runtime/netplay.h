#pragma once
#include <stdint.h>

enum {
    NETPLAY_OFFLINE = 0,
    NETPLAY_CONNECTING,
    NETPLAY_HOSTING,
    NETPLAY_CONNECTED,
    NETPLAY_ERROR
};

typedef struct {
    int state;
    int is_host;
    char status[128];
    char code[16];
    char peer[32];
} NetplayInfo;

/* Relay address may be host:port, tls://host:port, or wss://hostname (port defaults to 443). */
void netplay_init(void);
void netplay_shutdown(void);
int netplay_host(const char *endpoint, const char *name);
int netplay_join(const char *endpoint, const char *code, const char *name);
void netplay_disconnect(void);
void netplay_get_info(NetplayInfo *out);
int netplay_is_connected(void);
int netplay_is_host(void);

/* Lockstep API for SML/SML2 local co-op simulations on two clients. */
enum { NETPLAY_ACTION_RESPAWN_P2 = 1u << 0 };
int netplay_game_begin(int game_id, uint32_t rom_crc);
int netplay_exchange_inputs(uint64_t frame, uint8_t local_buttons, uint8_t local_dpad, uint8_t local_actions,
                            uint8_t *p1_buttons, uint8_t *p1_dpad, uint8_t *p1_actions,
                            uint8_t *p2_buttons, uint8_t *p2_dpad, uint8_t *p2_actions);
void netplay_game_end(void);
