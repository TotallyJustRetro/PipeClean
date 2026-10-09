#pragma once
#include "gb.h"
#include "games.h"

#define P2_SFX_EVENT_JUMP       (1u << 0)
#define P2_SFX_EVENT_FIREBALL   (1u << 1)
#define P2_SFX_EVENT_POWER_UP   (1u << 2)
#define P2_SFX_EVENT_POWER_DOWN (1u << 3)
#define P2_SFX_EVENT_DIE        (1u << 4)
#define P2_SFX_EVENT_RESPAWN    (1u << 5)
#define MP_MAX_OAM_SPRITES 40 /* Game Boy OAM contains 40 four-byte entries. */

typedef struct {
    uint8_t shade[GB_H][GB_WMAX], layer[GB_H][GB_WMAX];
    uint32_t rgb[GB_H][GB_WMAX]; /* CGB-native pixels, 0xRRGGBB. */
    uint8_t cgb_mode;
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
    uint8_t p2_blink_hidden;      /* Temporary flicker during Luigi's post-respawn invulnerability */
    uint8_t p2_projectile_oam[12]; /* Three private projectile sprites from Player 2. */
    uint8_t p2_effect_oam[52];      /* Player 2's non-enemy effects (OAM slots 7-19). */
    uint8_t p2_sound_event;       /* P2 triggered a one-shot interaction SFX */
    uint8_t p2_sfx_events;        /* P2_SFX_EVENT_* bitmask: jump, fireball, power, death. */
    uint8_t mp_player_count;      /* active SML1 players, 2..4 */
    uint8_t mp_player_lives[MAX_MP_PLAYERS];
    uint8_t mp_player_game_state[MAX_MP_PLAYERS];
    uint8_t mp_player_visible[MAX_MP_PLAYERS];
    uint8_t mp_player_blink_hidden[MAX_MP_PLAYERS];
    uint8_t mp_player_oam[MAX_MP_PLAYERS][MP_MAX_OAM_SPRITES * 4]; /* player 2..4 sprite pieces */
    uint8_t mp_player_sprite_count[MAX_MP_PLAYERS]; /* valid OAM entries per player */
    uint8_t mp_player_projectile_oam[MAX_MP_PLAYERS][12];
    uint8_t mp_player_effect_oam[MAX_MP_PLAYERS][52];
    uint8_t mp_player_sfx_events[MAX_MP_PLAYERS];
    uint8_t mp_player_sound_event[MAX_MP_PLAYERS];
    uint8_t bg_map[0x400];
    uint8_t tiles[0x1800];
    uint8_t tiles_cgb1[0x1800]; /* CGB VRAM bank 1 for color-mode sprite overlays. */
    uint32_t cgb_obj_palette[32]; /* Eight CGB OBJ palettes, four RGB24 colors each. */
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
int emu_cpu_faulted(void);
void emu_cpu_fault_info(uint8_t *opcode, uint16_t *pc);

/* Local co-op for SML1 and SML2. Player indices are 0=host, 1=Player 2,
 * 2=Player 3, and 3=Player 4. */
int emu_mp_begin(void);
int emu_mp_step(int player, uint8_t buttons, uint8_t dpad, Frame *frame, int16_t *audio, int audio_max);
void emu_mp_end(void);
void emu_mp_frame_refresh(Frame *frame);
/* SML2 multiplayer's cloned PPU has separate sprite graphics per player. */
const uint8_t *emu_mp_player_sprite_tiles(int player, int bank);
void emu_mp_request_respawn(int player);

/* Emulator state features */
int emu_state_save_file(const char *path, int resume_after);
int emu_state_load_file(const char *path, int resume_after);
int emu_rewind_step(void);
void emu_rewind_capture(void);
void emu_rewind_end(void);
int emu_rewind_available(void);

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