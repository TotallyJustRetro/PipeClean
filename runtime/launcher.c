/* The launcher window: one tab per game (play, display, controls, controller light/sound, textures),
 * a filter tab and an audio tab. Everything is drawn by the immediate-mode UI at the window's real resolution. */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "launcher.h"
#include <SDL_image.h>
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
#include "widescreen.h"

static SDL_Renderer *g_ren; static SDL_Renderer *ren_get(void){return g_ren;}
void launcher_set_renderer(SDL_Renderer *r){g_ren=r;}
void ui_text_fit_tail(int font, float size, float x, float y, float maxw, uint32_t c, const char *s);

enum { TAB_MULTIPLAYER = N_GAMES, TAB_MP_CONTROLLERS, TAB_FILTERS, TAB_AUDIO, N_TABS };
enum { SUB_GAME, SUB_ROMHACKS, SUB_DISPLAY, SUB_CONTROLS, SUB_BINDINGS, SUB_DUALSENSE, SUB_TEXTURES, SUB_SAVE_STATES, N_SUB };
static const char *sub_names[N_SUB] = {"Game", "ROM Hacks", "Display", "Controllers", "Bindings", "DualSense", "Textures", "Save States"};

static int tab, sub[N_GAMES];
static RomStatus rs[N_GAMES];            /* is the configured ROM usable */
static char hack_msg[N_GAMES][256];
static int hack_ok[N_GAMES];
static Frame prev[N_GAMES];
static int prev_ok[N_GAMES];
static int filter_game;                  /* which game the Filters tab previews */
static int cap_kind, cap_btn, cap_slot;  /* 1 key, 2 pad, 3 shortcut key, 4 shortcut pad, 5 P2 key, 6 P2 pad */
static char toast_msg[200];
static float toast_t;
static int click_id;
static float save_timer;
static int loaded_pack_game = -1, pack_n;
static char loaded_pack_path[512];
static char export_msg[N_GAMES][160];
static int export_scale = 8;
static char bg_loaded[512], bg_msg[256];
static int bg_for_game = -1;
static float anim_clock;
static int wide_dirty, wide_note[N_GAMES];
static SDL_Texture *state_thumb_tex[N_GAMES][10];
static char state_thumb_path_cache[N_GAMES][10][1200];
static SDL_Texture *romhack_thumb_tex[N_GAMES][MAX_ROMHACKS];
static char romhack_thumb_path_cache[N_GAMES][MAX_ROMHACKS][512];
static float romhack_scroll[N_GAMES];
static int romhack_title_editing_game = -1, romhack_title_editing_index = -1;
static char romhack_title_before[ROMHACK_TITLE_LEN];
static int load_state_request[N_GAMES];
static char sfx_test_msg[64];
static int controller_menu = -1;
static int multiplayer_name_editing;
static int multiplayer_count_menu = -1;
static int mp_controller_view = 0; /* 0 = bindings, 1 = character sounds */
static int mp_config_game = GAME_SML;
static int launcher_controller_cursor_active;
static int launcher_controller_confirm_was_down, launcher_controller_back_was_down;
static int launcher_controller_wait_neutral, launcher_ignore_pad_confirm;
static float launcher_controller_x, launcher_controller_y;
static int clickable(float x, float y, float w, float h, int *over_out);

static void controller_name(int device, char *out, size_t n)
{
    if (device < 0) { snprintf(out, n, "Not assigned"); return; }
    if (device >= pad_count()) { snprintf(out, n, "Controller %d (not connected)", device + 1); return; }
    snprintf(out, n, "%s", pad_name(device));
}

static void controller_dropdown(int g, int player, float x, float y, float w)
{
    GameCfg *c = &settings.g[g];
    int *sel = &c->pad_device[player];
    char name[96];
    controller_name(*sel, name, sizeof name);

    int over = 0;
    int clicked = clickable(x, y, w, 34, &over);
    ui_rrect(x, y, w, 34, 9, over ? C_BTN_H : C_BTN);
    char title[120];
    snprintf(title, sizeof title, "Player %d   %s", player + 1, name);
    ui_text_fit(F_REG, 13, x + 12, y + 8, w - 42, C_TEXT, title);
    ui_tri(x + w - 20, y + 12, x + w - 10, y + 12, x + w - 15, y + 20, C_MUTED);
    if (clicked) controller_menu = controller_menu == player ? -1 : player;

    if (controller_menu != player) return;

    float py = y + 38;
    float ph = 32.0f;
    int count = pad_count();
    int options = count + 1;
    ui_shadow(x, py, w, options * 34 + 8, 10, 8, RGBA(0, 0, 0, 100));
    ui_rrect(x, py, w, options * 34 + 8, 10, C_BG2);

    for (int o = 0; o < options; o++) {
        float oy = py + 4 + o * 34;
        int over_o = 0;
        int pick = clickable(x + 4, oy, w - 8, ph, &over_o);
        ui_rrect(x + 4, oy, w - 8, ph, 7, (o == (*sel + 1)) ? HEX(ui_accent) : (over_o ? C_BTN_H : C_BTN));
        char label_text[96];
        if (o == 0) {
            snprintf(label_text, sizeof label_text, "Not assigned");
        } else {
            int dev = o - 1;
            snprintf(label_text, sizeof label_text, "Controller %d — %s", dev + 1, pad_name(dev));
        }
        ui_text_fit(F_REG, 13, x + 14, oy + 7, w - 28, (o == (*sel + 1)) ? HEX(0xFFFFFF) : C_TEXT, label_text);
        if (pick) {
            int device = o - 1;
            if (!pad_assign_device(g, player, device))
                launcher_toast("That controller is already assigned to the other player.");
            controller_menu = -1;
        }
    }
    (void)player;
}


static int last_game_tab;
int launcher_current_game(void) { return tab < N_GAMES ? tab : last_game_tab; }
int launcher_capturing(void) { return cap_kind != 0; }

int launcher_take_load_state(int game)
{
    if (game < 0 || game >= N_GAMES) return -1;
    int slot = load_state_request[game];
    load_state_request[game] = -1;
    return slot;
}

static void clear_romhack_thumbs(void)
{
    for (int g = 0; g < N_GAMES; g++) {
        for (int i = 0; i < MAX_ROMHACKS; i++) {
            if (romhack_thumb_tex[g][i]) SDL_DestroyTexture(romhack_thumb_tex[g][i]);
            romhack_thumb_tex[g][i] = NULL;
            romhack_thumb_path_cache[g][i][0] = 0;
        }
    }
}

static void clear_romhack_thumbs_game(int g)
{
    if (g < 0 || g >= N_GAMES) return;
    for (int i = 0; i < MAX_ROMHACKS; i++) {
        if (romhack_thumb_tex[g][i]) SDL_DestroyTexture(romhack_thumb_tex[g][i]);
        romhack_thumb_tex[g][i] = NULL;
        romhack_thumb_path_cache[g][i][0] = 0;
    }
}

static SDL_Texture *romhack_thumb_get(int g, int index)
{
    if (g < 0 || g >= N_GAMES || index < 0 || index >= MAX_ROMHACKS || !g_ren) return NULL;
    const char *path = settings.g[g].romhacks[index].thumbnail;
    if (!path[0] || !file_exists(path)) {
        if (romhack_thumb_tex[g][index]) SDL_DestroyTexture(romhack_thumb_tex[g][index]);
        romhack_thumb_tex[g][index] = NULL;
        romhack_thumb_path_cache[g][index][0] = 0;
        return NULL;
    }
    if (romhack_thumb_tex[g][index] &&
        !strcmp(romhack_thumb_path_cache[g][index], path))
        return romhack_thumb_tex[g][index];
    if (romhack_thumb_tex[g][index]) SDL_DestroyTexture(romhack_thumb_tex[g][index]);
    romhack_thumb_tex[g][index] = NULL;
    SDL_Surface *surface = IMG_Load(path);
    if (!surface) return NULL;
    romhack_thumb_tex[g][index] = SDL_CreateTextureFromSurface(g_ren, surface);
    SDL_FreeSurface(surface);
    if (!romhack_thumb_tex[g][index]) return NULL;
    snprintf(romhack_thumb_path_cache[g][index], sizeof romhack_thumb_path_cache[g][index], "%s", path);
    return romhack_thumb_tex[g][index];
}

static void clear_state_thumbs(void)
{
    for (int g = 0; g < N_GAMES; g++) {
        for (int slot = 0; slot < 10; slot++) {
            if (state_thumb_tex[g][slot]) SDL_DestroyTexture(state_thumb_tex[g][slot]);
            state_thumb_tex[g][slot] = NULL;
            state_thumb_path_cache[g][slot][0] = 0;
        }
    }
}

static SDL_Texture *state_thumb_get(int g, int slot)
{
    if (g < 0 || g >= N_GAMES || slot < 0 || slot >= 10 || !g_ren) return NULL;
    char path[1200];
    snprintf(path, sizeof path, "%sstates/%s.slot%d.thumb.bmp", settings_dir(), games[g].id, slot);
    if (!file_exists(path)) {
        if (state_thumb_tex[g][slot]) SDL_DestroyTexture(state_thumb_tex[g][slot]);
        state_thumb_tex[g][slot] = NULL;
        state_thumb_path_cache[g][slot][0] = 0;
        return NULL;
    }
    if (state_thumb_tex[g][slot] && !strcmp(state_thumb_path_cache[g][slot], path)) return state_thumb_tex[g][slot];
    if (state_thumb_tex[g][slot]) SDL_DestroyTexture(state_thumb_tex[g][slot]);
    state_thumb_tex[g][slot] = NULL;
    SDL_Surface *s = IMG_Load(path);
    if (!s) return NULL;
    state_thumb_tex[g][slot] = SDL_CreateTextureFromSurface(g_ren, s);
    SDL_FreeSurface(s);
    if (!state_thumb_tex[g][slot]) return NULL;
    snprintf(state_thumb_path_cache[g][slot], sizeof state_thumb_path_cache[g][slot], "%s", path);
    return state_thumb_tex[g][slot];
}

void launcher_toast(const char *m) { snprintf(toast_msg, sizeof toast_msg, "%s", m); toast_t = 3.0f; }

/* ------------------------------------------------------------------ ROM / preview plumbing */
static void rom_check(int g)
{
    GameCfg *c = &settings.g[g];
    memset(&rs[g], 0, sizeof rs[g]);
    if (!c->rom_path[0]) { snprintf(rs[g].msg, sizeof rs[g].msg, "No ROM chosen yet."); return; }
    if (!file_exists(c->rom_path)) { snprintf(rs[g].msg, sizeof rs[g].msg, "That file is gone. Choose it again."); return; }
    rom_probe(g, c->rom_path, &rs[g]);
}

int launcher_prepare(int g, char *err, size_t n)
{
    GameCfg *c = &settings.g[g];
    RomStatus st;
    if (!c->rom_path[0] || rom_load(g, c->rom_path, &st)) {
        snprintf(err, n, "%s", c->rom_path[0] ? st.msg : "Choose your ROM on this game's tab first.");
        return 1;
    }
    hack_ok[g] = 0;
    if (c->hack_path[0]) {
        if (rom_apply_hack(c->hack_path, &st) == 0) {
            hack_ok[g] = 1;
            snprintf(hack_msg[g], sizeof hack_msg[g], "%s", st.msg);
        } else {
            hack_ok[g] = 0;
            snprintf(hack_msg[g], sizeof hack_msg[g], "%s", st.msg);
            snprintf(err, n, "Couldn't apply the selected romhack: %.180s", st.msg);
            return 1;
        }
    }
    int wl, wr;
    wide_dims(g, c->wide, &wl, &wr);
    if (wl + wr > 0 && !wide_install(g, wl, wr)) { wl = wr = 0; wide_note[g] = 1; } else wide_note[g] = 0;
    ppu_set_wide(wl, wr, games[g].hud_lines, games[g].hud_window, games[g].wide_gate);
    return 0;
}

static void ensure_pack(int g)
{
    GameCfg *c = &settings.g[g];
    const char *want = c->tex_on ? c->tex_path : "";
    if (loaded_pack_game == g && !strcmp(loaded_pack_path, want)) return;
    pack_n = texpack_load(want);
    loaded_pack_game = g;
    snprintf(loaded_pack_path, sizeof loaded_pack_path, "%s", want);
}

static void make_preview(int g)
{
    char err[256];
    prev_ok[g] = 0;
    if (games[g].external_player || !rs[g].ok) return;
    if (launcher_prepare(g, err, sizeof err)) return;
    tex_collect_begin(g);
    emu_preview(games[g].preview_frames);
    if (emu_cpu_faulted()) {
        uint8_t opcode = 0; uint16_t pc = 0; char msg[144];
        emu_cpu_fault_info(&opcode, &pc);
        snprintf(msg, sizeof msg, "Preview stopped at opcode %02X (%04X); verify this hack's required ROM revision.", opcode, pc);
        launcher_toast(msg);
        prev_ok[g] = 0;
        return;
    }
    frame_from_ppu(&prev[g]);
    prev_ok[g] = 1;
    tex_collect_frame(&prev[g]);
    tex_collect_save();
}

static void set_rom(int g, const char *path)
{
    snprintf(settings.g[g].rom_path, sizeof settings.g[g].rom_path, "%s", path);
    rom_check(g);
    if (rs[g].ok) make_preview(g);
    else prev_ok[g] = 0;
}

static void set_hack(int g, const char *path)
{
    GameCfg *c = &settings.g[g];
    if (!rs[g].ok) { launcher_toast("Choose the original ROM first."); return; }
    snprintf(c->hack_path, sizeof c->hack_path, "%s", path);
    RomStatus st;
    if (rom_load(g, c->rom_path, &st)) {
        hack_ok[g] = 0;
        snprintf(hack_msg[g], sizeof hack_msg[g], "%s", st.msg);
        launcher_toast(st.msg);
        return;
    }
    if (path[0] && rom_apply_hack(path, &st)) {
        /* Keep the selected path and its error visible so the user can see
         * why it was rejected and replace it without browsing a second time. */
        hack_ok[g] = 0;
        snprintf(hack_msg[g], sizeof hack_msg[g], "%s", st.msg);
        launcher_toast(st.msg);
        return;
    }
    snprintf(hack_msg[g], sizeof hack_msg[g], "%s", path[0] ? st.msg : "");
    hack_ok[g] = path[0] != 0;
    make_preview(g);
}

static void ensure_background(int g)
{
    if (bg_for_game == g && !strcmp(bg_loaded, settings.g[g].bg_path)) return;
    bg_for_game = g;
    snprintf(bg_loaded, sizeof bg_loaded, "%s", settings.g[g].bg_path);
    bg_msg[0] = 0;
    if (bg_load(bg_loaded) != 0) { if (bg_loaded[0]) snprintf(bg_msg, sizeof bg_msg, "Couldn't open that picture."); settings.g[g].bg_path[0] = 0; bg_loaded[0] = 0; }
}

void launcher_init(void)
{
    clear_state_thumbs();
    for (int g = 0; g < N_GAMES; g++) load_state_request[g] = -1;
    for (int g = 0; g < N_GAMES; g++) rom_check(g);
    char found[N_GAMES][512];
    memset(found, 0, sizeof found);
    rom_scan(found);
    for (int g = 0; g < N_GAMES; g++)
        if (!rs[g].ok && found[g][0]) { snprintf(settings.g[g].rom_path, sizeof settings.g[g].rom_path, "%s", found[g]); rom_check(g); }
    tab = settings.last_tab >= 0 && settings.last_tab < N_TABS ? settings.last_tab : 0;
    launcher_controller_cursor_active = 0;
    launcher_controller_confirm_was_down = launcher_controller_back_was_down = 0;
    launcher_controller_wait_neutral = launcher_ignore_pad_confirm = 0;
    launcher_controller_x = 136.0f;
    launcher_controller_y = 129.0f;
    for (int g = 0; g < N_GAMES; g++) if (!games[g].external_player && rs[g].ok && !prev_ok[g]) make_preview(g);
    filter_game = 0;
    if (getenv("GBL_TAB")) { int t = 0, u = 0; sscanf(getenv("GBL_TAB"), "%d,%d", &t, &u); tab = t; if (t < N_GAMES) sub[t] = u; }
    audio_menu_apply();
}

void launcher_enter(void)
{
    clear_state_thumbs();
    for (int g = 0; g < N_GAMES; g++) rom_check(g);
    for (int g = 0; g < N_GAMES; g++) if (rs[g].ok && !prev_ok[g]) make_preview(g);
    loaded_pack_game = -1;
    bg_for_game = -1;
    audio_menu_apply();
    audio_menu_music(1);
}

void launcher_shutdown(void) { clear_state_thumbs(); tex_collect_save(); }

/* ------------------------------------------------------------------ small drawing helpers */
static SDL_Rect to_px(float x, float y, float w, float h)
{
    SDL_Rect r;
    r.x = (int)floorf((x - ui_view_x0()) * ui_scale + 0.5f);
    r.y = (int)floorf((y - ui_view_y0()) * ui_scale + 0.5f);
    r.w = (int)floorf(w * ui_scale + 0.5f);
    r.h = (int)floorf(h * ui_scale + 0.5f);
    return r;
}

static int clickable(float x, float y, float w, float h, int *over_out)
{
    int id = ui_next_id();
    int over = ui_hover(x, y, w, h) && !ui_active_any();
    if (over && ui_mouse.pressed) click_id = id;
    int clicked = over && ui_mouse.released && click_id == id;
    if (over_out) *over_out = over;
    if (clicked && ui_sfx_cb) ui_sfx_cb(SFX_CLICK);
    return clicked;
}

static void launcher_controller_default_cursor(void)
{
    /* Start on the selected sidebar entry, so the initial position is clear. */
    const float sx = 24.0f, sy = 108.0f;
    const float row = N_GAMES > 4 ? 46.0f : 76.0f;
    const float h = N_GAMES > 4 ? 42.0f : 68.0f;
    launcher_controller_x = sx + 112.0f;
    if (tab < N_GAMES) {
        launcher_controller_y = sy + tab * row + h * 0.5f;
    } else {
        float y2 = sy + N_GAMES * row + 8.0f;
        launcher_controller_y = y2 + 12.0f + (tab - TAB_MULTIPLAYER) * 52.0f + 22.0f;
    }
}

static void launcher_controller_update(float dt)
{
    float ax = 0.0f, ay = 0.0f;
    int confirm = 0, back = 0;
    pad_poll_launcher(&ax, &ay, &confirm, &back);

    /* A button used for binding capture mustn't also activate a UI control. */
    if (launcher_ignore_pad_confirm) {
        if (!confirm) launcher_ignore_pad_confirm = 0;
        confirm = 0;
    }

    int input = fabsf(ax) > 0.001f || fabsf(ay) > 0.001f || confirm || back;
    if (launcher_controller_wait_neutral) {
        if (input) {
            launcher_controller_back_was_down = back;
            return;
        }
        launcher_controller_wait_neutral = 0;
    }

    if (!launcher_controller_cursor_active && input) {
        launcher_controller_cursor_active = 1;
        launcher_controller_default_cursor();
    }
    if (!launcher_controller_cursor_active) {
        launcher_controller_back_was_down = back;
        return;
    }

    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.05f) dt = 0.05f;
    launcher_controller_x += ax * 560.0f * dt;
    launcher_controller_y += ay * 560.0f * dt;
    if (launcher_controller_x < 6.0f) launcher_controller_x = 6.0f;
    if (launcher_controller_x > UI_W - 6.0f) launcher_controller_x = UI_W - 6.0f;
    if (launcher_controller_y < 6.0f) launcher_controller_y = 6.0f;
    if (launcher_controller_y > UI_H - 6.0f) launcher_controller_y = UI_H - 6.0f;

    ui_mouse.x = launcher_controller_x;
    ui_mouse.y = launcher_controller_y;
    ui_mouse.inside = 1;

    int can_click = !cap_kind && !multiplayer_name_editing;
    int ui_confirm = can_click ? confirm : 0;
    if (ui_confirm != launcher_controller_confirm_was_down)
        ui_mouse_button(ui_confirm);
    launcher_controller_confirm_was_down = ui_confirm;

    if (back && !launcher_controller_back_was_down) {
        SDL_Event cancel;
        memset(&cancel, 0, sizeof cancel);
        cancel.type = SDL_KEYDOWN;
        cancel.key.type = SDL_KEYDOWN;
        cancel.key.state = SDL_PRESSED;
        cancel.key.keysym.sym = SDLK_ESCAPE;
        launcher_event(&cancel);
        if (controller_menu >= 0) controller_menu = -1;
        if (multiplayer_count_menu >= 0) multiplayer_count_menu = -1;
    }
    launcher_controller_back_was_down = back;
}

static void launcher_leave_controller_cursor(void)
{
    if (!launcher_controller_cursor_active) return;
    launcher_controller_cursor_active = 0;
    launcher_controller_wait_neutral = 1;
    ui_mouse_button(0);
    /* A virtual release caused by moving the physical mouse isn't a click. */
    ui_mouse.released = 0;
    launcher_controller_confirm_was_down = 0;
}

static uint32_t mixc(uint32_t a, uint32_t b, float t)
{
    return RGBA((int)(((a >> 24) & 255) * (1 - t) + ((b >> 24) & 255) * t), (int)(((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t),
                (int)(((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t), (int)((a & 255) * (1 - t) + (b & 255) * t));
}

static void card(float x, float y, float w, float h, const char *title)
{
    ui_shadow(x, y, w, h, 14, 8, RGBA(0, 0, 0, 50));
    ui_rrect(x, y, w, h, 14, (C_PANEL & 0xFFFFFF00u) | 232);
    if (title) ui_text(F_BOLD, 12, x + 18, y + 14, C_DIM, title);
}

static void label(float x, float y, const char *s) { ui_text(F_REG, 13, x, y, C_MUTED, s); }
static void hint_area(float x, float y, float w, float h, const char *s) { if (ui_hover(x, y, w, h)) ui_hint(s); }

static void path_box(float x, float y, float w, const char *path, const char *placeholder)
{
    ui_rrect(x, y, w, 36, 9, C_BG2);
    if (path[0]) ui_text_fit_tail(F_REG, 13, x + 12, y + 8, w - 24, C_TEXT, path);
    else ui_text_fit(F_REG, 13, x + 12, y + 8, w - 24, C_DIM, placeholder);
}

static void status_line(float x, float y, float w, int kind, const char *msg)
{
    uint32_t c = kind == 1 ? C_OK : (kind == 2 ? C_WARN : (kind == 3 ? C_ERR : C_DIM));
    ui_rrect(x, y + 4, 10, 10, 5, c);
    ui_text_fit(F_REG, 13, x + 18, y, w - 18, kind ? C_TEXT : C_MUTED, msg);
}

static void draw_cover(SDL_Texture *t, int tw, int th, float x, float y, float w, float h)
{
    SDL_Renderer *r = ren_get();
    SDL_Rect dst = to_px(x, y, w, h);
    float s = fmaxf((float)dst.w / tw, (float)dst.h / th);
    SDL_Rect src = {(int)((tw - dst.w / s) * 0.5f), (int)((th - dst.h / s) * 0.5f), (int)(dst.w / s), (int)(dst.h / s)};
    SDL_RenderCopy(r, t, &src, &dst);
}

/* ------------------------------------------------------------------ preview box */
static void draw_preview(int g, float x, float y, float w, float h, int use_cfg_shape)
{
    ui_rrect(x, y, w, h, 12, HEX(0x07080C));
    if (!prev_ok[g]) {
        ui_text_c(F_BOLD, 14, x + w * 0.5f, y + h * 0.5f - 18, C_MUTED, "No picture yet");
        ui_text_c(F_REG, 12, x + w * 0.5f, y + h * 0.5f + 4, C_DIM, rs[g].ok ? "Press Play to start" : "Add the ROM to see the game here");
        return;
    }
    ensure_pack(g);
    render_build(&prev[g], g, 0);
    const GameCfg *c = &settings.g[g];
    float ar = render_width() / 144.0f;
    if (use_cfg_shape) ar = c->aspect == ASPECT_4_3 ? 4.0f / 3.0f : (c->aspect == ASPECT_16_9 ? 16.0f / 9.0f : ar);
    float pw = w - 16, ph = pw / ar;
    if (c->scaling == SCALE_STRETCH && use_cfg_shape) { pw = w - 16; ph = h - 16; }
    if (ph > h - 16) { ph = h - 16; pw = ph * ar; }
    SDL_Rect r = to_px(x + (w - pw) * 0.5f, y + (h - ph) * 0.5f, pw, ph);
    render_draw(&r, c->scaling == SCALE_PIXEL ? SCALE_PIXEL : SCALE_SMOOTH);
}

/* External Wario entries use the ROM-independent gbrecomp player for now. */
static void sub_external_game(int g, float x, float y)
{
    GameCfg *c = &settings.g[g];
    card(x, y, 808, 476, "GAME ROM");
    label(x + 18, y + 42, "Game Boy / Game Boy Color ROM");
    path_box(x + 18, y + 64, 808 - 36 - 110, c->rom_path, "Drop a ROM here or browse");
    if (ui_button(x + 808 - 18 - 100, y + 64, 100, 36, "Browse…", B_NORMAL, 1)) {
        char p[1100];
        if (dlg_pick(DLG_ROM, "Choose this game's ROM", p, sizeof p)) {
            int id = rom_identify_file(p);
            if (id >= 0 && id != g) {
                snprintf(settings.g[id].rom_path, sizeof settings.g[id].rom_path, "%s", p);
                rom_check(id);
                tab = id;
                launcher_toast("That ROM belongs to a different game. Switched tabs.");
            } else {
                set_rom(g, p);
            }
        }
    }
    status_line(x + 18, y + 108, 772, rs[g].ok ? 1 : (c->rom_path[0] ? 3 : 2),
                rs[g].ok ? "ROM detected and ready to launch." : (c->rom_path[0] ? rs[g].msg : "Choose a ROM to enable Play."));

    ui_text(F_BOLD, 12, x + 18, y + 150, C_MUTED, "PLAYER");
    ui_text_wrap(F_REG, 12, x + 18, y + 174, 772, C_DIM,
                 "These four Wario Land entries currently run in the separate gbrecomp compatibility player while we work through game-specific issues. PipeClean's rendering filters, save-state slots and per-game controller bindings do not apply to that separate player window yet.", 4);

    ui_text(F_BOLD, 12, x + 18, y + 276, C_MUTED, "CONTROLS");
    ui_text_wrap(F_REG, 12, x + 18, y + 300, 772, C_TEXT,
                 "Keyboard: arrows or WASD to move, Z/J for A, X/K for B, Enter/Space for Start, Backspace/Tab/Right Shift for Select. Controller: D-pad or left stick, A/Y for Game Boy A, B/X for B, Start and Back.", 3);
    ui_text_wrap(F_REG, 12, x + 18, y + 382, 772, C_DIM,
                 "Battery saves are written alongside the selected ROM as a .sav file. Keep each ROM and its save file together.", 2);
}

/* ------------------------------------------------------------------ game tab: Game */
static void sub_game(int g, float x, float y)
{
    GameCfg *c = &settings.g[g];
    card(x, y, 360, 476, "PREVIEW");
    draw_preview(g, x + 16, y + 40, 328, 300, 0);
    ui_text_c(F_REG, 12, x + 180, y + 352, C_DIM, "A short run of the game from power-on");
    if (ui_button(x + 16, y + 386, 328, 38, "Refresh preview", B_NORMAL, rs[g].ok)) make_preview(g);
    ui_text_wrap(F_REG, 12, x + 18, y + 434, 324, C_DIM, "Tip: drag and drop a ROM, patch or picture onto this window.", 2);

    float rx = x + 372, rw = 436;
    card(rx, y, rw, 476, "FILES");
    /* ROM */
    label(rx + 18, y + 42, "Game ROM");
    path_box(rx + 18, y + 64, rw - 18 * 2 - 110, c->rom_path, "Not chosen yet");
    if (ui_button(rx + rw - 18 - 100, y + 64, 100, 36, "Browse…", B_NORMAL, 1)) {
        char p[1100];
        if (dlg_pick(DLG_ROM, "Choose your ROM", p, sizeof p)) {
            int id = rom_identify_file(p);
            if (id >= 0 && id != g) { snprintf(settings.g[id].rom_path, sizeof settings.g[id].rom_path, "%s", p); rom_check(id); make_preview(id); tab = id; launcher_toast("That ROM belongs to another game. Switched tabs."); }
            else set_rom(g, p);
        }
    }
    status_line(rx + 18, y + 108, rw - 36, rs[g].ok ? 1 : (c->rom_path[0] ? 3 : 2), rs[g].msg);
    /* hack */
    label(rx + 18, y + 150, "Romhack or patch (optional)");
    path_box(rx + 18, y + 172, rw - 18 * 2 - 110 - 66, c->hack_path, "None");
    if (ui_button(rx + rw - 18 - 100 - 60, y + 172, 100, 36, "Browse…", B_NORMAL, rs[g].ok)) {
        char p[1100];
        if (dlg_pick(DLG_PATCH, "Choose a romhack or patch", p, sizeof p)) set_hack(g, p);
    }
    if (ui_button(rx + rw - 18 - 52, y + 172, 52, 36, "Clear", B_GHOST, c->hack_path[0] != 0)) set_hack(g, "");
    if (hack_msg[g][0] && c->hack_path[0]) status_line(rx + 18, y + 216, rw - 36, hack_ok[g] ? 1 : 3, hack_msg[g]);
    else ui_text(F_REG, 12, rx + 18, y + 216, C_DIM, "Accepts .ips  .bps  .ups patches, or an already patched .gb");
    ui_text_wrap(F_REG, 12, rx + 18, y + 244, rw - 36, C_DIM,
                 games[g].lifted ? "Dr. Mario runs as native code. If a hack changes the game's program (not just its graphics or data) it automatically switches to compatibility mode."
                                  : "This game runs through the built-in CPU core with full cartridge bank switching, so romhacks work without any extra step.", 3);
    /* save data */
    label(rx + 18, y + 330, "Save data");
    char sp[1200];
    int bat = games[g].crc == 0 && (g == GAME_SML2 || g >= GAME_WARIO_SML3);
    snprintf(sp, sizeof sp, "%ssaves/", settings_dir());
    ui_text_wrap(F_REG, 12, rx + 18, y + 352, rw - 36, C_DIM, bat ? "Saved games are kept automatically next to the program, in the \"saves\" folder, one file per game." : "This cartridge has no save memory.", 3);
    if (bat && ui_button(rx + 18, y + 396, 180, 36, "Open saves folder", B_NORMAL, 1)) { mkdir_u(sp); open_folder(sp); }
}

/* ------------------------------------------------------------------ game tab: Display */
static void sub_display(int g, float x, float y)
{
    GameCfg *c = &settings.g[g];
    card(x, y, 536, 476, "GAME BOY COLORS");
    for (int i = 0; i < n_palettes; i++) {
        float cx = x + 16 + (i % 4) * 129, cy = y + 40 + (i / 4) * 52;
        int over;
        int clicked = clickable(cx, cy, 121, 44, &over);
        int sel = c->palette == i;
        ui_rrect(cx, cy, 121, 44, 10, sel ? mixc(C_PANEL2, HEX(ui_accent), 0.25f) : (over ? C_BTN_H : C_BTN));
        if (sel) ui_stroke(cx, cy, 121, 44, 10, 2, HEX(ui_accent));
        const Palette *p = &palettes[i];
        for (int k = 0; k < 4; k++) {
            ui_rrect(cx + 8 + k * 9, cy + 6, 9, 16, 2, HEX(p->bg[k]));
            if (p->multi) { ui_rrect(cx + 8 + k * 9, cy + 22, 9, 8, 2, HEX(p->ob0[k])); }
        }
        ui_text_fit(F_BOLD, 11, cx + 48, cy + 6, 66, sel ? C_TEXT : C_MUTED, p->name);
        if (p->multi) ui_text(F_REG, 10, cx + 48, cy + 24, C_DIM, "sprites differ");
        if (clicked) c->palette = i;
        if (over) ui_hint(p->name);
    }
    float yy = y + 40 + 4 * 52 + 14;
    label(x + 18, yy, "Window shape"); ui_seg(x + 18, yy + 22, 500, 36, aspect_names, N_ASPECT, &c->aspect);
    yy += 66;
    label(x + 18, yy, "Scaling"); ui_seg(x + 18, yy + 22, 500, 36, scale_names, N_SCALE, &c->scaling);
    hint_area(x + 18, yy, 500, 60, "Pixel-perfect keeps every pixel crisp. Smooth blends. Stretch fills the whole window.");
    yy += 66;
    label(x + 18, yy, "Starting window size"); ui_seg(x + 18, yy + 22, 500, 36, size_names, N_SIZE, &c->size);

    float rx = x + 548, rw = 260;
    card(rx, y, rw, 476, "LOOK");
    draw_preview(g, rx + 16, y + 40, rw - 32, 190, 1);
    label(rx + 18, y + 246, "Background picture");
    path_box(rx + 18, y + 268, rw - 36, c->bg_path, "None (solid colour)");
    if (ui_button(rx + 18, y + 312, 110, 34, "Choose…", B_NORMAL, 1)) {
        char p[1100];
        if (dlg_pick(DLG_IMAGE, "Choose a background picture", p, sizeof p)) { snprintf(c->bg_path, sizeof c->bg_path, "%s", p); ensure_background(g); if (bg_msg[0]) launcher_toast(bg_msg); }
    }
    if (ui_button(rx + 136, y + 312, 106, 34, "Remove", B_GHOST, c->bg_path[0] != 0)) { c->bg_path[0] = 0; ensure_background(g); }
    char t[64];
    if (bg_frames() > 1) snprintf(t, sizeof t, "Animated, %d frames", bg_frames());
    else snprintf(t, sizeof t, "PNG, JPG, BMP, TGA or animated GIF");
    ui_text_fit(F_REG, 11, rx + 18, y + 354, rw - 36, C_DIM, t);
    label(rx + 18, y + 380, "Darken background"); snprintf(t, sizeof t, "%d%%", c->bg_dim); ui_text_r(F_REG, 13, rx + rw - 18, y + 380, C_TEXT, t);
    ui_slider(rx + 22, y + 402, rw - 44, &c->bg_dim, 0, 80);
    if (games[g].wide_l + games[g].wide_r > 0) {
        label(rx + 18, y + 428, "Widescreen");
        snprintf(t, sizeof t, "%d%%", c->wide); ui_text_r(F_REG, 13, rx + rw - 18, y + 428, C_TEXT, t);
        if (ui_slider(rx + 22, y + 450, rw - 44, &c->wide, 0, 100)) wide_dirty = g + 1;
        hint_area(rx + 18, y + 424, rw - 36, 40, "Shows more of the level on the sides. 0% is the original 10:9 screen.");
        if (wide_note[g]) ui_text_fit(F_REG, 11, rx + 18, y + 468 - 6, rw - 36, C_WARN, "This ROM version isn't supported for widescreen.");
    } else {
        label(rx + 18, y + 428, "Widescreen");
        ui_text(F_REG, 12, rx + 18, y + 450, C_DIM, "Not available for this game");
    }
}

/* ------------------------------------------------------------------ game tab: Controls */
static void bind_cell(int g, float x, float y, float w, int kind, int btn, int slot)
{
    GameCfg *c = &settings.g[g];
    int pad_device = slot >= 0 && slot < MAX_MP_PLAYERS ? c->pad_device[slot] : -1;
    int available = kind != 2 || (pad_device >= 0 && pad_device < pad_count());
    int over = 0;
    int clicked = available ? clickable(x, y, w, 28, &over) : 0;
    int active = cap_kind == kind && cap_btn == btn && cap_slot == slot;
    uint32_t fill = !available ? RGBA(255,255,255,3) :
                    (active ? mixc(C_BTN, HEX(ui_accent), 0.5f) : (over ? C_BTN_H : C_BTN));
    ui_rrect(x, y, w, 28, 7, fill);
    char b[48];
    if (!available) {
        snprintf(b, sizeof b, "—");
        ui_text_c(F_REG, 13, x + w / 2, y + 5, C_DIM, b);
    } else if (active) ui_text_c(F_BOLD, 12, x + w / 2, y + 5, HEX(0xFFFFFF), kind == 1 ? "press a key…" : "press a button…");
    else {
        if (kind == 1) key_code_name(c->key[btn][slot], b, sizeof b); else pad_code_name_device(pad_device, c->pad[btn][slot], b, sizeof b);
        int none = kind == 1 ? !c->key[btn][slot] : c->pad[btn][slot] < 0;
        ui_text_c(F_REG, 13, x + w / 2, y + 5, none ? C_DIM : C_TEXT, b);
    }
    if (clicked) { cap_kind = kind; cap_btn = btn; cap_slot = slot; }
    if (over && !active && available) ui_hint("Click, then press the key or button you want. Backspace clears, Esc cancels.");
    else if (!available && ui_hover(x, y, w, 28)) ui_hint("Assign a controller to this player to enable its button bindings.");
}

static char *mp_name_ptr(GameCfg *c, int player_number)
{
    if (!c) return NULL;
    if (player_number == 2) return c->p2_name;
    if (player_number == 3) return c->p3_name;
    if (player_number == 4) return c->p4_name;
    return NULL;
}

static const char *mp_default_name(int player_number)
{
    if (player_number == 2) return "Luigi";
    if (player_number == 3) return "Bunzo";
    if (player_number == 4) return "Florbo";
    return "Player";
}

static int mp_color_index(const GameCfg *c, int player_number)
{
    if (!c) return LUIGI_GREEN;
    int color = player_number == 2 ? c->p2_color :
                (player_number == 3 ? c->p3_color : c->p4_color);
    if (color < 0 || color >= N_LUIGI_COLORS)
        return player_number == 2 ? LUIGI_GREEN : (player_number == 3 ? LUIGI_BLUE : LUIGI_YELLOW);
    return color;
}

static int *mp_respawn_key_ptr(GameCfg *c, int player_slot)
{
    if (!c) return NULL;
    if (player_slot == 1) return &c->p2_respawn_key;
    if (player_slot == 2) return &c->p3_respawn_key;
    if (player_slot == 3) return &c->p4_respawn_key;
    return NULL;
}

static int *mp_respawn_pad_ptr(GameCfg *c, int player_slot)
{
    if (!c) return NULL;
    if (player_slot == 1) return &c->p2_respawn_pad;
    if (player_slot == 2) return &c->p3_respawn_pad;
    if (player_slot == 3) return &c->p4_respawn_pad;
    return NULL;
}

static char *mp_sfx_path_ptr(GameCfg *c, int player_number, int event)
{
    if (!c || event < 0 || event >= N_P2_SFX) return NULL;
    if (player_number == 2) return c->p2_sfx_path[event];
    if (player_number == 3) return c->p3_sfx_path[event];
    if (player_number == 4) return c->p4_sfx_path[event];
    return NULL;
}

static void bind_mp_respawn_cell(int g, int player_slot, float x, float y, float w, int kind)
{
    GameCfg *c = &settings.g[g];
    int *key = mp_respawn_key_ptr(c, player_slot);
    int *pad = mp_respawn_pad_ptr(c, player_slot);
    int device = c->pad_device[player_slot];
    int available = kind != 6 || (device >= 0 && device < pad_count());
    int over = 0;
    int clicked = available ? clickable(x, y, w, 28, &over) : 0;
    int active = cap_kind == kind && cap_btn == player_slot;
    ui_rrect(x, y, w, 28, 7, !available ? RGBA(255,255,255,3) :
             (active ? mixc(C_BTN, HEX(ui_accent), 0.5f) : (over ? C_BTN_H : C_BTN)));
    char value[48];
    if (!available) snprintf(value, sizeof value, "—");
    else if (active) snprintf(value, sizeof value, kind == 5 ? "press a key…" : "press a button…");
    else if (kind == 5) key_code_name(key ? *key : 0, value, sizeof value);
    else pad_code_name_device(device, pad ? *pad : -1, value, sizeof value);
    int none = kind == 5 ? (!key || !*key) : (!pad || *pad < 0);
    ui_text_c(F_REG, 12, x + w / 2, y + 5,
              (!available || (none && !active)) ? C_DIM : C_TEXT, value);
    if (clicked) { cap_kind = kind; cap_btn = player_slot; cap_slot = player_slot; }
    if (over && available && !active)
        ui_hint(kind == 5 ? "Press a keyboard key for this player's respawn. Backspace clears, Esc cancels."
                          : "Press a button on this player's controller. Backspace clears, Esc cancels.");
    else if (!available && ui_hover(x, y, w, 28))
        ui_hint("Assign this player a controller to bind the controller respawn button.");
}

static void bind_p2_respawn_cell(int g, float x, float y, float w, int kind)
{
    bind_mp_respawn_cell(g, 1, x, y, w, kind);
}

static void bind_action_cell(int g, float x, float y, float w, int kind, int action)
{
    GameCfg *c = &settings.g[g];
    int pad_device = c->pad_device[0];
    int available = kind != 4 || (pad_device >= 0 && pad_device < pad_count());
    int over = 0;
    int clicked = available ? clickable(x, y, w, 32, &over) : 0;
    int active = (cap_kind == kind && cap_btn == action);
    uint32_t fill = !available ? RGBA(255,255,255,3) :
                    (active ? mixc(C_BTN, HEX(ui_accent), 0.5f) : (over ? C_BTN_H : C_BTN));
    ui_rrect(x, y, w, 32, 8, fill);
    char b[64];
    if (!available) {
        snprintf(b, sizeof b, "—");
        ui_text_c(F_REG, 12, x + w / 2, y + 7, C_DIM, b);
    } else if (active) ui_text_c(F_BOLD, 12, x + w / 2, y + 7, HEX(0xFFFFFF), kind == 3 ? "press a key…" : "press a button…");
    else {
        if (kind == 3) key_code_name(c->action_key[action], b, sizeof b);
        else pad_code_name_device(pad_device, c->action_pad[action], b, sizeof b);
        int none = kind == 3 ? !c->action_key[action] : c->action_pad[action] < 0;
        ui_text_c(F_REG, 12, x + w / 2, y + 7, none ? C_DIM : C_TEXT, b);
    }
    if (clicked) { cap_kind = kind; cap_btn = action; cap_slot = 0; }
    if (over && !active && available) ui_hint("Click, then press the key or controller button. Backspace clears, Esc cancels.");
    else if (!available && ui_hover(x, y, w, 32)) ui_hint("Assign a controller to Player 1 to enable controller shortcuts.");
}

static void sub_save_states(int g, float x, float y)
{
    GameCfg *c = &settings.g[g];
    char path[1200];
    char text[96];

    card(x, y, 808, 476, "SAVE STATES");
    ui_text(F_REG, 13, x + 18, y + 40, C_DIM,
            "Choose a slot here. Saving uses the Save State binding while you are playing; Load can start a game from any saved slot.");

    label(x + 18, y + 70, "Current slot");
    ui_slider(x + 118, y + 68, 300, &c->state_slot, 0, 9);
    snprintf(text, sizeof text, "Slot %d", c->state_slot + 1);
    ui_text_r(F_BOLD, 14, x + 432, y + 70, C_TEXT, text);

    ui_text(F_BOLD, 12, x + 18, y + 100, C_MUTED, "SAVE STATES");
    ui_text(F_BOLD, 12, x + 286, y + 100, C_MUTED, "LOAD STATES");

    for (int slot = 0; slot < 10; slot++) {
        int col = slot < 5 ? 0 : 1;
        int row = slot < 5 ? slot : slot - 5;
        float bx = x + 18 + col * 386;
        float by = y + 112 + row * 64;

        snprintf(path, sizeof path, "%sstates/%s.slot%d.pcs", settings_dir(), games[g].id, slot);
        int saved = file_exists(path);
        int selected = slot == c->state_slot;
        int over = 0;
        int clicked = clickable(bx, by, 368, 56, &over);
        ui_rrect(bx, by, 368, 56, 9, selected ? RGBA(255,255,255,14) : (over ? C_BTN_H : RGBA(255,255,255,6)));
        if (selected) ui_stroke(bx, by, 368, 56, 9, 2, HEX(ui_accent));

        SDL_Texture *thumb = saved ? state_thumb_get(g, slot) : NULL;
        if (thumb) ui_image(thumb, NULL, bx + 8, by + 7, 88, 42);
        else {
            ui_rrect(bx + 8, by + 7, 88, 42, 6, RGBA(8,10,15,210));
            ui_text_c(F_BOLD, 10, bx + 52, by + 18, C_DIM, "NO");
            ui_text_c(F_REG, 9, bx + 52, by + 29, C_DIM, "THUMBNAIL");
        }

        snprintf(text, sizeof text, "Slot %d", slot + 1);
        ui_text(F_BOLD, 12, bx + 106, by + 8, selected ? HEX(ui_accent) : C_TEXT, text);
        ui_text(F_REG, 10, bx + 106, by + 26, saved ? C_OK : C_DIM, saved ? "Saved state" : "Empty");

        if (clicked) c->state_slot = slot;
        if (ui_button(bx + 278, by + 11, 78, 34, "Load", saved ? B_PRIMARY : B_GHOST, saved)) {
            c->state_slot = slot;
            load_state_request[g] = slot;
        }
    }

    snprintf(path, sizeof path, "%sstates/%s.suspend.pcs", settings_dir(), games[g].id);
    int suspended = file_exists(path);
    ui_text(F_BOLD, 11, x + 18, y + 443, C_MUTED, "Suspend");
    ui_text(F_REG, 11, x + 78, y + 443, suspended ? C_OK : C_DIM,
            suspended ? "A suspended game is available and will resume automatically when no saved-state load is requested." : "No suspended game");
}
static void sub_bindings(int g, float x, float y)
{
    GameCfg *c = &settings.g[g];
    card(x, y, 808, 450, "SAVE STATE BINDINGS");

    ui_text_wrap(F_REG, 12, x + 18, y + 40, 772, C_DIM,
                 "These bindings are only for PipeClean's save-state controls. Game Boy button and controller bindings stay on the Controllers tab.", 2);

    ui_text(F_BOLD, 11, x + 18, y + 82, C_MUTED, "ACTION");
    ui_text(F_BOLD, 11, x + 288, y + 82, C_MUTED, "KEYBOARD");
    ui_text(F_BOLD, 11, x + 510, y + 82, C_MUTED, "CONTROLLER");

    for (int a = 0; a < N_ACTION; a++) {
        float ry = y + 98 + a * 52;
        if (a % 2 == 0) ui_rrect(x + 10, ry - 4, 788, 46, 8, RGBA(255,255,255,6));
        ui_text(F_BOLD, 13, x + 18, ry + 7, C_TEXT, action_names[a]);
        bind_action_cell(g, x + 270, ry + 3, 190, 3, a);
        bind_action_cell(g, x + 490, ry + 3, 190, 4, a);
    }

    ui_text_wrap(F_REG, 11, x + 18, y + 350, 772, C_DIM,
                 "Save State stores the selected slot, Load State restores it, Rewind moves backward through recent history, Suspend saves and returns to the launcher, and Next State Slot changes the active slot.", 3);

    label(x + 18, y + 394, "Current slot");
    ui_text(F_BOLD, 13, x + 118, y + 394, HEX(ui_accent), "Change slots on Save States");

    if (ui_button(x + 18, y + 410, 180, 36, "Reset state bindings", B_NORMAL, 1)) {
        shortcut_defaults(c);
        launcher_toast("Save-state bindings reset.");
    }
}
static void sub_controls(int g, float x, float y)
{
    GameCfg *c = &settings.g[g];
    card(x, y, 808, 476, "PLAYER CONTROLLERS");

    float my = y + 76;
    ui_text_fit(F_REG, 11, x + 18, my, 770, C_DIM,
                "Local multiplayer settings, Luigi's name/color, and the rescue binding are on the Multiplayer tab.");

    static const float colx[5] = {18, 150, 310, 470, 630};
    ui_text(F_BOLD, 12, x + colx[0], y + 112, C_MUTED, "Game Boy button");
    ui_text(F_BOLD, 12, x + colx[1] + 10, y + 112, C_MUTED, "Keyboard");
    ui_text(F_BOLD, 12, x + colx[2] + 10, y + 112, C_MUTED, "Keyboard 2");
    ui_text(F_BOLD, 12, x + colx[3] + 10, y + 112, C_MUTED, "Player 1 controller");
    ui_text(F_BOLD, 12, x + colx[4] + 10, y + 112, C_MUTED, "Player 2 controller");
    for (int b = 0; b < N_BTN; b++) {
        float ry = y + 128 + b * 28;
        if (b % 2 == 0) ui_rrect(x + 10, ry - 2, 788, 28, 7, RGBA(255, 255, 255, 6));
        ui_text(F_BOLD, 14, x + colx[0], ry + 2, C_TEXT, btn_names[b]);
        bind_cell(g, x + colx[1], ry, 150, 1, b, 0);
        bind_cell(g, x + colx[2], ry, 150, 1, b, 1);
        bind_cell(g, x + colx[3], ry, 150, 2, b, 0);
        bind_cell(g, x + colx[4], ry, 150, 2, b, 1);
    }
    float by = y + 128 + N_BTN * 28 + 4;
    if (ui_button(x + 18, by, 170, 30, "Reset to defaults", B_NORMAL, 1)) { controls_defaults(c); controller_menu = -1; launcher_toast("Controller bindings reset."); }
    label(x + 220, by + 8, "Stick dead zone");
    ui_slider(x + 330, by + 7, 220, &settings.pad_deadzone, 5, 80);
    char t[16]; snprintf(t, sizeof t, "%d%%", settings.pad_deadzone); ui_text(F_REG, 13, x + 566, by + 8, C_TEXT, t);

    char st[128];
    pad_status(st, sizeof st);
    ui_text_fit(F_REG, 11, x + 18, by + 48, 770, C_DIM, st);
    ui_text_fit(F_REG, 11, x + 18, by + 66, 770, C_DIM, "Player 2's buttons use the Keyboard 2 and Player 2 controller columns.");

    /*
     * Draw the controller menus last so their popups sit above the binding
     * grid and cannot be visually covered by the controls underneath.
     */
    controller_dropdown(g, 0, x + 18, y + 34, 374);
    controller_dropdown(g, 1, x + 414, y + 34, 374);
}

/* ------------------------------------------------------------------ dedicated local multiplayer tab */
static void mp_name_field(GameCfg *c, int player_number, float x, float y, float w)
{
    char *name = mp_name_ptr(c, player_number);
    if (!name) return;
    int over = 0;
    int clicked = clickable(x, y, w, 32, &over);
    int active = multiplayer_name_editing == player_number;
    int ci = mp_color_index(c, player_number);
    ui_rrect(x, y, w, 32, 8, active ? mixc(C_BTN, HEX(ui_accent), 0.38f) :
             (over ? C_BTN_H : C_BTN));
    const char *current = name[0] ? name : (active ? "Type a name…" : "Click to rename");
    ui_text_fit(F_REG, 13, x + 12, y + 8, w - 42, name[0] ? C_TEXT : C_DIM, current);
    ui_rrect(x + w - 25, y + 6, 14, 20, 5, HEX(luigi_colors[ci].swatch));
    if (active) ui_hint("Type the name. Enter/Escape finishes; Backspace deletes.");
    else if (over) {
        char hint[100];
        snprintf(hint, sizeof hint, "Click to rename Player %d, then type.", player_number);
        ui_hint(hint);
    }
    if (clicked) {
        cap_kind = 0;
        multiplayer_name_editing = player_number;
        name[0] = 0;
        SDL_StartTextInput();
    }
}


static void mp_game_target_button(float x, float y, float w, float h)
{
    const char *text = mp_config_game == GAME_SML ? "Game Target: SML1" : "Game Target: SML2";
    if (ui_button(x, y, w, h, text, B_NORMAL, 1)) {
        mp_config_game = mp_config_game == GAME_SML ? GAME_SML2 : GAME_SML;
        multiplayer_count_menu = -1;
        audio_mp_set_game(mp_config_game);
        settings_save();
    }
}

static void tab_multiplayer(float x, float y)
{
    GameCfg *c = &settings.g[mp_config_game];
    card(x, y, 808, 512, mp_config_game == GAME_SML ?
         "SUPER MARIO LAND 1 — MULTIPLAYER" : "SUPER MARIO LAND 2 — MULTIPLAYER");

    label(x + 18, y + 42, "Local multiplayer");
    if (ui_toggle(x + 178, y + 38, &c->multiplayer) && c->multiplayer)
        launcher_toast(mp_config_game == GAME_SML ? "Local multiplayer enabled for SML1." :
                                                   "Local multiplayer enabled for SML2.");
    ui_text_fit(F_REG, 12, x + 238, y + 43, 246, C_DIM,
                c->multiplayer ? "Shared-world local co-op." :
                                 "Off by default.");
    mp_game_target_button(x + 520, y + 38, 248, 32);

    label(x + 18, y + 86, "Players");
    int over = 0;
    int opened = clickable(x + 540, y + 74, 228, 34, &over);
    char count_text[64];
    snprintf(count_text, sizeof count_text, "%d Players", c->multiplayer_players);
    ui_rrect(x + 540, y + 74, 228, 34, 8,
             (over || multiplayer_count_menu >= 0) ? C_BTN_H : C_BTN);
    ui_text_fit(F_REG, 13, x + 552, y + 83, 188, C_TEXT, count_text);
    ui_tri(x + 746, y + 87, x + 758, y + 87, x + 752, y + 94, C_MUTED);
    if (opened) multiplayer_count_menu = multiplayer_count_menu < 0 ? 1 : -1;
    ui_text_fit(F_REG, 11, x + 18, y + 78, 460, C_DIM,
                "Choose 2–4 independent character simulations.");

    label(x + 18, y + 120, "Player names");
    for (int i = 0; i < 3; i++) {
        int p = i + 2;
        float ry = y + 140 + i * 40.0f;
        char row_label[24];
        snprintf(row_label, sizeof row_label, "Player %d", p);
        ui_text(F_BOLD, 12, x + 18, ry + 8, C_MUTED, row_label);
        mp_name_field(c, p, x + 100, ry, 350);
        const char *role = p == 2 ? "Luigi" : (p == 3 ? "Blue • Bunzo" : "Yellow • Florbo");
        ui_text_fit(F_REG, 11, x + 464, ry + 8, 300, C_DIM, role);
    }

    if (multiplayer_count_menu >= 0) {
        const int options[3] = {2, 3, 4};
        float mx = x + 540, my = y + 110;
        ui_shadow(mx, my, 228, 3 * 32 + 8, 9, 7, RGBA(0,0,0,100));
        ui_rrect(mx, my, 228, 3 * 32 + 8, 9, C_BG2);
        for (int i = 0; i < 3; i++) {
            float oy = my + 4 + i * 32;
            int ov = 0;
            int pick = clickable(mx + 4, oy, 220, 30, &ov);
            int selected = c->multiplayer_players == options[i];
            ui_rrect(mx + 4, oy, 220, 30, 6,
                     selected ? mixc(C_BTN, HEX(ui_accent), 0.5f) : (ov ? C_BTN_H : C_BTN));
            char label_text[32];
            snprintf(label_text, sizeof label_text, "%d Players", options[i]);
            ui_text_fit(F_REG, 13, mx + 14, oy + 7, 198, selected ? HEX(0xFFFFFF) : C_TEXT, label_text);
            if (pick) {
                c->multiplayer_players = options[i];
                multiplayer_count_menu = -1;
                settings_save();
                launcher_toast("Multiplayer player count saved.");
            }
        }
    }

    ui_rect(x + 18, y + 270, 772, 1, C_LINE);
    label(x + 18, y + 281, "Player 2 color");
    ui_text_fit(F_REG, 11, x + 148, y + 282, 630, C_DIM,
                "P3 Bunzo starts blue; P4 Florbo starts yellow. Rename them above.");
    for (int i = 0; i < N_LUIGI_COLORS; i++) {
        float bx = x + 18 + i * 128.0f, by = y + 304;
        int hov = 0;
        int clicked = clickable(bx, by, 120, 38, &hov);
        int selected = c->p2_color == i;
        ui_rrect(bx, by, 120, 38, 8, selected ? RGBA(255,255,255,18) : (hov ? C_BTN_H : C_BTN));
        ui_rrect(bx + 5, by + 5, 18, 28, 6, HEX(luigi_colors[i].swatch));
        if (selected) ui_stroke(bx, by, 120, 38, 8, 2, HEX(luigi_colors[i].swatch));
        ui_text_fit(F_BOLD, 12, bx + 29, by + 11, 86, selected ? HEX(0xFFFFFF) : C_TEXT, luigi_colors[i].name);
        if (clicked) c->p2_color = i;
    }
    ui_rect(x + 18, y + 356, 772, 1, C_LINE);
    ui_text_wrap(F_REG, 12, x + 18, y + 370, 772, C_DIM,
                 "Use Multiplayer Controllers for independent controller assignments, respawn bindings, and six customizable sound events per character.",
                 3);
    ui_text_fit(F_REG, 11, x + 18, y + 446, 770, C_DIM,
                "Defaults: Luigi (green), Bunzo (blue), and Florbo (yellow).");
}
 
/* ------------------------------------------------------------------ game tab: DualSense */
static void tab_mp_controllers(float x, float y)
{
    GameCfg *c = &settings.g[mp_config_game];
    card(x, y, 808, 512, mp_config_game == GAME_SML ?
         "SML1 MULTIPLAYER CONTROLLERS" : "SML2 MULTIPLAYER CONTROLLERS");
    mp_game_target_button(x + 592, y + 7, 196, 28);

    for (int player = 0; player < MAX_MP_PLAYERS; player++) {
        float bx = x + 18 + player * 192.0f;
        char title[24];
        snprintf(title, sizeof title, "Player %d", player + 1);
        label(bx, y + 38, title);
        controller_dropdown(mp_config_game, player, bx, y + 56, 180);
    }

    if (ui_button(x + 18, y + 100, 180, 30, "Control bindings",
                  mp_controller_view == 0 ? B_PRIMARY : B_NORMAL, 1)) mp_controller_view = 0;
    if (ui_button(x + 206, y + 100, 180, 30, "Character sounds",
                  mp_controller_view == 1 ? B_PRIMARY : B_NORMAL, 1)) mp_controller_view = 1;

    if (mp_controller_view == 0) {
        label(x + 405, y + 108, "Keyboard + assigned controller");
        static const char *control_labels[N_BTN] = {
            "Jump/A", "B", "Select", "Start", "Right", "Left", "Up", "Down"
        };

        for (int player = 0; player < MAX_MP_PLAYERS; player++) {
            float bx = x + 18 + player * 193.0f;
            char title[64];
            if (player == 0) snprintf(title, sizeof title, "P1  Mario");
            else {
                char *name = mp_name_ptr(c, player + 1);
                snprintf(title, sizeof title, "P%d  %.14s", player + 1,
                         name && name[0] ? name : mp_default_name(player + 1));
            }
            uint32_t player_color = player == 0 ? C_TEXT :
                HEX(luigi_colors[mp_color_index(c, player + 1)].swatch);
            ui_text_fit(F_BOLD, 12, bx, y + 138, 183, player_color, title);
            ui_text_c(F_BOLD, 9, bx + 70, y + 157, C_MUTED, "KEY");
            ui_text_c(F_BOLD, 9, bx + 145, y + 157, C_MUTED, "PAD");

            for (int btn = 0; btn < N_BTN; btn++) {
                float ry = y + 168 + btn * 29.0f;
                ui_text_fit(F_REG, 10, bx, ry + 8, 34, C_TEXT, control_labels[btn]);
                bind_cell(mp_config_game, bx + 36, ry, 69, 1, btn, player);
                bind_cell(mp_config_game, bx + 109, ry, 74, 2, btn, player);
            }
        }

        ui_rect(x + 18, y + 405, 772, 1, C_LINE);
        label(x + 18, y + 411, "Respawn bindings");
        ui_text_fit(F_REG, 10, x + 176, y + 412, 600, C_DIM,
                    "Keyboard and assigned-controller button for each extra character.");
        for (int player = 1; player < MAX_MP_PLAYERS; player++) {
            float ry = y + 432 + (player - 1) * 26.0f;
            char title[18];
            snprintf(title, sizeof title, "P%d respawn", player + 1);
            ui_text_fit(F_REG, 10, x + 18, ry + 7, 72, C_TEXT, title);
            bind_mp_respawn_cell(mp_config_game, player, x + 92, ry, 150, 5);
            bind_mp_respawn_cell(mp_config_game, player, x + 250, ry, 168, 6);
        }
    } else {
        label(x + 18, y + 142, "Independent character sound banks");
        ui_text_fit(F_REG, 10, x + 236, y + 143, 545, C_DIM,
                    "Test, import, or reset sounds per player; empty paths use built-in chiptunes.");

        static const char *sound_names[N_P2_SFX] = {
            "Jump", "Fireball", "Power up", "Power down", "Death", "Respawn"
        };
        static const int players[3] = {2, 3, 4};
        for (int col = 0; col < 3; col++) {
            int player = players[col];
            int ci = mp_color_index(c, player);
            float bx = x + 18 + col * 258.0f;
            float cw = 250.0f;
            ui_rrect(bx, y + 164, cw, 24, 7, RGBA(255,255,255,8));
            char player_title[56];
            char *name = mp_name_ptr(c, player);
            snprintf(player_title, sizeof player_title, "P%d  %.18s", player,
                     name && name[0] ? name : mp_default_name(player));
            ui_text_fit(F_BOLD, 12, bx + 8, y + 169, cw - 16,
                        HEX(luigi_colors[ci].swatch), player_title);

            for (int event = 0; event < N_P2_SFX; event++) {
                char *path = mp_sfx_path_ptr(c, player, event);
                float ry = y + 192 + event * 45.0f;
                ui_text_fit(F_BOLD, 10, bx + 5, ry + 1, 82, C_TEXT, sound_names[event]);
                ui_text_fit_tail(F_REG, 9, bx + 88, ry + 1, 155, C_DIM,
                                 path && path[0] ? path_base(path) : "Built-in chiptune");
                if (ui_button(bx + 4, ry + 14, 48, 24, "Test", B_NORMAL, 1))
                    audio_mp_sfx_preview(player, event);
                if (ui_button(bx + 57, ry + 14, 57, 24, "Import", B_NORMAL, 1)) {
                    char sound_path[1200], title[96];
                    snprintf(title, sizeof title, "Choose Player %d %s sound", player, sound_names[event]);
                    if (dlg_pick(DLG_AUDIO, title, sound_path, sizeof sound_path)) {
                        snprintf(path, 512, "%s", sound_path);
                        audio_menu_apply();
                        settings_save();
                        launcher_toast(path[0] ? "Multiplayer sound imported." : "Could not load sound; using built-in.");
                    }
                }
                if (ui_button(bx + 119, ry + 14, 52, 24, "Reset", B_GHOST, path && path[0])) {
                    path[0] = 0;
                    audio_menu_apply();
                    settings_save();
                    launcher_toast("Using the built-in multiplayer sound.");
                }
            }
        }
        ui_text_fit(F_REG, 9, x + 18, y + 494, 770, C_DIM,
                    "WAV, MP3, OGG and FLAC are supported; each player has an independent six-event bank.");
    }
}

static void hue_to_rgb(int hue, uint32_t *rgb)
{
    float h = hue / 60.0f; int i = (int)h; float f = h - i;
    float r, gg, b;
    switch (i % 6) { case 0: r = 1; gg = f; b = 0; break; case 1: r = 1 - f; gg = 1; b = 0; break; case 2: r = 0; gg = 1; b = f; break;
                     case 3: r = 0; gg = 1 - f; b = 1; break; case 4: r = f; gg = 0; b = 1; break; default: r = 1; gg = 0; b = 1 - f; break; }
    *rgb = ((uint32_t)(r * 255) << 16) | ((uint32_t)(gg * 255) << 8) | (uint32_t)(b * 255);
}

static void apply_pad_gain(int g)
{
    float s = settings.g[g].ds_speaker_vol / 100.0f;
    audio_pad_set_gain(s, s * 0.7f);
}

static void sub_dualsense(int g, float x, float y)
{
    GameCfg *c = &settings.g[g];
    int ds = 0, edge = 0;
    for (int i = 0; i < pad_count(); i++) { if (pad_is_dualsense(i)) ds = 1; if (pad_is_edge(i)) edge = 1; }
    char st[160];
    float lw = 392;
    card(x, y, lw, 476, "CONTROLLER");
    pad_status(st, sizeof st);
    status_line(x + 18, y + 38, lw - 36, ds ? 1 : (pad_count() ? 2 : 3), st);
    if (ds) ui_text_fit(F_REG, 12, x + 18, y + 60, lw - 36, C_DIM, audio_pad_available() ? (edge ? "DualSense Edge: light, rumble and speaker ready" : "DualSense: light, rumble and speaker ready") : "Light and rumble ready. Speaker needs the USB cable.");
    else ui_text_fit(F_REG, 12, x + 18, y + 60, lw - 36, C_DIM, "Plug in a DualSense or DualSense Edge (USB for the speaker).");

    label(x + 18, y + 90, "Light");
    ui_seg(x + 18, y + 112, lw - 36, 34, led_names, N_LED, &c->ds_led_mode);
    label(x + 18, y + 158, "Brightness"); ui_slider(x + 110, y + 156, lw - 110 - 60, &c->ds_bright, 0, 100);
    char t[24]; snprintf(t, sizeof t, "%d%%", c->ds_bright); ui_text_r(F_REG, 13, x + lw - 18, y + 158, C_TEXT, t);
    if (c->ds_led_mode == LED_CUSTOM) {
        static int hue_for[N_GAMES] = {-1, -1, -1};
        static int hue[N_GAMES];
        if (hue_for[g] < 0) { hue_for[g] = 1; hue[g] = 210; }
        label(x + 18, y + 192, "Colour");
        if (ui_slider(x + 110, y + 190, lw - 110 - 60, &hue[g], 0, 359)) hue_to_rgb(hue[g], &c->ds_color);
        ui_rrect(x + lw - 18 - 36, y + 190, 36, 22, 6, HEX(c->ds_color));
    }
    ui_text(F_BOLD, 12, x + 18, y + 232, C_DIM, "RUMBLE AND SPEAKER");
    label(x + 18, y + 256, "Rumble strength"); ui_slider(x + 160, y + 254, lw - 160 - 60, &c->ds_rumble, 0, 100);
    snprintf(t, sizeof t, "%d%%", c->ds_rumble); ui_text_r(F_REG, 13, x + lw - 18, y + 256, C_TEXT, t);
    label(x + 18, y + 290, "Speaker volume");
    if (ui_slider(x + 160, y + 288, lw - 160 - 60, &c->ds_speaker_vol, 0, 100)) apply_pad_gain(g);
    snprintf(t, sizeof t, "%d%%", c->ds_speaker_vol); ui_text_r(F_REG, 13, x + lw - 18, y + 290, C_TEXT, t);
    float bw = (lw - 36 - 16) / 3;
    if (ui_button(x + 18, y + 334, bw, 36, "Test light", B_NORMAL, 1)) { pad_set_context(g, 0); pad_flash(0xFFFFFF); }
    if (ui_button(x + 18 + bw + 8, y + 334, bw, 36, "Test rumble", B_NORMAL, 1)) pad_rumble_selected(g, c->ds_rumble, 350);
    if (ui_button(x + 18 + (bw + 8) * 2, y + 334, bw, 36, "Test speaker", B_NORMAL, 1)) {
        if (audio_pad_open()) { apply_pad_gain(g); audio_pad_play_beep(0); } else launcher_toast("Speaker not found. Connect the controller with the USB cable.");
    }
    ui_text_wrap(F_REG, 12, x + 18, y + 386, lw - 36, C_DIM,
                 "The speaker plays the game's own sound effect while the event happens, so you hear a coin or a clear from the controller. Rumble and light follow the choices on the right.", 3);

    float rx = x + lw + 12, rw = 808 - lw - 12;
    card(rx, y, rw, 476, "WHEN THIS HAPPENS");
    int n = events_count(g);
    float tx[3] = {rw - 18 - 44 - 64 - 64 - 44 + 0, 0, 0};
    (void)tx;
    float c1 = rx + rw - 18 - 44 * 3 - 20 * 2 - 0, c2 = c1 + 64, c3 = c2 + 64;
    c1 = rx + 196; c2 = c1 + 50; c3 = c2 + 50;
    ui_text_c(F_BOLD, 11, c1 + 22, y + 40, C_MUTED, "LIGHT"); ui_text_c(F_BOLD, 11, c2 + 22, y + 40, C_MUTED, "RUMBLE"); ui_text_c(F_BOLD, 11, c3 + 22, y + 40, C_MUTED, "SPEAKER");
    for (int i = 0; i < n; i++) {
        const EventDef *d = events_def(g, i);
        float ry = y + 62 + i * 46;
        ui_rrect(rx + 10, ry - 3, rw - 20, 44, 8, RGBA(255, 255, 255, i % 2 ? 0 : 6));
        ui_rrect(rx + 18, ry + 8, 8, 24, 3, HEX(d->color));
        ui_text_fit(F_BOLD, 13, rx + 34, ry + 2, 150, C_TEXT, d->name);
        ui_text_fit(F_REG, 11, rx + 34, ry + 20, 150, C_DIM, d->desc);
        int v[3] = {(c->ds_ev_led >> i) & 1, (c->ds_ev_rumble >> i) & 1, (c->ds_ev_speaker >> i) & 1};
        float cx[3] = {c1, c2, c3};
        for (int k = 0; k < 3; k++) {
            if (ui_toggle(cx[k], ry + 9, &v[k])) {
                int *m = k == 0 ? &c->ds_ev_led : (k == 1 ? &c->ds_ev_rumble : &c->ds_ev_speaker);
                *m = (*m & ~(1 << i)) | (v[k] << i);
            }
        }
        if (ui_button(c3 + 52, ry + 6, 46, 30, "Try", B_GHOST, 1)) {
            pad_set_context(g, 0);
            pad_flash(d->color);
            pad_rumble_selected(g, c->ds_rumble * d->rumble_str / 100, d->rumble_ms);
            if ((c->ds_ev_speaker >> i) & 1 || 1) { if (audio_pad_open()) { apply_pad_gain(g); audio_pad_play_beep(d->beep ? d->beep : 0); } }
        }
    }
    ui_text_wrap(F_REG, 11, rx + 18, y + 62 + n * 46 + 8, rw - 36, C_DIM, "The Try button tries the light, rumble and speaker together.", 2);
}

/* ------------------------------------------------------------------ game tab: Textures */
static void sub_textures(int g, float x, float y)
{
    GameCfg *c = &settings.g[g];
    card(x, y, 808, 476, "TEXTURE PACK");
    label(x + 18, y + 42, "Pack folder");
    path_box(x + 18, y + 64, 808 - 36 - 330, c->tex_path, "Choose a folder of tile pictures");
    float bx = x + 808 - 18 - 312;
    if (ui_button(bx, y + 64, 116, 36, "Choose…", B_NORMAL, 1)) {
        char p[1100];
        if (dlg_pick(DLG_FOLDER, "Choose a texture pack folder", p, sizeof p)) { snprintf(c->tex_path, sizeof c->tex_path, "%s", p); c->tex_on = 1; loaded_pack_game = -1; }
    }
    if (ui_button(bx + 124, y + 64, 90, 36, "Clear", B_GHOST, c->tex_path[0] != 0)) { c->tex_path[0] = 0; c->tex_on = 0; loaded_pack_game = -1; }
    if (ui_button(bx + 222, y + 64, 90, 36, "Open", B_GHOST, c->tex_path[0] != 0)) open_folder(c->tex_path);
    label(x + 18, y + 118, "Use this pack");
    if (ui_toggle(x + 130, y + 114, &c->tex_on)) loaded_pack_game = -1;
    ensure_pack(g);
    char t[160];
    if (c->tex_on && c->tex_path[0]) {
        if (pack_n < 0) snprintf(t, sizeof t, "Can't read that folder.");
        else if (pack_n == 0) snprintf(t, sizeof t, "No usable tiles found. Files must be named like 0123456789abcdef.png");
        else snprintf(t, sizeof t, "%d tiles loaded, up to %dx sharper", pack_n, texpack_scale());
        status_line(x + 200, y + 116, 590, pack_n > 0 ? 1 : 3, t);
    } else ui_text(F_REG, 12, x + 200, y + 118, C_DIM, "Replaces the game's 8x8 tiles with your own pictures (any size that is a multiple of 8).");
    ui_rect(x + 18, y + 156, 772, 1, C_LINE);

    ui_text(F_BOLD, 12, x + 18, y + 172, C_DIM, "MAKE YOUR OWN PACK");
    ui_text_wrap(F_REG, 13, x + 18, y + 196, 772, C_MUTED,
                 "Play (or just open the game) and the launcher remembers every tile the game draws. Export them to a folder, redraw the ones you like, then choose that folder above. Tiles you don't change keep their original look.", 3);
    label(x + 18, y + 262, "Remember tiles while I play");
    ui_toggle(x + 220, y + 258, &c->tex_collect);
    tex_collect_begin(g);
    snprintf(t, sizeof t, "%d tiles collected so far", tex_collected());
    ui_text(F_BOLD, 13, x + 290, y + 262, C_TEXT, t);
    label(x + 18, y + 304, "Export size");
    static const char *sz[3] = {"4x", "8x", "16x"};
    int si = export_scale == 4 ? 0 : (export_scale == 8 ? 1 : 2);
    if (ui_seg(x + 120, y + 296, 210, 36, sz, 3, &si)) export_scale = si == 0 ? 4 : (si == 1 ? 8 : 16);
    ui_text(F_REG, 12, x + 346, y + 304, C_DIM, "pixels per original pixel (8x = 64x64 per tile)");
    if (ui_button(x + 18, y + 350, 250, 42, "Export tiles to a folder…", B_PRIMARY, tex_collected() > 0)) {
        char p[1100];
        if (dlg_pick(DLG_FOLDER, "Choose where to save the tiles", p, sizeof p)) {
            int n = tex_export(p, &palettes[c->palette], export_scale, export_msg[g], sizeof export_msg[g]);
            if (n > 0) { snprintf(c->tex_path, sizeof c->tex_path, "%s", p); loaded_pack_game = -1; }
            launcher_toast(export_msg[g]);
        }
    }
    if (ui_button(x + 280, y + 350, 160, 42, "Forget collected", B_GHOST, tex_collected() > 0)) { tex_collect_clear(); export_msg[g][0] = 0; }
    if (export_msg[g][0]) ui_text_fit(F_REG, 13, x + 18, y + 404, 772, C_OK, export_msg[g]);
    ui_text_wrap(F_REG, 12, x + 18, y + 428, 772, C_DIM, "Each file is named after its tile. _overview.png shows them all on one sheet. Transparent pixels in a replacement let the original show through.", 3);
}

/* ------------------------------------------------------------------ filters tab */
static void slider_row(float x, float y, float w, const char *name, int *v, const char *hint)
{
    char t[16];
    label(x, y + 2, name);
    if (ui_slider(x + 130, y, w - 130 - 54, v, 0, 100)) settings.flt.preset = 0;
    snprintf(t, sizeof t, "%d", *v);
    ui_text_r(F_REG, 13, x + w, y + 2, C_TEXT, t);
    hint_area(x, y - 4, w, 30, hint);
}

static void tab_filters(float x, float y)
{
    FilterCfg *f = &settings.flt;
    card(x, y, 428, 504, "SCREEN LOOK");
    ui_text(F_BOLD, 12, x + 18, y + 38, C_MUTED, "Presets");
    for (int i = 0; i < n_filter_presets; i++) {
        float cx = x + 18 + (i % 3) * 133, cy = y + 58 + (i / 3) * 38;
        if (ui_chip(cx, cy, 125, 30, filter_preset_names[i], f->preset == i && i != 0 ? 1 : (i == 0 && !f->lcd_grid && !f->ghost && !f->scanlines && !f->mask && !f->curve && !f->bloom && !f->hdr && !f->vignette && !f->blur)))
            filter_preset(f, i);
    }
    float sy = y + 144;
    ui_text(F_BOLD, 12, x + 18, sy, C_MUTED, "Game Boy screen");
    slider_row(x + 18, sy + 24, 392, "Dot grid", &f->lcd_grid, "Dark gaps between the screen's dots, like the real LCD");
    slider_row(x + 18, sy + 52, 392, "Ghosting", &f->ghost, "Pixels fade slowly like the old LCD (visible while playing)");
    sy += 88;
    ui_text(F_BOLD, 12, x + 18, sy, C_MUTED, "CRT TV");
    slider_row(x + 18, sy + 24, 392, "Scanlines", &f->scanlines, "Dark lines between rows");
    slider_row(x + 18, sy + 52, 392, "Shadow mask", &f->mask, "Red, green and blue stripes");
    slider_row(x + 18, sy + 80, 392, "Curvature", &f->curve, "Bulges the picture like a curved tube");
    sy += 116;
    ui_text(F_BOLD, 12, x + 18, sy, C_MUTED, "Light");
    slider_row(x + 18, sy + 24, 392, "Bloom", &f->bloom, "Bright areas glow");
    slider_row(x + 18, sy + 52, 392, "HDR", &f->hdr, "Brighter highlights, richer colour and a stronger glow");
    slider_row(x + 18, sy + 80, 392, "Vignette", &f->vignette, "Darker corners");
    slider_row(x + 18, sy + 108, 392, "Softness", &f->blur, "Slight horizontal blur");

    float rx = x + 440, rw = 808 - 440;
    card(rx, y, rw, 504, "PREVIEW");
    static const char *short_names[] = {"Dr. Mario", "Mario Land", "Mario Land 2"};
    for (int i = 0; i < GAME_WARIO_SML3; i++) {
        float cw = (rw - 36 - 16) / 3;
        if (ui_chip(rx + 18 + i * (cw + 8), y + 38, cw, 30, short_names[i], filter_game == i)) filter_game = i;
    }
    draw_preview(filter_game, rx + 16, y + 80, rw - 32, 300, 1);
    ui_text_wrap(F_REG, 12, rx + 18, y + 394, rw - 36, C_DIM,
                 "These filters apply to every game, in the launcher preview and while you play. Try the presets first, then adjust.", 3);
    if (ui_button(rx + 18, y + 452, 140, 36, "Turn all off", B_GHOST, 1)) filter_preset(f, 0);
}

/* ------------------------------------------------------------------ audio tab */
static const char *sfx_names[N_UI_SFX] = {"Hover", "Click", "Confirm", "Back", "Toggle"};

static void tab_audio(float x, float y)
{
    Settings *s = &settings;
    char t[96];
    float lw = 396;
    card(x, y, lw, 200, "GAME SOUND");
    label(x + 18, y + 44, "Volume");
    ui_slider(x + 100, y + 42, lw - 100 - 70, &s->volume, 0, 200);
    snprintf(t, sizeof t, "%d%%", s->volume); ui_text_r(F_REG, 13, x + lw - 18, y + 44, C_TEXT, t);
    label(x + 18, y + 84, "Sound delay");
    ui_seg(x + 18, y + 108, lw - 36, 34, lat_names, N_LAT, &s->latency);
    hint_area(x + 18, y + 84, lw - 36, 60, "Higher is safest. Choose High if you ever hear crackling or choppy sound.");
    AudioStats st;
    audio_get_stats(&st);
    if (audio_ok()) snprintf(t, sizeof t, "Output: %d Hz, %d sample buffer", st.device_rate, st.device_samples);
    else snprintf(t, sizeof t, "No audio device found");
    ui_text_fit(F_REG, 12, x + 18, y + 154, lw - 36, C_DIM, t);
    ui_text(F_REG, 12, x + 18, y + 174, C_DIM, "Changes to the delay apply the next time you start the program.");

    card(x, y + 212, lw, 292, "MENU MUSIC");
    label(x + 18, y + 256, "Play music in the launcher");
    if (ui_toggle(x + lw - 18 - 44, y + 252, &s->menu_music)) audio_menu_apply();
    label(x + 18, y + 296, "Volume");
    if (ui_slider(x + 100, y + 294, lw - 100 - 70, &s->menu_music_vol, 0, 100)) audio_menu_apply();
    snprintf(t, sizeof t, "%d%%", s->menu_music_vol); ui_text_r(F_REG, 13, x + lw - 18, y + 296, C_TEXT, t);
    label(x + 18, y + 334, "Track");
    path_box(x + 18, y + 356, lw - 36, s->menu_music_path, "Built-in chiptune");
    float bw = (lw - 36 - 16) / 3;
    if (ui_button(x + 18, y + 404, bw, 36, "Import…", B_NORMAL, 1)) {
        char p[1100];
        if (dlg_pick(DLG_AUDIO, "Choose a music file", p, sizeof p)) {
            char old[512]; snprintf(old, sizeof old, "%s", s->menu_music_path);
            snprintf(s->menu_music_path, sizeof s->menu_music_path, "%s", p);
            audio_menu_apply();
            if (!s->menu_music_path[0]) { launcher_toast("Couldn't read that audio file."); snprintf(s->menu_music_path, sizeof s->menu_music_path, "%s", old); audio_menu_apply(); }
            else { audio_music_preview(); launcher_toast("Menu music changed."); }
        }
    }
    if (ui_button(x + 18 + bw + 8, y + 404, bw, 36, "Built-in", B_GHOST, s->menu_music_path[0] != 0)) { s->menu_music_path[0] = 0; audio_menu_apply(); }
    if (ui_button(x + 18 + (bw + 8) * 2, y + 404, bw, 36, "Restart", B_GHOST, 1)) audio_music_preview();
    ui_text_wrap(F_REG, 12, x + 18, y + 456, lw - 36, C_DIM, "WAV, MP3, OGG or FLAC. It loops while you are in the launcher and stops when a game starts. You can also drop a file onto this tab.", 3);

    float rx = x + lw + 12, rw = 808 - lw - 12;
    card(rx, y, rw, 504, "INTERFACE SOUNDS");
    label(rx + 18, y + 44, "Click and hover sounds");
    ui_toggle(rx + rw - 18 - 44, y + 40, &s->ui_sfx);
    label(rx + 18, y + 84, "Volume");
    if (ui_slider(rx + 100, y + 82, rw - 100 - 70, &s->ui_sfx_vol, 0, 100)) audio_menu_apply();
    snprintf(t, sizeof t, "%d%%", s->ui_sfx_vol); ui_text_r(F_REG, 13, rx + rw - 18, y + 84, C_TEXT, t);
    for (int i = 0; i < N_UI_SFX; i++) {
        float ry = y + 126 + i * 70;
        ui_rrect(rx + 10, ry - 4, rw - 20, 64, 10, RGBA(255, 255, 255, i % 2 ? 0 : 6));
        ui_text(F_BOLD, 14, rx + 18, ry + 2, C_TEXT, sfx_names[i]);
        ui_text_fit_tail(F_REG, 12, rx + 18, ry + 26, rw - 36 - 0, C_DIM, s->ui_sfx_path[i][0] ? path_base(s->ui_sfx_path[i]) : "Built-in");
        float bx = rx + rw - 18 - 3 * 64 - 2 * 6;
        if (ui_button(bx, ry + 2, 64, 30, "Play", B_NORMAL, 1)) audio_sfx_preview(i);
        if (ui_button(bx + 70, ry + 2, 64, 30, "Import", B_NORMAL, 1)) {
            char p[1100];
            if (dlg_pick(DLG_AUDIO, "Choose a sound file", p, sizeof p)) {
                snprintf(s->ui_sfx_path[i], sizeof s->ui_sfx_path[i], "%s", p);
                audio_menu_apply();
                if (!s->ui_sfx_path[i][0]) launcher_toast("Couldn't read that audio file."); else audio_sfx_preview(i);
            }
        }
        if (ui_button(bx + 140, ry + 2, 64, 30, "Reset", B_GHOST, s->ui_sfx_path[i][0] != 0)) { s->ui_sfx_path[i][0] = 0; audio_menu_apply(); }
    }
    ui_text_wrap(F_REG, 12, rx + 18, y + 126 + N_UI_SFX * 70 + 6, rw - 36, C_DIM, "Short sounds work best. Any WAV, MP3, OGG or FLAC.", 3);
    (void)sfx_test_msg;
}

/* ------------------------------------------------------------------ frame */
static const char *tab_labels[N_TABS] = {
    [TAB_MULTIPLAYER] = "Multiplayer",
    [TAB_MP_CONTROLLERS] = "MP Controllers",
    [TAB_FILTERS] = "Filters",
    [TAB_AUDIO] = "Audio & menu"
};

static int tab_button(float x, float y, float w, float h, int selected, uint32_t accent)
{
    int over;
    int clicked = clickable(x, y, w, h, &over);
    float a = ui_anim(ui_next_id() * 4 + 3, selected ? 1.0f : (over ? 0.5f : 0.0f), 16.0f);
    if (a > 0.01f) ui_rrect(x, y, w, h, 12, RGBA(255, 255, 255, (int)(a * (selected ? 24 : 14))));
    if (selected) ui_rrect(x, y + 10, 4, h - 20, 2, HEX(accent));
    return clicked;
}

LauncherResult launcher_frame(float dt)
{
    LauncherResult res = {-1, 0};
    launcher_controller_update(dt);
    anim_clock += dt;
    int g = (tab == TAB_MULTIPLAYER || tab == TAB_MP_CONTROLLERS) ? mp_config_game : launcher_current_game();
    if (tab < N_GAMES) last_game_tab = tab;
    ui_accent = tab < N_GAMES ? games[tab].accent : ((tab == TAB_MULTIPLAYER || tab == TAB_MP_CONTROLLERS) ? games[mp_config_game].accent : 0x4C8DFF);
    ensure_background(g);
    bg_update(dt);

    /* backdrop */
    float vx = ui_view_x0(), vy = ui_view_y0(), vw = ui_view_w(), vh = ui_view_h();
    ui_vgrad(vx, vy, vw, vh, C_BG2, C_BG);
    int bw, bh;
    SDL_Texture *bt = bg_texture(&bw, &bh);
    if (bt) {
        draw_cover(bt, bw, bh, vx, vy, vw, vh);
        ui_rect(vx, vy, vw, vh, RGBA(8, 9, 14, (int)(60 + settings.g[g].bg_dim * 2.4f)));
    }

    /* Use only the supplied transparent PipeClean logo in the header. */
    SDL_Texture *brand_logo = branding_logo_texture();
    if (brand_logo) ui_image(brand_logo, NULL, 20, 1, 170, 83);
    ui_rect(20, 90, UI_W - 40, 1, C_LINE);

    /* sidebar */
    float sx = 24, sy = 108;
    int compact_games = N_GAMES > 4;
    float game_row = compact_games ? 46.0f : 76.0f;
    float game_h = compact_games ? 42.0f : 68.0f;
    for (int i = 0; i < N_GAMES; i++) {
        float y = sy + i * game_row;
        if (tab_button(sx, y, 224, game_h, tab == i, games[i].accent)) { tab = i; }
        if (compact_games) {
            ui_text_fit(F_BOLD, 13, sx + 14, y + 4, 202, tab == i ? C_TEXT : C_MUTED, games[i].name);
            int kind = rs[i].ok ? 1 : (settings.g[i].rom_path[0] ? 3 : 2);
            const char *msg = rs[i].ok ? "Ready to play" : (settings.g[i].rom_path[0] ? "ROM issue" : "ROM needed");
            status_line(sx + 14, y + 23, 202, kind, msg);
        } else {
            ui_text_fit(F_BOLD, 15, sx + 18, y + 11, 196, tab == i ? C_TEXT : C_MUTED, games[i].name);
            status_line(sx + 18, y + 38, 196, rs[i].ok ? 1 : 2, rs[i].ok ? (settings.g[i].hack_path[0] && hack_ok[i] ? "Ready with romhack" : "Ready to play") : "ROM needed");
        }
    }
    float y2 = sy + N_GAMES * game_row + 8;
    ui_rect(sx + 8, y2, 208, 1, C_LINE);
    for (int i = TAB_MULTIPLAYER; i < N_TABS; i++) {
        float y = y2 + 12 + (i - TAB_MULTIPLAYER) * 52;
        uint32_t accent = i == TAB_MULTIPLAYER ? games[mp_config_game].accent : 0x4C8DFF;
        if (tab_button(sx, y, 224, 44, tab == i, accent)) tab = i;
        ui_text(F_BOLD, 15, sx + 18, y + 11, tab == i ? C_TEXT : C_MUTED, tab_labels[i]);
    }

    float cx = 268, cy = 108;
    if (tab < N_GAMES) {
        ui_accent = games[tab].accent;
        ui_text(F_BOLD, 28, cx, cy - 4, C_TEXT, games[tab].name);
        ui_text(F_REG, 13, cx, cy + 34, C_MUTED, games[tab].sub);
        int can = rs[tab].ok;
        if (ui_button(cx + 808 - 176, cy + 2, 176, 52, can ? "Play" : "Needs ROM", B_PRIMARY, can)) res.play = tab;
        if (!can) hint_area(cx + 808 - 176, cy + 2, 176, 52, "Choose your ROM on the Game tab (or drop it on this window)");
        float y = cy + 118;
        pad_set_context(tab, 0);
        if (games[tab].external_player) {
            sub_external_game(tab, cx, y);
        } else {
            ui_seg(cx, cy + 66, 808, 38, sub_names, N_SUB, &sub[tab]);
            switch (sub[tab]) {
        case SUB_GAME:
            sub_game(tab, cx, y);
            break;
        case SUB_DISPLAY:
            sub_display(tab, cx, y);
            break;
        case SUB_CONTROLS:
            sub_controls(tab, cx, y);
            break;
        case SUB_BINDINGS:
            sub_bindings(tab, cx, y);
            break;
        case SUB_DUALSENSE:
            sub_dualsense(tab, cx, y);
            break;
        case SUB_SAVE_STATES:
            sub_save_states(tab, cx, y);
            break;
            default:
                sub_textures(tab, cx, y);
                break;
            }
        }
        if (load_state_request[tab] >= 0) res.play = tab;
    } else if (tab == TAB_MULTIPLAYER) {
        ui_text(F_BOLD, 28, cx, cy - 4, C_TEXT, "Multiplayer");
        ui_text(F_REG, 13, cx, cy + 34, C_MUTED, "Choose 2–4 players and customize names and character colors.");
        tab_multiplayer(cx, cy + 58);
    } else if (tab == TAB_MP_CONTROLLERS) {
        ui_text(F_BOLD, 28, cx, cy - 4, C_TEXT, "Multiplayer Controllers");
        ui_text(F_REG, 13, cx, cy + 34, C_MUTED, "Assign controllers, respawn bindings, and independent sound banks for players 2–4.");
        tab_mp_controllers(cx, cy + 58);
    } else if (tab == TAB_FILTERS) {
        ui_text(F_BOLD, 28, cx, cy - 4, C_TEXT, "Filters");
        ui_text(F_REG, 13, cx, cy + 34, C_MUTED, "Make the screen look like a real Game Boy, a CRT TV, or something glowing.");
        tab_filters(cx, cy + 58);
    } else {
        ui_text(F_BOLD, 28, cx, cy - 4, C_TEXT, "Audio & menu");
        ui_text(F_REG, 13, cx, cy + 34, C_MUTED, "Game volume, the launcher's own music and the little click sounds.");
        tab_audio(cx, cy + 58);
    }

    /* footer */
    const char *h = ui_hint_text();
    const char *footer = launcher_controller_cursor_active && !cap_kind && !multiplayer_name_editing
        ? "Controller: D-pad/left stick move | A select | B cancel | Move mouse to switch back"
        : (h && h[0] ? h : "Drag and drop a ROM, patch, picture, sound or texture folder anywhere on this window.");
    ui_text_fit(F_REG, 12, 28, UI_H - 26, UI_W - 56, C_DIM, footer);

    /* toast */
    if (toast_t > 0) {
        toast_t -= dt;
        float a = toast_t < 0.4f ? toast_t / 0.4f : 1.0f;
        float tw = ui_text_w(F_BOLD, 14, toast_msg) + 40;
        float tx = UI_W * 0.5f - tw * 0.5f, ty = UI_H - 74;
        ui_shadow(tx, ty, tw, 40, 12, 10, RGBA(0, 0, 0, (int)(90 * a)));
        ui_rrect(tx, ty, tw, 40, 12, RGBA(40, 46, 66, (int)(250 * a)));
        ui_text_c(F_BOLD, 14, UI_W * 0.5f, ty + 10, RGBA(237, 239, 246, (int)(255 * a)), toast_msg);
    }

    /* Draw a high-contrast virtual pointer while controller navigation is active. */
    if (launcher_controller_cursor_active) {
        ui_rrect(launcher_controller_x - 7.0f, launcher_controller_y - 7.0f,
                 14.0f, 14.0f, 7.0f, RGBA(12, 16, 24, 235));
        ui_stroke(launcher_controller_x - 7.0f, launcher_controller_y - 7.0f,
                  14.0f, 14.0f, 7.0f, 2.0f, RGBA(174, 232, 255, 255));
        ui_rrect(launcher_controller_x - 2.0f, launcher_controller_y - 2.0f,
                 4.0f, 4.0f, 2.0f, RGBA(255, 255, 255, 255));
    }

    /* binding capture hint */
    if (cap_kind) {
        if (cap_kind == 1 || cap_kind == 3 || cap_kind == 5) ui_hint("Press a key. Backspace clears the slot, Esc cancels.");
        else ui_hint("Press a controller button or trigger. Esc (keyboard) cancels.");
    }

    if (wide_dirty && !ui_mouse.down) { make_preview(wide_dirty - 1); wide_dirty = 0; }
    if (ui_mouse.released) click_id = 0;
    settings.last_tab = tab;
    save_timer += dt;
    if (save_timer > 2.0f && !ui_mouse.down && !multiplayer_name_editing) { save_timer = 0; settings_save(); }

    /* pad light follows the tab while in the launcher */
    pad_frame(dt);
    return res;
}

/* ------------------------------------------------------------------ events */
void launcher_event(const SDL_Event *e)
{
    if (e && (e->type == SDL_MOUSEMOTION || e->type == SDL_MOUSEBUTTONDOWN ||
              e->type == SDL_MOUSEWHEEL)) {
        launcher_leave_controller_cursor();
    }
    if (multiplayer_name_editing) {
        GameCfg *mc = &settings.g[mp_config_game];
        char *name = mp_name_ptr(mc, multiplayer_name_editing);
        const char *fallback = mp_default_name(multiplayer_name_editing);
        if (!name) { multiplayer_name_editing = 0; SDL_StopTextInput(); return; }
        if (e->type == SDL_TEXTINPUT) {
            size_t have = strlen(name), add = strlen(e->text.text);
            if (have + add < 32 && have + add <= 22 &&
                (unsigned char)e->text.text[0] >= 32)
                memcpy(name + have, e->text.text, add + 1);
            return;
        }
        if (e->type == SDL_KEYDOWN && !e->key.repeat) {
            SDL_Keycode k = e->key.keysym.sym;
            if (k == SDLK_ESCAPE || k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_TAB) {
                multiplayer_name_editing = 0;
                SDL_StopTextInput();
                if (!name[0]) snprintf(name, 32, "%s", fallback);
                settings_save();
                return;
            }
            if (k == SDLK_BACKSPACE || k == SDLK_DELETE) {
                size_t len = strlen(name);
                if (len) {
                    len--;
                    while (len && (((unsigned char)name[len] & 0xC0u) == 0x80u)) len--;
                    name[len] = 0;
                }
                return;
            }
            if ((e->key.keysym.mod & KMOD_CTRL) && k == SDLK_a) {
                name[0] = 0;
                return;
            }
        }
        if ((e->type == SDL_WINDOWEVENT && e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) ||
            e->type == SDL_MOUSEBUTTONDOWN) {
            multiplayer_name_editing = 0;
            SDL_StopTextInput();
            if (!name[0]) snprintf(name, 32, "%s", fallback);
            settings_save();
        }
        return;
    }
    if (cap_kind) {
        int cap_game = (tab == TAB_MP_CONTROLLERS || cap_kind == 5 || cap_kind == 6) ? mp_config_game : launcher_current_game();
        GameCfg *current = &settings.g[cap_game];
        if ((cap_kind == 2 && (cap_slot < 0 || cap_slot >= MAX_MP_PLAYERS ||
                               current->pad_device[cap_slot] < 0 || current->pad_device[cap_slot] >= pad_count())) ||
            (cap_kind == 4 && (current->pad_device[0] < 0 || current->pad_device[0] >= pad_count())) ||
            (cap_kind == 6 && (cap_slot < 1 || cap_slot >= MAX_MP_PLAYERS ||
                               current->pad_device[cap_slot] < 0 || current->pad_device[cap_slot] >= pad_count())))
            cap_kind = 0;
    }
    if (cap_kind) {
        int cap_game = (tab == TAB_MP_CONTROLLERS || cap_kind == 5 || cap_kind == 6) ? mp_config_game : launcher_current_game();
        GameCfg *c = &settings.g[cap_game];
        if (e->type == SDL_KEYDOWN && !e->key.repeat) {
            SDL_Keycode k = e->key.keysym.sym;
            if (k == SDLK_ESCAPE) { cap_kind = 0; return; }
            if (cap_kind == 1) {
                c->key[cap_btn][cap_slot] = (k == SDLK_BACKSPACE || k == SDLK_DELETE) ? 0 : k;
                cap_kind = 0;
                settings_save();
            } else if (cap_kind == 3) {
                c->action_key[cap_btn] = (k == SDLK_BACKSPACE || k == SDLK_DELETE) ? 0 : k;
                cap_kind = 0;
            } else if (cap_kind == 5) {
                int *binding = mp_respawn_key_ptr(c, cap_btn);
                if (binding) *binding = (k == SDLK_BACKSPACE || k == SDLK_DELETE) ? 0 : k;
                cap_kind = 0;
                settings_save();
                launcher_toast("Multiplayer respawn key saved.");
            } else if (k == SDLK_BACKSPACE || k == SDLK_DELETE) {
                if (cap_kind == 2) { c->pad[cap_btn][cap_slot] = -1; settings_save(); }
                else if (cap_kind == 4) c->action_pad[cap_btn] = -1;
                else if (cap_kind == 6) {
                    int *binding = mp_respawn_pad_ptr(c, cap_btn);
                    if (binding) *binding = -1;
                    settings_save();
                    launcher_toast("Multiplayer controller respawn binding cleared.");
                }
                cap_kind = 0;
            }
        } else if (cap_kind == 2) {
            int code = pad_capture(c->pad_device[cap_slot], e);
            if (code >= 0) {
                c->pad[cap_btn][cap_slot] = code;
                cap_kind = 0;
                launcher_ignore_pad_confirm = 1;
                settings_save();
            }
        } else if (cap_kind == 4) {
            int code = pad_capture(c->pad_device[0], e);
            if (code >= 0) {
                c->action_pad[cap_btn] = code;
                cap_kind = 0;
                launcher_ignore_pad_confirm = 1;
            }
        } else if (cap_kind == 6) {
            int code = pad_capture(c->pad_device[cap_slot], e);
            if (code >= 0) {
                int *binding = mp_respawn_pad_ptr(c, cap_btn);
                if (binding) *binding = code;
                cap_kind = 0;
                launcher_ignore_pad_confirm = 1;
                settings_save();
                launcher_toast("Multiplayer controller respawn binding saved.");
            }
        }
        return;
    }
    if (e->type == SDL_KEYDOWN && !e->key.repeat) {
        if (e->key.keysym.sym == SDLK_TAB && (e->key.keysym.mod & KMOD_CTRL)) tab = (tab + 1) % N_TABS;
    }
}

static int ends_with(const char *s, const char *ext)
{
    size_t a = strlen(s), b = strlen(ext);
    if (a < b) return 0;
    for (size_t i = 0; i < b; i++) if ((s[a - b + i] | 32) != (ext[i] | 32)) return 0;
    return 1;
}

void launcher_drop(const char *path)
{
    int g = launcher_current_game();
    int id = rom_identify_file(path);
    if (id >= 0) {
        snprintf(settings.g[id].rom_path, sizeof settings.g[id].rom_path, "%s", path);
        rom_check(id);
        if (rs[id].ok) make_preview(id);
        tab = id;
        launcher_toast(rs[id].ok ? "ROM added." : rs[id].msg);
        return;
    }
    if (ends_with(path, ".ips") || ends_with(path, ".bps") || ends_with(path, ".ups")) { if (tab >= N_GAMES) tab = g; set_hack(tab, path); launcher_toast(hack_msg[tab]); return; }
    if (ends_with(path, ".png") || ends_with(path, ".jpg") || ends_with(path, ".jpeg") || ends_with(path, ".bmp") || ends_with(path, ".gif") || ends_with(path, ".tga")) {
        if (tab >= N_GAMES) tab = g;
        snprintf(settings.g[tab].bg_path, sizeof settings.g[tab].bg_path, "%s", path);
        ensure_background(tab);
        launcher_toast(bg_msg[0] ? bg_msg : "Background changed.");
        return;
    }
    if (ends_with(path, ".wav") || ends_with(path, ".mp3") || ends_with(path, ".ogg") || ends_with(path, ".flac")) {
        snprintf(settings.menu_music_path, sizeof settings.menu_music_path, "%s", path);
        audio_menu_apply();
        tab = TAB_AUDIO;
        launcher_toast(settings.menu_music_path[0] ? "Menu music changed." : "Couldn't read that audio file.");
        return;
    }
    if (ends_with(path, ".gb") || ends_with(path, ".gbc")) { launcher_toast("That ROM isn't one of the supported games."); return; }
    /* a folder: texture pack */
    {
        SDL_RWops *r = SDL_RWFromFile(path, "rb");
        if (r) { SDL_RWclose(r); launcher_toast("Not a file type I know."); return; }
        if (tab >= N_GAMES) tab = g;
        snprintf(settings.g[tab].tex_path, sizeof settings.g[tab].tex_path, "%s", path);
        settings.g[tab].tex_on = 1;
        loaded_pack_game = -1;
        sub[tab] = SUB_TEXTURES;
        launcher_toast("Texture pack folder set.");
    }
}