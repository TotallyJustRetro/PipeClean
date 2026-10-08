#pragma once
#include <stdint.h>

enum { GAME_DRMARIO, GAME_SML, GAME_SML2, N_GAMES };

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
} GameDef;

extern const GameDef games[N_GAMES];