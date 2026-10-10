#pragma once
#include <stdint.h>
#include "util.h"

typedef struct {
    const char *name;
    uint32_t bg[4], ob0[4], ob1[4];     /* 0xRRGGBB, lightest -> darkest */
    int multi;                           /* 1 = sprites use their own colours (GBC style) */
} Palette;

extern const Palette palettes[];
extern const int n_palettes;

#include "games.h"

enum { ASPECT_ORIGINAL, ASPECT_4_3, ASPECT_16_9, N_ASPECT };
enum { SCALE_PIXEL, SCALE_SMOOTH, SCALE_STRETCH, N_SCALE };
enum { SIZE_SMALL, SIZE_MEDIUM, SIZE_LARGE, SIZE_FULLSCREEN, N_SIZE };
enum { BTN_A, BTN_B, BTN_SELECT, BTN_START, BTN_RIGHT, BTN_LEFT, BTN_UP, BTN_DOWN, N_BTN };
enum { LED_OFF, LED_PALETTE, LED_CUSTOM, LED_SCREEN, N_LED };
enum { LUIGI_GREEN, LUIGI_BLUE, LUIGI_RED, LUIGI_PURPLE, LUIGI_ORANGE, LUIGI_YELLOW, N_LUIGI_COLORS };
enum { P2_SFX_JUMP, P2_SFX_FIREBALL, P2_SFX_POWER_UP, P2_SFX_POWER_DOWN, P2_SFX_DIE, P2_SFX_RESPAWN, N_P2_SFX };

typedef struct {
    const char *name;
    uint32_t swatch, light, mid, dark; /* RGB24 colors for Luigi's sprite recolor. */
} LuigiColor;
extern const LuigiColor luigi_colors[N_LUIGI_COLORS];
enum { ACT_SAVE_STATE, ACT_LOAD_STATE, ACT_REWIND, ACT_SUSPEND, ACT_NEXT_SLOT, N_ACTION };
enum { LAT_LOW, LAT_NORMAL, LAT_HIGH, N_LAT };

#define PAD_AXIS_BASE 100               /* controller binding codes >= 100 are triggers: 100 = L2, 101 = R2 */
#define MAX_EVENTS 8
#define N_UI_SFX 5                      /* hover, click, confirm, back, toggle */
#define MAX_ROMHACKS 16
#define ROMHACK_TITLE_LEN 96

typedef struct {
    char path[512];
    char title[ROMHACK_TITLE_LEN];
    char thumbnail[512];
} RomHackEntry;

typedef struct {
    char rom_path[512], hack_path[512], bg_path[512], tex_path[512];
    int romhack_count;                  /* number of registered secondary ROM profiles */
    int romhack_selected;                /* -1 = original ROM, otherwise index in romhacks[] */
    RomHackEntry romhacks[MAX_ROMHACKS];
    int palette, aspect, scaling, size;
    int bg_dim;                         /* 0..80 percent darkening of the background image */
    int wide;                           /* widescreen amount 0..100 (% of what the game supports) */
    int tex_on, tex_collect;
    int multiplayer;                    /* local multiplayer mode; default off */
    int multiplayer_players;             /* selected player count, clamped to 2..4 */
    char p2_name[32];                    /* Player 2 default name: Luigi */
    int p2_color;
    int p2_respawn_key, p2_respawn_pad;
    char p2_sfx_path[N_P2_SFX][512];
    char p3_name[32];                    /* Player 3 default name: Bunzo */
    int p3_color;
    int p3_respawn_key, p3_respawn_pad;
    char p3_sfx_path[N_P2_SFX][512];
    char p4_name[32];                    /* Player 4 default name: Florbo */
    int p4_color;
    int p4_respawn_key, p4_respawn_pad;
    char p4_sfx_path[N_P2_SFX][512];
    int state_slot;                      /* save-state slot 0..9 */
    int action_key[N_ACTION];            /* emulator shortcut keyboard bindings */
    int action_pad[N_ACTION];            /* emulator shortcut controller bindings */
    int key[N_BTN][MAX_MP_PLAYERS];     /* SDL keycodes, 0 = unbound */
    int pad[N_BTN][MAX_MP_PLAYERS];     /* SDL_GameControllerButton or PAD_AXIS_BASE+n, -1 = unbound */
    int pad_device[MAX_MP_PLAYERS];     /* controller slot assigned to each player */
    char pad_guid[MAX_MP_PLAYERS][512]; /* persisted physical controller identities */
    /* DualSense / DualSense Edge */
    int ds_led_mode, ds_bright;         /* LED_*, 0..100 */
    uint32_t ds_color;                  /* 0xRRGGBB for LED_CUSTOM */
    int ds_rumble;                      /* 0..100 strength */
    int ds_speaker_vol;                 /* 0..100 */
    int ds_ev_led, ds_ev_rumble, ds_ev_speaker;     /* bitmasks over the game's events */
} GameCfg;

typedef struct {
    int lcd_grid, ghost, scanlines, mask, curve, bloom, hdr, vignette, blur;   /* 0..100 each */
    int preset;
} FilterCfg;

typedef struct {
    int volume;                         /* game volume 0..200 percent */
    int latency;                        /* LAT_* */
    int pad_deadzone;                   /* stick deadzone percent */
    int menu_music, menu_music_vol, ui_sfx, ui_sfx_vol;
    char menu_music_path[512], ui_sfx_path[N_UI_SFX][512];
    int last_tab, show_stats;
    FilterCfg flt;
    GameCfg g[N_GAMES];
} Settings;

extern Settings settings;
extern const char *aspect_names[], *scale_names[], *size_names[], *btn_names[], *led_names[], *lat_names[];
extern const char *action_names[];

void settings_defaults(Settings *s);
void game_cfg_defaults(GameCfg *c, int game);
void controls_defaults(GameCfg *c);
void shortcut_defaults(GameCfg *c);
void filter_preset(FilterCfg *f, int preset);
extern const char *filter_preset_names[];
extern const int n_filter_presets;
void settings_load(void);           /* from <exe folder>/dr_mario_launcher.ini */
void settings_save(void);
const char *settings_dir(void);     /* folder next to the program (with trailing slash) */