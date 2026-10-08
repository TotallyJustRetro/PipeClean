#pragma once
#include "gb.h"

typedef struct {
    uint8_t shade[GB_H][GB_WMAX], layer[GB_H][GB_WMAX];
    uint8_t bguv[GB_H][GB_WMAX], spruv[GB_H][GB_WMAX];
    uint16_t bgtile[GB_H][GB_WMAX], sprtile[GB_H][GB_WMAX];
    int w, xoff;
    int player_x, player_y;
    uint8_t scroll_x, game_state;
    uint8_t obp0, obp1, sprite_size16;
    uint8_t mario_oam[16];      /* Player 1 Mario OAM */
    uint8_t mario_oam2[16];     /* Player 2 OAM in local multiplayer */
    uint8_t luigi_mask[GB_H][GB_WMAX]; /* Pixels belonging to the Luigi overlay */
    uint8_t p1_lives;             /* Authoritative SML1 lives, decoded to 0..99 */
    uint8_t p2_lives;             /* PipeClean Player 2 lives, decoded to 0..99 */
    uint8_t p2_game_state;        /* Private SML1 state: 0 normal, 3/4 death animation */
    uint8_t p2_visible;           /* 1 while Luigi is active on the shared screen */
    uint8_t p2_sound_event;       /* P2 triggered a one-shot interaction SFX */
    uint8_t bg_map[0x400];
    uint8_t tiles[0x1800];
    uint64_t seq;
    int lcd_on;
} Frame;

int  emu_start(int force_interp);
void emu_stop(void);
int  emu_running(void);
int  emu_frame_get(Frame *f);
void emu_input(uint8_t buttons, uint8_t dpad);
void emu_set_paused(int p);
void emu_set_turbo(int t);
void emu_set_save_path(const char *path);
uint64_t emu_frames(void);

/* Local two-player SML1 runtime: one authoritative world plus an isolated Player 2 state. */
int emu_mp_begin(void);
int emu_mp_step(int player, uint8_t buttons, uint8_t dpad, Frame *frame, int16_t *audio, int audio_max);
void emu_mp_end(void);

void emu_preview(int frames);
void emu_run_blocking(int force_interp);

typedef struct {
    int max_frames;
    int hash, fuzz;
    uint32_t seed;
    const char *hash_log, *script;
    int region_on, region[4];
} EmuDev;
extern EmuDev emu_dev;
void emu_dev_poke(int frame, uint16_t addr, uint8_t val);
int  emu_dev_init(void);
void emu_dev_report(void);
void emu_dev_close(void);