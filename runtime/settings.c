#include "settings.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef USE_SDL
#include <SDL.h>
#else
/* same numeric values as SDL2, so settings files are interchangeable with headless builds */
#define SDLK_x 120
#define SDLK_k 107
#define SDLK_z 122
#define SDLK_j 106
#define SDLK_d 100
#define SDLK_a 97
#define SDLK_w 119
#define SDLK_s 115
#define SDLK_BACKSPACE 8
#define SDLK_RETURN 13
#define SDLK_RSHIFT 0x400000E5
#define SDLK_RIGHT 0x4000004F
#define SDLK_LEFT 0x40000050
#define SDLK_DOWN 0x40000051
#define SDLK_UP 0x40000052
#define SDLK_F5 0x4000003E
#define SDLK_F6 0x4000003F
#define SDLK_F7 0x40000040
#define SDLK_F8 0x40000041
#define SDLK_F10 0x40000043
enum { SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
       SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_GUIDE, SDL_CONTROLLER_BUTTON_START,
       SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
       SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
       SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT };
#endif

const char *aspect_names[] = {"Original (10:9)", "4:3", "16:9"};
const char *scale_names[] = {"Pixel-perfect", "Smooth", "Stretch", "Pixel stretch"};
const char *size_names[] = {"Small", "Medium", "Large", "Fullscreen"};
const char *btn_names[] = {"A", "B", "Select", "Start", "Right", "Left", "Up", "Down"};
const char *action_names[] = {"Save state", "Load state", "Rewind", "Suspend", "Next state slot"};
const char *led_names[] = {"Off", "Match colors", "Custom", "Follow screen"};
const char *lat_names[] = {"Low", "Normal", "High (safest)"};

#define MONO(n, a, b, c, d) {n, {a, b, c, d}, {a, b, c, d}, {a, b, c, d}, 0}
const Palette palettes[] = {
    MONO("Classic Green",   0xE0F8D0, 0x88C070, 0x346856, 0x081820),
    MONO("Original DMG",    0x9BBC0F, 0x8BAC0F, 0x306230, 0x0F380F),
    MONO("Pocket",          0xC4CFA1, 0x8B956D, 0x4D533C, 0x1F1F1F),
    MONO("Black & White",   0xFFFFFF, 0xAAAAAA, 0x555555, 0x000000),
    MONO("Ice Blue",        0xE8F4FF, 0x8EC5F0, 0x3A6EA5, 0x0B1F3A),
    MONO("Cherry",          0xFFE8E8, 0xF09090, 0xB03050, 0x3A0A1A),
    MONO("Gold",            0xFFF6D0, 0xF0C860, 0xA87820, 0x3A2A08),
    MONO("Grape",           0xF4E8FF, 0xB890E8, 0x6A3FA0, 0x24103F),
    MONO("Mint",            0xE8FFF4, 0x90E0B8, 0x3FA078, 0x0E3A28),
    MONO("Sepia",           0xF4E8D0, 0xC8A878, 0x7A5A3A, 0x2A1C10),
    MONO("Sunset",          0xFFF0D0, 0xFFA060, 0xC04060, 0x3A1040),
    MONO("Midnight",        0x9AB0D8, 0x5870B0, 0x28387A, 0x0A0F2A),
    /* GBC-style: background and the two sprite palettes each get their own colours */
    {"Classic (color)",
     {0xF8FFF0, 0xA8D8A0, 0x4F8F6F, 0x10281F},
     {0xFFF0E8, 0xF08878, 0xB02830, 0x3A0810},
     {0xF0F8FF, 0x88B8F0, 0x3050B8, 0x081840}, 1},
    {"Arcade (color)",
     {0xE8E8FF, 0x9090D0, 0x404090, 0x101030},
     {0xFFF8D0, 0xFFC040, 0xD06010, 0x401800},
     {0xE0FFFF, 0x60E0E0, 0x1890A0, 0x083038}, 1},
    {"Candy (color)",
     {0xFFF0F8, 0xF8B8D8, 0xB868A0, 0x502048},
     {0xFFFFF0, 0xF0E060, 0xD09020, 0x483008},
     {0xF0FFF0, 0x80E0A0, 0x30A070, 0x0C3828}, 1},
    {"Ocean (color)",
     {0xE8FAFF, 0x80D0E8, 0x2878B0, 0x08203C},
     {0xFFF4E0, 0xFFB868, 0xE06838, 0x481808},
     {0xFFFFFF, 0xD0D8F0, 0x7080C0, 0x202858}, 1},
};
const int n_palettes = (int)(sizeof palettes / sizeof palettes[0]);

const LuigiColor luigi_colors[N_LUIGI_COLORS] = {
    {"Green",  0x2CA83D, 0x8AE05A, 0x2CA83D, 0x175822},
    {"Blue",   0x2676D9, 0x8CD0FF, 0x2676D9, 0x123D82},
    {"Red",    0xE53935, 0xFF8A80, 0xE53935, 0x8C1818},
    {"Purple", 0x9C4DCC, 0xD6A5F5, 0x9C4DCC, 0x4C176D},
    {"Orange", 0xE27B1C, 0xFFC078, 0xE27B1C, 0x8F440D},
    {"Yellow", 0xD8B51A, 0xFFF18A, 0xD8B51A, 0x80650B}
};


Settings settings;

const char *filter_preset_names[] = {"None", "Game Boy LCD", "Pocket", "CRT TV", "Bloom", "HDR"};
const int n_filter_presets = 6;

void filter_preset(FilterCfg *f, int p)
{
    FilterCfg z = {0};
    z.preset = p;
    switch (p) {
    case 1: z.lcd_grid = 55; z.ghost = 45; z.blur = 10; z.vignette = 10; break;                 /* DMG */
    case 2: z.lcd_grid = 35; z.ghost = 25; z.vignette = 0; break;                                /* Pocket */
    case 3: z.scanlines = 55; z.mask = 35; z.curve = 40; z.bloom = 25; z.vignette = 35; z.blur = 30; break;
    case 4: z.bloom = 70; z.blur = 10; break;
    case 5: z.hdr = 70; z.bloom = 45; z.vignette = 10; break;
    default: break;
    }
    *f = z;
}

void controls_defaults(GameCfg *c)
{
    static const int key[N_BTN][2] = {
        {SDLK_x, SDLK_j}, {SDLK_z, SDLK_k}, {SDLK_RSHIFT, SDLK_BACKSPACE}, {SDLK_RETURN, 0},
        {SDLK_RIGHT, SDLK_d}, {SDLK_LEFT, SDLK_a}, {SDLK_UP, SDLK_w}, {SDLK_DOWN, SDLK_s}};
    static const int pad[N_BTN][2] = {
        {SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_B}, {SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_A},
        {SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_BACK}, {SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_START},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT}, {SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_LEFT},
        {SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_UP}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_DOWN}};
    for (int b = 0; b < N_BTN; b++) {
        for (int player = 0; player < 2; player++) {
            c->key[b][player] = key[b][player];
            c->pad[b][player] = pad[b][player];
        }
        for (int player = 2; player < MAX_MP_PLAYERS; player++) {
            c->key[b][player] = 0;
            c->pad[b][player] = pad[b][0];
        }
    }
    for (int player = 0; player < MAX_MP_PLAYERS; player++)
        c->pad_device[player] = player == 0 ? 0 : -1;
    c->p2_respawn_key = SDLK_r;
    c->p2_respawn_pad = SDL_CONTROLLER_BUTTON_RIGHTSTICK;
    c->p3_respawn_key = SDLK_t;
    c->p3_respawn_pad = SDL_CONTROLLER_BUTTON_RIGHTSTICK;
    c->p4_respawn_key = SDLK_y;
    c->p4_respawn_pad = SDL_CONTROLLER_BUTTON_RIGHTSTICK;
}


void shortcut_defaults(GameCfg *c)
{
    static const int key[N_ACTION] = {SDLK_F5, SDLK_F8, SDLK_F7, SDLK_F10, SDLK_F6};
    if (!c) return;
    memcpy(c->action_key, key, sizeof key);
    for (int i = 0; i < N_ACTION; i++) c->action_pad[i] = -1;
}

void game_cfg_defaults(GameCfg *c, int game)
{
    memset(c, 0, sizeof *c);
    c->aspect = ASPECT_ORIGINAL;
    c->scaling = SCALE_PIXEL;
    c->size = SIZE_MEDIUM;
    c->palette = game == GAME_DRMARIO ? 0 : (game == GAME_SML ? 1 : 12);
    c->bg_dim = 25;
    c->wide = 0;
    c->state_slot = 0;
    c->romhack_count = 0;
    c->romhack_selected = -1; /* The unmodified game is the default profile. */
    c->p2_color = LUIGI_GREEN; c->p2_rgb = 0x2CA83D;
    c->p3_color = LUIGI_BLUE; c->p3_rgb = 0x2676D9;
    c->p4_color = LUIGI_YELLOW; c->p4_rgb = 0xD8B51A;
    c->multiplayer_players = 2;
    if (game == GAME_SML || game == GAME_SML2) {
        snprintf(c->p2_name, sizeof c->p2_name, "Luigi");
        snprintf(c->p3_name, sizeof c->p3_name, "Bunzo");
        snprintf(c->p4_name, sizeof c->p4_name, "Florbo");
    }
    shortcut_defaults(c);
    c->tex_collect = 1;
    c->ds_led_mode = LED_PALETTE; c->ds_bright = 60; c->ds_color = 0x40A0FF;
    c->ds_rumble = 70; c->ds_speaker_vol = 55;
    c->ds_ev_led = 0xFF; c->ds_ev_rumble = 0xFF;
    int wario_game = game == GAME_WARIO_SML3 || game == GAME_WARIO_LAND2_GB ||
                     game == GAME_WARIO_LAND3_GBC || game == GAME_WARIO_LAND2_GBC;
    c->ds_ev_speaker = !wario_game ? 0 :
        ((game == GAME_WARIO_LAND2_GB || game == GAME_WARIO_LAND2_GBC) ? 0x1F : 0x7F);
    c->ds_fx_version = 2;
    controls_defaults(c);
}

void settings_defaults(Settings *s)
{
    memset(s, 0, sizeof *s);
    s->volume = 100;
    s->latency = LAT_NORMAL;
    s->pad_deadzone = 35;
    s->menu_music = 1; s->menu_music_vol = 55;
    s->ui_sfx = 1; s->ui_sfx_vol = 70;
    snprintf(s->online_name, sizeof s->online_name, "Player");
    snprintf(s->online_server, sizeof s->online_server, "%s", PIPECLEAN_DEFAULT_RELAY_ENDPOINT);
    s->online_connection_mode = 1; /* traversal server by default */
    s->online_color = 0x2CA83D;
    s->touch_controls = 1;
    s->launcher_ui_scale = 100;
    s->launcher_window_size = 1;
    s->launcher_fullscreen = 0;
    s->launcher_dual_screen = 0;
    s->launcher_pipe_screen_swap = 0;
    s->recent_played_count = 0;
    s->menu_playlist_count = 0;
    s->menu_playlist_selected = 0;
    for (int i = 0; i < N_RECENT_PLAYED; i++) s->recent_played[i] = -1;
    for (int i = 0; i < N_GAMES; i++) {
        s->game_play_seconds[i] = 0;
        game_cfg_defaults(&s->g[i], i);
        CompanionLayoutCfg *d = &s->deck_layout[i];
        d->header_scale = d->item_scale = d->controls_scale =
            d->timeline_scale = d->status_scale = d->decoration_scale = 100;
        d->header_scale_y = d->item_scale_y = d->controls_scale_y =
            d->timeline_scale_y = d->status_scale_y = d->decoration_scale_y = 100;
        for (int slot = 0; slot < N_DECK_DECORATIONS - 1; slot++)
            d->decoration_extra_scale[slot] = d->decoration_extra_scale_y[slot] = 100;
        d->sounds_enabled = 1;
    }
}

const char *settings_dir(void)
{
    static char dir[1024];
    if (dir[0]) return dir;
#ifdef USE_SDL
#ifdef __ANDROID__
    /* Android's packaged application directory is not writable; keep settings
       and save files in SDL's app-private persistent preference directory. */
    char *p = SDL_GetPrefPath("TotallyJustRetro", "PipeClean");
    if (p) {
        snprintf(dir, sizeof dir, "%s", p);
        SDL_free(p);
        (void)mkdir_u(dir);
        return dir;
    }
#endif
    char *b = SDL_GetBasePath();
    if (b) { snprintf(dir, sizeof dir, "%s", b); SDL_free(b); return dir; }
#endif
    snprintf(dir, sizeof dir, "./");
    return dir;
}

static void ini_path(char *out, size_t n) { snprintf(out, n, "%spipeclean.ini", settings_dir()); }
static void legacy_ini_path(char *out, size_t n) { snprintf(out, n, "%sdr_mario_launcher.ini", settings_dir()); }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ---- key=value table ---- */
typedef struct { const char *name; int *ip; char *sp; size_t slen; int lo, hi; } Field;

static int field_table(Field *t, int cap)
{
    int n = 0;
#define I(nm, p, l, h) do { if (n < cap) t[n++] = (Field){nm, &(p), NULL, 0, l, h}; } while (0)
#define S(nm, p)       do { if (n < cap) t[n++] = (Field){nm, NULL, p, sizeof(p), 0, 0}; } while (0)
    Settings *s = &settings;
    I("volume", s->volume, 0, 200); I("latency", s->latency, 0, N_LAT - 1); I("deadzone", s->pad_deadzone, 5, 80);
    I("menu_music", s->menu_music, 0, 1); I("menu_music_vol", s->menu_music_vol, 0, 100);
    I("ui_sfx", s->ui_sfx, 0, 1); I("ui_sfx_vol", s->ui_sfx_vol, 0, 100);
    S("menu_music_path", s->menu_music_path);
    static char names[N_UI_SFX][24];
    for (int i = 0; i < N_UI_SFX; i++) { snprintf(names[i], sizeof names[i], "ui_sfx_path%d", i); S(names[i], s->ui_sfx_path[i]); }
    I("last_tab", s->last_tab, 0, 20); I("show_stats", s->show_stats, 0, 1);
    I("launcher_ui_scale", s->launcher_ui_scale, 75, 125);
    I("launcher_window_size", s->launcher_window_size, 0, 2);
    I("launcher_fullscreen", s->launcher_fullscreen, 0, 1);
    I("launcher_dual_screen", s->launcher_dual_screen, 0, 1);
    I("launcher_pipe_screen_swap", s->launcher_pipe_screen_swap, 0, 1);
    I("recent_played_count", s->recent_played_count, 0, N_RECENT_PLAYED);
    static char recent_names[N_RECENT_PLAYED][32];
    for (int i = 0; i < N_RECENT_PLAYED; i++) {
        snprintf(recent_names[i], sizeof recent_names[i], "recent_played_%d", i);
        I(recent_names[i], s->recent_played[i], -1, N_GAMES - 1);
    }
    static char played_seconds_names[N_GAMES][40];
    for (int i = 0; i < N_GAMES; i++) {
        snprintf(played_seconds_names[i], sizeof played_seconds_names[i], "game_play_seconds_%d", i);
        I(played_seconds_names[i], s->game_play_seconds[i], 0, 2147483647);
    }
    I("menu_playlist_count", s->menu_playlist_count, 0, MAX_MENU_TRACKS);
    I("menu_playlist_selected", s->menu_playlist_selected, 0, MAX_MENU_TRACKS - 1);
    static char playlist_names[MAX_MENU_TRACKS][40];
    for (int i = 0; i < MAX_MENU_TRACKS; i++) {
        snprintf(playlist_names[i], sizeof playlist_names[i], "menu_playlist_%d", i);
        S(playlist_names[i], s->menu_playlist[i]);
    }
    S("online_name", s->online_name); S("online_server", s->online_server); I("online_connection_mode", s->online_connection_mode, 0, 1); I("online_color", s->online_color, 0, 0xFFFFFF);
    I("touch_controls", s->touch_controls, 0, 1);
    FilterCfg *f = &s->flt;
    I("f_grid", f->lcd_grid, 0, 100); I("f_ghost", f->ghost, 0, 100); I("f_scan", f->scanlines, 0, 100);
    I("f_mask", f->mask, 0, 100); I("f_curve", f->curve, 0, 100); I("f_bloom", f->bloom, 0, 100);
    I("f_hdr", f->hdr, 0, 100); I("f_vignette", f->vignette, 0, 100); I("f_blur", f->blur, 0, 100);
    I("f_preset", f->preset, 0, n_filter_presets - 1);
    /* Includes the added Companion Deck layer paths/transforms and per-asset modes. */
    static char gn[N_GAMES][512][64];
    static int gi = 0; (void)gi;
    for (int g = 0; g < N_GAMES; g++) {
        GameCfg *c = &s->g[g];
        int k = 0;
#define GI(nm, p, l, h) do { snprintf(gn[g][k], sizeof gn[g][k], "%s.%s", games[g].id, nm); I(gn[g][k], p, l, h); k++; } while (0)
#define GS(nm, p) do { snprintf(gn[g][k], sizeof gn[g][k], "%s.%s", games[g].id, nm); S(gn[g][k], p); k++; } while (0)
        GS("rom", c->rom_path); GS("hack", c->hack_path); GS("background", c->bg_path); GS("texpack", c->tex_path);
        GI("romhack_count", c->romhack_count, 0, MAX_ROMHACKS);
        GI("romhack_selected", c->romhack_selected, -1, MAX_ROMHACKS - 1);
        GS("p2_name", c->p2_name);
        GS("p2_sfx_jump", c->p2_sfx_path[P2_SFX_JUMP]); GS("p2_sfx_fireball", c->p2_sfx_path[P2_SFX_FIREBALL]);
        GS("p2_sfx_power_up", c->p2_sfx_path[P2_SFX_POWER_UP]); GS("p2_sfx_power_down", c->p2_sfx_path[P2_SFX_POWER_DOWN]);
        GS("p2_sfx_die", c->p2_sfx_path[P2_SFX_DIE]); GS("p2_sfx_respawn", c->p2_sfx_path[P2_SFX_RESPAWN]); GS("p2_sfx_coin", c->p2_sfx_path[P2_SFX_COIN]);
        GI("p2_color", c->p2_color, 0, N_LUIGI_COLORS - 1); GI("p2_rgb", c->p2_rgb, 0, 0xFFFFFF); GI("p2_custom", c->p2_custom, 0, 1);
        GS("p3_name", c->p3_name);
        GS("p3_sfx_jump", c->p3_sfx_path[P2_SFX_JUMP]); GS("p3_sfx_fireball", c->p3_sfx_path[P2_SFX_FIREBALL]);
        GS("p3_sfx_power_up", c->p3_sfx_path[P2_SFX_POWER_UP]); GS("p3_sfx_power_down", c->p3_sfx_path[P2_SFX_POWER_DOWN]);
        GS("p3_sfx_die", c->p3_sfx_path[P2_SFX_DIE]); GS("p3_sfx_respawn", c->p3_sfx_path[P2_SFX_RESPAWN]); GS("p3_sfx_coin", c->p3_sfx_path[P2_SFX_COIN]);
        GI("p3_color", c->p3_color, 0, N_LUIGI_COLORS - 1); GI("p3_rgb", c->p3_rgb, 0, 0xFFFFFF); GI("p3_custom", c->p3_custom, 0, 1);
        GS("p4_name", c->p4_name);
        GS("p4_sfx_jump", c->p4_sfx_path[P2_SFX_JUMP]); GS("p4_sfx_fireball", c->p4_sfx_path[P2_SFX_FIREBALL]);
        GS("p4_sfx_power_up", c->p4_sfx_path[P2_SFX_POWER_UP]); GS("p4_sfx_power_down", c->p4_sfx_path[P2_SFX_POWER_DOWN]);
        GS("p4_sfx_die", c->p4_sfx_path[P2_SFX_DIE]); GS("p4_sfx_respawn", c->p4_sfx_path[P2_SFX_RESPAWN]); GS("p4_sfx_coin", c->p4_sfx_path[P2_SFX_COIN]);
        GI("p4_color", c->p4_color, 0, N_LUIGI_COLORS - 1); GI("p4_rgb", c->p4_rgb, 0, 0xFFFFFF); GI("p4_custom", c->p4_custom, 0, 1);
        GI("multiplayer_players", c->multiplayer_players, 2, MAX_MP_PLAYERS);
        GI("palette", c->palette, 0, n_palettes - 1); GI("aspect", c->aspect, 0, N_ASPECT - 1);
        GI("scaling", c->scaling, 0, N_SCALE - 1); GI("size", c->size, 0, N_SIZE - 1);
        GI("bg_dim", c->bg_dim, 0, 80); GI("wide", c->wide, 0, 100); GI("tex_on", c->tex_on, 0, 1); GI("state_slot", c->state_slot, 0, 9); GI("tex_collect", c->tex_collect, 0, 1); GI("multiplayer", c->multiplayer, 0, 1); GI("dual_screen", c->dual_screen, 0, 1); GI("pipe_screen_swap", c->pipe_screen_swap, 0, 1);
        GI("p2_respawn_key", c->p2_respawn_key, 0, 0x7FFFFFFF); GI("p2_respawn_pad", c->p2_respawn_pad, -1, PAD_AXIS_BASE + 1);
        GI("p3_respawn_key", c->p3_respawn_key, 0, 0x7FFFFFFF); GI("p3_respawn_pad", c->p3_respawn_pad, -1, PAD_AXIS_BASE + 1);
        GI("p4_respawn_key", c->p4_respawn_key, 0, 0x7FFFFFFF); GI("p4_respawn_pad", c->p4_respawn_pad, -1, PAD_AXIS_BASE + 1);
        GI("pad_device1", c->pad_device[0], -1, 3); GI("pad_device2", c->pad_device[1], -1, 3);
        GI("pad_device3", c->pad_device[2], -1, 3); GI("pad_device4", c->pad_device[3], -1, 3);
        GS("pad_guid1", c->pad_guid[0]); GS("pad_guid2", c->pad_guid[1]);
        GS("pad_guid3", c->pad_guid[2]); GS("pad_guid4", c->pad_guid[3]);
        GI("led", c->ds_led_mode, 0, N_LED - 1); GI("led_bright", c->ds_bright, 0, 100);
        GI("rumble", c->ds_rumble, 0, 100); GI("spk_vol", c->ds_speaker_vol, 0, 100);
        GI("ev_led", c->ds_ev_led, 0, 255); GI("ev_rumble", c->ds_ev_rumble, 0, 255); GI("ev_speaker", c->ds_ev_speaker, 0, 255);
        GI("fx_version", c->ds_fx_version, 0, 2);

        CompanionLayoutCfg *d = &s->deck_layout[g];
        GI("deck_header_dx", d->header_dx, -400, 400); GI("deck_header_dy", d->header_dy, -400, 400); GI("deck_header_scale", d->header_scale, 25, 250); GI("deck_header_scale_y", d->header_scale_y, 25, 250);
        GI("deck_item_dx", d->item_dx, -400, 400); GI("deck_item_dy", d->item_dy, -400, 400); GI("deck_item_scale", d->item_scale, 25, 250); GI("deck_item_scale_y", d->item_scale_y, 25, 250);
        GI("deck_controls_dx", d->controls_dx, -400, 400); GI("deck_controls_dy", d->controls_dy, -400, 400); GI("deck_controls_scale", d->controls_scale, 25, 250); GI("deck_controls_scale_y", d->controls_scale_y, 25, 250);
        GI("deck_timeline_dx", d->timeline_dx, -400, 400); GI("deck_timeline_dy", d->timeline_dy, -400, 400); GI("deck_timeline_scale", d->timeline_scale, 25, 250); GI("deck_timeline_scale_y", d->timeline_scale_y, 25, 250);
        GI("deck_status_dx", d->status_dx, -400, 400); GI("deck_status_dy", d->status_dy, -400, 400); GI("deck_status_scale", d->status_scale, 25, 250); GI("deck_status_scale_y", d->status_scale_y, 25, 250);
        GI("deck_decoration_dx", d->decoration_dx, -400, 400); GI("deck_decoration_dy", d->decoration_dy, -400, 400); GI("deck_decoration_scale", d->decoration_scale, 25, 250); GI("deck_decoration_scale_y", d->decoration_scale_y, 25, 250);
        for (int slot = 0; slot < N_DECK_DECORATIONS - 1; slot++) {
            int slot_id = slot + 2;
            snprintf(gn[g][k], sizeof gn[g][k], "%s.deck_decoration_%d_dx", games[g].id, slot_id);
            I(gn[g][k], d->decoration_extra_dx[slot], -400, 400); k++;
            snprintf(gn[g][k], sizeof gn[g][k], "%s.deck_decoration_%d_dy", games[g].id, slot_id);
            I(gn[g][k], d->decoration_extra_dy[slot], -400, 400); k++;
            snprintf(gn[g][k], sizeof gn[g][k], "%s.deck_decoration_%d_scale", games[g].id, slot_id);
            I(gn[g][k], d->decoration_extra_scale[slot], 25, 250); k++;
            snprintf(gn[g][k], sizeof gn[g][k], "%s.deck_decoration_%d_scale_y", games[g].id, slot_id);
            I(gn[g][k], d->decoration_extra_scale_y[slot], 25, 250); k++;
        }
        GI("deck_sounds_enabled", d->sounds_enabled, 0, 1);
        GS("deck_background", d->background_path);
        GS("deck_header_texture", d->header_texture_path);
        GS("deck_status_texture", d->status_texture_path);
        GS("deck_button_texture", d->button_texture_path);
        GS("deck_item_texture", d->item_texture_path); GS("deck_decoration", d->decoration_path);
        for (int slot = 0; slot < N_DECK_DECORATIONS - 1; slot++) {
            snprintf(gn[g][k], sizeof gn[g][k], "%s.deck_decoration_%d", games[g].id, slot + 2);
            S(gn[g][k], d->decoration_extra_path[slot]); k++;
        }
        static const char *deck_button_texture_names[6] = {
            "deck_button_save_texture", "deck_button_load_texture",
            "deck_button_pause_texture", "deck_button_rewind_texture",
            "deck_button_background_texture", "deck_button_close_texture"
        };
        for (int b = 0; b < 6; b++) {
            snprintf(gn[g][k], sizeof gn[g][k], "%s.%s", games[g].id, deck_button_texture_names[b]);
            S(gn[g][k], d->button_texture_by_action[b]); k++;
        }
        for (int item_id = 1; item_id < N_DECK_ITEM_TEXTURE_IDS; item_id++) {
            snprintf(gn[g][k], sizeof gn[g][k], "%s.deck_item_texture_%d", games[g].id, item_id);
            S(gn[g][k], d->item_texture_by_id[item_id]); k++;
        }
        static const char *deck_sound_names[N_DECK_BUTTON_SOUNDS] = {
            "deck_sound_save", "deck_sound_load", "deck_sound_pause", "deck_sound_rewind",
            "deck_sound_background", "deck_sound_close"
        };
        for (int b = 0; b < N_DECK_BUTTON_SOUNDS; b++) {
            snprintf(gn[g][k], sizeof gn[g][k], "%s.%s", games[g].id, deck_sound_names[b]);
            S(gn[g][k], d->button_sound_path[b]); k++;
        }
        GS("deck_item_sound", d->item_sound_path); GS("deck_empty_item_sound", d->empty_item_sound_path);
        for (int asset = 0; asset < N_DECK_ASSET_PATHS; asset++) {
            snprintf(gn[g][k], sizeof gn[g][k], "%s.deck_asset_stretch_%d", games[g].id, asset);
            I(gn[g][k], d->asset_stretch[asset], 0, 1); k++;
        }
#undef GI
#undef GS
    }
#undef I
#undef S
    return n;
}

void settings_load(void)
{
    settings_defaults(&settings);
    char path[1100];
    ini_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) { legacy_ini_path(path, sizeof path); f = fopen(path, "r"); }
    if (!f) return;
    /* Per-game Companion Deck fit/stretch flags add 28 keys for each game.
       Keep capacity comfortably above the full global + per-game settings table. */
    Field tab[1800];
    int nf = field_table(tab, 1800);
    int saw_launcher_dual_screen = 0;
    int saw_launcher_pipe_screen_swap = 0;
    int saw_wario_fx_version[N_GAMES] = {0};
    char line[1100];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        if (!strcmp(line, "launcher_dual_screen")) saw_launcher_dual_screen = 1;
        if (!strcmp(line, "launcher_pipe_screen_swap")) saw_launcher_pipe_screen_swap = 1;
        for (int g = GAME_WARIO_SML3; g < N_GAMES; g++) {
            char fx_key[80];
            snprintf(fx_key, sizeof fx_key, "%s.fx_version", games[g].id);
            if (!strcmp(line, fx_key)) saw_wario_fx_version[g] = 1;
        }
        char *v = eq + 1;
        size_t l = strlen(v);
        while (l && (v[l - 1] == '\n' || v[l - 1] == '\r')) v[--l] = 0;
        int done = 0;
        for (int i = 0; i < nf && !done; i++) {
            if (strcmp(line, tab[i].name)) continue;
            if (tab[i].ip) *tab[i].ip = clampi(atoi(v), tab[i].lo, tab[i].hi);
            else snprintf(tab[i].sp, tab[i].slen, "%s", v);
            done = 1;
        }
        if (done) continue;
        /* arrays: "<game>.key<btn>_<slot>", "<game>.pad<btn>_<slot>", "<game>.color" */
        for (int g = 0; g < N_GAMES; g++) {
            size_t gl = strlen(games[g].id);
            if (strncmp(line, games[g].id, gl) || line[gl] != '.') continue;
            const char *k = line + gl + 1;
            int b, s2, hi;
            char profile_field[32];
            if (sscanf(k, "romhack%d_%31s", &hi, profile_field) == 2 &&
                hi >= 0 && hi < MAX_ROMHACKS) {
                RomHackEntry *entry = &settings.g[g].romhacks[hi];
                if (!strcmp(profile_field, "path")) snprintf(entry->path, sizeof entry->path, "%s", v);
                else if (!strcmp(profile_field, "title")) snprintf(entry->title, sizeof entry->title, "%s", v);
                else if (!strcmp(profile_field, "thumbnail")) snprintf(entry->thumbnail, sizeof entry->thumbnail, "%s", v);
            } else if (sscanf(k, "key%d_%d", &b, &s2) == 2 && b >= 0 && b < N_BTN && s2 >= 0 && s2 < MAX_MP_PLAYERS) settings.g[g].key[b][s2] = atoi(v);
            else if (sscanf(k, "pad%d_%d", &b, &s2) == 2 && b >= 0 && b < N_BTN && s2 >= 0 && s2 < MAX_MP_PLAYERS) settings.g[g].pad[b][s2] = atoi(v);
            else if (sscanf(k, "actionkey%d", &b) == 1 && b >= 0 && b < N_ACTION) settings.g[g].action_key[b] = atoi(v);
            else if (sscanf(k, "actionpad%d", &b) == 1 && b >= 0 && b < N_ACTION) settings.g[g].action_pad[b] = atoi(v);
            else if (!strcmp(k, "ledcolor")) settings.g[g].ds_color = (uint32_t)strtoul(v, NULL, 16) & 0xFFFFFF;
        }
    }
    fclose(f);
    /* Older Wario configs saved speaker events disabled. Initialize those
       once, then add only the new Wario Land II coin-loss bit for version 2
       so users' existing event toggles are preserved. */
    int wario_fx_migrated = 0;
    for (int g = GAME_WARIO_SML3; g < N_GAMES; g++) {
        GameCfg *c = &settings.g[g];
        if (!saw_wario_fx_version[g] || c->ds_fx_version < 1) {
            c->ds_ev_speaker = (g == GAME_WARIO_LAND2_GB ||
                                g == GAME_WARIO_LAND2_GBC) ? 0x1F : 0x7F;
            c->ds_fx_version = 2;
            wario_fx_migrated = 1;
        } else if (c->ds_fx_version < 2) {
            if (g == GAME_WARIO_LAND2_GB || g == GAME_WARIO_LAND2_GBC)
                c->ds_ev_speaker |= 0x10;
            c->ds_fx_version = 2;
            wario_fx_migrated = 1;
        }
    }
    if (wario_fx_migrated) settings_save();
    if (!saw_launcher_dual_screen) {
        /* Migrate the old per-game switch so existing Android users keep their
         * preference after the option moves into Launcher Display settings. */
        for (int g = 0; g < N_GAMES; g++) {
            if (settings.g[g].dual_screen) {
                settings.launcher_dual_screen = 1;
                break;
            }
        }
    }
    if (!saw_launcher_pipe_screen_swap)
        settings.launcher_pipe_screen_swap = settings.g[GAME_SML].pipe_screen_swap;
    int highest_playlist = 0;
    for (int i = 0; i < MAX_MENU_TRACKS; i++)
        if (settings.menu_playlist[i][0]) highest_playlist = i + 1;
    if (settings.menu_playlist_count < highest_playlist)
        settings.menu_playlist_count = highest_playlist;
    if (settings.menu_playlist_count > MAX_MENU_TRACKS)
        settings.menu_playlist_count = MAX_MENU_TRACKS;
    if (settings.menu_playlist_count == 0 && settings.menu_music_path[0]) {
        snprintf(settings.menu_playlist[0], sizeof settings.menu_playlist[0], "%s",
                 settings.menu_music_path);
        settings.menu_playlist_count = 1;
        settings.menu_playlist_selected = 0;
    }
    if (settings.menu_playlist_count <= 0) settings.menu_playlist_selected = 0;
    else if (settings.menu_playlist_selected >= settings.menu_playlist_count)
        settings.menu_playlist_selected = settings.menu_playlist_count - 1;
    if (settings.recent_played_count > N_RECENT_PLAYED)
        settings.recent_played_count = N_RECENT_PLAYED;
    for (int i = 0; i < N_RECENT_PLAYED; i++)
        if (settings.recent_played[i] < -1 || settings.recent_played[i] >= N_GAMES)
            settings.recent_played[i] = -1;
    for (int g = 0; g < N_GAMES; g++) {
        GameCfg *c = &settings.g[g];
        int highest = 0;
        for (int i = 0; i < MAX_ROMHACKS; i++)
            if (c->romhacks[i].path[0]) highest = i + 1;
        if (c->romhack_count < highest) c->romhack_count = highest;
        if (c->romhack_count > MAX_ROMHACKS) c->romhack_count = MAX_ROMHACKS;
        if (c->romhack_selected < -1 || c->romhack_selected >= c->romhack_count)
            c->romhack_selected = -1;
    }
    if (settings.last_tab > N_GAMES + 5) settings.last_tab = 0;

    /* Migrate the prior local-only default to the deployed traversal endpoint.
       If the user explicitly configured another address, leave it untouched. */
    if (strcmp(settings.online_server, "127.0.0.1:27800") == 0 &&
        strcmp(PIPECLEAN_DEFAULT_RELAY_ENDPOINT, "127.0.0.1:27800") != 0) {
        snprintf(settings.online_server, sizeof settings.online_server, "%s",
                 PIPECLEAN_DEFAULT_RELAY_ENDPOINT);
        settings_save();
    }
}

void settings_save(void)
{
    char path[1100];
    ini_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    /* Per-game Companion Deck fit/stretch flags add 28 keys for each game.
       Keep capacity comfortably above the full global + per-game settings table. */
    Field tab[1800];
    int nf = field_table(tab, 1800);
    for (int i = 0; i < nf; i++) {
        if (tab[i].ip) fprintf(f, "%s=%d\n", tab[i].name, *tab[i].ip);
        else fprintf(f, "%s=%s\n", tab[i].name, tab[i].sp);
    }
    for (int g = 0; g < N_GAMES; g++) {
        for (int b = 0; b < N_BTN; b++)
            for (int s = 0; s < MAX_MP_PLAYERS; s++) {
                fprintf(f, "%s.key%d_%d=%d\n", games[g].id, b, s, settings.g[g].key[b][s]);
                fprintf(f, "%s.pad%d_%d=%d\n", games[g].id, b, s, settings.g[g].pad[b][s]);
            }
        for (int a = 0; a < N_ACTION; a++) {
            fprintf(f, "%s.actionkey%d=%d\n", games[g].id, a, settings.g[g].action_key[a]);
            fprintf(f, "%s.actionpad%d=%d\n", games[g].id, a, settings.g[g].action_pad[a]);
        }
        fprintf(f, "%s.ledcolor=%06X\n", games[g].id, settings.g[g].ds_color);
        for (int i = 0; i < MAX_ROMHACKS; i++) {
            const RomHackEntry *entry = &settings.g[g].romhacks[i];
            if (!entry->path[0]) continue;
            fprintf(f, "%s.romhack%d_path=%s\n", games[g].id, i, entry->path);
            fprintf(f, "%s.romhack%d_title=%s\n", games[g].id, i, entry->title);
            fprintf(f, "%s.romhack%d_thumbnail=%s\n", games[g].id, i, entry->thumbnail);
        }
    }
    fclose(f);
}