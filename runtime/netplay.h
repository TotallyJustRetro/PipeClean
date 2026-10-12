#pragma once
#include <stdint.h>

enum {
    NETPLAY_OFFLINE = 0,
    NETPLAY_CONNECTING,
    NETPLAY_HOSTING,
    NETPLAY_CONNECTED,
    NETPLAY_ERROR
};

enum {
    NETPLAY_ROM_UNCHECKED = 0,
    NETPLAY_ROM_WAITING,
    NETPLAY_ROM_MATCH,
    NETPLAY_ROM_MISMATCH,
    NETPLAY_ROM_INVALID
};

typedef struct {
    int state;
    int is_host;
    char status[128];
    char code[16];
    char peer[32];
    int rom_check_state;
    int rom_check_game;
    char rom_check_status[128];
} NetplayInfo;

typedef struct {
    char name[32];
    char text[96];
} NetplayChatMessage;

/* Relay address may be host:port, tls://host:port, or wss://hostname (port defaults to 443). */
void netplay_init(void);
void netplay_shutdown(void);
int netplay_host(const char *endpoint, const char *name);
int netplay_join(const char *endpoint, const char *code, const char *name);
void netplay_disconnect(void);
void netplay_get_info(NetplayInfo *out);
int netplay_is_connected(void);
int netplay_is_host(void);

/* Lobby API. Online-selectable games currently use GAME_SML and GAME_SML2. */
int netplay_lobby_set_game(int game);
int netplay_lobby_get_game(void);
int netplay_lobby_start_game(int game);
int netplay_lobby_report_rom(int game, uint32_t rom_crc, int valid);
int netplay_take_start_request(void);
int netplay_chat_send(const char *text);
int netplay_chat_snapshot(NetplayChatMessage *out, int capacity);

/* Lockstep API for SML/SML2 local co-op simulations on two clients. */
enum {
    NETPLAY_ACTION_RESPAWN_P2 = 1u << 0,
    NETPLAY_ACTION_SAVE_STATE = 1u << 1,
    NETPLAY_ACTION_LOAD_STATE = 1u << 2
};
#define NETPLAY_SHARED_SLOT_COUNT 10
void netplay_shared_slot_configure(int slot, const char *state_path,
                                   const char *marker_path, const char *name,
                                   int local_saved);
void netplay_shared_slot_name(int slot, char *out, size_t out_size);
void netplay_shared_slot_status(int slot, int *local_saved, int *peer_saved,
                                int *pending);
int netplay_shared_slot_is_synced(int slot);
int netplay_shared_slot_active(void);
int netplay_shared_slot_load(void);
int netplay_shared_slot_select(int active_slot, int load_slot);
int netplay_shared_slot_rename(int slot, const char *name);
void netplay_shared_slot_mark_unsynced(int slot);
int netplay_shared_save_send_file(int slot, const char *path);
int netplay_game_begin(int game_id, uint32_t rom_crc);
int netplay_exchange_inputs(uint64_t frame, uint8_t local_buttons, uint8_t local_dpad, uint8_t local_actions,
                            uint8_t *p1_buttons, uint8_t *p1_dpad, uint8_t *p1_actions,
                            uint8_t *p2_buttons, uint8_t *p2_dpad, uint8_t *p2_actions);
void netplay_game_end(void);
