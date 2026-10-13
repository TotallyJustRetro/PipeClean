/* Desktop-only Companion Deck simulator.
 * This renders the Android second-screen layout into a resizable peer window so
 * the layout and state can be inspected on machines with one physical display. */
#include "companion_sim.h"
#include "settings.h"
#include "games.h"
#include "emu.h"
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
static int current_paused, current_slot, current_item;
static int pressed_action, mouse_down_action;
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
        fill(x, y, w, h, bright ? 45 : 24, bright ? 75 : 43, bright ? 37 : 30, 245);
    else if (game == GAME_SML2)
        fill(x, y, w, h, bright ? 69 : 38, bright ? 38 : 25, bright ? 73 : 47, 245);
    else
        fill(x, y, w, h, bright ? 33 : 22, bright ? 63 : 42, bright ? 82 : 60, 245);
    Uint8 ar = game == GAME_SML ? 118 : game == GAME_SML2 ? 255 : 110;
    Uint8 ag = game == GAME_SML ? 207 : game == GAME_SML2 ? 202 : 229;
    Uint8 ab = game == GAME_SML ? 111 : game == GAME_SML2 ? 91 : 244;
    outline(x, y, w, h, ar, ag, ab, 255);
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

static void draw_button(const Settings *s, int game, int action, int x, int y, int w, int h,
                        const char *label, int active)
{
    const CompanionLayoutCfg *d = &s->deck_layout[game];
    fill(x + 2, y + 3, w, h, 4, 7, 12, 255);
    fill(x, y, w, h, active ? 54 : 29, active ? 53 : 42, active ? 39 : 55, 255);
    outline(x, y, w, h, active ? 246 : 79, active ? 203 : 113, active ? 91 : 139, 255);
    if (assets[game][1]) {
        SDL_Rect dest = {x + 1, y + 1, w - 2, h - 2};
        SDL_RenderCopy(deck_renderer, assets[game][1], NULL, &dest);
        fill(x + 1, y + 1, w - 2, h - 2, 3, 8, 16, active ? 55 : 90);
    }
    draw_text_fit(x + 8, y + h / 2 - 7, w - 16, 2, 244, 246, 255, label);
    (void)d; (void)action;
}

static void draw_slot_timeline(const Settings *s, int game)
{
    const CompanionLayoutCfg *d = &s->deck_layout[game];
    float x = 24, y = 365, w = 752, h = 70;
    transform_box(d, 3, 400, 400, &x, &y, &w, &h);
    draw_panel((int)x, (int)y, (int)w, (int)h, game, 0);
    draw_text((int)x + 12, (int)y + 7, 2, 174, 197, 223, "SAVE STATE TIMELINE");
    for (int i = 0; i < 10; i++) {
        int sx = (int)x + 10 + i * ((int)w - 20) / 10;
        int sw = ((int)w - 24) / 10;
        int selected = i == current_slot;
        fill(sx, (int)y + 26, sw, (int)h - 34,
             selected ? 38 : 13, selected ? 80 : 29, selected ? 68 : 43, 255);
        outline(sx, (int)y + 26, sw, (int)h - 34,
                selected ? 255 : 82, selected ? 207 : 99, selected ? 111 : 125, 255);
        char number[4];
        snprintf(number, sizeof number, "%02d", i + 1);
        draw_text_fit(sx + 3, (int)y + 33, sw - 6, 2, 237, 243, 252, number);
    }
}

static void draw_companion(const Settings *s)
{
    int g = current_game >= 0 && current_game < N_GAMES ? current_game : -1;
    const CompanionLayoutCfg *d = g >= 0 ? &s->deck_layout[g] : &s->deck_layout[GAME_SML];
    int theme_game = g >= 0 ? g : GAME_SML;
    color(7, 12, 24, 255);
    SDL_RenderClear(deck_renderer);
    fill(0, 0, 800, 450, 8, 13, 25, 255);
    if (g >= 0 && assets[g][0]) image_cover(assets[g][0], 0, 0, 800, 450);
    else {
        fill(0, 0, 800, 290, 13, 24, 40, 255);
        fill(0, 290, 800, 160, 15, 26, 31, 255);
        for (int i = 0; i < 18; i++) fill((i * 53 + 17) % 800, (i * 29 + 31) % 280, 2, 2, 95, 147, 173, 180);
    }

    float hx = 18, hy = 7, hw = 758, hh = 55;
    transform_box(d, 0, 400, 31, &hx, &hy, &hw, &hh);
    draw_panel((int)hx, (int)hy, (int)hw, (int)hh, theme_game, 0);
    draw_text_fit((int)hx + 12, (int)hy + 8, (int)hw - 24, 2, 111, 227, 225, "PIPECLEAN COMPANION DECK");
    const char *name = g >= 0 && games[g].name ? games[g].name : "DESKTOP SIMULATOR";
    draw_text_fit((int)hx + 12, (int)hy + 29, (int)hw - 24, 2, 242, 245, 250, name);
    if (g >= 0) {
        draw_text_fit(655, 22, 125, 2, 211, 224, 240, current_paused ? "PAUSED" : "IN GAME");
    } else {
        draw_text_fit(655, 22, 125, 2, 211, 224, 240, "READY");
    }

    /* Adventure status badge mirrors the Android Deck. */
    draw_panel(620, 76, 156, 128, theme_game, 0);
    if (g >= 0) {
        draw_text(634, 88, 2, 150, 190, 217, "LIVES");
        char lives[20];
        snprintf(lives, sizeof lives, "X %02d", current_lives);
        draw_text(634, 104, 3, 247, 226, 174, lives);
        draw_text(634, 141, 2, 150, 190, 217, g == GAME_SML ? "WORLD / LEVEL" : "CURRENT STAGE");
        char stage[32];
        if (g == GAME_SML && current_world >= 0)
            snprintf(stage, sizeof stage, "%d-%d", current_world + 1,
                     current_stage >= 0 ? current_stage + 1 : 1);
        else snprintf(stage, sizeof stage, "STAGE %02d", current_stage < 0 ? 0 : current_stage + 1);
        draw_text_fit(634, 160, 128, 2, 247, 202, 91, stage);
    } else {
        draw_text(634, 91, 2, 150, 190, 217, "DECK STATUS");
        draw_text(634, 111, 2, 236, 244, 253, "SECOND SCREEN");
        draw_text(634, 131, 2, 125, 227, 177, "ATTACHED");
        draw_text(634, 160, 2, 150, 190, 217, "LAUNCH A GAME");
    }

    /* Reserve item is the largest interactive element. */
    float ix = 327, iy = 94, iw = 146, ih = 146;
    transform_box(d, 1, 400, 164, &ix, &iy, &iw, &ih);
    int cx = (int)(ix + iw / 2), cy = (int)(iy + ih / 2);
    int radius = (int)(iw * .36f);
    fill(cx - radius - 16, cy - radius - 16, (radius + 16) * 2, (radius + 16) * 2, 40, 40, 25, 100);
    color(255, 221, 120, 255); SDL_RenderDrawRect(deck_renderer, &(SDL_Rect){cx-radius, cy-radius, radius*2, radius*2});
    color(255, 224, 142, 255); SDL_RenderDrawRect(deck_renderer, &(SDL_Rect){cx-radius+5, cy-radius+5, radius*2-10, radius*2-10});
    fill(cx - radius + 8, cy - radius + 8, radius*2 - 16, radius*2 - 16, 30, 42, 48, 255);
    if (g >= 0 && assets[g][2]) {
        SDL_Rect dest = {cx - radius + 8, cy - radius + 8, radius*2 - 16, radius*2 - 16};
        SDL_RenderCopy(deck_renderer, assets[g][2], NULL, &dest);
    } else {
        draw_text_fit(cx - radius + 9, cy - 6, radius*2 - 18, 3, 255, 224, 127,
                      current_item ? "ITEM" : "?");
    }
    if (current_item)
        draw_text_fit(cx - 65, cy + radius - 1, 130, 2, 245, 247, 253, "USE ITEM");
    else
        draw_text_fit(cx - 65, cy + radius - 1, 130, 2, 190, 201, 217, "EMPTY SLOT");

    /* Action controls use the same layout offsets/scales as Android. */
    float cx0 = 94, cy0 = 320, cw = 612, ch = 46;
    transform_box(d, 2, 400, 343, &cx0, &cy0, &cw, &ch);
    int y = (int)cy0, hh0 = (int)ch - 4, gap = 8, left = (int)cx0 + 2;
    int widths[4] = {(int)(cw * .24f), (int)(cw * .24f), (int)(cw * .24f), (int)(cw * .24f)};
    const char *labels[4] = {"SAVE", "LOAD", current_paused ? "RESUME" : "PAUSE", "REWIND"};
    const int actions[4] = {SIM_SAVE, SIM_LOAD, SIM_PAUSE, SIM_REWIND};
    for (int i = 0; i < 4; i++) {
        int bw = widths[i];
        draw_button(s, theme_game, actions[i], left, y, bw, hh0, labels[i], pressed_action == actions[i]);
        left += bw + gap;
    }
    draw_slot_timeline(s, theme_game);

    float dx = 280, dy = 150, dw = 240, dh = 150;
    transform_box(d, 4, 400, 225, &dx, &dy, &dw, &dh);
    if (g >= 0 && assets[g][3]) image_cover(assets[g][3], (int)dx, (int)dy, (int)dw, (int)dh);
    else {
        /* Decorative placement helper remains visible in simulation even without artwork. */
        outline((int)dx, (int)dy, (int)dw, (int)dh, 118, 158, 195, 90);
        draw_text_fit((int)dx + 6, (int)dy + (int)dh/2, (int)dw - 12, 2, 128, 154, 184, "ADD IMAGE IN EDITOR");
    }
    draw_text(18, 440, 1, 128, 154, 184, "PIPE CLEAN - DESKTOP COMPANION SIMULATOR");
    SDL_RenderPresent(deck_renderer);
}

void companion_sim_sync(const Settings *s, SDL_Window *main_window)
{
    if (!s || !main_window) return;
    if (!s->launcher_dual_screen) {
        if (deck_window) {
            for (int g = 0; g < N_GAMES; g++)
                for (int i = 0; i < 4; i++) {
                    if (assets[g][i]) SDL_DestroyTexture(assets[g][i]);
                    assets[g][i] = NULL;
                    asset_paths[g][i][0] = 0;
                }
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
        settings_save();
        return;
    }
    SDL_SetWindowMinimumSize(deck_window, 480, 320);
    deck_renderer = SDL_CreateRenderer(deck_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!deck_renderer) deck_renderer = SDL_CreateRenderer(deck_window, -1, SDL_RENDERER_SOFTWARE);
    if (!deck_renderer) {
        fprintf(stderr, "Companion Deck simulator renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(deck_window); deck_window = NULL;
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

void companion_sim_render(const Settings *s, SDL_Window *main_window)
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
    int game = current_game >= 0 && current_game < N_GAMES ? current_game : GAME_SML;
    const CompanionLayoutCfg *d = &s->deck_layout[game];

    if (e->type == SDL_MOUSEBUTTONDOWN) {
        mouse_down_action = 0;
        float tx=x, ty=y;
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
                tx=x; ty=y;
                inverse_point(d, 3, &tx, &ty, 400, 400);
                if (inside(tx, ty, 24, 365, 752, 83)) {
                    int slot = (int)((tx - 24) * 10 / 752);
                    if (slot < 0) slot = 0;
                    if (slot > 9) slot = 9;
                    mouse_down_action = SIM_SLOT_SELECT_BASE + slot;
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
        for (int g = 0; g < N_GAMES; g++)
            for (int i = 0; i < 4; i++) {
                if (assets[g][i]) SDL_DestroyTexture(assets[g][i]);
                assets[g][i] = NULL;
            }
        if (deck_renderer) SDL_DestroyRenderer(deck_renderer);
        deck_renderer = NULL;
        SDL_DestroyWindow(deck_window);
        deck_window = NULL;
    }
}
