/* Application loop: launcher <-> game. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "app.h"
#include "launcher.h"
#include "ui.h"
#include "settings.h"
#include "rom.h"
#include "emu.h"
#include "render.h"
#include "bgimg.h"
#include "texpack.h"
#include "pad.h"
#include "audio.h"
#include "events.h"
#include "util.h"
#include "branding.h"
#include <SDL_image.h>

static SDL_Window *win;
static SDL_Renderer *ren;
static int win_w = 1100, win_h = 720;

static void sfx(int w) { audio_sfx(w); }

static char game_notice_text[160];
static float game_notice_t;

static void game_notice(const char *s)
{
    snprintf(game_notice_text, sizeof game_notice_text, "%s", s ? s : "");
    game_notice_t = 2.2f;
}

static void game_notice_draw(float dt)
{
    if (game_notice_t <= 0 || !game_notice_text[0]) return;
    game_notice_t -= dt;
    float a = game_notice_t < 0.35f ? game_notice_t / 0.35f : 1.0f;
    float w = ui_text_w(F_BOLD, 13, game_notice_text) + 34.0f;
    float x = UI_W * 0.5f - w * 0.5f, y = UI_H - 64.0f;
    ui_shadow(x, y, w, 34, 10, 6, RGBA(0, 0, 0, (int)(100 * a)));
    ui_rrect(x, y, w, 34, 10, RGBA(36, 42, 60, (int)(245 * a)));
    ui_text_c(F_BOLD, 13, UI_W * 0.5f, y + 8, RGBA(245, 247, 252, (int)(255 * a)), game_notice_text);
}

static int app_key_down(int key)
{
    if (!key) return 0;
    SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)key);
    if (sc == SDL_SCANCODE_UNKNOWN) return 0;
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    return ks[sc] != 0;
}

static int state_file_exists(const char *path)
{
    FILE *f = path && path[0] ? fopen(path, "rb") : NULL;
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int thumb_pending_game = -1;
static int thumb_pending_slot = -1;

static void state_thumb_path(int g, int slot, char *out, size_t n)
{
    char dir[1200];
    snprintf(dir, sizeof dir, "%sstates/", settings_dir());
    mkdir_u(dir);
    snprintf(out, n, "%s%s.slot%d.thumb.bmp", dir, games[g].id, slot);
}

static void queue_state_thumbnail(int g, int slot)
{
    thumb_pending_game = g;
    thumb_pending_slot = slot;
}

static void capture_pending_state_thumbnail(void)
{
    if (thumb_pending_game < 0 || thumb_pending_slot < 0 || !ren) return;
    int W, H;
    if (SDL_GetRendererOutputSize(ren, &W, &H) != 0 || W <= 0 || H <= 0) return;

    SDL_Surface *src = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, 320, 180, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!src || !dst) {
        if (src) SDL_FreeSurface(src);
        if (dst) SDL_FreeSurface(dst);
        return;
    }
    if (SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, src->pixels, src->pitch) == 0) {
        SDL_FillRect(dst, NULL, SDL_MapRGB(dst->format, 0, 0, 0));
        float scale = fminf(320.0f / (float)W, 180.0f / (float)H);
        int dw = (int)(W * scale + 0.5f);
        int dh = (int)(H * scale + 0.5f);
        SDL_Rect dr = {(320 - dw) / 2, (180 - dh) / 2, dw, dh};
        SDL_BlitScaled(src, NULL, dst, &dr);
        char path[1200];
        state_thumb_path(thumb_pending_game, thumb_pending_slot, path, sizeof path);
        SDL_SaveBMP(dst, path);
    }
    SDL_FreeSurface(src);
    SDL_FreeSurface(dst);
    thumb_pending_game = -1;
    thumb_pending_slot = -1;
}

static void state_path(int g, int slot, char *out, size_t n)
{
    char dir[1200];
    snprintf(dir, sizeof dir, "%sstates/", settings_dir());
    mkdir_u(dir);
    snprintf(out, n, "%s%s.slot%d.pcs", dir, games[g].id, slot);
}

static void suspend_path(int g, char *out, size_t n)
{
    char dir[1200];
    snprintf(dir, sizeof dir, "%sstates/", settings_dir());
    mkdir_u(dir);
    snprintf(out, n, "%s%s.suspend.pcs", dir, games[g].id);
}

static int game_state_save(int g, int paused)
{
    char path[1200];
    state_path(g, settings.g[g].state_slot, path, sizeof path);
    if (emu_rewind_available()) emu_rewind_end();
    if (emu_state_save_file(path, 1) != 0) {
        game_notice("Couldn't save state.");
        return -1;
    }
    queue_state_thumbnail(g, settings.g[g].state_slot);
    emu_set_paused(paused);
    char msg[96];
    snprintf(msg, sizeof msg, "State saved — Slot %d", settings.g[g].state_slot + 1);
    game_notice(msg);
    return 0;
}

static int game_state_load(int g, int paused)
{
    char path[1200];
    state_path(g, settings.g[g].state_slot, path, sizeof path);
    if (!state_file_exists(path)) {
        char msg[96];
        snprintf(msg, sizeof msg, "No saved state in Slot %d", settings.g[g].state_slot + 1);
        game_notice(msg);
        return -1;
    }
    emu_rewind_end();
    if (emu_state_load_file(path, 1) != 0) {
        game_notice("Couldn't load state.");
        return -1;
    }
    emu_set_paused(paused);
    char msg[96];
    snprintf(msg, sizeof msg, "State loaded — Slot %d", settings.g[g].state_slot + 1);
    game_notice(msg);
    return 0;
}

static int game_suspend(int g)
{
    char path[1200];
    suspend_path(g, path, sizeof path);
    emu_rewind_end();
    if (emu_state_save_file(path, 0) != 0) {
        game_notice("Couldn't suspend the game.");
        return -1;
    }
    game_notice("Game suspended.");
    return 0;
}

static int game_action_key_match(int g, int action, const SDL_Event *e)
{
    if (!e || e->type != SDL_KEYDOWN || e->key.repeat || action < 0 || action >= N_ACTION) return 0;
    return settings.g[g].action_key[action] != 0 &&
           e->key.keysym.sym == settings.g[g].action_key[action];
}

static int app_handle_action_event(int g, const SDL_Event *e, int *paused, int *quit)
{
    if (!e || g < 0 || g >= N_GAMES) return 0;
    for (int a = 0; a < N_ACTION; a++) {
        if (!game_action_key_match(g, a, e) && !pad_binding_event(g, a, e)) continue;
        switch (a) {
        case ACT_SAVE_STATE:
            game_state_save(g, paused ? *paused : 0);
            break;
        case ACT_LOAD_STATE:
            game_state_load(g, paused ? *paused : 0);
            break;
        case ACT_REWIND:
            break; /* held state is handled every frame below */
        case ACT_SUSPEND:
            if (game_suspend(g) == 0 && quit) *quit = 1;
            break;
        case ACT_NEXT_SLOT:
            settings.g[g].state_slot = (settings.g[g].state_slot + 1) % 10;
            {
                char msg[64];
                snprintf(msg, sizeof msg, "State Slot %d", settings.g[g].state_slot + 1);
                game_notice(msg);
            }
            break;
        }
        return a + 1;
    }
    return 0;
}

static int app_rewind_held(int g)
{
    if (g < 0 || g >= N_GAMES) return 0;
    return app_key_down(settings.g[g].action_key[ACT_REWIND]) ||
           pad_binding_down(g, ACT_REWIND);
}

enum {
    DS_MENU_SAVE, DS_MENU_LOAD, DS_MENU_SLOT_PLUS, DS_MENU_REWIND,
    DS_MENU_SLOT_MINUS, DS_MENU_SUSPEND, DS_MENU_PAUSE,
    DS_MENU_COUNT
};

typedef struct {
    int open;
    int touch_active;
    float touch_x, touch_y;
    int selected;
} DsStateMenu;

static DsStateMenu ds_menu;

static int ds_menu_select(float x, float y)
{
    float dx = x - 0.5f, dy = y - 0.5f;
    if (dx * dx + dy * dy < 0.12f * 0.12f) return -1;
    float a = atan2f(dy, dx);
    float sector = (float)(2.0 * 3.14159265358979323846 / DS_MENU_COUNT);
    int s = (int)floorf(((float)3.14159265358979323846f / 8.0f - a) / sector + 0.5f);
    s %= DS_MENU_COUNT;
    if (s < 0) s += DS_MENU_COUNT;
    return s;
}

static void ds_menu_open(void)
{
    ds_menu.open = 1;
    ds_menu.touch_active = 0;
    ds_menu.selected = -1;
}

static void ds_menu_close(void)
{
    ds_menu.open = 0;
    ds_menu.touch_active = 0;
    ds_menu.selected = -1;
}

static int ds_menu_event(int g, const SDL_Event *e, int *paused, int *quit)
{
    if (!e || g < 0 || g >= N_GAMES) return 0;
    if (e->type == SDL_CONTROLLERBUTTONDOWN &&
        e->cbutton.button == SDL_CONTROLLER_BUTTON_TOUCHPAD &&
        pad_is_dualsense_instance(e->cbutton.which)) {
        if (ds_menu.open) {
            emu_rewind_end();
            ds_menu_close();
        } else {
            ds_menu_open();
        }
        return 1;
    }

    float x, y;
    int kind;
    if (!pad_touchpad_event(e, &x, &y, &kind)) return 0;

    if (kind == 0) {
        if (!ds_menu.open) ds_menu_open();
        ds_menu.touch_active = 1;
        ds_menu.touch_x = x; ds_menu.touch_y = y;
        ds_menu.selected = ds_menu_select(x, y);
    } else if (kind == 1 && ds_menu.open) {
        int old = ds_menu.selected;
        ds_menu.touch_x = x; ds_menu.touch_y = y;
        ds_menu.selected = ds_menu_select(x, y);
        if (old == DS_MENU_REWIND && ds_menu.selected != DS_MENU_REWIND) emu_rewind_end();
    } else if (kind == 2 && ds_menu.open) {
        ds_menu.touch_active = 0;
        ds_menu.touch_x = x; ds_menu.touch_y = y;
        ds_menu.selected = ds_menu_select(x, y);
        int a = ds_menu.selected;
        if (a == DS_MENU_SAVE) game_state_save(g, paused ? *paused : 0);
        else if (a == DS_MENU_LOAD) game_state_load(g, paused ? *paused : 0);
        else if (a == DS_MENU_SLOT_PLUS) { settings.g[g].state_slot = (settings.g[g].state_slot + 1) % 10; game_notice("Next state slot"); }
        else if (a == DS_MENU_SLOT_MINUS) { settings.g[g].state_slot = (settings.g[g].state_slot + 9) % 10; game_notice("Previous state slot"); }
        else if (a == DS_MENU_REWIND) emu_rewind_end();
        else if (a == DS_MENU_SUSPEND) { if (game_suspend(g) == 0 && quit) *quit = 1; }
        else if (a == DS_MENU_PAUSE) { *paused = !*paused; emu_set_paused(*paused); }
        if (a != DS_MENU_REWIND) emu_rewind_end();
        ds_menu_close();
    }
    return 1;
}

static uint32_t ds_mixc(uint32_t a, uint32_t b, float t)
{
    return RGBA((int)(((a >> 24) & 255) * (1 - t) + ((b >> 24) & 255) * t),
                (int)(((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t),
                (int)(((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t),
                (int)((a & 255) * (1 - t) + (b & 255) * t));
}

static void ds_menu_draw(int g)
{
    if (!ds_menu.open || g < 0 || g >= N_GAMES) return;
    static const char *labels[DS_MENU_COUNT] = {"Save", "Load", "Slot +", "Rewind", "Slot -", "Suspend", "Pause"};
    static const char *desc[DS_MENU_COUNT] = {
        "save state", "load state", "next slot", "hold to rewind",
        "previous slot", "save & return", "pause game"
    };
    float cx = UI_W * 0.5f, cy = UI_H * 0.5f;
    ui_shadow(cx - 270, cy - 220, 540, 440, 20, 12, RGBA(0, 0, 0, 135));
    ui_rrect(cx - 270, cy - 220, 540, 440, 20, RGBA(16, 19, 28, 244));
    ui_text_c(F_BOLD, 22, cx, cy - 190, C_TEXT, "DualSense • PipeClean");
    char slot[64];
    snprintf(slot, sizeof slot, "State Slot %d", settings.g[g].state_slot + 1);
    ui_text_c(F_REG, 12, cx, cy - 162, C_MUTED, slot);
    for (int i = 0; i < DS_MENU_COUNT; i++) {
        float a = -(3.14159265358979323846f * 2.0f * i / DS_MENU_COUNT) + 3.14159265358979323846f / 8.0f;
        float x = cx + cosf(a) * 180.0f, y = cy + sinf(a) * 140.0f;
        int hot = i == ds_menu.selected;
        ui_rrect(x - 68, y - 27, 136, 54, 12, hot ? ds_mixc(C_BTN_H, HEX(ui_accent), 0.28f) : C_BTN);
        if (hot) ui_stroke(x - 68, y - 27, 136, 54, 12, 2, HEX(ui_accent));
        ui_text_c(F_BOLD, 13, x, y - 15, hot ? C_TEXT : C_MUTED, labels[i]);
        ui_text_c(F_REG, 9, x, y + 4, C_DIM, desc[i]);
    }
    ui_text_c(F_REG, 11, cx, cy + 188, C_DIM, "Touch • slide to choose • release to confirm");
}

static void out_size(int *w, int *h)
{
    if (SDL_GetRendererOutputSize(ren, w, h) != 0 || *w <= 0 || *h <= 0) SDL_GetWindowSize(win, w, h);
}

static void mouse_sync(void)
{
    int mx, my, ww, wh;
    SDL_GetMouseState(&mx, &my);
    SDL_GetWindowSize(win, &ww, &wh);
    ui_set_mouse(mx, my, ww, wh);
}

static void set_game_window(int g)
{
    const GameCfg *c = &settings.g[g];
    if (c->size == SIZE_FULLSCREEN) { SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP); return; }
    SDL_SetWindowFullscreen(win, 0);
    int hh = c->size == SIZE_SMALL ? 432 : (c->size == SIZE_MEDIUM ? 576 : 720);
    float ar = c->aspect == ASPECT_4_3 ? 4.0f / 3 : (c->aspect == ASPECT_16_9 ? 16.0f / 9 : ppu_w / 144.0f);
    SDL_SetWindowSize(win, (int)(hh * ar), hh);
    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

static void draw_bg_cover(float dim)
{
    int bw, bh, W, H;
    SDL_Texture *t = bg_texture(&bw, &bh);
    out_size(&W, &H);
    if (!t) return;
    float s = fmaxf((float)W / bw, (float)H / bh);
    SDL_Rect src = {(int)((bw - W / s) * 0.5f), (int)((bh - H / s) * 0.5f), (int)(W / s), (int)(H / s)};
    SDL_RenderCopy(ren, t, &src, NULL);
    if (dim > 0) {
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, (Uint8)(dim * 2.55f));
        SDL_RenderFillRect(ren, NULL);
    }
}

static int sml1_oam_anchor(const Frame *f, const uint8_t oam[16], float *gx, float *gy)
{
    if (!f || !oam || !gx || !gy || f->w <= 0) return 0;
    int L = f->xoff;
    int sprite_neg = L > 8 ? 256 - (L - 8) : 256;
    int minx = 10000, maxx = -10000, miny = 10000;
    int count = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *s = &oam[i * 4];
        if (!s[0]) continue;
        int x = s[1];
        if (x >= sprite_neg) x -= 256;
        x = x - 8 + L;
        int y = s[0] - 16;
        if (x < minx) minx = x;
        if (x + 8 > maxx) maxx = x + 8;
        if (y < miny) miny = y;
        count++;
    }
    if (!count) return 0;
    *gx = (minx + maxx) * 0.5f;
    *gy = (float)miny;
    return 1;
}

static void sml1_draw_player_tag(const Frame *f, const uint8_t oam[16], const SDL_Rect *r,
                                 const char *label, uint32_t color)
{
    float gx, gy;
    if (!sml1_oam_anchor(f, oam, &gx, &gy) || !r || r->w <= 0 || r->h <= 0) return;

    float sx = r->x + gx / (float)f->w * r->w;
    float sy = r->y + (gy - 2.0f) / (float)GB_H * r->h;
    float ux = sx / ui_scale + ui_view_x0();
    float uy = sy / ui_scale + ui_view_y0();

    float tw = ui_text_w(F_BOLD, 12, label) + 12.0f;
    float tx = ux - tw * 0.5f;
    float ty = uy - 22.0f;

    ui_shadow(tx, ty, tw, 18, 6, 3, RGBA(0, 0, 0, 105));
    ui_rrect(tx, ty, tw, 18, 6, color);
    ui_text_c(F_BOLD, 12, ux, ty + 1, HEX(0xFFFFFF), label);
    ui_tri(ux - 5, ty + 16, ux + 5, ty + 16, ux, uy - 1, color);
}

static int sml1_bcd_to_int(uint8_t b)
{
    return ((b >> 4) & 0x0F) * 10 + (b & 0x0F);
}

static void sml1_draw_coop_hud(const Frame *f)
{
    if (!f) return;

    float vx = ui_view_x0(), vw = ui_view_w();
    const float y = 12.0f, w = 230.0f, h = 42.0f, pad = 14.0f;
    float lx = vx + pad;
    float rx = vx + vw - pad - w;

    char lives[16];

    ui_shadow(lx, y, w, h, 10, 4, RGBA(0, 0, 0, 110));
    ui_rrect(lx, y, w, h, 10, RGBA(22, 25, 34, 235));
    ui_text(F_BOLD, 14, lx + 12, y + 5, C_ACCENT, "P1  MARIO");
    snprintf(lives, sizeof lives, "x %d", sml1_bcd_to_int(f->p1_lives));
    ui_text_r(F_BOLD, 16, lx + w - 12, y + 3, C_TEXT, lives);

    ui_shadow(rx, y, w, h, 10, 4, RGBA(0, 0, 0, 110));
    ui_rrect(rx, y, w, h, 10, RGBA(22, 25, 34, 235));
    ui_text(F_BOLD, 14, rx + 12, y + 5, C_OK, "P2  LUIGI");
    snprintf(lives, sizeof lives, "x %d", f->p2_lives);
    ui_text_r(F_BOLD, 16, rx + w - 12, y + 3, C_TEXT, lives);

    if (!f->p2_visible && f->p2_lives > 0) {
        ui_text(F_REG, 11, rx + 12, y + 24, HEX(0xA8B0C0), "Respawning...");
    }
}

/* Local SML1 multiplayer: one shared world with two real SML1 player states. */
static int play_multiplayer_sml1(int g)
{
    GameCfg *c = &settings.g[g];
    char sp[1200];
    snprintf(sp, sizeof sp, "%ssaves/", settings_dir());
    mkdir_u(sp);
    snprintf(sp, sizeof sp, "%ssaves/%s.sav", settings_dir(), games[g].id);
    emu_set_save_path(sp);

    bg_load(c->bg_path);
    texpack_load(c->tex_on ? c->tex_path : "");
    tex_collect_begin(g);
    render_reset();
    pad_set_context(g, 1);
    /*
     * Multiplayer is plug-and-play. Keep the two players on different physical
     * devices whenever two controllers are available. Keyboard 2 (WASD + J/K)
     * remains the fallback when P2 has no controller assigned.
     */
    int pads = pad_count();
    if (pads > 0 && (c->pad_device[0] < 0 || c->pad_device[0] >= pads))
        c->pad_device[0] = 0;
    if (pads > 1) {
        if (c->pad_device[1] < 0 || c->pad_device[1] >= pads || c->pad_device[1] == c->pad_device[0])
            c->pad_device[1] = c->pad_device[0] == 0 ? 1 : 0;
    } else if (c->pad_device[1] == c->pad_device[0]) {
        c->pad_device[1] = -1;
    }
    set_game_window(g);

    int requested_load = launcher_take_load_state(g);
    char suspended[1200];
    suspend_path(g, suspended, sizeof suspended);
    int has_suspend = state_file_exists(suspended);
    if (emu_mp_begin()) {
        launcher_toast("Couldn't start SML1 multiplayer.");
        return 0;
    }
    if (requested_load >= 0) {
        c->state_slot = requested_load;
        (void)game_state_load(g, 0);
    } else if (has_suspend) {
        if (emu_state_load_file(suspended, 1) == 0) {
            remove(suspended);
        }
    }
    apu_set_volume(settings.volume / 100.0f);
    audio_game_begin();

    Frame *f = (Frame *)calloc(1, sizeof *f);
    Frame *p2f = (Frame *)calloc(1, sizeof *p2f);
    int16_t a0[4096 * 2];
    if (!f || !p2f) {
        free(f);
        free(p2f);
        audio_game_end();
        emu_mp_end();
        tex_collect_save();
        pad_set_context(g, 0);
        launcher_toast("Not enough memory for SML1 multiplayer.");
        return 0;
    }
    int quit = 0, paused = 0, have = 0, shot = 0;
    int rewind_held = 0;
    uint8_t b0 = 0, d0 = 0, b1 = 0, d1 = 0;
    Uint64 last = SDL_GetPerformanceCounter();

    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            pad_event(&e);
            int ds_consumed = ds_menu_event(g, &e, &paused, &quit);
            if (ds_consumed) continue;
            int action = app_handle_action_event(g, &e, &paused, &quit);
            if (e.type == SDL_QUIT) quit = 2;
            else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                switch (e.key.keysym.sym) {
                case SDLK_ESCAPE: quit = 1; break;
                case SDLK_F11:
                    SDL_SetWindowFullscreen(win, (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    break;
                case SDLK_p:
                    paused = !paused;
                    break;
                case SDLK_TAB: emu_set_turbo(1); break;
                case SDLK_F12: shot = 1; break;
                }
            } else if (e.type == SDL_KEYUP && e.key.keysym.sym == SDLK_TAB) {
                emu_set_turbo(0);
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE && !action) {
                quit = 1;
            }
        }

        int ds_rewinding = ds_menu.open && ds_menu.touch_active && ds_menu.selected == DS_MENU_REWIND;
        int now_rewind = app_rewind_held(g) && !paused;
        if (ds_rewinding) {
            emu_rewind_step();
            emu_mp_frame_refresh(f);
            have = 1;
        } else if (now_rewind) {
            if (!rewind_held) {
                if (emu_rewind_available()) rewind_held = 1;
                else game_notice("Rewind buffer is not ready yet.");
            }
            if (rewind_held) {
                emu_rewind_step();
                emu_mp_frame_refresh(f);
                have = 1;
            }
        } else {
            if (rewind_held) {
                emu_rewind_end();
                rewind_held = 0;
            }
            if (!paused) {
                pad_poll_player(g, 0, &b0, &d0);
                pad_poll_player(g, 1, &b1, &d1);

                int n0 = emu_mp_step(0, b0, d0, f, a0, 4096);
                if (n0 < 0) {
                    quit = 1;
                } else {
                    int n1 = emu_mp_step(1, b1, d1, p2f, NULL, 0);
                    if (n1 < 0) {
                        quit = 1;
                    } else {
                        have = 1;
                        emu_rewind_capture();
                        if (f->game_state == 0 && f->p2_visible)
                            render_overlay_sml1_luigi_oam(f, p2f->mario_oam2, 0, 0);
                        if (n0 > 0) audio_game_push(a0, n0);
                        if (audio_ok()) audio_game_wait(audio_game_target());
                    }
                }
            }
        }

        int ev;
        while ((ev = events_pop()) >= 0) pad_event_fx(g, ev);

        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)(now - last) / SDL_GetPerformanceFrequency();
        last = now;

        int W, H;
        out_size(&W, &H);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        draw_bg_cover((float)c->bg_dim);
        bg_update(dt);

        SDL_Rect game_rect = {0, 0, 0, 0};
        if (have) {
            render_build(f, g, 1);
            render_fit(W, H, c->aspect, c->scaling, &game_rect);
            render_draw(&game_rect, c->scaling);
            pad_set_screen_color(render_avg_color());
        }

        capture_pending_state_thumbnail();
        ui_begin(W, H, dt);
        if (have) {
            sml1_draw_player_tag(f, f->mario_oam, &game_rect, "P1", C_ACCENT);
            if (f->p2_visible)
                sml1_draw_player_tag(f, p2f->mario_oam2, &game_rect, "P2", C_OK);
            sml1_draw_coop_hud(f);
        }
        if (paused) {
            ui_rect(ui_view_x0(), ui_view_y0(), ui_view_w(), ui_view_h(), RGBA(0, 0, 0, 120));
            ui_text_c(F_BOLD, 32, UI_W / 2, UI_H / 2 - 20, C_TEXT, "Paused");
        }
        game_notice_draw(dt);
        ds_menu_draw(g);
        ui_end();

        pad_frame(dt);

        if (shot) {
            shot = 0;
            SDL_Surface *snap = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
            if (snap) {
                SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, snap->pixels, snap->pitch);
                char path[1200];
                snprintf(path, sizeof path, "%sscreenshot_%u.bmp", settings_dir(), SDL_GetTicks());
                SDL_SaveBMP(snap, path);
                SDL_FreeSurface(snap);
            }
        }

        SDL_RenderPresent(ren);
    }

    emu_set_turbo(0);
    audio_game_end();
    emu_mp_end();
    tex_collect_save();
    free(f);
    free(p2f);
    pad_set_context(g, 0);
    ds_menu_close();
    SDL_SetWindowFullscreen(win, 0);
    SDL_SetWindowSize(win, win_w, win_h);
    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    return quit == 2;
}

/* returns 0 = back to launcher, 1 = quit */
static int play(int g)
{
    char err[256];
    GameCfg *c = &settings.g[g];
    if (launcher_prepare(g, err, sizeof err)) { launcher_toast(err); return 0; }
    audio_menu_music(0);
    if (g == GAME_SML && c->multiplayer) return play_multiplayer_sml1(g);
    char sp[1200];
    snprintf(sp, sizeof sp, "%ssaves/", settings_dir());
    mkdir_u(sp);
    snprintf(sp, sizeof sp, "%ssaves/%s.sav", settings_dir(), games[g].id);
    emu_set_save_path(sp);
    bg_load(c->bg_path);
    texpack_load(c->tex_on ? c->tex_path : "");
    tex_collect_begin(g);
    render_reset();
    pad_set_context(g, 1);
    set_game_window(g);
    int requested_load = launcher_take_load_state(g);
    char suspended[1200];
    suspend_path(g, suspended, sizeof suspended);
    int has_suspend = state_file_exists(suspended);
    if (emu_start(0)) { launcher_toast("Couldn't start the game."); return 0; }
    if (requested_load >= 0) {
        c->state_slot = requested_load;
        (void)game_state_load(g, 0);
    } else if (has_suspend) {
        if (emu_state_load_file(suspended, 1) == 0) remove(suspended);
    }

    Frame *f = SDL_malloc(sizeof *f);
    memset(f, 0, sizeof *f);
    int quit = 0, paused = 0, have = 0, rewind_held = 0;
    Uint64 last = SDL_GetPerformanceCounter();
    int shot = 0;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            pad_event(&e);
            int ds_consumed = ds_menu_event(g, &e, &paused, &quit);
            if (ds_consumed) continue;
            int action = app_handle_action_event(g, &e, &paused, &quit);
            if (e.type == SDL_QUIT) { quit = 2; }
            else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                switch (e.key.keysym.sym) {
                case SDLK_ESCAPE: quit = 1; break;
                case SDLK_F11: SDL_SetWindowFullscreen(win, (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP); break;
                case SDLK_p: paused = !paused; emu_set_paused(paused); break;
                case SDLK_TAB: emu_set_turbo(1); break;
                case SDLK_F12: shot = 1; break;
                }
            } else if (e.type == SDL_KEYUP && e.key.keysym.sym == SDLK_TAB) { emu_set_turbo(0); }
            else if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE && !action) quit = 1;
        }
        if (ds_menu.open && ds_menu.touch_active && ds_menu.selected == DS_MENU_REWIND) {
            emu_rewind_step();
            if (emu_frame_get(f)) have = 1;
        }
        int now_rewind = app_rewind_held(g) && !paused;
        if (now_rewind) {
            if (!rewind_held) {
                if (emu_rewind_available()) rewind_held = 1;
                else game_notice("Rewind buffer is not ready yet.");
            }
        } else if (rewind_held) {
            emu_rewind_end();
            rewind_held = 0;
        }
        uint8_t b, d;
        pad_poll(g, &b, &d);
        emu_input(rewind_held ? 0 : b, rewind_held ? 0 : d);
        if (emu_frame_get(f)) { have = 1; tex_collect_frame(f); }
        int ev;
        while ((ev = events_pop()) >= 0) pad_event_fx(g, ev);
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)(now - last) / SDL_GetPerformanceFrequency();
        last = now;

        int W, H;
        out_size(&W, &H);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        draw_bg_cover((float)c->bg_dim);
        bg_update(dt);
        if (have) {
            render_build(f, g, 1);
            SDL_Rect r;
            render_fit(W, H, c->aspect, c->scaling, &r);
            render_draw(&r, c->scaling);
            pad_set_screen_color(render_avg_color());
        }
        capture_pending_state_thumbnail();
        ui_begin(W, H, dt);
        if (paused) {
            ui_rect(ui_view_x0(), ui_view_y0(), ui_view_w(), ui_view_h(), RGBA(0, 0, 0, 120));
            ui_text_c(F_BOLD, 32, UI_W / 2, UI_H / 2 - 20, C_TEXT, "Paused");
        }
        if (rewind_held) {
            ui_rrect(UI_W / 2 - 112, 18, 224, 34, 10, RGBA(20, 24, 35, 230));
            ui_text_c(F_BOLD, 13, UI_W / 2, 26, C_TEXT, "REWIND");
        }
        game_notice_draw(dt);
        ds_menu_draw(g);
        ui_end();
        pad_frame(dt);
        if (shot) { shot = 0; SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
                    if (s) { SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
                             char p[1200]; snprintf(p, sizeof p, "%sscreenshot_%u.bmp", settings_dir(), SDL_GetTicks()); SDL_SaveBMP(s, p); SDL_FreeSurface(s); } }
        SDL_RenderPresent(ren);
    }
    emu_stop();
    SDL_free(f);
    tex_collect_save();
    pad_set_context(g, 0);
    SDL_SetWindowFullscreen(win, 0);
    SDL_SetWindowSize(win, win_w, win_h);
    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    return quit == 2;
}

int app_run(const char *autorun, const char *hack)
{
    (void)hack;
#ifdef _WIN32
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    int img_flags = IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_TIF | IMG_INIT_WEBP;
    if ((IMG_Init(img_flags) & img_flags) != img_flags) {
        fprintf(stderr, "SDL_image: %s\\n", IMG_GetError());
    }
    settings_load();
    win = SDL_CreateWindow("PipeClean", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    SDL_SetWindowMinimumSize(win, 640, 420);
    if (getenv("GBL_SIZE")) { sscanf(getenv("GBL_SIZE"), "%dx%d", &win_w, &win_h); SDL_SetWindowSize(win, win_w, win_h); }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    ui_init(ren);
    branding_init(ren, win);
    bg_init(ren);
    render_init(ren);
    launcher_set_renderer(ren);
    audio_init(settings.latency);
    pad_init();
    ui_sfx_cb = sfx;
    launcher_init();
    launcher_enter();
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    int quit = 0;
    if (autorun) {
        int id = rom_identify_file(autorun);
        if (id >= 0) { snprintf(settings.g[id].rom_path, sizeof settings.g[id].rom_path, "%s", autorun); quit = play(id); launcher_enter(); }
    }
    Uint64 last = SDL_GetPerformanceCounter();
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            pad_event(&e);
            launcher_event(&e);
            switch (e.type) {
            case SDL_QUIT: quit = 1; break;
            case SDL_MOUSEBUTTONDOWN: if (e.button.button == SDL_BUTTON_LEFT) { mouse_sync(); ui_mouse_button(1); } break;
            case SDL_MOUSEBUTTONUP: if (e.button.button == SDL_BUTTON_LEFT) { mouse_sync(); ui_mouse_button(0); } break;
            case SDL_MOUSEWHEEL: ui_mouse.wheel += e.wheel.y; break;
            case SDL_DROPFILE: launcher_drop(e.drop.file); SDL_free(e.drop.file); break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_LEAVE) ui_mouse.inside = 0;
                break;
            }
        }
        mouse_sync();
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)(now - last) / SDL_GetPerformanceFrequency();
        last = now;
        int W, H;
        out_size(&W, &H);
        SDL_SetRenderDrawColor(ren, 14, 16, 22, 255);
        SDL_RenderClear(ren);
        ui_begin(W, H, dt);
        LauncherResult r = launcher_frame(dt);
        ui_end();
        if (getenv("GBL_SHOT")) {
            static int fr;
            if (fr == 3 && getenv("GBL_MOUSE")) { int mx, my; sscanf(getenv("GBL_MOUSE"), "%d,%d", &mx, &my); SDL_WarpMouseInWindow(win, mx, my); }
            if (++fr == 8) {
                SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
                SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
                SDL_SaveBMP(s, getenv("GBL_SHOT"));
                printf("out %dx%d hint='%s' mouse=%.0f,%.0f\n", W, H, ui_hint_text(), ui_mouse.x, ui_mouse.y);
                quit = 1;
            }
        }
        SDL_RenderPresent(ren);
        if (r.play >= 0) {
            audio_sfx(SFX_CONFIRM);
            settings_save();
            quit = play(r.play);
            launcher_enter();
            memset(&ui_mouse, 0, sizeof ui_mouse);
        }
        if (!(SDL_GetWindowFlags(win) & SDL_WINDOW_SHOWN)) SDL_Delay(30);
    }
    launcher_shutdown();
    branding_shutdown();
    settings_save();
    pad_shutdown();
    audio_shutdown();
    render_shutdown();
    bg_shutdown();
    ui_shutdown();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    IMG_Quit();
    SDL_Quit();
    return 0;
}