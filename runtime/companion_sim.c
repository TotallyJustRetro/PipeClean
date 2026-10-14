/* Desktop-only Companion Deck simulator.
 * This renders the Android second-screen layout into a resizable peer window so
 * the layout and state can be inspected on machines with one physical display. */
#include "companion_sim.h"
#include "settings.h"
#include "games.h"
#include "emu.h"
#include "cart.h"
#include "launcher.h"
#include "audio.h"
#include "util.h"
#include <sys/stat.h>
#include <time.h>
#include <SDL_image.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define SIM_EVENT_CODE 0x5043
#define SIM_SAVE 1
#define SIM_LOAD 2
#define SIM_PAUSE 3
#define SIM_REWIND 4
#define SIM_PALETTE 5
#define SIM_FILTER 6
#define SIM_SLOT_PREV 7
#define SIM_SLOT_NEXT 8
#define SIM_ITEM_USE 9
#define SIM_BACKGROUND 10
#define SIM_CLOSE_GAME 12
#define SIM_SLOT_SELECT_BASE 20

static SDL_Window *deck_window;
static SDL_Renderer *deck_renderer;
static SDL_Texture *assets[N_GAMES][N_DECK_ASSET_PATHS];
static IMG_Animation *asset_animations[N_GAMES][N_DECK_ASSET_PATHS];
static int asset_animation_frame[N_GAMES][N_DECK_ASSET_PATHS];
static Uint32 asset_animation_started[N_GAMES][N_DECK_ASSET_PATHS];
static char asset_paths[N_GAMES][N_DECK_ASSET_PATHS][512];
static int current_game = -1, current_lives, current_stage = -1, current_world = -1;
static int current_paused, current_slot, current_item, current_playing;
static int current_dr_mario_score = -1, current_dr_mario_level = -1;
static int current_dr_mario_speed = -1, current_dr_mario_capsule = 0xFF;
static int current_wario_coins = -1, current_wario_hearts = -1, current_wario_power = -1;
static int current_wario_treasure_mask = -1, current_wario_time = -1;
static char wario_expedition_log[5][64];
static int wario_expedition_log_count, wario_expedition_baseline;
static int wario_previous_coins = -1, wario_previous_hearts = -1, wario_previous_power = -1;
static int wario_previous_treasure_mask = -1, wario_previous_stage = -1, wario_previous_lives = -1;
static int wario_treasure_animation_id = -1;
static Uint32 wario_treasure_animation_started;
static int wario_treasure_animation_active;
enum { SIM_ITEM_ANIM_IDLE, SIM_ITEM_ANIM_ARRIVE, SIM_ITEM_ANIM_SEND_OFF };
static int item_animation_kind, item_animation_item;
static Uint32 item_animation_started;
static int pressed_action, mouse_down_action;
static SDL_Texture *dashboard_preview_texture;
static int dashboard_preview_game = -2;
static Uint32 dashboard_preview_tick;
static int desktop_music_notice_serial;
static Uint32 desktop_music_notice_started;
static int desktop_music_notice_visible;
static char desktop_music_notice_title[512] = "PipeClean Chiptune";
static SDL_Texture *slot_thumb_texture[N_GAMES][10];
static char slot_thumb_path[N_GAMES][10][1200];
static time_t slot_thumb_mtime[N_GAMES][10];
static const uint8_t font5x7[36][7] = {
    {14,17,17,31,17,17,17}, /* A */
    {30,17,17,30,17,17,30}, /* B */
    {14,17,16,16,16,17,14}, /* C */
    {30,17,17,17,17,17,30}, /* D */
    {31,16,16,30,16,16,31}, /* E */
    {31,16,16,30,16,16,16}, /* F */
    {14,17,16,23,17,17,15}, /* G */
    {17,17,17,31,17,17,17}, /* H */
    {14,4,4,4,4,4,14},      /* I */
    {7,2,2,2,18,18,12},     /* J */
    {17,18,20,24,20,18,17}, /* K */
    {16,16,16,16,16,16,31}, /* L */
    {17,27,21,21,17,17,17}, /* M */
    {17,25,21,19,17,17,17}, /* N */
    {14,17,17,17,17,17,14}, /* O */
    {30,17,17,30,16,16,16}, /* P */
    {14,17,17,17,21,18,13}, /* Q */
    {30,17,17,30,20,18,17}, /* R */
    {15,16,16,14,1,1,30},   /* S */
    {31,4,4,4,4,4,4},       /* T */
    {17,17,17,17,17,17,14}, /* U */
    {17,17,17,17,17,10,4},  /* V */
    {17,17,17,21,21,21,10}, /* W */
    {17,17,10,4,10,17,17},  /* X */
    {17,17,10,4,4,4,4},     /* Y */
    {31,1,2,4,8,16,31},     /* Z */
    {14,17,19,21,25,17,14}, /* 0 */
    {4,12,4,4,4,4,14},      /* 1 */
    {14,17,1,2,4,8,31},     /* 2 */
    {30,1,1,14,1,1,30},     /* 3 */
    {2,6,10,18,31,2,2},     /* 4 */
    {31,16,16,30,1,1,30},   /* 5 */
    {14,16,16,30,17,17,14}, /* 6 */
    {31,1,2,4,8,8,8},       /* 7 */
    {14,17,17,14,17,17,14}, /* 8 */
    {14,17,17,15,1,1,14},   /* 9 */
};

/* Wario Land's two supported cartridge profiles use one 8 KiB RAM bank.
 * Sample the backing bytes directly; the game disables SRAM between accesses. */
extern uint8_t *cart_ram;
static uint8_t wario_ram_read(uint16_t address)
{
    return cart_ram ? cart_ram[address & 0x1FFFu] : 0xFF;
}

static int wario_bcd_pair(uint8_t value)
{
    int tens = (value >> 4) & 0x0F, ones = value & 0x0F;
    return tens <= 9 && ones <= 9 ? tens * 10 + ones : -1;
}

static int wario_bcd_coins(void)
{
    uint8_t high = wario_ram_read(0xA805), mid = wario_ram_read(0xA806), low = wario_ram_read(0xA807);
    int d0 = high & 0x0F, d1 = (mid >> 4) & 0x0F;
    int d2 = mid & 0x0F, d3 = (low >> 4) & 0x0F, d4 = low & 0x0F;
    if (d0 > 9 || d1 > 9 || d2 > 9 || d3 > 9 || d4 > 9) return -1;
    return d0 * 10000 + d1 * 1000 + d2 * 100 + d3 * 10 + d4;
}

static int wario_bcd_time(void)
{
    uint8_t high = wario_ram_read(0xA964), low = wario_ram_read(0xA965);
    int hundreds = high & 0x0F, tens = (low >> 4) & 0x0F, ones = low & 0x0F;
    if (high == 0xFF || low == 0xFF || hundreds > 9 || tens > 9 || ones > 9) return -1;
    return hundreds * 100 + tens * 10 + ones;
}

static const char *wario_power_name(int power)
{
    switch (power) {
    case 0: return "SMALL";
    case 1: return "GARLIC";
    case 2: return "BULL";
    case 3: return "JET";
    case 4: return "DRAGON";
    default: return "UNKNOWN";
    }
}

static void wario_log_push(const char *entry)
{
    for (int i = 4; i > 0; i--)
        memcpy(wario_expedition_log[i], wario_expedition_log[i - 1],
               sizeof wario_expedition_log[i]);
    snprintf(wario_expedition_log[0], sizeof wario_expedition_log[0], "%s",
             entry ? entry : "");
    if (wario_expedition_log_count < 5) wario_expedition_log_count++;
}

static void companion_sim_update_wario(int game, const Frame *frame)
{
    if (game != GAME_WARIO_SML3 || !frame) {
        wario_expedition_baseline = 0;
        wario_treasure_animation_active = 0;
        current_wario_coins = current_wario_hearts = current_wario_power = -1;
        current_wario_treasure_mask = current_wario_time = -1;
        return;
    }

    int course = wario_ram_read(0xA804);
    uint8_t hearts_raw = wario_ram_read(0xA808), lives_raw = wario_ram_read(0xA809);
    uint8_t power_raw = wario_ram_read(0xA80A);
    uint8_t treasure_low = wario_ram_read(0xA80E), treasure_high = wario_ram_read(0xA80F);
    int lives = wario_bcd_pair(lives_raw);
    current_lives = lives;
    current_stage = course == 0xFF ? -1 : course;
    current_wario_hearts = wario_bcd_pair(hearts_raw);
    current_wario_power = power_raw <= 4 ? power_raw : -1;
    current_wario_coins = wario_bcd_coins();
    current_wario_time = wario_bcd_time();
    current_wario_treasure_mask =
        treasure_low != 0xFF && treasure_high != 0xFF
            ? ((int)treasure_low | ((int)treasure_high << 8)) : -1;

    if (!wario_expedition_baseline) {
        memset(wario_expedition_log, 0, sizeof wario_expedition_log);
        wario_expedition_log_count = 0;
        wario_log_push("EXPEDITION LINKED");
        if (current_stage >= 0) {
            char entry[64];
            snprintf(entry, sizeof entry, "COURSE %02X ENTERED", current_stage & 0xFF);
            wario_log_push(entry);
        }
        wario_previous_coins = current_wario_coins;
        wario_previous_hearts = current_wario_hearts;
        wario_previous_power = current_wario_power;
        wario_previous_treasure_mask = current_wario_treasure_mask;
        wario_previous_stage = current_stage;
        wario_previous_lives = current_lives;
        wario_expedition_baseline = 1;
        return;
    }

    if (current_stage >= 0 && wario_previous_stage >= 0 &&
        current_stage != wario_previous_stage) {
        char entry[64];
        snprintf(entry, sizeof entry, "COURSE %02X ENTERED", current_stage & 0xFF);
        wario_log_push(entry);
    }

    if (current_wario_treasure_mask >= 0 && wario_previous_treasure_mask >= 0) {
        int next_mask = current_wario_treasure_mask & 0xFFFF;
        int old_mask = wario_previous_treasure_mask & 0xFFFF;
        if ((next_mask & old_mask) != old_mask) {
            /* A load/rewind rolled back the bitfield; don't label it as a find. */
            wario_treasure_animation_active = 0;
        } else {
            int gained = next_mask & ~old_mask;
            for (int bit = 0; bit < 16; bit++) {
                if (gained & (1 << bit)) {
                    char entry[64];
                    snprintf(entry, sizeof entry, "TREASURE %02d FOUND", bit + 1);
                    wario_log_push(entry);
                }
            }
            if (gained) {
                int bit = 31;
                while (bit >= 0 && !(gained & (1u << bit))) bit--;
                wario_treasure_animation_id = bit + 1;
                wario_treasure_animation_started = SDL_GetTicks();
                wario_treasure_animation_active = 1;
            }
        }
    }
    if (current_wario_power >= 0 && wario_previous_power >= 0 &&
        current_wario_power != wario_previous_power) {
        char entry[64];
        snprintf(entry, sizeof entry, "POWER: %s", wario_power_name(current_wario_power));
        wario_log_push(entry);
    }
    if (current_lives > wario_previous_lives && wario_previous_lives >= 0) {
        char entry[64];
        snprintf(entry, sizeof entry, "EXTRA LIFE + %d", current_lives - wario_previous_lives);
        wario_log_push(entry);
    }
    if (current_wario_hearts >= 0 && wario_previous_hearts >= 0 &&
        current_wario_hearts > wario_previous_hearts)
        wario_log_push("HEARTS RESTORED");
    if (current_wario_coins >= 0 && wario_previous_coins >= 0 &&
        current_wario_coins > wario_previous_coins &&
        current_wario_coins / 100 > wario_previous_coins / 100) {
        char entry[64];
        snprintf(entry, sizeof entry, "COIN BANK %05d", current_wario_coins / 100 * 100);
        wario_log_push(entry);
    }

    if (current_wario_coins >= 0) wario_previous_coins = current_wario_coins;
    if (current_wario_hearts >= 0) wario_previous_hearts = current_wario_hearts;
    if (current_wario_power >= 0) wario_previous_power = current_wario_power;
    if (current_wario_treasure_mask >= 0) wario_previous_treasure_mask = current_wario_treasure_mask;
    if (current_stage >= 0) wario_previous_stage = current_stage;
    if (current_lives >= 0) wario_previous_lives = current_lives;
}

static void color(Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    SDL_SetRenderDrawColor(deck_renderer, r, g, b, a);
}

static void fill(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    SDL_Rect rect = {x, y, w, h};
    color(r, g, b, a);
    SDL_RenderFillRect(deck_renderer, &rect);
}

static void outline(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    SDL_Rect rect = {x, y, w, h};
    color(r, g, b, a);
    SDL_RenderDrawRect(deck_renderer, &rect);
}

static int glyph_id(char ch)
{
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= '0' && ch <= '9') return 26 + ch - '0';
    return -1;
}

static void draw_text(int x, int y, int px, Uint8 r, Uint8 g, Uint8 b, const char *text)
{
    if (px < 1) px = 1;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; ++p) {
        if (p[0] == 0xE2 && p[1] == 0x80 && p[2] == 0xA2) {
            fill(x + 2 * px, y + 3 * px, px, px, r, g, b, 255);
            x += 5 * px;
            p += 2;
            continue;
        }
        if (p[0] == 0xC3 && p[1] == 0x97) {
            fill(x + px, y + 1 * px, 3 * px, px, r, g, b, 255);
            fill(x + 2 * px, y, px, 4 * px, r, g, b, 255);
            x += 6 * px;
            p++;
            continue;
        }
        if (*p == ' ') { x += 4 * px; continue; }
        if (*p == '-') { fill(x + px, y + 3 * px, 3 * px, px, r, g, b, 255); x += 5 * px; continue; }
        if (*p == ':') {
            fill(x + px, y + 2 * px, px, px, r, g, b, 255);
            fill(x + px, y + 5 * px, px, px, r, g, b, 255);
            x += 4 * px;
            continue;
        }
        if (*p == '/') {
            for (int row = 0; row < 7; row++) fill(x + (4 - row % 5) * px, y + row * px, px, px, r, g, b, 255);
            x += 5 * px;
            continue;
        }
        int id = glyph_id((char)*p);
        if (id >= 0) {
            for (int row = 0; row < 7; row++)
                for (int col = 0; col < 5; col++)
                    if (font5x7[id][row] & (1u << (4 - col)))
                        fill(x + col * px, y + row * px, px, px, r, g, b, 255);
        }
        x += 6 * px;
    }
}

static void draw_text_fit(int x, int y, int max_width, int px,
                          Uint8 r, Uint8 g, Uint8 b, const char *text)
{
    int len = (int)strlen(text ? text : "");
    while (px > 1 && len * 6 * px > max_width) px--;
    draw_text(x, y, px, r, g, b, text);
}

static void draw_panel(int x, int y, int w, int h, int game, int bright)
{
    if (game == GAME_SML)
        fill(x, y, w, h, bright ? 233 : 240, bright ? 243 : 245, bright ? 208 : 213, 242);
    else if (game == GAME_SML2)
        fill(x, y, w, h, bright ? 65 : 53, bright ? 46 : 35, bright ? 83 : 66, 242);
    else
        fill(x, y, w, h, bright ? 44 : 36, bright ? 61 : 53, bright ? 82 : 76, 242);
    Uint8 ar = game == GAME_SML ? 128 : game == GAME_SML2 ? 173 : 87;
    Uint8 ag = game == GAME_SML ? 169 : game == GAME_SML2 ? 123 : 125;
    Uint8 ab = game == GAME_SML ? 81 : game == GAME_SML2 ? 65 : 157;
    outline(x, y, w, h, ar, ag, ab, 255);
}


static void transform_box(const CompanionLayoutCfg *d, int element,
                           float cx, float cy, float *x, float *y, float *w, float *h);

static void fill_round_rect(int x, int y, int w, int h, int radius,
                            Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    if (w <= 0 || h <= 0) return;
    if (radius < 0) radius = 0;
    if (radius > h / 2) radius = h / 2;
    if (radius > w / 2) radius = w / 2;
    color(r, g, b, a);
    for (int row = 0; row < h; row++) {
        float dy = row < radius ? (float)(radius - row) - 0.5f :
                   row >= h - radius ? (float)(row - (h - radius)) + 0.5f : 0.0f;
        int inset = dy > 0.0f ? radius - (int)sqrtf(fmaxf(0.0f, (float)radius * radius - dy * dy)) : 0;
        if (inset * 2 < w) {
            SDL_Rect rect = {x + inset, y + row, w - inset * 2, 1};
            SDL_RenderFillRect(deck_renderer, &rect);
        }
    }
}

static void outline_round_rect(int x, int y, int w, int h, int radius,
                               Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    if (w <= 0 || h <= 0) return;
    if (radius < 0) radius = 0;
    if (radius > h / 2) radius = h / 2;
    if (radius > w / 2) radius = w / 2;
    color(r, g, b, a);
    SDL_RenderDrawLine(deck_renderer, x + radius, y, x + w - radius - 1, y);
    SDL_RenderDrawLine(deck_renderer, x + radius, y + h - 1, x + w - radius - 1, y + h - 1);
    for (int row = radius; row < h - radius; row++) {
        SDL_RenderDrawPoint(deck_renderer, x, y + row);
        SDL_RenderDrawPoint(deck_renderer, x + w - 1, y + row);
    }
    for (int row = 0; row < radius; row++) {
        float dy = (float)(radius - row) - 0.5f;
        int inset = radius - (int)sqrtf(fmaxf(0.0f, (float)radius * radius - dy * dy));
        SDL_RenderDrawPoint(deck_renderer, x + inset, y + row);
        SDL_RenderDrawPoint(deck_renderer, x + w - 1 - inset, y + row);
        SDL_RenderDrawPoint(deck_renderer, x + inset, y + h - 1 - row);
        SDL_RenderDrawPoint(deck_renderer, x + w - 1 - inset, y + h - 1 - row);
    }
}

static void fill_circle(int cx, int cy, int radius, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    if (radius <= 0) return;
    color(r, g, b, a);
    for (int dy = -radius; dy <= radius; dy++) {
        int dx = (int)sqrtf(fmaxf(0.0f, (float)radius * radius - (float)dy * dy));
        SDL_RenderDrawLine(deck_renderer, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

static void outline_circle(int cx, int cy, int radius, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    if (radius <= 0) return;
    color(r, g, b, a);
    for (int dy = -radius; dy <= radius; dy++) {
        int dx = (int)sqrtf(fmaxf(0.0f, (float)radius * radius - (float)dy * dy));
        SDL_RenderDrawPoint(deck_renderer, cx - dx, cy + dy);
        SDL_RenderDrawPoint(deck_renderer, cx + dx, cy + dy);
    }
}

static int simulator_layout_preview_enabled(void)
{
#if defined(PIPECLEAN_DEV_BUILD)
    return launcher_companion_layout_preview_enabled();
#else
    return 0;
#endif
}

static int simulator_layout_preview_item(void)
{
#if defined(PIPECLEAN_DEV_BUILD)
    return launcher_companion_layout_preview_item();
#else
    return 0;
#endif
}

static Uint8 theme_bg_r(int g) { return g == GAME_DRMARIO ? 240 : g == GAME_SML ? 184 : g == GAME_SML2 ? 37 : g == GAME_WARIO_SML3 ? 33 : 23; }
static Uint8 theme_bg_g(int g) { return g == GAME_DRMARIO ? 247 : g == GAME_SML ? 217 : g == GAME_SML2 ? 21 : g == GAME_WARIO_SML3 ? 26 : 35; }
static Uint8 theme_bg_b(int g) { return g == GAME_DRMARIO ? 247 : g == GAME_SML ? 145 : g == GAME_SML2 ? 47 : g == GAME_WARIO_SML3 ? 19 : 55; }
static Uint8 theme_accent_r(int g) { return g == GAME_DRMARIO ? 21 : g == GAME_SML ? 57 : g == GAME_SML2 ? 255 : g == GAME_WARIO_SML3 ? 255 : 110; }
static Uint8 theme_accent_g(int g) { return g == GAME_DRMARIO ? 157 : g == GAME_SML ? 124 : g == GAME_SML2 ? 202 : g == GAME_WARIO_SML3 ? 200 : 229; }
static Uint8 theme_accent_b(int g) { return g == GAME_DRMARIO ? 173 : g == GAME_SML ? 48 : g == GAME_SML2 ? 91 : g == GAME_WARIO_SML3 ? 87 : 239; }
static Uint8 theme_ink_r(int g) { return g == GAME_DRMARIO ? 23 : g == GAME_SML ? 37 : g == GAME_SML2 ? 255 : g == GAME_WARIO_SML3 ? 255 : 234; }
static Uint8 theme_ink_g(int g) { return g == GAME_DRMARIO ? 56 : g == GAME_SML ? 60 : g == GAME_SML2 ? 240 : g == GAME_WARIO_SML3 ? 241 : 245; }
static Uint8 theme_ink_b(int g) { return g == GAME_DRMARIO ? 73 : g == GAME_SML ? 32 : g == GAME_SML2 ? 210 : g == GAME_WARIO_SML3 ? 212 : 255; }
static Uint8 theme_muted_r(int g) { return g == GAME_DRMARIO ? 98 : g == GAME_SML ? 87 : g == GAME_SML2 ? 209 : g == GAME_WARIO_SML3 ? 184 : 175; }
static Uint8 theme_muted_g(int g) { return g == GAME_DRMARIO ? 127 : g == GAME_SML ? 118 : g == GAME_SML2 ? 183 : g == GAME_WARIO_SML3 ? 170 : 195; }
static Uint8 theme_muted_b(int g) { return g == GAME_DRMARIO ? 139 : g == GAME_SML ? 77 : g == GAME_SML2 ? 213 : g == GAME_WARIO_SML3 ? 149 : 221; }
static Uint8 theme_edge_r(int g) { return g == GAME_DRMARIO ? 139 : g == GAME_SML ? 128 : g == GAME_SML2 ? 173 : g == GAME_WARIO_SML3 ? 189 : 87; }
static Uint8 theme_edge_g(int g) { return g == GAME_DRMARIO ? 202 : g == GAME_SML ? 169 : g == GAME_SML2 ? 123 : g == GAME_WARIO_SML3 ? 133 : 125; }
static Uint8 theme_edge_b(int g) { return g == GAME_DRMARIO ? 204 : g == GAME_SML ? 81 : g == GAME_SML2 ? 65 : g == GAME_WARIO_SML3 ? 55 : 157; }

static void image_contain(SDL_Texture *texture, int x, int y, int w, int h);

static void draw_asset_image(const Settings *s, int game, int asset_index,
                              int x, int y, int w, int h)
{
    if (game < 0 || game >= N_GAMES || asset_index < 0 ||
        asset_index >= N_DECK_ASSET_PATHS || w <= 0 || h <= 0 ||
        !assets[game][asset_index]) return;
    if (s && s->deck_layout[game].asset_stretch[asset_index]) {
        SDL_Rect destination = {x, y, w, h};
        SDL_RenderCopy(deck_renderer, assets[game][asset_index], NULL, &destination);
    } else {
        image_contain(assets[game][asset_index], x, y, w, h);
    }
}

static void draw_android_card(const Settings *s, int x, int y, int w, int h, int game, const char *label)
{
    fill_round_rect(x + 2, y + 4, w, h, 16, 0, 0, 0, 96);
    fill_round_rect(x, y, w, h, 15,
        game == GAME_DRMARIO ? 248 : game == GAME_SML ? 240 : game == GAME_SML2 ? 53 : 36,
        game == GAME_DRMARIO ? 255 : game == GAME_SML ? 245 : game == GAME_SML2 ? 35 : 53,
        game == GAME_DRMARIO ? 255 : game == GAME_SML ? 213 : game == GAME_SML2 ? 66 : 76, 242);
    if (game >= 0 && game < N_GAMES && !strcmp(label, "ADVENTURE STATUS"))
        draw_asset_image(s, game, 16, x + 2, y + 2, w - 4, h - 4);
    outline_round_rect(x + 1, y + 1, w - 2, h - 2, 14,
        theme_edge_r(game), theme_edge_g(game), theme_edge_b(game), 255);
    fill(x + 12, y + 9, w - 24, 3,
        theme_accent_r(game), theme_accent_g(game), theme_accent_b(game), 255);
    draw_text(x + 12, y + 21, 1,
        theme_muted_r(game), theme_muted_g(game), theme_muted_b(game), label);
}

static void draw_android_header(const Settings *s, int x, int y, int w, int h, int game)
{
    fill_round_rect(x + 2, y + 3, w, h, 12, 0, 0, 0, 106);
    fill_round_rect(x, y, w, h, 12,
        game == GAME_DRMARIO ? 248 : game == GAME_SML ? 240 : game == GAME_SML2 ? 53 : 36,
        game == GAME_DRMARIO ? 255 : game == GAME_SML ? 245 : game == GAME_SML2 ? 35 : 53,
        game == GAME_DRMARIO ? 255 : game == GAME_SML ? 213 : game == GAME_SML2 ? 66 : 76, 242);
    if (game >= 0 && game < N_GAMES)
        draw_asset_image(s, game, 15, x + 2, y + 2, w - 4, h - 4);
    outline_round_rect(x, y, w, h, 12,
        theme_edge_r(game), theme_edge_g(game), theme_edge_b(game), 255);
    fill(x + 10, y + 5, w - 20, 2,
        theme_accent_r(game), theme_accent_g(game), theme_accent_b(game), 255);
}

static const char *android_game_title(int game)
{
    if (game == GAME_SML) return "SUPER MARIO LAND";
    if (game == GAME_SML2) return "6 GOLDEN COINS";
    if (game == 0) return "DR MARIO";
    if (game == GAME_WARIO_SML3) return "WARIO LAND EXPEDITION";
    return game >= 0 && game < N_GAMES && games[game].name ? games[game].name : "PIPECLEAN READY";
}

static void play_time_label(int seconds, char *out, size_t n)
{
    if (seconds < 0) seconds = 0;
    int hours = seconds / 3600;
    int minutes = (seconds % 3600) / 60;
    if (hours > 0) snprintf(out, n, "%dH %02dM PLAYED", hours, minutes);
    else snprintf(out, n, "%dM PLAYED", minutes);
}

static void refresh_dashboard_preview(int game)
{
    Uint32 now = SDL_GetTicks();
    if (game == dashboard_preview_game &&
        (Uint32)(now - dashboard_preview_tick) < 5000u) return;
    if (dashboard_preview_texture) {
        SDL_DestroyTexture(dashboard_preview_texture);
        dashboard_preview_texture = NULL;
    }
    dashboard_preview_game = game;
    dashboard_preview_tick = now;
    if (game < 0 || game >= N_GAMES || !deck_renderer) return;
    const uint32_t *pixels = NULL;
    int w = 0, h = 0;
    if (!launcher_preview_pixels(game, &pixels, &w, &h) || !pixels ||
        w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h > 1048576u) return;
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(
        (void *)pixels, w, h, 32, w * (int)sizeof(uint32_t), SDL_PIXELFORMAT_ARGB8888);
    if (!surface) return;
    dashboard_preview_texture = SDL_CreateTextureFromSurface(deck_renderer, surface);
    SDL_FreeSurface(surface);
    if (dashboard_preview_texture) SDL_SetTextureBlendMode(dashboard_preview_texture, SDL_BLENDMODE_BLEND);
}

static void draw_dashboard_preview(int x, int y, int w, int h, int game)
{
    fill_round_rect(x, y, w, h, 8, 7, 17, 31, 255);
    outline_round_rect(x, y, w, h, 8, 55, 84, 117, 255);
    refresh_dashboard_preview(game);
    if (!dashboard_preview_texture) {
        int cx = x + w / 2, cy = y + h / 2 - 4;
        fill_circle(cx, cy, 15, theme_accent_r(-1), theme_accent_g(-1), theme_accent_b(-1), 255);
        draw_text_fit(x + 20, cy + 23, w - 40, 1, 175, 195, 221, "GAME PREVIEW");
        return;
    }
    int iw = 0, ih = 0;
    SDL_QueryTexture(dashboard_preview_texture, NULL, NULL, &iw, &ih);
    if (iw <= 0 || ih <= 0) return;
    float fit = fminf((float)(w - 4) / iw, (float)(h - 4) / ih);
    int dw = (int)(iw * fit + .5f), dh = (int)(ih * fit + .5f);
    SDL_Rect dst = {x + (w - dw) / 2, y + (h - dh) / 2, dw, dh};
    SDL_RenderCopy(deck_renderer, dashboard_preview_texture, NULL, &dst);
}

static void draw_dashboard(const Settings *s, int selected_game)
{
    fill(0, 0, 800, 450, 23, 35, 55, 255);
    for (int i = 0; i < 32; i++) {
        int sx = (i * 53 + 9) % 800, sy = (i * 31 + 17) % 450;
        fill(sx, sy, (i % 5 == 0) ? 4 : 2, (i % 5 == 0) ? 4 : 2,
             (i % 3 == 0) ? 110 : 203, (i % 3 == 0) ? 229 : 181,
             (i % 3 == 0) ? 239 : 232, 150);
    }
    for (int y = 0; y < 450; y += 6) fill(0, y, 800, 1, 0, 0, 0, 18);

    draw_android_header(s, 18, 7, 282, 48, -1);
    draw_text(27, 15, 1, 110, 229, 239, "PIPE CLEAN  \xE2\x80\xA2  GAME LIBRARY");
    draw_text(27, 28, 2, 234, 245, 255, "READY TO PLAY");

    draw_android_card(s, 20, 65, 460, 362, -1, "SELECTED GAME");
    draw_dashboard_preview(34, 94, 432, 199, selected_game);
    const char *name = selected_game >= 0 && selected_game < N_GAMES &&
        games[selected_game].name ? games[selected_game].name : "CHOOSE A GAME";
    if (selected_game == GAME_SML) name = "SUPER MARIO LAND";
    else if (selected_game == GAME_SML2) name = "SUPER MARIO LAND 2";
    draw_text_fit(36, 300, 420, 2, 234, 245, 255, name);
    int played = selected_game >= 0 && selected_game < N_GAMES ? s->game_play_seconds[selected_game] : 0;
    char time_text[48];
    play_time_label(played, time_text, sizeof time_text);
    draw_text(36, 332, 1, 175, 195, 221, "TOTAL PLAY TIME");
    draw_text_fit(36, 339, 420, 2, 110, 229, 239, time_text);

    draw_android_card(s, 492, 65, 288, 362, -1, "RECENTLY PLAYED");
    int rows = 0;
    for (int i = 0; i < N_RECENT_PLAYED; i++)
        if (s->recent_played[i] >= 0 && s->recent_played[i] < N_GAMES) rows++;
    if (rows == 0) {
        draw_text(510, 119, 1, 175, 195, 221, "YOUR ADVENTURE");
        draw_text_fit(510, 145, 246, 1, 234, 245, 255, "GAMES YOU LAUNCH APPEAR HERE");
        draw_text_fit(510, 166, 246, 1, 175, 195, 221, "PLAY TIME IS SAVED AUTOMATICALLY");
    } else {
        int row = 0;
        for (int i = 0; i < N_RECENT_PLAYED; i++) {
            int id = s->recent_played[i];
            if (id < 0 || id >= N_GAMES) continue;
            int ry = 99 + row * 27;
            fill_round_rect(504, ry, 264, 24, 6,
                row == 0 ? 56 : 23, row == 0 ? 214 : 34, row == 0 ? 155 : 55, 65);
            fill_circle(518, ry + 12, 4, 110, 229, 239, 255);
            const char *recent_name = games[id].name ? games[id].name : "GAME";
            if (id == GAME_SML) recent_name = "SUPER MARIO LAND";
            else if (id == GAME_SML2) recent_name = "SUPER MARIO LAND 2";
            draw_text_fit(531, ry - 4, 226, 1,
                row == 0 ? 110 : 234, row == 0 ? 229 : 245, row == 0 ? 239 : 255, recent_name);
            play_time_label(s->game_play_seconds[id], time_text, sizeof time_text);
            draw_text_fit(531, ry + 7, 226, 1, 175, 195, 221, time_text);
            row++;
        }
    }
    /* The decorative corner markers and simple scan accents match the Android deck's frame. */
    fill(9, 9, 12, 3, 110, 229, 239, 204); fill(9, 9, 3, 12, 110, 229, 239, 204);
    fill(779, 9, 12, 3, 110, 229, 239, 204); fill(788, 9, 3, 12, 110, 229, 239, 204);
    fill(9, 438, 12, 3, 110, 229, 239, 204); fill(9, 429, 3, 12, 110, 229, 239, 204);
    fill(779, 438, 12, 3, 110, 229, 239, 204); fill(788, 429, 3, 12, 110, 229, 239, 204);
}

static void update_desktop_music_notice(const Settings *s)
{
    int serial = launcher_music_notification_serial();
    if (!launcher_music_notification_allowed()) {
        desktop_music_notice_visible = 0;
        desktop_music_notice_serial = serial;
        return;
    }
    if (serial != desktop_music_notice_serial) {
        desktop_music_notice_serial = serial;
        if (s->menu_music && serial != 0) {
            const char *path = s->menu_music_path;
            if (!path[0]) {
                snprintf(desktop_music_notice_title, sizeof desktop_music_notice_title,
                         "PipeClean Chiptune");
            } else {
                const char *base = strrchr(path, '/');
                const char *winbase = strrchr(path, '\\');
                if (winbase && (!base || winbase > base)) base = winbase;
                base = base ? base + 1 : path;
                snprintf(desktop_music_notice_title, sizeof desktop_music_notice_title, "%s", base);
                char *dot = strrchr(desktop_music_notice_title, '.');
                if (dot && dot != desktop_music_notice_title) *dot = 0;
                if (!desktop_music_notice_title[0])
                    snprintf(desktop_music_notice_title, sizeof desktop_music_notice_title, "Untitled track");
            }
            desktop_music_notice_started = SDL_GetTicks();
            desktop_music_notice_visible = 1;
        } else {
            desktop_music_notice_visible = 0;
        }
    }
    if ((Uint32)(SDL_GetTicks() - desktop_music_notice_started) >= 2750u)
        desktop_music_notice_visible = 0;
}

static void draw_vinyl_notification(const Settings *s)
{
    update_desktop_music_notice(s);
    if (!desktop_music_notice_visible || !launcher_music_notification_allowed()) return;
    Uint32 elapsed = SDL_GetTicks() - desktop_music_notice_started;
    const float width = 368.0f, height = 58.0f;
    float offset = 0.0f;
    if (elapsed < 300u) {
        float q = elapsed / 300.0f;
        float ease = 1.0f - (1.0f - q) * (1.0f - q) * (1.0f - q);
        offset = -width * (1.0f - ease);
    } else if (elapsed > 2340u) {
        float q = fminf(1.0f, (elapsed - 2340u) / 360.0f);
        float ease = q * q * (3.0f - 2.0f * q);
        offset = -width * ease;
    }
    int x = 18 + (int)offset, y = 450 - (int)height - 11;
    fill_round_rect(x, y, (int)width, (int)height, 12, 17, 23, 34, 232);
    outline_round_rect(x, y, (int)width, (int)height, 12, 92, 111, 147, 230);
    int cx = x + 31, cy = y + 29;
    fill_circle(cx, cy, 21, 7, 9, 15, 255);
    outline_circle(cx, cy, 18, 59, 67, 87, 255);
    outline_circle(cx, cy, 15, 59, 67, 87, 255);
    outline_circle(cx, cy, 12, 59, 67, 87, 255);
    outline_circle(cx, cy, 9, 59, 67, 87, 255);
    float angle = s->menu_music ? elapsed * 0.0010472f : -0.45f;
    color(128, 217, 227, 255);
    SDL_RenderDrawLine(deck_renderer, cx, cy,
        cx + (int)(cosf(angle) * 17.0f), cy + (int)(sinf(angle) * 17.0f));
    fill_circle(cx, cy, 5, 128, 217, 227, 255);
    fill_circle(cx, cy, 1, 7, 9, 15, 255);
    draw_text(x + 61, y + 11, 1, 113, 215, 163, "NOW PLAYING");
    draw_text_fit(x + 61, y + 27, (int)width - 73, 2,
        234, 245, 255, desktop_music_notice_title);
}

static int slot_file_occupied(int game, int slot)
{
    char id[128], path[1200];
    if (game < 0 || game >= N_GAMES || slot < 0 || slot >= 10) return 0;
    launcher_game_file_id(game, id, sizeof id);
    snprintf(path, sizeof path, "%sstates/%s.slot%d.pcs", settings_dir(), id, slot);
    return file_exists(path);
}

static SDL_Texture *slot_thumbnail(int game, int slot)
{
    if (game < 0 || game >= N_GAMES || slot < 0 || slot >= 10) return NULL;
    char id[128], path[1200];
    struct stat st;
    launcher_game_file_id(game, id, sizeof id);
    snprintf(path, sizeof path, "%sstates/%s.slot%d.thumb.png", settings_dir(), id, slot);
    if (stat(path, &st) != 0) snprintf(path, sizeof path, "%sstates/%s.slot%d.thumb.bmp", settings_dir(), id, slot);
    time_t modified = stat(path, &st) == 0 ? st.st_mtime : 0;
    if (strcmp(path, slot_thumb_path[game][slot]) != 0 ||
        modified != slot_thumb_mtime[game][slot]) {
        if (slot_thumb_texture[game][slot]) SDL_DestroyTexture(slot_thumb_texture[game][slot]);
        slot_thumb_texture[game][slot] = NULL;
        snprintf(slot_thumb_path[game][slot], sizeof slot_thumb_path[game][slot], "%s", path);
        slot_thumb_mtime[game][slot] = modified;
        if (modified) {
            SDL_Surface *surface = IMG_Load(path);
            if (surface) {
                slot_thumb_texture[game][slot] = SDL_CreateTextureFromSurface(deck_renderer, surface);
                SDL_FreeSurface(surface);
                if (slot_thumb_texture[game][slot])
                    SDL_SetTextureBlendMode(slot_thumb_texture[game][slot], SDL_BLENDMODE_BLEND);
            }
        }
    }
    return slot_thumb_texture[game][slot];
}

static time_t slot_state_mtime(int game, int slot)
{
    char id[128], path[1200];
    struct stat st;
    if (game < 0 || game >= N_GAMES || slot < 0 || slot >= 10) return 0;
    launcher_game_file_id(game, id, sizeof id);
    snprintf(path, sizeof path, "%sstates/%s.slot%d.pcs", settings_dir(), id, slot);
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}

static void draw_slot_timeline(const Settings *s, int game)
{
    float x = 24.0f, y = 365.0f, w = 752.0f, h = 83.0f;
    const CompanionLayoutCfg *d = &s->deck_layout[game];
    transform_box(d, 3, 400.0f, 406.5f, &x, &y, &w, &h);
    draw_text((int)x + 2, (int)y + 2, 1,
        theme_accent_r(game), theme_accent_g(game), theme_accent_b(game),
        "SAVE-STATE TIMELINE");
    draw_text_fit((int)(x + w - 142), (int)y + 2, 136, 1,
        theme_muted_r(game), theme_muted_g(game), theme_muted_b(game), "SWIPE TO BROWSE");

    /* Match Android's ten-tile carousel: active tile is wider/taller and centered,
       with symmetric end padding so the first and last slots can sit in the middle. */
    float focus = (float)current_slot;
    float gap = 5.0f;
    float widths[10], centers[10], cursor = 0.0f;
    for (int i = 0; i < 10; i++) {
        float weight = fmaxf(0.0f, 1.0f - fabsf((float)i - focus));
        widths[i] = 72.0f + 90.0f * weight;
        centers[i] = cursor + widths[i] * 0.5f;
        cursor += widths[i] + gap;
    }
    float side_padding = fmaxf(0.0f, (w - 162.0f) * 0.5f);
    for (int i = 0; i < 10; i++) centers[i] += side_padding;
    float total_width = side_padding * 2.0f + cursor - gap;
    int left_index = (int)floorf(focus);
    int right_index = left_index < 9 ? left_index + 1 : left_index;
    float blend = focus - left_index;
    float focus_center = centers[left_index] +
        (centers[right_index] - centers[left_index]) * blend;
    float scroll = fmaxf(0.0f, fminf(fmaxf(0.0f, total_width - w),
                                     focus_center - w * 0.5f));

    float base_height = 36.0f, active_height = h - 20.0f;
    cursor = 0.0f;
    SDL_Rect clip = {(int)x, (int)y + 14, (int)w, (int)h - 14};
    SDL_RenderSetClipRect(deck_renderer, &clip);
    for (int i = 0; i < 10; i++) {
        float weight = fmaxf(0.0f, 1.0f - fabsf((float)i - focus));
        float tile_w = widths[i];
        float tile_h = base_height + (active_height - base_height) * weight;
        float tile_x = x + side_padding + cursor - scroll;
        float tile_y = y + h - tile_h - 1.0f;
        float tile_bottom = y + h - 1.0f;
        int selected = weight > 0.55f;
        if (tile_x + tile_w >= x && tile_x <= x + w) {
            int occupied = slot_file_occupied(game, i);
            fill_round_rect((int)tile_x + 1, (int)tile_y + 2, (int)tile_w, (int)tile_h,
                            selected ? 8 : 5, 0, 0, 0, 90);
            fill_round_rect((int)tile_x, (int)tile_y, (int)tile_w, (int)tile_h,
                selected ? 8 : 5,
                occupied ? (game == GAME_SML ? 233 : game == GAME_SML2 ? 53 : 36) : 23,
                occupied ? (game == GAME_SML ? 243 : game == GAME_SML2 ? 35 : 34) : 34,
                occupied ? (game == GAME_SML ? 208 : game == GAME_SML2 ? 66 : 76) : 55, 245);

            float image_margin = selected ? 4.0f : 3.0f;
            float label_area = selected ? 19.0f : 9.0f;
            float image_top = tile_y + image_margin;
            float image_bottom = fmaxf(image_top + 3.0f, tile_bottom - label_area - 2.0f);
            SDL_Texture *thumb = occupied ? slot_thumbnail(game, i) : NULL;
            SDL_Rect image_dst = {(int)(tile_x + image_margin), (int)image_top,
                (int)(tile_w - image_margin * 2.0f), (int)(image_bottom - image_top)};
            if (thumb && image_dst.w > 4 && image_dst.h > 3) {
                SDL_RenderCopy(deck_renderer, thumb, NULL, &image_dst);
            } else {
                fill_round_rect(image_dst.x, image_dst.y, image_dst.w, image_dst.h, 4,
                    occupied ? theme_accent_r(game) : 48,
                    occupied ? theme_accent_g(game) : 64,
                    occupied ? theme_accent_b(game) : 83, occupied ? 72 : 100);
                if (selected) {
                    draw_text_fit(image_dst.x + 2, image_dst.y + image_dst.h / 2 - 3,
                        image_dst.w - 4, 1,
                        occupied ? theme_accent_r(game) : theme_muted_r(game),
                        occupied ? theme_accent_g(game) : theme_muted_g(game),
                        occupied ? theme_accent_b(game) : theme_muted_b(game),
                        occupied ? "SAVED" : "EMPTY");
                } else if (!occupied) {
                    fill(image_dst.x + image_dst.w / 2 - 2, image_dst.y + image_dst.h / 2,
                         4, 1, theme_muted_r(game), theme_muted_g(game), theme_muted_b(game), 150);
                }
            }

            if (selected) {
                char slot_label[24], time_label[32];
                snprintf(slot_label, sizeof slot_label, "SLOT %02d", i + 1);
                draw_text_fit((int)(tile_x + 4),
                    (int)tile_bottom - 20, (int)tile_w - 8, 1,
                    255, 214, 107, slot_label);
                time_t saved_at = slot_state_mtime(game, i);
                if (occupied && saved_at > 0) {
                    struct tm saved_tm;
#ifdef _WIN32
                    if (localtime_s(&saved_tm, &saved_at) == 0)
#else
                    if (localtime_r(&saved_at, &saved_tm) != NULL)
#endif
                        strftime(time_label, sizeof time_label, "SAVED %H:%M", &saved_tm);
                    else snprintf(time_label, sizeof time_label, "SAVED");
                } else snprintf(time_label, sizeof time_label, "%s", occupied ? "SAVED" : "READY TO SAVE");
                draw_text_fit((int)(tile_x + 4), (int)tile_bottom - 9,
                    (int)tile_w - 8, 1,
                    theme_muted_r(game), theme_muted_g(game), theme_muted_b(game), time_label);
            } else {
                char slot_label[8];
                snprintf(slot_label, sizeof slot_label, "%02d", i + 1);
                draw_text_fit((int)(tile_x + 2), (int)tile_bottom - 9,
                    (int)tile_w - 4, 1,
                    occupied ? theme_ink_r(game) : theme_muted_r(game),
                    occupied ? theme_ink_g(game) : theme_muted_g(game),
                    occupied ? theme_ink_b(game) : theme_muted_b(game), slot_label);
            }
            outline_round_rect((int)tile_x, (int)tile_y, (int)tile_w, (int)tile_h,
                selected ? 8 : 5,
                selected ? 255 : theme_edge_r(game),
                selected ? 214 : theme_edge_g(game),
                selected ? 107 : theme_edge_b(game), 255);
        }
        cursor += tile_w + gap;
    }
    SDL_RenderSetClipRect(deck_renderer, NULL);
}
static void draw_action_button(const Settings *s, int game, int action,
                               int x, int y, int w, int h, const char *label,
                               const char *sub, int hold)
{
    const CompanionLayoutCfg *d = &s->deck_layout[game];
    fill_round_rect(x + 2, y + 4, w, h, 12, 0, 0, 0, 96);
    fill_round_rect(x, y, w, h, 12,
        action == pressed_action ? theme_accent_r(game) : (game == GAME_SML ? 240 : game == GAME_SML2 ? 53 : 36),
        action == pressed_action ? theme_accent_g(game) : (game == GAME_SML ? 245 : game == GAME_SML2 ? 35 : 53),
        action == pressed_action ? theme_accent_b(game) : (game == GAME_SML ? 213 : game == GAME_SML2 ? 66 : 76), 245);
    int action_texture_index = action == SIM_SAVE ? 17 : action == SIM_LOAD ? 18 :
        action == SIM_PAUSE ? 19 : action == SIM_REWIND ? 20 : 1;
    if (!assets[game][action_texture_index]) action_texture_index = 1;
    if (assets[game][action_texture_index]) {
        draw_asset_image(s, game, action_texture_index, x + 2, y + 2, w - 4, h - 4);
        fill_round_rect(x + 1, y + 1, w - 2, h - 2, 11, 3, 8, 16, pressed_action == action ? 24 : 34);
    }
    outline_round_rect(x + 1, y + 1, w - 2, h - 2, 11,
        action == pressed_action ? 255 : theme_edge_r(game),
        action == pressed_action ? 214 : theme_edge_g(game),
        action == pressed_action ? 107 : theme_edge_b(game), 255);
    fill(x + 10, y + 6, w - 20, 2,
        action == pressed_action ? theme_ink_r(game) : theme_accent_r(game),
        action == pressed_action ? theme_ink_g(game) : theme_accent_g(game),
        action == pressed_action ? theme_ink_b(game) : theme_accent_b(game), 255);
    /* The left glyph block mirrors the Android deck's icon/text hierarchy. */
    fill_round_rect(x + 10, y + h / 2 - 9, 18, 18, 5,
        theme_edge_r(game), theme_edge_g(game), theme_edge_b(game), 160);
    const char *glyph = action == SIM_SAVE ? "S" : action == SIM_LOAD ? "L" :
        action == SIM_PAUSE ? "P" : action == SIM_REWIND ? "R" : "B";
    draw_text_fit(x + 16, y + h / 2 - 3, 8, 1, 255, 255, 255, glyph);
    draw_text_fit(x + 40, y + h / 2 - 10, w - 48, 1,
        theme_ink_r(game), theme_ink_g(game), theme_ink_b(game), label);
    draw_text_fit(x + 40, y + h / 2 + 3, w - 48, 1,
        theme_muted_r(game), theme_muted_g(game), theme_muted_b(game), sub);
    if (hold) draw_text_fit(x + w - 33, y + h - 9, 29, 1, 255, 214, 107, "HOLD");
    (void)d;
}

static void draw_tiny_button(const Settings *s, int x, int y, int w, int h, int game,
                             const char *label, const char *sub, int action)
{
    int highlighted = action != 0 && action == pressed_action;
    fill_round_rect(x, y, w, h, 8,
        highlighted ? 255 : (game == GAME_SML ? 240 : game == GAME_SML2 ? 53 : 36),
        highlighted ? 214 : (game == GAME_SML ? 245 : game == GAME_SML2 ? 35 : 53),
        highlighted ? 107 : (game == GAME_SML ? 213 : game == GAME_SML2 ? 66 : 76), 245);
    int texture_index = action == SIM_BACKGROUND ? 28 :
                        action == SIM_CLOSE_GAME ? 29 : 1;
    if (!assets[game][texture_index]) texture_index = 1;
    if (assets[game][texture_index]) {
        draw_asset_image(s, game, texture_index, x + 1, y + 1, w - 2, h - 2);
        fill_round_rect(x + 1, y + 1, w - 2, h - 2, 7, 3, 8, 16,
                        highlighted ? 55 : 68);
    }
    outline_round_rect(x + 1, y + 1, w - 2, h - 2, 8,
        highlighted ? 255 : theme_edge_r(game),
        highlighted ? 214 : theme_edge_g(game),
        highlighted ? 107 : theme_edge_b(game), 255);
    draw_text_fit(x + 4, y + 5, w - 8, 1,
        theme_ink_r(game), theme_ink_g(game), theme_ink_b(game), label);
    draw_text_fit(x + 4, y + h - 13, w - 8, 1,
        theme_muted_r(game), theme_muted_g(game), theme_muted_b(game), sub ? sub : "");
}

static void draw_android_scenery(int game)
{
    fill(0, 0, 800, 450, theme_bg_r(game), theme_bg_g(game), theme_bg_b(game), 255);
    if (game == GAME_SML) {
        fill(0, 375, 800, 75, 156, 201, 119, 255);
        fill_circle(170, 410, 165, 145, 193, 109, 255);
        fill_circle(395, 414, 133, 138, 185, 103, 255);
        fill_circle(680, 405, 150, 153, 200, 117, 255);
        for (int i = 0; i < 9; i++) fill(18 + i * 30, 436, 4, 4, 104, 151, 75, 255);
    } else if (game == GAME_SML2) {
        fill(0, 386, 800, 64, 57, 39, 73, 255);
        fill_circle(180, 414, 155, 48, 32, 61, 255);
        fill_circle(485, 420, 170, 52, 33, 66, 255);
        fill_circle(734, 405, 130, 48, 32, 61, 255);
        fill_circle(733, 66, 25, 255, 216, 122, 255);
        fill_circle(744, 60, 21, 55, 35, 63, 255);
        for (int i = 0; i < 32; i++) {
            int sx = (i * 73 + 17) % 800, sy = (i * 47 + 21) % 390;
            fill(sx, sy, (i % 5 == 0) ? 4 : 2, (i % 5 == 0) ? 4 : 2,
                i % 3 == 0 ? 255 : 203, i % 3 == 0 ? 214 : 181, i % 3 == 0 ? 107 : 232, 255);
        }
    } else if (game == GAME_WARIO_SML3) {
        fill(0, 388, 800, 62, 66, 45, 24, 255);
        fill_circle(140, 424, 128, 55, 38, 22, 255);
        fill_circle(420, 435, 155, 60, 41, 23, 255);
        fill_circle(702, 418, 120, 50, 35, 20, 255);
        fill_circle(720, 70, 25, 255, 214, 107, 220);
        for (int i = 0; i < 28; i++) {
            int sx = (i * 79 + 23) % 800, sy = (i * 43 + 17) % 365;
            fill(sx, sy, (i % 5 == 0) ? 4 : 2, (i % 5 == 0) ? 4 : 2,
                 (i % 4 == 0) ? 255 : 211, (i % 4 == 0) ? 230 : 170, 95, 220);
        }
    } else {
        for (int i = 0; i < 32; i++)
            fill((i * 53 + 9) % 800, (i * 31 + 17) % 450, 3, 3, 110, 229, 239, 180);
    }
    for (int y = 0; y < 450; y += 6) fill(0, y, 800, 1, 0, 0, 0, 18);
    fill(9, 9, 12, 3, theme_accent_r(game), theme_accent_g(game), theme_accent_b(game), 204);
    fill(9, 9, 3, 12, theme_accent_r(game), theme_accent_g(game), theme_accent_b(game), 204);
    fill(779, 9, 12, 3, theme_accent_r(game), theme_accent_g(game), theme_accent_b(game), 204);
    fill(788, 9, 3, 12, theme_accent_r(game), theme_accent_g(game), theme_accent_b(game), 204);
    fill(9, 438, 12, 3, theme_accent_r(game), theme_accent_g(game), theme_accent_b(game), 204);
    fill(9, 429, 3, 12, theme_accent_r(game), theme_accent_g(game), theme_accent_b(game), 204);
    fill(779, 438, 12, 3, theme_accent_r(game), theme_accent_g(game), theme_accent_b(game), 204);
    fill(788, 429, 3, 12, theme_accent_r(game), theme_accent_g(game), theme_accent_b(game), 204);
}

static void draw_hero_item(const Settings *s, int game, int item, float center_x, float center_y, float radius)
{
    Uint32 now = SDL_GetTicks();
    float t = now / 1000.0f;
    int cx = (int)center_x, cy = (int)center_y, r = (int)radius;
    int icon_cx = cx, icon_cy = cy, icon_r = r, render_item = item;
    int animation = simulator_layout_preview_enabled() ? SIM_ITEM_ANIM_IDLE : item_animation_kind;
    float phase = 1.0f;
    if (animation == SIM_ITEM_ANIM_ARRIVE && item_animation_item > 0) {
        phase = fminf(1.0f, (float)(now - item_animation_started) / 460.0f);
        icon_cy -= (int)(r * 0.48f * (1.0f - phase));
        icon_r = (int)(r * (0.35f + 0.65f * phase));
        render_item = item_animation_item;
        if (phase >= 1.0f) {
            item_animation_kind = SIM_ITEM_ANIM_IDLE;
            render_item = item;
            icon_cy = cy; icon_r = r;
        }
    } else if (animation == SIM_ITEM_ANIM_SEND_OFF && item_animation_item > 0) {
        phase = fminf(1.0f, (float)(now - item_animation_started) / 360.0f);
        icon_cy -= (int)(r * 1.12f * phase);
        icon_r = (int)(r * (1.0f - 0.48f * phase));
        render_item = item_animation_item;
        if (phase >= 1.0f) {
            item_animation_kind = SIM_ITEM_ANIM_IDLE;
            render_item = item;
            icon_cy = cy; icon_r = r;
        }
    } else if (!simulator_layout_preview_enabled() && item > 0) {
        icon_cy += (int)(sinf(t * 2.25f) * r * 0.025f);
        icon_r = (int)(r * (1.0f + sinf(t * 3.1f) * 0.018f));
    }
    fill_circle(cx, cy, r + 22, 255, 216, 106, 40);
    fill_circle(cx, cy, r + 11, 255, 216, 106, 40);
    fill_circle(cx, cy, r - 7,
        game == GAME_SML ? 38 : game == GAME_SML2 ? 33 : 26,
        game == GAME_SML ? 60 : game == GAME_SML2 ? 20 : 41,
        game == GAME_SML ? 32 : game == GAME_SML2 ? 46 : 58, 255);
    outline_circle(cx, cy, r - 16, 255, 231, 160, 160);
    for (int i = 0; i < 44; i++) {
        float a = (float)(6.28318530718 * i / 44.0) + t * .34f;
        float wobble = (i % 4 == 0) ? 2.8f : 0.0f;
        float orbit = radius + ((i % 3 == 0) ? 8.0f : 4.0f) + wobble;
        int bx = cx + (int)(cosf(a) * orbit), by = cy + (int)(sinf(a) * orbit);
        int sz = (i % 5 == 0) ? 4 : (i % 2 == 0) ? 3 : 2;
        fill(bx - sz / 2, by - sz / 2, sz, sz,
            i % 4 == 0 ? 255 : i % 3 == 0 ? 172 : 255,
            i % 4 == 0 ? 240 : i % 3 == 0 ? 107 : 214,
            i % 4 == 0 ? 174 : i % 3 == 0 ? 34 : 107, 255);
    }
    if (icon_r < 1) icon_r = 1;
    /* SML2's live RAM uses 2 = Carrot and 3 = Fire Flower, while the Deck's
     * editable texture slots use semantic IDs 2 = Fire Flower and 3 = Carrot.
     * Never swap editor-preview selections; only normalize live RAM values. */
    int shown_item = render_item;
    if (!simulator_layout_preview_enabled() && game == GAME_SML2) {
        if (shown_item == 2) shown_item = 3;
        else if (shown_item == 3) shown_item = 2;
    }
    if (shown_item > 0) {
        int item_asset = shown_item >= 1 && shown_item <= 3 ? shown_item + 11 : 2;
        if (!assets[game][item_asset]) item_asset = 2;
        if (assets[game][item_asset]) {
            draw_asset_image(s, game, item_asset,
                icon_cx - icon_r * 46 / 100, icon_cy - icon_r * 46 / 100,
                icon_r * 92 / 100, icon_r * 92 / 100);
        } else {
            const char *label = shown_item == 1 ? "MUSHROOM" :
                game == GAME_SML ? "SUPERBALL FLOWER" :
                shown_item == 3 ? "CARROT" : "FIRE FLOWER";
            fill_circle(icon_cx, icon_cy - 8, icon_r / 3, 230, 53, 65, 255);
            fill(icon_cx - icon_r / 3, icon_cy - 1, icon_r * 2 / 3, icon_r / 4, 238, 238, 220, 255);
            fill(icon_cx - icon_r / 4, icon_cy + icon_r / 5, icon_r / 2, icon_r / 2, 238, 238, 220, 255);
            draw_text_fit(icon_cx - icon_r + 7, icon_cy + icon_r * 2 / 3, icon_r * 2 - 14, 1,
                theme_ink_r(game), theme_ink_g(game), theme_ink_b(game), label);
        }
    } else {
        /* The empty pocket is represented by a lilac, pixel-like star. */
        fill(cx - 2, cy - 27, 4, 54, 180, 161, 203, 255);
        fill(cx - 27, cy - 2, 54, 4, 180, 161, 203, 255);
        fill(cx - 14, cy - 14, 28, 28, 180, 161, 203, 255);
        fill(cx - 8, cy - 8, 16, 16, 36, 53, 76, 255);
    }
    if (shown_item > 0) {
        const char *label = shown_item == 1 ? "MUSHROOM" :
            game == GAME_SML ? "SUPERBALL FLOWER" :
            shown_item == 3 ? "CARROT" : "FIRE FLOWER";
        draw_text_fit(icon_cx - icon_r, icon_cy + (int)(icon_r * .66f) - 10, icon_r * 2, 1,
            theme_ink_r(game), theme_ink_g(game), theme_ink_b(game), label);
        draw_text_fit(icon_cx - icon_r, icon_cy + (int)(icon_r * .81f) - 7, icon_r * 2, 1,
            theme_muted_r(game), theme_muted_g(game), theme_muted_b(game),
            "TAP WHEN READY");
    }
    (void)s;
}

static void destroy_asset(int game, int index)
{
    if (game < 0 || game >= N_GAMES || index < 0 || index >= N_DECK_ASSET_PATHS) return;
    if (assets[game][index]) SDL_DestroyTexture(assets[game][index]);
    assets[game][index] = NULL;
    if (asset_animations[game][index]) IMG_FreeAnimation(asset_animations[game][index]);
    asset_animations[game][index] = NULL;
    asset_animation_frame[game][index] = 0;
    asset_animation_started[game][index] = 0;
    asset_paths[game][index][0] = 0;
}

static void load_asset(int game, int index, const char *path)
{
    if (game < 0 || game >= N_GAMES || index < 0 || index >= N_DECK_ASSET_PATHS) return;
    const char *next = path ? path : "";
    if (!strcmp(asset_paths[game][index], next)) return;
    destroy_asset(game, index);
    snprintf(asset_paths[game][index], sizeof asset_paths[game][index], "%s", next);
    if (!next[0]) return;

    size_t path_len = strlen(next);
    if (path_len >= 4 && SDL_strcasecmp(next + path_len - 4, ".gif") == 0)
        asset_animations[game][index] = IMG_LoadAnimation(next);
    if (asset_animations[game][index] && asset_animations[game][index]->count > 0) {
        assets[game][index] = SDL_CreateTextureFromSurface(
            deck_renderer, asset_animations[game][index]->frames[0]);
        asset_animation_started[game][index] = SDL_GetTicks();
        asset_animation_frame[game][index] = 0;
    } else {
        if (asset_animations[game][index]) {
            IMG_FreeAnimation(asset_animations[game][index]);
            asset_animations[game][index] = NULL;
        }
        SDL_Surface *surface = IMG_Load(next);
        if (!surface) return;
        assets[game][index] = SDL_CreateTextureFromSurface(deck_renderer, surface);
        SDL_FreeSurface(surface);
    }
    if (assets[game][index]) SDL_SetTextureBlendMode(assets[game][index], SDL_BLENDMODE_BLEND);
}

static void update_animated_assets(void)
{
    Uint32 now = SDL_GetTicks();
    for (int g = 0; g < N_GAMES; g++) {
        for (int i = 0; i < N_DECK_ASSET_PATHS; i++) {
            IMG_Animation *animation = asset_animations[g][i];
            if (!animation || animation->count < 2) continue;
            Uint32 total_ms = 0;
            for (int f = 0; f < animation->count; f++)
                total_ms += (Uint32)(animation->delays[f] > 0 ? animation->delays[f] : 100);
            if (!total_ms) continue;
            Uint32 elapsed = (Uint32)(now - asset_animation_started[g][i]) % total_ms;
            int frame = 0;
            while (frame + 1 < animation->count) {
                Uint32 delay = (Uint32)(animation->delays[frame] > 0 ? animation->delays[frame] : 100);
                if (elapsed < delay) break;
                elapsed -= delay;
                frame++;
            }
            if (frame == asset_animation_frame[g][i]) continue;
            SDL_Texture *texture = SDL_CreateTextureFromSurface(deck_renderer, animation->frames[frame]);
            if (!texture) continue;
            SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
            if (assets[g][i]) SDL_DestroyTexture(assets[g][i]);
            assets[g][i] = texture;
            asset_animation_frame[g][i] = frame;
        }
    }
}

static void sync_assets(const Settings *s)
{
    if (!s || !deck_renderer) return;
    for (int g = 0; g < N_GAMES; g++) {
        const CompanionLayoutCfg *d = &s->deck_layout[g];
        load_asset(g, 0, d->background_path);
        load_asset(g, 1, d->button_texture_path);
        load_asset(g, 2, d->item_texture_path); /* backward-compatible generic fallback */
        load_asset(g, 3, d->decoration_path);
        load_asset(g, 12, d->item_texture_by_id[1]); /* Mushroom */
        load_asset(g, 13, d->item_texture_by_id[2]); /* Superball / Fire Flower */
        load_asset(g, 14, d->item_texture_by_id[3]); /* Carrot */
        load_asset(g, 15, d->header_texture_path);
        load_asset(g, 16, d->status_texture_path);
        for (int b = 0; b < 4; b++)
            load_asset(g, 17 + b, d->button_texture_by_action[b]);
        load_asset(g, 28, d->button_texture_by_action[4]);
        load_asset(g, 29, d->button_texture_by_action[5]);
        for (int slot = 0; slot < N_DECK_DECORATIONS - 1; slot++) {
            int asset_index = slot < 7 ? 21 + slot : 23 + slot;
            load_asset(g, asset_index, d->decoration_extra_path[slot]);
        }
    }
}

static void image_cover(SDL_Texture *texture, int x, int y, int w, int h)
{
    if (!texture) return;
    int tw = 0, th = 0;
    if (SDL_QueryTexture(texture, NULL, NULL, &tw, &th) || tw <= 0 || th <= 0) return;
    float scale = fmaxf((float)w / tw, (float)h / th);
    int dw = (int)(tw * scale + .5f), dh = (int)(th * scale + .5f);
    SDL_Rect dest = {x + (w - dw) / 2, y + (h - dh) / 2, dw, dh};
    SDL_RenderSetClipRect(deck_renderer, &(SDL_Rect){x, y, w, h});
    SDL_RenderCopy(deck_renderer, texture, NULL, &dest);
    SDL_RenderSetClipRect(deck_renderer, NULL);
}

static void image_contain(SDL_Texture *texture, int x, int y, int w, int h)
{
    if (!texture || w <= 0 || h <= 0) return;
    int tw = 0, th = 0;
    if (SDL_QueryTexture(texture, NULL, NULL, &tw, &th) || tw <= 0 || th <= 0) return;
    float scale = fminf((float)w / tw, (float)h / th);
    int dw = (int)(tw * scale + .5f), dh = (int)(th * scale + .5f);
    SDL_Rect dest = {x + (w - dw) / 2, y + (h - dh) / 2, dw, dh};
    SDL_RenderCopy(deck_renderer, texture, NULL, &dest);
}

static void transform_box(const CompanionLayoutCfg *d, int element,
                          float cx, float cy, float *x, float *y, float *w, float *h)
{
    int dx = 0, dy = 0, size_x = 100, size_y = 100;
    switch (element) {
    case 0: dx = d->header_dx; dy = d->header_dy; size_x = d->header_scale; size_y = d->header_scale_y; break;
    case 1: dx = d->item_dx; dy = d->item_dy; size_x = d->item_scale; size_y = d->item_scale_y; break;
    case 2: dx = d->controls_dx; dy = d->controls_dy; size_x = d->controls_scale; size_y = d->controls_scale_y; break;
    case 3: dx = d->timeline_dx; dy = d->timeline_dy; size_x = d->timeline_scale; size_y = d->timeline_scale_y; break;
    case 4: dx = d->status_dx; dy = d->status_dy; size_x = d->status_scale; size_y = d->status_scale_y; break;
    case 5: dx = d->decoration_dx; dy = d->decoration_dy; size_x = d->decoration_scale; size_y = d->decoration_scale_y; break;
    default:
        if (element >= 6 && element < 5 + N_DECK_DECORATIONS) {
            int slot = element - 6;
            dx = d->decoration_extra_dx[slot];
            dy = d->decoration_extra_dy[slot];
            size_x = d->decoration_extra_scale[slot];
            size_y = d->decoration_extra_scale_y[slot];
        }
        break;
    }
    float scale_x = (float)(size_x > 0 ? size_x : 100) / 100.f;
    float scale_y = (float)(size_y > 0 ? size_y : 100) / 100.f;
    *x = cx + (*x - cx) * scale_x + dx;
    *y = cy + (*y - cy) * scale_y + dy;
    *w *= scale_x;
    *h *= scale_y;
}


static void draw_centered_text_fit(int x, int y, int w, int px, Uint8 r, Uint8 g, Uint8 b, const char *text)
{
    int len = (int)strlen(text ? text : "");
    while (px > 1 && len * 6 * px > w) px--;
    int text_width = len * 6 * px;
    draw_text(x + (w - text_width) / 2, y, px, r, g, b, text);
}
static void draw_dr_mario_medical_cross(int cx, int cy, int size, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    int arm = size / 4; if (arm < 2) arm = 2;
    fill_round_rect(cx-arm,cy-size/2,arm*2,size,arm/2,r,g,b,a);
    fill_round_rect(cx-size/2,cy-arm,size,arm*2,arm/2,r,g,b,a);
}
static void draw_dr_mario_capsule(int cx, int cy, int width, int height, int raw)
{
    int valid=raw>=0&&raw<=0x17, first=0, second=2, orientation=0;
    if(valid){
        switch(raw&0xFC){
        case 0x00:first=0;second=0;break; case 0x04:first=0;second=1;break;
        case 0x08:first=0;second=2;break; case 0x0C:first=1;second=1;break;
        case 0x10:first=1;second=2;break; case 0x14:first=2;second=2;break;
        default:valid=0;break;
        }
        orientation=raw&3;
    }
    Uint8 pr[3]={53,239,242},pg[3]={184,100,201},pb[3]={209,112,76};
    int vertical=orientation&1, sw=vertical?height:width, sh=vertical?width:height;
    int left=cx-sw/2, top=cy-sh/2;
    if(!valid){first=0;second=2;orientation=0;vertical=0;sw=width;sh=height;left=cx-sw/2;top=cy-sh/2;}
    if(orientation==2||orientation==3){int tmp=first;first=second;second=tmp;}
    int radius=(sw<sh?sw:sh)/2;
    fill_round_rect(left+3,top+5,sw,sh,radius,21,56,73,48);
    SDL_Rect shell={left,top,sw,sh}, half=shell;
    if(!vertical)half.w=sw/2;else half.h=sh/2;
    SDL_RenderSetClipRect(deck_renderer,&half);
    fill_round_rect(left,top,sw,sh,radius,valid?pr[first]:193,valid?pg[first]:216,valid?pb[first]:221,255);
    half=shell;
    if(!vertical){half.x+=sw/2;half.w-=sw/2;}else{half.y+=sh/2;half.h-=sh/2;}
    SDL_RenderSetClipRect(deck_renderer,&half);
    fill_round_rect(left,top,sw,sh,radius,valid?pr[second]:227,valid?pg[second]:236,valid?pb[second]:238,255);
    SDL_RenderSetClipRect(deck_renderer,NULL);
    outline_round_rect(left,top,sw,sh,radius,49,75,88,255);
    if(!vertical)fill(left+sw/2-1,top+sh/5,2,sh*3/5,49,75,88,210);
    else fill(left+sw/5,top+sh/2-1,sw*3/5,2,49,75,88,210);
    if(!vertical)fill_round_rect(left+sh/5,top+sh/5,sw/3,sh/12>1?sh/12:2,2,255,255,255,120);
    else fill_round_rect(left+sw/5,top+sh/5,sw/12>1?sw/12:2,sh/3,2,255,255,255,120);
}
static const char *dr_mario_speed_label(int speed)
{
    if(speed==2)return "LOW";if(speed==3)return "MED";if(speed==4)return "HI";return "--";
}
static void draw_dr_mario_metric(const Settings *s,const CompanionLayoutCfg *d,int x,int y,int w,int h,
                                  const char *label,const char *value,int value_px)
{
    draw_android_card(s,x,y,w,h,GAME_DRMARIO,label);
    draw_centered_text_fit(x+8,y+h-(value_px*7)-13,w-16,value_px,
        theme_ink_r(GAME_DRMARIO),theme_ink_g(GAME_DRMARIO),theme_ink_b(GAME_DRMARIO),value);
    (void)d;
}
static void draw_dr_mario_companion(const Settings *s,const CompanionLayoutCfg *d)
{
    fill(0,0,800,450,theme_bg_r(GAME_DRMARIO),theme_bg_g(GAME_DRMARIO),theme_bg_b(GAME_DRMARIO),255);
    for(int y=64;y<450;y+=24)fill(0,y,800,1,21,157,173,16);
    for(int i=0;i<16;i++)draw_dr_mario_medical_cross((i*61+35)%800,(i*43+80)%440,9+(i%3)*3,21,157,173,20);
    draw_asset_image(s,GAME_DRMARIO,0,0,0,800,450);
    float hx=18,hy=7,hw=265,hh=49;transform_box(d,0,400,31.5f,&hx,&hy,&hw,&hh);
    draw_android_header(s,(int)hx,(int)hy,(int)hw,(int)hh,GAME_DRMARIO);
    draw_text_fit((int)hx+8,(int)hy+10,(int)hw-16,1,theme_accent_r(GAME_DRMARIO),theme_accent_g(GAME_DRMARIO),theme_accent_b(GAME_DRMARIO),"PIPE CLEAN MEDICAL DECK");
    draw_text_fit((int)hx+8,(int)hy+25,(int)hw-16,2,theme_ink_r(GAME_DRMARIO),theme_ink_g(GAME_DRMARIO),theme_ink_b(GAME_DRMARIO),"DR MARIO");
    float bgx=593,bgy=13,bgw=105,bgh=42;transform_box(d,0,400,31.5f,&bgx,&bgy,&bgw,&bgh);
    draw_tiny_button(s,(int)bgx,(int)bgy,(int)bgw,(int)bgh,GAME_DRMARIO,"BACKGROUND","PNG / GIF",SIM_BACKGROUND);
    float closex=704,closey=13,closew=72,closeh=42;transform_box(d,0,400,31.5f,&closex,&closey,&closew,&closeh);
    draw_tiny_button(s,(int)closex,(int)closey,(int)closew,(int)closeh,GAME_DRMARIO,"CLOSE","GAME",SIM_CLOSE_GAME);
    float px=20,py=70,pw=270,ph=235;transform_box(d,1,400,164,&px,&py,&pw,&ph);
    draw_android_card(s,(int)px,(int)py,(int)pw,(int)ph,GAME_DRMARIO,"NEXT CAPSULE");
    draw_dr_mario_medical_cross((int)(px+pw-25),(int)(py+26),13,21,157,173,255);
    draw_dr_mario_capsule((int)(px+pw*.5f),(int)(py+ph*.49f),160,66,current_dr_mario_capsule);
    draw_centered_text_fit((int)px+10,(int)(py+ph-32),(int)pw-20,1,theme_muted_r(GAME_DRMARIO),theme_muted_g(GAME_DRMARIO),theme_muted_b(GAME_DRMARIO),
        current_dr_mario_capsule>=0&&current_dr_mario_capsule<=0x17?"READY TO DROP":"WAITING FOR PILL");
    float sx=306,sy=70,sw=470,sh=235;transform_box(d,4,541,187.5f,&sx,&sy,&sw,&sh);
    char score_text[16],level_text[16];
    if(current_dr_mario_score>=0)snprintf(score_text,sizeof score_text,"%08u",(unsigned)current_dr_mario_score);else snprintf(score_text,sizeof score_text,"--------");
    if(current_dr_mario_level>=0)snprintf(level_text,sizeof level_text,"%02d",current_dr_mario_level);else snprintf(level_text,sizeof level_text,"--");
    draw_dr_mario_metric(s,d,(int)sx,(int)sy,(int)sw,(int)(sh*.45f),"SCORE",score_text,3);
    draw_dr_mario_metric(s,d,(int)sx,(int)(sy+sh*.50f),(int)(sw*.48f),(int)(sh*.50f),"VIRUS LEVEL",level_text,3);
    draw_dr_mario_metric(s,d,(int)(sx+sw*.50f),(int)(sy+sh*.50f),(int)(sw*.50f),(int)(sh*.50f),"SPEED",dr_mario_speed_label(current_dr_mario_speed),3);
    draw_centered_text_fit(690,54,86,1,theme_muted_r(GAME_DRMARIO),theme_muted_g(GAME_DRMARIO),theme_muted_b(GAME_DRMARIO),current_paused?"PAUSED":"IN SESSION");
    const int action_ids[4]={SIM_SAVE,SIM_LOAD,SIM_PAUSE,SIM_REWIND};
    const char *action_labels[4]={"SAVE","LOAD",current_paused?"RESUME":"PAUSE","REWIND"};
    const char *action_subs[4]={"SNAPSHOT","RESTORE",current_paused?"CONTINUE":"HOLD GAME","HOLD TO REWIND"};
    for(int i=0;i<4;i++){
        float ax=94.0f+i*153.0f,ay=320.0f,aw=i==3?153.0f:145.0f,ah=42.0f;
        transform_box(d,2,400,343,&ax,&ay,&aw,&ah);
        draw_action_button(s,GAME_DRMARIO,action_ids[i],(int)ax,(int)ay,(int)aw,(int)ah,action_labels[i],action_subs[i],i==3);
    }
    draw_slot_timeline(s,GAME_DRMARIO);
    draw_vinyl_notification(s);
    SDL_RenderPresent(deck_renderer);
}

static const char *wario_course_text(void)
{
    static char text[32];
    if (current_stage < 0) snprintf(text, sizeof text, "COURSE --");
    else snprintf(text, sizeof text, "COURSE %02X", current_stage & 0xFF);
    return text;
}

static int wario_treasure_count(void)
{
    int mask = current_wario_treasure_mask & 0xFFFF, count = 0;
    if (current_wario_treasure_mask < 0) return 0;
    for (int bit = 0; bit < 16; bit++) if (mask & (1 << bit)) count++;
    return count > 15 ? 15 : count;
}

static void draw_wario_treasure_mark(int cx, int cy, int size, int found)
{
    fill_circle(cx, cy, size, found ? 255 : 82, found ? 204 : 75, found ? 87 : 62, 255);
    fill_circle(cx - size / 3, cy - size / 3, size / 3, found ? 255 : 111,
                found ? 242 : 100, found ? 181 : 85, found ? 255 : 180);
    if (found) fill(cx - 1, cy - 1, 3, 3, 255, 249, 216, 255);
}

static void draw_wario_emblem(void)
{
    float t = SDL_GetTicks() / 1000.0f;
    int cx = 400, cy = 142, radius = 57;
    fill_circle(cx, cy + 3, radius + 17, 67, 42, 13, 150);
    fill_circle(cx, cy, radius + 8, 88, 58, 21, 180);
    fill_circle(cx, cy, radius, 48, 38, 27, 255);
    outline_circle(cx, cy, radius - 3, 255, 214, 107, 255);
    outline_circle(cx, cy, radius - 11, 189, 133, 55, 255);
    /* Original chest emblem, assembled from shapes rather than game sprites. */
    fill_round_rect(cx - 31, cy - 1, 62, 31, 5, 142, 79, 34, 255);
    fill_round_rect(cx - 31, cy - 18, 62, 21, 5, 199, 129, 52, 255);
    fill(cx - 4, cy - 17, 8, 46, 255, 214, 107, 255);
    fill(cx - 23, cy + 11, 46, 4, 113, 62, 27, 255);
    fill_circle(cx, cy + 5, 5, 255, 241, 181, 255);
    for (int i = 0; i < 6; i++) {
        float a = t * .65f + 6.2831853f * i / 6.0f;
        int orbit = radius + 12 + (int)(sinf(t * 2.f + i) * 2.f);
        int sx = cx + (int)(cosf(a) * orbit), sy = cy + (int)(sinf(a) * orbit);
        fill(sx - 2, sy - 2, 4, 4, 255, (i % 2) ? 200 : 240, 107, 255);
    }
}

static void draw_wario_vault(const Settings *s, int x, int y, int w, int h)
{
    int g = GAME_WARIO_SML3;
    draw_android_card(s, x, y, w, h, g, "TREASURE VAULT");
    int count = wario_treasure_count();
    char text[48];
    if (current_wario_treasure_mask >= 0)
        snprintf(text, sizeof text, "%02d / 15 TREASURES", count);
    else snprintf(text, sizeof text, "-- / 15 TREASURES");
    draw_text_fit(x + 14, y + 47, w - 28, 1, 255, 241, 212, text);
    fill_round_rect(x + 14, y + 55, w - 28, 7, 3, 55, 42, 28, 255);
    if (count > 0)
        fill_round_rect(x + 14, y + 55, (w - 28) * count / 15, 7, 3,
                        255, 200, 87, 255);
    int start = x + 18, step = (w - 36) / 15;
    for (int i = 0; i < 15; i++) {
        int found = current_wario_treasure_mask >= 0 &&
                    (current_wario_treasure_mask & (1 << i));
        draw_wario_treasure_mark(start + i * step, y + h - 24, 4, found);
    }
    snprintf(text, sizeof text, "COIN BANK  %05d",
             current_wario_coins >= 0 ? current_wario_coins : 0);
    if (current_wario_coins < 0) snprintf(text, sizeof text, "COIN BANK  -----");
    draw_text_fit(x + 14, y + h - 8, w - 28, 1, 184, 170, 149, text);
}

static void draw_wario_status(const Settings *s, int x, int y, int w, int h)
{
    int g = GAME_WARIO_SML3;
    draw_android_card(s, x, y, w, h, g, "EXPEDITION STATUS");
    char value[48];
    draw_text_fit(x + 14, y + 48, 86, 1, 184, 170, 149, "LIVES");
    snprintf(value, sizeof value, "%02d", current_lives);
    if (current_lives < 0) snprintf(value, sizeof value, "--");
    draw_text_fit(x + w - 50, y + 48, 36, 2, 255, 241, 212, value);
    draw_text_fit(x + 14, y + 67, 86, 1, 184, 170, 149, "HEARTS");
    snprintf(value, sizeof value, "%02d", current_wario_hearts);
    if (current_wario_hearts < 0) snprintf(value, sizeof value, "--");
    draw_text_fit(x + w - 50, y + 67, 36, 2, 255, 241, 212, value);
    draw_text_fit(x + 14, y + 86, 86, 1, 184, 170, 149, "POWER");
    draw_text_fit(x + w - 76, y + 86, 62, 1, 255, 200, 87,
                  wario_power_name(current_wario_power));
    draw_text_fit(x + 14, y + 105, 86, 1, 184, 170, 149, "TIME LEFT");
    snprintf(value, sizeof value, "%03d", current_wario_time);
    if (current_wario_time < 0) snprintf(value, sizeof value, "---");
    draw_text_fit(x + w - 50, y + 105, 36, 2, 255, 241, 212, value);
}

static void draw_wario_expedition_log(const Settings *s, int x, int y, int w, int h)
{
    int g = GAME_WARIO_SML3;
    draw_android_card(s, x, y, w, h, g, "EXPEDITION LOG");
    if (!wario_expedition_log_count) {
        draw_text_fit(x + 14, y + 49, w - 28, 1, 184, 170, 149, "NO EVENTS RECORDED YET");
        return;
    }
    int rows = wario_expedition_log_count < 4 ? wario_expedition_log_count : 4;
    for (int i = 0; i < rows; i++) {
        int row_y = y + 48 + i * 12;
        int treasure = strncmp(wario_expedition_log[i], "TREASURE", 8) == 0;
        fill_circle(x + 17, row_y - 3, 2, treasure ? 255 : 189,
                    treasure ? 200 : 133, treasure ? 87 : 55, 255);
        draw_text_fit(x + 25, row_y, w - 38, 1,
            treasure ? 255 : (i == 0 ? 255 : 184),
            treasure ? 200 : (i == 0 ? 241 : 170),
            treasure ? 87 : (i == 0 ? 212 : 149), wario_expedition_log[i]);
    }
}

static void draw_wario_treasure_animation(void)
{
    if (!wario_treasure_animation_active) return;
    Uint32 elapsed = SDL_GetTicks() - wario_treasure_animation_started;
    if (elapsed >= 1850) {
        wario_treasure_animation_active = 0;
        return;
    }
    float phase = elapsed / 1850.f;
    int alpha = (int)((1.f - phase) * 170.f);
    for (int i = 0; i < 3; i++)
        outline_circle(400, 141, (int)(22 + phase * (95 + i * 20)),
                       255, 214 - i * 15, 107, (Uint8)(alpha > 255 ? 255 : alpha));
    fill_round_rect(246, 207, 308, 42, 9, 43, 31, 17, 238);
    outline_round_rect(246, 207, 308, 42, 9, 255, 214, 107, 255);
    draw_text_fit(267, 224, 266, 1, 255, 235, 165, "TREASURE FOUND!");
    char text[48];
    snprintf(text, sizeof text, "RELIC %02d ADDED TO YOUR VAULT",
             wario_treasure_animation_id > 0 ? wario_treasure_animation_id : 1);
    draw_text_fit(267, 239, 266, 1, 247, 233, 208, text);
    for (int i = 0; i < 12; i++) {
        float a = 6.2831853f * i / 12.f + phase * 1.3f;
        int distance = (int)(32 + phase * 70 + (i % 3) * 7);
        int sx = 400 + (int)(cosf(a) * distance), sy = 141 + (int)(sinf(a) * distance);
        int sz = (int)(4.5f * (1.f - phase));
        if (sz < 1) sz = 1;
        fill(sx - sz / 2, sy - sz / 2, sz, sz, 255, (i % 3) ? 200 : 243, 107, 255);
    }
}

static void draw_wario_companion(const Settings *s)
{
    int g = GAME_WARIO_SML3;
    draw_android_scenery(g);
    draw_asset_image(s, g, 0, 0, 0, 800, 450);
    draw_android_header(s, 18, 7, 265, 49, g);
    draw_text_fit(26, 23, 250, 1, 255, 200, 87, "PIPE CLEAN  •  TREASURE EXPEDITION");
    draw_text_fit(25, 43, 260, 2, 255, 241, 212, "WARIO LAND: SUPER MARIO LAND 3");
    draw_tiny_button(s, 593, 13, 105, 42, g, "BACKGROUND", "PNG / GIF", SIM_BACKGROUND);
    draw_tiny_button(s, 704, 13, 72, 42, g, "CLOSE", "GAME", SIM_CLOSE_GAME);

    draw_wario_vault(s, 20, 70, 230, 133);
    draw_wario_emblem();
    draw_wario_status(s, 556, 70, 220, 133);

    draw_android_card(s, 267, 213, 266, 94, g, "CURRENT COURSE");
    draw_text_fit(283, 254, 234, 2, 255, 200, 87, wario_course_text());
    char info[48];
    snprintf(info, sizeof info, "COIN BANK  %05d",
             current_wario_coins >= 0 ? current_wario_coins : 0);
    if (current_wario_coins < 0) snprintf(info, sizeof info, "COIN BANK  -----");
    draw_text_fit(283, 279, 234, 1, 255, 241, 212, info);
    draw_wario_expedition_log(s, 20, 213, 232, 94);

    const int action_ids[4] = {SIM_SAVE, SIM_LOAD, SIM_PAUSE, SIM_REWIND};
    const char *labels[4] = {"SAVE", "LOAD", current_paused ? "RESUME" : "PAUSE", "REWIND"};
    const char *subs[4] = {"SNAPSHOT", "RESTORE", current_paused ? "CONTINUE" : "HOLD GAME", "HOLD TO REWIND"};
    for (int i = 0; i < 4; i++)
        draw_action_button(s, g, action_ids[i], 94 + i * 153, 320,
            i == 3 ? 153 : 145, 42, labels[i], subs[i], i == 3);
    draw_slot_timeline(s, g);
    draw_wario_treasure_animation();
}

static void draw_companion(const Settings *s)
{
    int g = current_game >= 0 && current_game < N_GAMES ? current_game : -1;
    int editor_preview = simulator_layout_preview_enabled();
    int is_deck_game = g == GAME_DRMARIO || g == GAME_SML || g == GAME_SML2 ||
                       g == GAME_WARIO_SML3;
    int show_game_deck = editor_preview || (current_playing && is_deck_game);

    if (!show_game_deck) {
        fill(0, 0, 800, 450, 23, 35, 55, 255);
        draw_dashboard(s, g);
        draw_vinyl_notification(s);
        SDL_RenderPresent(deck_renderer);
        return;
    }

    if (g < 0 || g >= N_GAMES) g = GAME_SML;
    const CompanionLayoutCfg *d = &s->deck_layout[g];
    if (g == GAME_DRMARIO) { draw_dr_mario_companion(s, d); return; }
    if (g == GAME_WARIO_SML3) { draw_wario_companion(s); return; }
    draw_android_scenery(g);
    draw_asset_image(s, g, 0, 0, 0, 800, 450);

    float hx = 18, hy = 7, hw = 265, hh = 49;
    transform_box(d, 0, 400, 31.5f, &hx, &hy, &hw, &hh);
    draw_android_header(s, (int)hx, (int)hy, (int)hw, (int)hh, g);
    draw_text_fit((int)hx + 8, (int)hy + 10, (int)hw - 16, 1,
        theme_accent_r(g), theme_accent_g(g), theme_accent_b(g), "PIPE CLEAN  \xE2\x80\xA2  POCKET QUEST");
    draw_text_fit((int)hx + 8, (int)hy + 25, (int)hw - 16, 2,
        theme_ink_r(g), theme_ink_g(g), theme_ink_b(g), android_game_title(g));

    float bgx = 593, bgy = 13, bgw = 105, bgh = 42;
    transform_box(d, 0, 400, 31.5f, &bgx, &bgy, &bgw, &bgh);
    draw_tiny_button(s, (int)bgx, (int)bgy, (int)bgw, (int)bgh, g, "BACKGROUND", "PNG / GIF", SIM_BACKGROUND);
    float closex = 704, closey = 13, closew = 72, closeh = 42;
    transform_box(d, 0, 400, 31.5f, &closex, &closey, &closew, &closeh);
    draw_tiny_button(s, (int)closex, (int)closey, (int)closew, (int)closeh, g, "CLOSE", "GAME", SIM_CLOSE_GAME);

    /* Save-state thumbnails are represented by the timeline below. */
    /* The reserve item stays at the visual center and uses the per-game layout transform. */
    float ix = 272, iy = 36, iw = 256, ih = 256;
    transform_box(d, 1, 400, 164, &ix, &iy, &iw, &ih);
    draw_hero_item(s, g, current_item, ix + iw * .5f, iy + ih * .5f, iw * .5f);

    /* Status card uses the exact same origin and independent width/height transform as the editor. */
    float sx = 620, sy = 76, sw = 156, sh = 128;
    transform_box(d, 4, 698, 140, &sx, &sy, &sw, &sh);
    draw_android_card(s, (int)sx, (int)sy, (int)sw, (int)sh, g, "ADVENTURE STATUS");
    float status_scale_x = sw / 156.0f, status_scale_y = sh / 128.0f;
    float status_uniform_scale = fminf(status_scale_x, status_scale_y);
    fill_circle((int)(sx + 17 * status_scale_x), (int)(sy + 35 * status_scale_y),
                (int)fmaxf(1.f, 8 * status_uniform_scale), 255, 214, 107, 190);
    draw_text_fit((int)(sx + 32 * status_scale_x), (int)(sy + 29 * status_scale_y),
        (int)(110 * status_scale_x), 1, theme_muted_r(g), theme_muted_g(g), theme_muted_b(g), "LIVES");
    char lives_text[24];
    snprintf(lives_text, sizeof lives_text, "\xC3\x97 %02d", current_lives);
    draw_text_fit((int)(sx + 32 * status_scale_x), (int)(sy + 43 * status_scale_y),
        (int)(110 * status_scale_x), 2, theme_ink_r(g), theme_ink_g(g), theme_ink_b(g), lives_text);
    draw_text_fit((int)(sx + 14 * status_scale_x), (int)(sy + 69 * status_scale_y),
        (int)(126 * status_scale_x), 1, theme_muted_r(g), theme_muted_g(g), theme_muted_b(g),
        g == GAME_SML ? "WORLD / LEVEL" : "CURRENT STAGE");
    char stage_text[32];
    if (g == GAME_SML && current_world > 0 && current_stage > 0)
        snprintf(stage_text, sizeof stage_text, "%d - %d", current_world, current_stage);
    else if (g == GAME_SML2 && current_stage >= 0)
        snprintf(stage_text, sizeof stage_text, "STAGE %02d", current_stage);
    else snprintf(stage_text, sizeof stage_text, "--");
    draw_text_fit((int)(sx + 14 * status_scale_x), (int)(sy + 87 * status_scale_y),
        (int)(126 * status_scale_x), 2, theme_accent_r(g), theme_accent_g(g), theme_accent_b(g), stage_text);
    draw_text_fit((int)(sx + 14 * status_scale_x), (int)(sy + 112 * status_scale_y),
        (int)(126 * status_scale_x), 1, theme_muted_r(g), theme_muted_g(g), theme_muted_b(g),
        current_paused ? "PAUSED" : "IN PLAY");

    /* Action controls use the same Android coordinates, with each transform shared with the editor. */
    const int action_ids[4] = {SIM_SAVE, SIM_LOAD, SIM_PAUSE, SIM_REWIND};
    const char *action_labels[4] = {"SAVE", "LOAD", current_paused ? "RESUME" : "PAUSE", "REWIND"};
    const char *action_subs[4] = {"SNAPSHOT", "RESTORE",
        current_paused ? "CONTINUE" : "HOLD GAME", "HOLD TO REWIND"};
    for (int i = 0; i < 4; i++) {
        float ax = 94.0f + i * 153.0f, ay = 320.0f;
        float aw = (i == 3) ? 153.0f : 145.0f, ah = 42.0f;
        transform_box(d, 2, 400, 343, &ax, &ay, &aw, &ah);
        draw_action_button(s, g, action_ids[i], (int)ax, (int)ay,
            (int)aw, (int)ah, action_labels[i], action_subs[i], i == 3);
    }

    draw_slot_timeline(s, g);

    static const float decoration_x[N_DECK_DECORATIONS] = {
        280, 20, 610, 315, 26, 634, 190, 488, 26, 675, 195, 565, 40, 695, 315, 495
    };
    static const float decoration_y[N_DECK_DECORATIONS] = {
        153, 78, 78, 245, 218, 218, 85, 260, 135, 135, 290, 290, 35, 35, 175, 175
    };
    static const float decoration_w[N_DECK_DECORATIONS] = {
        240, 170, 170, 170, 140, 140, 122, 122, 100, 100, 100, 100, 95, 95, 112, 112
    };
    static const float decoration_h[N_DECK_DECORATIONS] = {
        144, 120, 120, 120, 92, 92, 86, 86, 70, 70, 70, 70, 65, 65, 74, 74
    };
    for (int slot = 0; slot < N_DECK_DECORATIONS; slot++) {
        float dx = decoration_x[slot], dy = decoration_y[slot];
        float dw = decoration_w[slot], dh = decoration_h[slot];
        transform_box(d, 5 + slot, 400, 225, &dx, &dy, &dw, &dh);
        int asset_index = slot == 0 ? 3 : slot <= 7 ? 20 + slot : 22 + slot;
        if (assets[g][asset_index])
            draw_asset_image(s, g, asset_index, (int)dx, (int)dy, (int)dw, (int)dh);
    }

    SDL_RenderPresent(deck_renderer);
}

void companion_sim_sync(Settings *s, SDL_Window *main_window)
{
    if (!s || !main_window) return;
    if (!s->launcher_dual_screen) {
        if (deck_window) {
            for (int g = 0; g < N_GAMES; g++) {
                for (int i = 0; i < N_DECK_ASSET_PATHS; i++) destroy_asset(g, i);
                for (int i = 0; i < 10; i++) {
                    if (slot_thumb_texture[g][i]) SDL_DestroyTexture(slot_thumb_texture[g][i]);
                    slot_thumb_texture[g][i] = NULL;
                    slot_thumb_path[g][i][0] = 0;
                    slot_thumb_mtime[g][i] = 0;
                }
            }
            if (dashboard_preview_texture) SDL_DestroyTexture(dashboard_preview_texture);
            dashboard_preview_texture = NULL;
            dashboard_preview_game = -2;
            if (deck_renderer) SDL_DestroyRenderer(deck_renderer);
            deck_renderer = NULL;
            SDL_DestroyWindow(deck_window);
            deck_window = NULL;
        }
        return;
    }
    if (deck_window) return;

    int mx, my, mw, mh;
    SDL_GetWindowPosition(main_window, &mx, &my);
    SDL_GetWindowSize(main_window, &mw, &mh);
    int display = SDL_GetWindowDisplayIndex(main_window);
    SDL_Rect bounds = {0, 0, 0, 0};
    if (display < 0 || SDL_GetDisplayBounds(display, &bounds) != 0) {
        bounds.x = mx; bounds.y = my; bounds.w = 1920; bounds.h = 1080;
    }
    int x = mx + mw + 18;
    int y = my + 45;
    if (x + 800 > bounds.x + bounds.w) x = mx - 818;
    if (x < bounds.x) x = bounds.x + (bounds.w - 800) / 2;
    if (y + 450 > bounds.y + bounds.h) y = bounds.y + (bounds.h - 450) / 2;
    deck_window = SDL_CreateWindow("PipeClean Companion Deck - Desktop Simulator",
        x, y, 800, 450, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!deck_window) {
        fprintf(stderr, "Companion Deck simulator window: %s\n", SDL_GetError());
        s->launcher_dual_screen = 0;
        settings_save();
        return;
    }
    SDL_SetWindowMinimumSize(deck_window, 480, 320);
    deck_renderer = SDL_CreateRenderer(deck_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!deck_renderer) deck_renderer = SDL_CreateRenderer(deck_window, -1, SDL_RENDERER_SOFTWARE);
    if (!deck_renderer) {
        fprintf(stderr, "Companion Deck simulator renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(deck_window); deck_window = NULL;
        s->launcher_dual_screen = 0;
        settings_save();
        return;
    }
    SDL_SetRenderDrawBlendMode(deck_renderer, SDL_BLENDMODE_BLEND);
    SDL_RenderSetLogicalSize(deck_renderer, 800, 450);
    sync_assets(s);
}

void companion_sim_set_state(int game, const Frame *frame, int paused, int selected_slot)
{
    current_game = game;
    current_paused = paused;
    current_slot = selected_slot < 0 ? 0 : selected_slot > 9 ? 9 : selected_slot;
    current_playing = frame != NULL;
    int next_item = frame ? frame->reserve_item : simulator_layout_preview_item();
    if (!simulator_layout_preview_enabled() && next_item != current_item) {
        Uint32 now = SDL_GetTicks();
        if (next_item > 0) {
            item_animation_kind = SIM_ITEM_ANIM_ARRIVE;
            item_animation_item = next_item;
            item_animation_started = now;
        } else if (current_item > 0 &&
                   (item_animation_kind != SIM_ITEM_ANIM_SEND_OFF ||
                    item_animation_item != current_item)) {
            item_animation_kind = SIM_ITEM_ANIM_SEND_OFF;
            item_animation_item = current_item;
            item_animation_started = now;
        }
    } else if (simulator_layout_preview_enabled()) {
        item_animation_kind = SIM_ITEM_ANIM_IDLE;
    }
    current_item = next_item;
    current_stage = -1;
    current_world = -1;
    current_lives = 0;
    current_dr_mario_score = -1;
    current_dr_mario_level = -1;
    current_dr_mario_speed = -1;
    current_dr_mario_capsule = 0xFF;
    if (frame && game == GAME_DRMARIO) {
        current_dr_mario_score = frame->dr_mario_score <= 99999999u ? (int)frame->dr_mario_score : -1;
        current_dr_mario_level = frame->dr_mario_level <= 99 ? frame->dr_mario_level : -1;
        current_dr_mario_speed = frame->dr_mario_speed;
        current_dr_mario_capsule = frame->dr_mario_capsule <= 0x17 ? frame->dr_mario_capsule : 0xFF;
    }
    if (frame && (game == GAME_SML || game == GAME_SML2)) {
        Uint8 bcd = frame->p1_lives;
        int tens = (bcd >> 4) & 0xf, ones = bcd & 0xf;
        current_lives = tens <= 9 && ones <= 9 ? tens * 10 + ones : bcd;
        if (frame->level_id != 0xff) current_stage = frame->level_id;
        if (game == GAME_SML && frame->level_bank != 0xff) current_world = frame->level_bank;
    }
    companion_sim_update_wario(game, frame);
}

void companion_sim_render(Settings *s, SDL_Window *main_window)
{
    companion_sim_sync(s, main_window);
    if (!deck_window || !deck_renderer) return;
    sync_assets(s);
    update_animated_assets();
    draw_companion(s);
}

static void play_custom_deck_sound(const CompanionLayoutCfg *d, int action)
{
    if (!d || !d->sounds_enabled) return;
    const char *path = NULL;
    switch (action) {
    case SIM_SAVE: path = d->button_sound_path[DECK_SOUND_SAVE]; break;
    case SIM_LOAD: path = d->button_sound_path[DECK_SOUND_LOAD]; break;
    case SIM_PAUSE: path = d->button_sound_path[DECK_SOUND_PAUSE]; break;
    case SIM_REWIND: path = d->button_sound_path[DECK_SOUND_REWIND]; break;
    case SIM_BACKGROUND: path = d->button_sound_path[DECK_SOUND_BACKGROUND]; break;
    case SIM_CLOSE_GAME: path = d->button_sound_path[DECK_SOUND_CLOSE]; break;
    case SIM_ITEM_USE: path = current_item ? d->item_sound_path : d->empty_item_sound_path; break;
    default: break;
    }
    if (path && path[0]) audio_deck_sfx_play(path);
}

static void push_action(int action, int pressed)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_USEREVENT;
    ev.user.code = SIM_EVENT_CODE;
    ev.user.data1 = (void *)(intptr_t)action;
    ev.user.data2 = (void *)(intptr_t)pressed;
    (void)SDL_PushEvent(&ev);
}

static int inside(float x, float y, float rx, float ry, float rw, float rh);
static void inverse_point(const CompanionLayoutCfg *d, int element, float *x, float *y,
                          float cx, float cy);

static int timeline_hit_slot(const Settings *s, int game, float x, float y)
{
    const CompanionLayoutCfg *d = &s->deck_layout[game];
    float base_x = 24.0f, base_y = 365.0f, w = 752.0f, h = 83.0f;
    float logical_x = x, logical_y = y;
    inverse_point(d, 3, &logical_x, &logical_y, 400.0f, 406.5f);
    if (!inside(logical_x, logical_y, base_x, base_y + 14.0f, w, h - 14.0f)) return -1;

    float focus = (float)current_slot, gap = 5.0f;
    float widths[10], centers[10], cursor = 0.0f;
    for (int i = 0; i < 10; i++) {
        float weight = fmaxf(0.0f, 1.0f - fabsf((float)i - focus));
        widths[i] = 72.0f + 90.0f * weight;
        centers[i] = cursor + widths[i] * 0.5f;
        cursor += widths[i] + gap;
    }
    float side_padding = fmaxf(0.0f, (w - 162.0f) * 0.5f);
    for (int i = 0; i < 10; i++) centers[i] += side_padding;
    float total_width = side_padding * 2.0f + cursor - gap;
    float scroll = fmaxf(0.0f, fminf(fmaxf(0.0f, total_width - w),
                                     centers[current_slot] - w * 0.5f));
    cursor = 0.0f;
    for (int i = 0; i < 10; i++) {
        float tile_x = base_x + side_padding + cursor - scroll;
        if (logical_x >= tile_x && logical_x < tile_x + widths[i]) return i;
        cursor += widths[i] + gap;
    }
    return -1;
}

static int inside(float x, float y, float rx, float ry, float rw, float rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void inverse_point(const CompanionLayoutCfg *d, int element, float *x, float *y,
                          float cx, float cy)
{
    int dx = 0, dy = 0, size = 100;
    switch (element) {
    case 0: dx=d->header_dx; dy=d->header_dy; size=d->header_scale; break;
    case 1: dx=d->item_dx; dy=d->item_dy; size=d->item_scale; break;
    case 2: dx=d->controls_dx; dy=d->controls_dy; size=d->controls_scale; break;
    case 3: dx=d->timeline_dx; dy=d->timeline_dy; size=d->timeline_scale; break;
    default: dx=d->decoration_dx; dy=d->decoration_dy; size=d->decoration_scale; break;
    }
    float scale = size > 0 ? (float)size / 100.f : 1.f;
    *x = cx + (*x - cx - dx) / scale;
    *y = cy + (*y - cy - dy) / scale;
}

int companion_sim_handle_event(const SDL_Event *e, Settings *s)
{
    if (!e || !s || !deck_window) return 0;
    Uint32 id = SDL_GetWindowID(deck_window);
    if (e->type == SDL_WINDOWEVENT && e->window.windowID == id &&
        e->window.event == SDL_WINDOWEVENT_CLOSE) {
        s->launcher_dual_screen = 0;
        settings_save();
        companion_sim_shutdown();
        return 1;
    }
    if (e->type != SDL_MOUSEBUTTONDOWN && e->type != SDL_MOUSEBUTTONUP &&
        e->type != SDL_MOUSEMOTION && e->type != SDL_WINDOWEVENT) return 0;
    Uint32 event_window = 0;
    if (e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_MOUSEBUTTONUP) event_window = e->button.windowID;
    else if (e->type == SDL_MOUSEMOTION) event_window = e->motion.windowID;
    else event_window = e->window.windowID;
    if (event_window != id) return 0;
    if (e->type == SDL_WINDOWEVENT) return 1;
        float logical_x = 0.f, logical_y = 0.f;
        if (e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_MOUSEBUTTONUP)
            SDL_RenderWindowToLogical(deck_renderer, e->button.x, e->button.y, &logical_x, &logical_y);
        else
            SDL_RenderWindowToLogical(deck_renderer, e->motion.x, e->motion.y, &logical_x, &logical_y);
        float x = logical_x, y = logical_y;
    int game = current_game >= 0 && current_game < N_GAMES ? current_game : GAME_SML;
    const CompanionLayoutCfg *d = &s->deck_layout[game];

    if (e->type == SDL_MOUSEBUTTONDOWN) {
        /* Android's DeckView is deliberately read-only while the layout editor
           previews it; controls become live again once an SML game is running. */
        if (!current_playing || simulator_layout_preview_enabled()) {
            mouse_down_action = 0;
            pressed_action = 0;
            return 1;
        }
        mouse_down_action = 0;
        float tx=x, ty=y;
        inverse_point(d, 0, &tx, &ty, 400, 31.5f);
        if (inside(tx, ty, 704, 13, 72, 42)) {
            mouse_down_action = SIM_CLOSE_GAME;
        } else if (inside(tx, ty, 593, 13, 105, 42)) {
            mouse_down_action = SIM_BACKGROUND;
        } else {
        inverse_point(d, 2, &tx, &ty, 400, 343);
        if (inside(tx, ty, 94, 320, 147, 42)) mouse_down_action = SIM_SAVE;
        else if (inside(tx, ty, 247, 320, 147, 42)) mouse_down_action = SIM_LOAD;
        else if (inside(tx, ty, 400, 320, 147, 42)) mouse_down_action = SIM_PAUSE;
        else if (inside(tx, ty, 553, 320, 153, 42)) mouse_down_action = SIM_REWIND;
        else {
            tx=x; ty=y;
            inverse_point(d, 1, &tx, &ty, 400, 164);
            if (inside(tx, ty, 236, 8, 328, 310)) mouse_down_action = SIM_ITEM_USE;
            else {
                int slot = timeline_hit_slot(s, game, x, y);
                if (slot >= 0) mouse_down_action = SIM_SLOT_SELECT_BASE + slot;
            }
        }
        }
        pressed_action = mouse_down_action;
        if (mouse_down_action == SIM_REWIND) {
            play_custom_deck_sound(d, SIM_REWIND);
            push_action(SIM_REWIND, 1);
        }
        return 1;
    }
    if (e->type == SDL_MOUSEBUTTONUP) {
        int up = mouse_down_action;
        if (up == SIM_REWIND) push_action(SIM_REWIND, 0);
        else if (up == SIM_BACKGROUND) {
            play_custom_deck_sound(d, SIM_BACKGROUND);
            launcher_open_companion_layout_picker(1, game);
        } else if (up) {
            play_custom_deck_sound(d, up);
            if (up == SIM_ITEM_USE && current_item > 0) {
                item_animation_kind = SIM_ITEM_ANIM_SEND_OFF;
                item_animation_item = current_item;
                item_animation_started = SDL_GetTicks();
            }
            push_action(up, 1);
        }
        pressed_action = 0;
        mouse_down_action = 0;
        return 1;
    }
    return 1;
}

void companion_sim_shutdown(void)
{
    if (deck_window) {
        for (int g = 0; g < N_GAMES; g++) {
            for (int i = 0; i < N_DECK_ASSET_PATHS; i++) destroy_asset(g, i);
            for (int i = 0; i < 10; i++) {
                if (slot_thumb_texture[g][i]) SDL_DestroyTexture(slot_thumb_texture[g][i]);
                slot_thumb_texture[g][i] = NULL;
                slot_thumb_path[g][i][0] = 0;
                slot_thumb_mtime[g][i] = 0;
            }
        }
        if (dashboard_preview_texture) SDL_DestroyTexture(dashboard_preview_texture);
        dashboard_preview_texture = NULL;
        dashboard_preview_game = -2;
        if (deck_renderer) SDL_DestroyRenderer(deck_renderer);
        deck_renderer = NULL;
        SDL_DestroyWindow(deck_window);
        deck_window = NULL;
    }
}
