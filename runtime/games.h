#pragma once
#define MAX_MP_PLAYERS 4
#include <stdint.h>

enum {
    GAME_DRMARIO, GAME_SML, GAME_SML2,
    GAME_WARIO_SML3, GAME_WARIO_LAND2_GB, GAME_WARIO_LAND3_GBC, GAME_WARIO_LAND2_GBC,
    N_GAMES
};

typedef struct {
    const char *id;
    const char *name;
    const char *sub;
    const char *title;
    uint32_t crc;
    int lifted;
    uint32_t accent;
    int preview_frames;
    int wide_l, wide_r;
    int hud_lines;
    int hud_window;
    int wide_gate;
    int external_player;              /* launch through the bundled ROM-independent gbrecomp player */
} GameDef;

extern const GameDef games[N_GAMES];