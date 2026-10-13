/* Desktop-only Companion Deck simulator.
 * This renders the Android second-screen layout into a resizable peer window so
 * the layout and state can be inspected on machines with one physical display. */
#include "companion_sim.h"
#include "settings.h"
#include "games.h"
#include "emu.h"
#include "launcher.h"
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
#define SIM_CLOSE_GAME 12
#define SIM_SLOT_SELECT_BASE 20

static SDL_Window *deck_window;
static SDL_Renderer *deck_renderer;
static SDL_Texture *assets[N_GAMES][4];
static char asset_paths[N_GAMES][4][512];
static int current_game = -1, current_lives, current_stage = -1, current_world = -1;
static int current_paused, current_slot, current_item, current_playing;
static int pressed_action, mouse_down_action;
static SDL_Texture *dashboard_preview_texture;
static int dashboard_preview_game = -2;
static Uint32 dashboard_preview_tick;
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

static Uint8 theme_bg_r(int g) { return g == GAME_SML ? 184 : g == GAME_SML2 ? 37 : 23; }
static Uint8 theme_bg_g(int g) { return g == GAME_SML ? 217 : g == GAME_SML2 ? 21 : 35; }
static Uint8 theme_bg_b(int g) { return g == GAME_SML ? 145 : g == GAME_SML2 ? 47 : 55; }
static Uint8 theme_accent_r(int g) { return g == GAME_SML ? 57 : g == GAME_SML2 ? 255 : 110; }
static Uint8 theme_accent_g(int g) { return g == GAME_SML ? 124 : g == GAME_SML2 ? 202 : 229; }
static Uint8 theme_accent_b(int g) { return g == GAME_SML ? 48 : g == GAME_SML2 ? 91 : 239; }
static Uint8 theme_ink_r(int g) { return g == GAME_SML ? 37 : g == GAME_SML2 ? 255 : 234; }
static Uint8 theme_ink_g(int g) { return g == GAME_SML ? 60 : g == GAME_SML2 ? 240 : 245; }
static Uint8 theme_ink_b(int g) { return g == GAME_SML ? 32 : g == GAME_SML2 ? 210 : 255; }
static Uint8 theme_muted_r(int g) { return g == GAME_SML ? 87 : g == GAME_SML2 ? 209 : 175; }
static Uint8 theme_muted_g(int g) { return g == GAME_SML ? 118 : g == GAME_SML2 ? 183 : 195; }
static Uint8 theme_muted_b(int g) { return g == GAME_SML ? 77 : g == GAME_SML2 ? 213 : 221; }
static Uint8 theme_edge_r(int g) { return g == GAME_SML ? 128 : g == GAME_SML2 ? 173 : 87; }
static Uint8 theme_edge_g(int g) { return g == GAME_SML ? 169 : g == GAME_SML2 ? 123 : 125; }
static Uint8 theme_edge_b(int g) { return g == GAME_SML ? 81 : g == GAME_SML2 ? 65 : 157; }

static void draw_android_card(int x, int y, int w, int h, int game, const char *label)
{
    draw_panel(x + 2, y + 4, w, h, game, 0);
    fill_round_rect(x, y, w, h, 15,
        game == GAME_SML ? 240 : game == GAME_SML2 ? 53 : 36,
        game == GAME_SML ? 245 : game == GAME_SML2 ? 35 : 53,
        game == GAME_SML ? 213 : game == GAME_SML2 ? 66 : 76, 242);
    outline_round_rect(x + 1, y + 1, w - 2, h - 2, 14,
        theme_edge_r(game), theme_edge_g(game), theme_edge_b(game), 255);
    fill(x + 12, y + 9, w - 24, 3,
        theme_accent_r(game), theme_accent_g(game), theme_accent_b(game), 255);
    draw_text(x + 12, y + 18, 1,
        theme_muted_r(game), theme_muted_g(game), theme_muted_b(game), label);
}

static void draw_android_header(int x, int y, int w, int h, int game)
{
    fill_round_rect(x + 2, y + 3, w, h, 12, 0, 0, 0, 106);
    fill_round_rect(x, y, w, h, 12,
        game == GAME_SML ? 240 : game == GAME_SML2 ? 53 : 36,
        game == GAME_SML ? 245 : game == GAME_SML2 ? 35 : 53,
        game == GAME_SML ? 213 : game == GAME_SML2 ? 66 : 76, 242);
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

    draw_android_header(18, 7, 282, 48, -1);
    draw_text(27, 15, 1, 110, 229, 239, "PIPE CLEAN  \xE2\x80\xA2  GAME LIBRARY");
    draw_text(27, 30, 2, 234, 245, 255, "READY TO PLAY");

    draw_android_card(20, 65, 460, 362, -1, "SELECTED GAME");
    draw_dashboard_preview(34, 94, 432, 199, selected_game);
    const char *name = selected_game >= 0 && selected_game < N_GAMES &&
        games[selected_game].name ? games[selected_game].name : "CHOOSE A GAME";
    if (selected_game == GAME_SML) name = "SUPER MARIO LAND";
    else if (selected_game == GAME_SML2) name = "SUPER MARIO LAND 2";
    draw_text_fit(36, 306, 420, 2, 234, 245, 255, name);
    int played = selected_game >= 0 && selected_game < N_GAMES ? s->game_play_seconds[selected_game] : 0;
    char time_text[48];
    play_time_label(played, time_text, sizeof time_text);
    draw_text(36, 337, 1, 175, 195, 221, "TOTAL PLAY TIME");
    draw_text_fit(36, 352, 420, 2, 110, 229, 239, time_text);

    draw_android_card(492, 65, 288, 362, -1, "RECENTLY PLAYED");
    int rows = 0;
    for (int i = 0; i < N_RECENT_PLAYED; i++)
        if (s->recent_played[i] >= 0 && s->recent_played[i] < N_GAMES) rows++;
    if (rows == 0) {
        draw_text(510, 113, 1, 175, 195, 221, "YOUR ADVENTURE");
        draw_text_fit(510, 142, 246, 1, 234, 245, 255, "GAMES YOU LAUNCH APPEAR HERE");
        draw_text_fit(510, 158, 246, 1, 175, 195, 221, "PLAY TIME IS SAVED AUTOMATICALLY");
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
            draw_text_fit(531, ry + 3, 226, 1,
                row == 0 ? 110 : 234, row == 0 ? 229 : 245, row == 0 ? 239 : 255, recent_name);
            play_time_label(s->game_play_seconds[id], time_text, sizeof time_text);
            draw_text_fit(531, ry + 14, 226, 1, 175, 195, 221, time_text);
            row++;
        }
    }
    /* The decorative corner markers and simple scan accents match the Android deck's frame. */
    fill(9, 9, 12, 3, 110, 229, 239, 204); fill(9, 9, 3, 12, 110, 229, 239, 204);
    fill(779, 9, 12, 3, 110, 229, 239, 204); fill(788, 9, 3, 12, 110, 229, 239, 204);
    fill(9, 438, 12, 3, 110, 229, 239, 204); fill(9, 429, 3, 12, 110, 229, 239, 204);
    fill(779, 438, 12, 3, 110, 229, 239, 204); fill(788, 429, 3, 12, 110, 229, 239, 204);
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
            SDL_Texture *thumb = slot_thumbnail(game, i);
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
                draw_text_fit((int)(tile_x + tile_w * .5f - (tile_w - 8) * .5f),
                    (int)tile_bottom - 13, (int)tile_w - 8, 1,
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
                draw_text_fit((int)(tile_x + 4), (int)tile_bottom - 3,
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
    if (assets[game][1]) {
        SDL_Rect dest = {x + 1, y + 1, w - 2, h - 2};
        SDL_RenderCopy(deck_renderer, assets[game][1], NULL, &dest);
        fill_round_rect(x + 1, y + 1, w - 2, h - 2, 11, 3, 8, 16, pressed_action == action ? 55 : 68);
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

static void draw_tiny_button(int x, int y, int w, int h, int game,
                             const char *label, const char *sub, int action)
{
    int highlighted = action != 0 && action == pressed_action;
    fill_round_rect(x, y, w, h, 8,
        highlighted ? 255 : (game == GAME_SML ? 240 : game == GAME_SML2 ? 53 : 36),
        highlighted ? 214 : (game == GAME_SML ? 245 : game == GAME_SML2 ? 35 : 53),
        highlighted ? 107 : (game == GAME_SML ? 213 : game == GAME_SML2 ? 66 : 76), 245);
    if (assets[game][1]) {
        SDL_Rect dest = {x + 1, y + 1, w - 2, h - 2};
        SDL_RenderCopy(deck_renderer, assets[game][1], NULL, &dest);
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
    float t = SDL_GetTicks() / 1000.0f;
    int cx = (int)center_x, cy = (int)center_y, r = (int)radius;
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
    if (item > 0 && assets[game][2]) {
        SDL_Rect dest = {cx - r * 46 / 100, cy - r * 46 / 100, r * 92 / 100, r * 92 / 100};
        SDL_RenderCopy(deck_renderer, assets[game][2], NULL, &dest);
    } else if (item > 0) {
        const char *label = item == 1 ? "MUSHROOM" :
            game == GAME_SML ? "SUPERBALL FLOWER" :
            item == 3 ? "CARROT" : "FIRE FLOWER";
        fill_circle(cx, cy - 8, r / 3, 230, 53, 65, 255);
        fill(cx - r / 3, cy - 1, r * 2 / 3, r / 4, 238, 238, 220, 255);
        fill(cx - r / 4, cy + r / 5, r / 2, r / 2, 238, 238, 220, 255);
        draw_text_fit(cx - r + 7, cy + r * 2 / 3, r * 2 - 14, 1,
            theme_ink_r(game), theme_ink_g(game), theme_ink_b(game), label);
    } else {
        /* The empty pocket is represented by a lilac, pixel-like star. */
        fill(cx - 2, cy - 27, 4, 54, 180, 161, 203, 255);
        fill(cx - 27, cy - 2, 54, 4, 180, 161, 203, 255);
        fill(cx - 14, cy - 14, 28, 28, 180, 161, 203, 255);
        fill(cx - 8, cy - 8, 16, 16, 36, 53, 76, 255);
    }
    if (item > 0) {
        const char *label = item == 1 ? "MUSHROOM" :
            game == GAME_SML ? "SUPERBALL FLOWER" :
            item == 3 ? "CARROT" : "FIRE FLOWER";
        draw_text_fit(cx - r, cy + (int)(r * .66f), r * 2, 1,
            theme_ink_r(game), theme_ink_g(game), theme_ink_b(game), label);
        draw_text_fit(cx - r, cy + (int)(r * .81f), r * 2, 1,
            theme_muted_r(game), theme_muted_g(game), theme_muted_b(game),
            "TAP WHEN READY");
    }
    (void)s;
}

static void load_asset(int game, int index, const char *path)
{
    if (game < 0 || game >= N_GAMES || index < 0 || index >= 4) return;
    const char *next = path ? path : "";
    if (!strcmp(asset_paths[game][index], next)) return;
    if (assets[game][index]) SDL_DestroyTexture(assets[game][index]);
    assets[game][index] = NULL;
    snprintf(asset_paths[game][index], sizeof asset_paths[game][index], "%s", next);
    if (!next[0]) return;
    SDL_Surface *surface = IMG_Load(next);
    if (!surface) return;
    assets[game][index] = SDL_CreateTextureFromSurface(deck_renderer, surface);
    SDL_FreeSurface(surface);
    if (assets[game][index]) SDL_SetTextureBlendMode(assets[game][index], SDL_BLENDMODE_BLEND);
}

static void sync_assets(const Settings *s)
{
    if (!s || !deck_renderer) return;
    for (int g = 0; g < N_GAMES; g++) {
        const CompanionLayoutCfg *d = &s->deck_layout[g];
        load_asset(g, 0, d->background_path);
        load_asset(g, 1, d->button_texture_path);
        load_asset(g, 2, d->item_texture_path);
        load_asset(g, 3, d->decoration_path);
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

static void transform_box(const CompanionLayoutCfg *d, int element,
                          float cx, float cy, float *x, float *y, float *w, float *h)
{
    int dx = 0, dy = 0, size = 100;
    switch (element) {
    case 0: dx = d->header_dx; dy = d->header_dy; size = d->header_scale; break;
    case 1: dx = d->item_dx; dy = d->item_dy; size = d->item_scale; break;
    case 2: dx = d->controls_dx; dy = d->controls_dy; size = d->controls_scale; break;
    case 3: dx = d->timeline_dx; dy = d->timeline_dy; size = d->timeline_scale; break;
    default: dx = d->decoration_dx; dy = d->decoration_dy; size = d->decoration_scale; break;
    }
    float scale = (float)(size > 0 ? size : 100) / 100.f;
    *x = cx + (*x - cx) * scale + dx;
    *y = cy + (*y - cy) * scale + dy;
    *w *= scale;
    *h *= scale;
}

static void draw_companion(const Settings *s)
{
    int g = current_game >= 0 && current_game < N_GAMES ? current_game : -1;
    int editor_preview = launcher_companion_layout_preview_enabled();
    int is_deck_game = g == GAME_SML || g == GAME_SML2;
    int show_game_deck = editor_preview || (current_playing && is_deck_game);

    if (!show_game_deck) {
        fill(0, 0, 800, 450, 23, 35, 55, 255);
        draw_dashboard(s, g);
        SDL_RenderPresent(deck_renderer);
        return;
    }

    if (g < 0 || g >= N_GAMES) g = GAME_SML;
    const CompanionLayoutCfg *d = &s->deck_layout[g];
    draw_android_scenery(g);
    if (assets[g][0]) image_cover(assets[g][0], 0, 0, 800, 450);

    float hx = 18, hy = 7, hw = 265, hh = 49;
    transform_box(d, 0, 400, 31.5f, &hx, &hy, &hw, &hh);
    draw_android_header((int)hx, (int)hy, (int)hw, (int)hh, g);
    draw_text_fit((int)hx + 8, (int)hy + 10, (int)hw - 16, 1,
        theme_accent_r(g), theme_accent_g(g), theme_accent_b(g), "PIPE CLEAN  \xE2\x80\xA2  POCKET QUEST");
    draw_text_fit((int)hx + 8, (int)hy + 25, (int)hw - 16, 2,
        theme_ink_r(g), theme_ink_g(g), theme_ink_b(g), android_game_title(g));

    float bgx = 593, bgy = 13, bgw = 105, bgh = 42;
    transform_box(d, 0, 400, 31.5f, &bgx, &bgy, &bgw, &bgh);
    draw_tiny_button((int)bgx, (int)bgy, (int)bgw, (int)bgh, g, "BACKGROUND", "PNG / GIF", 0);
    float closex = 704, closey = 13, closew = 72, closeh = 42;
    transform_box(d, 0, 400, 31.5f, &closex, &closey, &closew, &closeh);
    draw_tiny_button((int)closex, (int)closey, (int)closew, (int)closeh, g, "CLOSE", "GAME", SIM_CLOSE_GAME);

    /* Left-hand save-slot preview / current save card. */
    draw_android_card(24, 72, 160, 132, g, current_playing ? "ACTIVE SAVE" : "SAVE SLOT PREVIEW");
    int occupied = slot_file_occupied(g, current_slot);
    SDL_Texture *thumb = slot_thumbnail(g, current_slot);
    SDL_Rect thumb_dst = {40, 103, 128, 56};
    fill_round_rect(40, 103, 128, 56, 5, 20, 33, 48, 255);
    if (thumb) SDL_RenderCopy(deck_renderer, thumb, NULL, &thumb_dst);
    else draw_text_fit(45, 126, 118, 1,
        occupied ? theme_accent_r(g) : theme_muted_r(g),
        occupied ? theme_accent_g(g) : theme_muted_g(g),
        occupied ? theme_accent_b(g) : theme_muted_b(g), occupied ? "THUMBNAIL PENDING" : "EMPTY SLOT");
    char slot_text[32];
    snprintf(slot_text, sizeof slot_text, "SLOT %02d", current_slot + 1);
    draw_text(38, 170, 1, theme_ink_r(g), theme_ink_g(g), theme_ink_b(g), slot_text);
    draw_text_fit(38, 188, 138, 1,
        theme_accent_r(g), theme_accent_g(g), theme_accent_b(g),
        occupied ? "SAVED" : "READY TO SAVE");

    /* The reserve item stays at the visual center and uses the per-game layout transform. */
    float ix = 272, iy = 36, iw = 256, ih = 256;
    transform_box(d, 1, 400, 164, &ix, &iy, &iw, &ih);
    draw_hero_item(s, g, current_item, ix + iw * .5f, iy + ih * .5f, iw * .5f);

    /* Status card matches the Android deck's lives / stage block. */
    draw_android_card(620, 76, 156, 128, g, "ADVENTURE STATUS");
    fill_circle(637, 111, 8, 255, 214, 107, 190);
    draw_text(652, 105, 1, theme_muted_r(g), theme_muted_g(g), theme_muted_b(g), "LIVES");
    char lives_text[24];
    snprintf(lives_text, sizeof lives_text, "\xC3\x97 %02d", current_lives);
    draw_text_fit(652, 119, 110, 2, theme_ink_r(g), theme_ink_g(g), theme_ink_b(g), lives_text);
    draw_text(634, 145, 1, theme_muted_r(g), theme_muted_g(g), theme_muted_b(g),
        g == GAME_SML ? "WORLD / LEVEL" : "CURRENT STAGE");
    char stage_text[32];
    if (g == GAME_SML && current_world >= 0 && current_stage >= 0)
        snprintf(stage_text, sizeof stage_text, "%d - %d", current_world + 1, current_stage + 1);
    else if (g == GAME_SML2 && current_stage >= 0)
        snprintf(stage_text, sizeof stage_text, "STAGE %02d", current_stage + 1);
    else snprintf(stage_text, sizeof stage_text, "--");
    draw_text_fit(634, 163, 126, 2,
        theme_accent_r(g), theme_accent_g(g), theme_accent_b(g), stage_text);
    draw_text(634, 188, 1, theme_muted_r(g), theme_muted_g(g), theme_muted_b(g),
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

    float dx = 280, dy = 150, dw = 240, dh = 150;
    transform_box(d, 4, 400, 225, &dx, &dy, &dw, &dh);
    if (assets[g][3]) image_cover(assets[g][3], (int)dx, (int)dy, (int)dw, (int)dh);

    SDL_RenderPresent(deck_renderer);
}

void companion_sim_sync(Settings *s, SDL_Window *main_window)
{
    if (!s || !main_window) return;
    if (!s->launcher_dual_screen) {
        if (deck_window) {
            for (int g = 0; g < N_GAMES; g++) {
                for (int i = 0; i < 4; i++) {
                    if (assets[g][i]) SDL_DestroyTexture(assets[g][i]);
                    assets[g][i] = NULL;
                    asset_paths[g][i][0] = 0;
                }
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
    current_item = frame ? frame->reserve_item : 0;
    current_stage = -1;
    current_world = -1;
    current_lives = 0;
    if (frame && (game == GAME_SML || game == GAME_SML2)) {
        Uint8 bcd = frame->p1_lives;
        int tens = (bcd >> 4) & 0xf, ones = bcd & 0xf;
        current_lives = tens <= 9 && ones <= 9 ? tens * 10 + ones : bcd;
        if (frame->level_id != 0xff) current_stage = frame->level_id;
        if (game == GAME_SML && frame->level_bank != 0xff) current_world = frame->level_bank;
    }
}

void companion_sim_render(Settings *s, SDL_Window *main_window)
{
    companion_sim_sync(s, main_window);
    if (!deck_window || !deck_renderer) return;
    sync_assets(s);
    draw_companion(s);
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
    float tx = x, ty = y, tw = 752.0f, th = 83.0f;
    const CompanionLayoutCfg *d = &s->deck_layout[game];
    transform_box(d, 3, 400.0f, 406.5f, &tx, &ty, &tw, &th);
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
        if (!current_playing && !launcher_companion_layout_preview_enabled()) return 1;
        mouse_down_action = 0;
        float tx=x, ty=y;
        inverse_point(d, 0, &tx, &ty, 400, 31.5f);
        if (inside(tx, ty, 704, 13, 72, 42)) {
            mouse_down_action = SIM_CLOSE_GAME;
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
        if (mouse_down_action == SIM_REWIND) push_action(SIM_REWIND, 1);
        return 1;
    }
    if (e->type == SDL_MOUSEBUTTONUP) {
        int up = mouse_down_action;
        if (up == SIM_REWIND) push_action(SIM_REWIND, 0);
        else if (up) push_action(up, 1);
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
            for (int i = 0; i < 4; i++) {
                if (assets[g][i]) SDL_DestroyTexture(assets[g][i]);
                assets[g][i] = NULL;
                asset_paths[g][i][0] = 0;
            }
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
