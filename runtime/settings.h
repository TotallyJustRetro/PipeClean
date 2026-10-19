#pragma once
#include <stdint.h>
#include "util.h"

/* Deployment workflow replaces this with the project's public TLS relay. */
#ifndef PIPECLEAN_DEFAULT_RELAY_ENDPOINT
#define PIPECLEAN_DEFAULT_RELAY_ENDPOINT "wss://pipeclean.onrender.com"
#endif

typedef struct {
    const char *name;
    uint32_t bg[4], ob0[4], ob1[4];     /* 0xRRGGBB, lightest -> darkest */
    int multi;                           /* 1 = sprites use their own colours (GBC style) */
} Palette;

extern const Palette palettes[];
extern const int n_palettes;

#include "games.h"

enum { ASPECT_ORIGINAL, ASPECT_4_3, ASPECT_16_9, N_ASPECT };
/* Preserve existing persisted values; append new modes only. */
enum { SCALE_PIXEL, SCALE_SMOOTH, SCALE_STRETCH, SCALE_PIXEL_STRETCH, N_SCALE };
enum { SIZE_SMALL, SIZE_MEDIUM, SIZE_LARGE, SIZE_FULLSCREEN, N_SIZE };
enum { BTN_A, BTN_B, BTN_SELECT, BTN_START, BTN_RIGHT, BTN_LEFT, BTN_UP, BTN_DOWN, N_BTN };
enum { LED_OFF, LED_PALETTE, LED_CUSTOM, LED_SCREEN, N_LED };
enum { LUIGI_GREEN, LUIGI_BLUE, LUIGI_RED, LUIGI_PURPLE, LUIGI_ORANGE, LUIGI_YELLOW, N_LUIGI_COLORS };
enum { P2_SFX_JUMP, P2_SFX_FIREBALL, P2_SFX_POWER_UP, P2_SFX_POWER_DOWN, P2_SFX_DIE, P2_SFX_RESPAWN, P2_SFX_COIN, N_P2_SFX };

typedef struct {
    const char *name;
    uint32_t swatch, light, mid, dark; /* RGB24 colors for Luigi's sprite recolor. */
} LuigiColor;
extern const LuigiColor luigi_colors[N_LUIGI_COLORS];
enum { ACT_SAVE_STATE, ACT_LOAD_STATE, ACT_REWIND, ACT_SUSPEND, ACT_NEXT_SLOT, N_ACTION };
enum { LAT_LOW, LAT_NORMAL, LAT_HIGH, N_LAT };
/* Android haptic routing: auto, phone vibrator, then connected controller slots. */
#define ANDROID_HAPTIC_PHONE_MOTOR_SLOTS 14
enum {
    ANDROID_HAPTIC_AUTO = 0,
    ANDROID_HAPTIC_PHONE = 1,
    ANDROID_HAPTIC_PHONE_MOTOR_FIRST = 2,
    ANDROID_HAPTIC_CONTROLLER_FIRST = ANDROID_HAPTIC_PHONE_MOTOR_FIRST + ANDROID_HAPTIC_PHONE_MOTOR_SLOTS,
    N_ANDROID_HAPTIC_TARGETS = ANDROID_HAPTIC_CONTROLLER_FIRST + 4
};

#define PAD_AXIS_BASE 100               /* controller binding codes >= 100 are triggers: 100 = L2, 101 = R2 */
#define MAX_EVENTS 8
#define N_UI_SFX 5                      /* hover, click, confirm, back, toggle */
#define MAX_ROMHACKS 16
#define ROMHACK_TITLE_LEN 96
#define MAX_MENU_TRACKS 12
#define N_RECENT_PLAYED 5

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
    int dual_screen;                    /* Show the Android second-screen deck for this game. */
    int pipe_screen_swap;                /* Swap gameplay to the companion screen while SML1 is underground. */
    int multiplayer;                    /* local multiplayer mode; default off */
    int multiplayer_players;             /* selected player count, clamped to 2..4 */
    char p2_name[32];                    /* Player 2 default name: Luigi */
    int p2_color;
    int p2_rgb;                     /* custom character colour, 0xRRGGBB */
    int p2_custom;                        /* use p2_rgb instead of the legacy preset */
    int p2_respawn_key, p2_respawn_pad;
    char p2_sfx_path[N_P2_SFX][512];
    char p3_name[32];                    /* Player 3 default name: Bunzo */
    int p3_color;
    int p3_rgb;
    int p3_custom;
    int p3_respawn_key, p3_respawn_pad;
    char p3_sfx_path[N_P2_SFX][512];
    char p4_name[32];                    /* Player 4 default name: Florbo */
    int p4_color;
    int p4_rgb;
    int p4_custom;
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
    int ds_fx_version;                  /* one-time migration for Wario controller effects */
} GameCfg;

typedef struct {
    int lcd_grid, ghost, scanlines, mask, curve, bloom, hdr, vignette, blur;   /* 0..100 each */
    int preset;
} FilterCfg;

/* Custom Companion Deck layout controls and assets. Offsets use the deck's
 * 800x450 reference coordinates and scales are percentages. */
enum { DECK_SOUND_SAVE, DECK_SOUND_LOAD, DECK_SOUND_PAUSE, DECK_SOUND_REWIND,
       DECK_SOUND_BACKGROUND, DECK_SOUND_CLOSE, N_DECK_BUTTON_SOUNDS };
/* Reserve item IDs: 1 = mushroom, 2 = SML1 superball / SML2 fire flower,
 * 3 = SML2 carrot. Index 0 is unused; item_texture_path is the legacy fallback. */
#define N_DECK_ITEM_TEXTURE_IDS 4
#define N_DECK_DECORATIONS 16
#define N_DECK_ASSET_PATHS 38
#define N_DECK_LAYOUT_ASSET_MODE_BASE 49
#define N_DECK_STATUS_VALUE_BASE (N_DECK_LAYOUT_ASSET_MODE_BASE + N_DECK_ASSET_PATHS)
#define N_DECK_EXTENDED_DECORATION_COUNT (N_DECK_DECORATIONS - 8)
#define N_DECK_EXTENDED_DECORATION_VALUE_BASE (N_DECK_STATUS_VALUE_BASE + 4)
#define N_DECK_EXTENDED_DECORATION_SCALE_Y_BASE \
    (N_DECK_EXTENDED_DECORATION_VALUE_BASE + N_DECK_EXTENDED_DECORATION_COUNT * 3)
#define N_DECK_LAYOUT_VALUES \
    (N_DECK_EXTENDED_DECORATION_SCALE_Y_BASE + N_DECK_EXTENDED_DECORATION_COUNT)
typedef struct {
    int header_dx, header_dy, header_scale, header_scale_y;
    int item_dx, item_dy, item_scale, item_scale_y;
    int controls_dx, controls_dy, controls_scale, controls_scale_y;
    int timeline_dx, timeline_dy, timeline_scale, timeline_scale_y;
    int status_dx, status_dy, status_scale, status_scale_y;
    int decoration_dx, decoration_dy, decoration_scale, decoration_scale_y;
    int decoration_extra_dx[N_DECK_DECORATIONS - 1], decoration_extra_dy[N_DECK_DECORATIONS - 1];
    int decoration_extra_scale[N_DECK_DECORATIONS - 1], decoration_extra_scale_y[N_DECK_DECORATIONS - 1];
    int sounds_enabled;
    char background_path[512];
    char header_texture_path[512];
    char status_texture_path[512];
    char button_texture_path[512]; /* Legacy fallback for all action buttons. */
    char button_texture_by_action[6][512]; /* Save, Load, Pause/Resume, Rewind, Background, Close Game. */
    char item_texture_path[512]; /* Legacy generic item image, used as fallback. */
    char item_texture_by_id[N_DECK_ITEM_TEXTURE_IDS][512];
    char decoration_path[512]; /* First decoration; retained for old configs. */
    char decoration_extra_path[N_DECK_DECORATIONS - 1][512];
    /* 1 = stretch to the assigned box; 0 = contain the full image without cropping.
       Indexed by the stable Companion Deck asset path table. */
    int asset_stretch[N_DECK_ASSET_PATHS];
    char button_sound_path[N_DECK_BUTTON_SOUNDS][512];
    char item_sound_path[512];
    char empty_item_sound_path[512];
} CompanionLayoutCfg;

typedef struct {
    int volume;                         /* game volume 0..200 percent */
    int latency;                        /* LAT_* */
    int pad_deadzone;                   /* stick deadzone percent */
    int menu_music, menu_music_vol, ui_sfx, ui_sfx_vol;
    char menu_music_path[512], ui_sfx_path[N_UI_SFX][512];
    int last_tab, show_stats;
    int launcher_ui_scale;              /* 75..125 percent, persisted UI zoom */
    int launcher_window_size;           /* 0 = compact, 1 = default, 2 = large */
    int launcher_fullscreen;            /* launcher fullscreen (desktop only) */
    int launcher_dual_screen;            /* Android: use the second display; desktop: open the Companion Deck simulator window. */
    int launcher_pipe_screen_swap;        /* Android: swap SML1 gameplay to the companion screen in pipes/bonus rooms. */
    int android_haptic_target;             /* Android only: auto, phone default, individual motor slots, then controller slots. */
    int android_haptic_target_version;      /* One-time migration from the original auto/phone/controller indices. */
    int museum_discovery_notifications;   /* Show a launcher toast when a new Museum entry is unlocked. */
    int recent_played_count;
    int recent_played[N_RECENT_PLAYED];  /* Game indices, most recent first; -1 = unused. */
    int game_play_seconds[N_GAMES];      /* Accumulated played time, persisted in whole seconds. */
    int menu_playlist_count;
    int menu_playlist_selected;
    char menu_playlist[MAX_MENU_TRACKS][512];
    char online_name[32];
    char online_server[160];          /* direct peer or traversal host:port */
    int online_connection_mode;          /* 0 = direct connection, 1 = traversal server */
    int online_color;                /* default profile colour, 0xRRGGBB */
    int touch_controls;                   /* on-screen controls are enabled */
    CompanionLayoutCfg deck_layout[N_GAMES];
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