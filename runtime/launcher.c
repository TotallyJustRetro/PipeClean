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
#include "cart.h"
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
#include "netplay.h"

static SDL_Renderer *g_ren; static SDL_Renderer *ren_get(void){return g_ren;}
void launcher_set_renderer(SDL_Renderer *r){g_ren=r;}
void ui_text_fit_tail(int font, float size, float x, float y, float maxw, uint32_t c, const char *s);

enum { TAB_MULTIPLAYER = N_GAMES, TAB_MP_CONTROLLERS, TAB_FILTERS, TAB_AUDIO, TAB_ONLINE, TAB_DISPLAY, N_TABS };
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
static int menu_playlist_page;
static unsigned music_notice_serial;
static unsigned music_notice_announced_serial;
static float music_notice_age = -1.0f;
static int music_notice_deferred;
static char music_notice_path[512];
static void menu_music_notice_start(void);
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
static int mp_color_selected; /* 0=P2, 1=P3, 2=P4 in the wheel editor */
static int online_name_editing, online_code_editing, online_server_editing, online_chat_editing;
static int online_save_name_editing, online_save_name_slot = -1;
static char online_save_name_buffer[32];
static int online_lobby_panel; /* 0 = session chat, 1 = shared save slots */
static int online_mode_menu;
static char online_join_code[9];
static char online_chat_buffer[96];
static char online_server_before[160];
static int online_selected_game = GAME_SML2;
static int online_last_netplay_state = NETPLAY_OFFLINE;
static int online_rom_reported_game = -1;
static int launcher_controller_cursor_active;
static int launcher_controller_confirm_was_down, launcher_controller_back_was_down;
static int launcher_controller_wait_neutral, launcher_ignore_pad_confirm;
static float launcher_controller_x, launcher_controller_y;
static int clickable(float x, float y, float w, float h, int *over_out);
static void ensure_pack(int g);

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
    char path[1200], id[128];
    launcher_game_file_id(g, id, sizeof id);
    snprintf(path, sizeof path, "%sstates/%s.slot%d.thumb.bmp", settings_dir(), id, slot);
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
    if (g < 0 || g >= N_GAMES) { snprintf(err, n, "Invalid game selection."); return 1; }
    GameCfg *c = &settings.g[g];
    RomStatus st;
    if (!c->rom_path[0] || rom_load(g, c->rom_path, &st)) {
        snprintf(err, n, "%s", c->rom_path[0] ? st.msg : "Choose your ROM on this game's tab first.");
        return 1;
    }
    hack_ok[g] = 0;
    hack_msg[g][0] = 0;
    if (c->romhack_selected >= 0) {
        int index = c->romhack_selected;
        if (index >= c->romhack_count || index >= MAX_ROMHACKS ||
            !c->romhacks[index].path[0]) {
            snprintf(err, n, "Choose a valid ROM hack in the ROM Hacks tab.");
            return 1;
        }
        const char *hack = c->romhacks[index].path;
        if (!file_exists(hack)) {
            snprintf(hack_msg[g], sizeof hack_msg[g], "The selected ROM hack file is missing.");
            snprintf(err, n, "%s", hack_msg[g]);
            return 1;
        }
        if (rom_apply_hack(hack, &st) != 0) {
            snprintf(hack_msg[g], sizeof hack_msg[g], "%s", st.msg);
            snprintf(err, n, "Couldn't apply the selected ROM hack: %.170s", st.msg);
            return 1;
        }
        hack_ok[g] = 1;
        snprintf(hack_msg[g], sizeof hack_msg[g], "%s", st.msg);
    }
    int wl, wr;
    wide_dims(g, c->wide, &wl, &wr);
    /* Always call wide_install, including when the requested width is zero.
     * It clears runtime SML/SML2 hooks left by a previous game/session before
     * the native-width PPU setup. Skipping it at 0% can leave widened spawn
     * and cull bounds active even though the picture is back to 160 pixels. */
    int wide_ready = wide_install(g, wl, wr);
    if (wl + wr > 0 && !wide_ready) { wl = wr = 0; wide_note[g] = 1; } else wide_note[g] = 0;
    ppu_set_wide(wl, wr, games[g].hud_lines, games[g].hud_window, games[g].wide_gate);
    return 0;
}

void launcher_game_file_id(int g, char *out, size_t n)
{
    if (!out || !n) return;
    if (g < 0 || g >= N_GAMES) { snprintf(out, n, "unknown"); return; }
    const GameCfg *c = &settings.g[g];
    int index = c->romhack_selected;
    if (index >= 0 && index < c->romhack_count && index < MAX_ROMHACKS &&
        c->romhacks[index].path[0]) {
        /* Stable across list reordering; also distinguish the same patch used
         * against different base ROM paths. */
        uint32_t hash = 2166136261u;
        const char *parts[2] = {c->rom_path, c->romhacks[index].path};
        for (int part = 0; part < 2; part++) {
            for (const unsigned char *p = (const unsigned char *)parts[part]; *p; p++) {
                hash ^= *p;
                hash *= 16777619u;
            }
            hash ^= 0xFFu;
            hash *= 16777619u;
        }
        snprintf(out, n, "%s-hack%08X", games[g].id, (unsigned)hash);
    } else {
        snprintf(out, n, "%s", games[g].id);
    }
}

static void shared_slot_name_path(int game, int slot, char *out, size_t out_size)
{
    char id[128];
    if (!out || !out_size) return;
    out[0] = 0;
    if (game < 0 || game >= N_GAMES || slot < 0 || slot >= NETPLAY_SHARED_SLOT_COUNT) return;
    launcher_game_file_id(game, id, sizeof id);
    char dir[1200];
    snprintf(dir, sizeof dir, "%sstates/", settings_dir());
    mkdir_u(dir);
    snprintf(out, out_size, "%s%s.slot%d.name", dir, id, slot);
}

void launcher_shared_slot_paths(int game, int slot,
                                char *state_path, size_t state_size,
                                char *marker_path, size_t marker_size)
{
    char id[128], dir[1200];
    if (state_path && state_size) state_path[0] = 0;
    if (marker_path && marker_size) marker_path[0] = 0;
    if (game < 0 || game >= N_GAMES || slot < 0 || slot >= NETPLAY_SHARED_SLOT_COUNT) return;
    snprintf(dir, sizeof dir, "%sstates/", settings_dir());
    mkdir_u(dir);
    launcher_game_file_id(game, id, sizeof id);
    if (state_path && state_size) snprintf(state_path, state_size, "%s%s.slot%d.pcs", dir, id, slot);
    if (marker_path && marker_size) snprintf(marker_path, marker_size, "%s%s.slot%d.shared", dir, id, slot);
}

int launcher_shared_slot_is_saved(int game, int slot)
{
    char state[1200], marker[1200];
    launcher_shared_slot_paths(game, slot, state, sizeof state, marker, sizeof marker);
    return state[0] && marker[0] && file_exists(state) && file_exists(marker);
}

void launcher_shared_slot_name_get(int game, int slot, char *out, size_t out_size)
{
    char path[1200], default_name[32], line[64];
    if (!out || !out_size) return;
    snprintf(default_name, sizeof default_name, "Slot %d", slot + 1);
    snprintf(out, out_size, "%s", default_name);
    shared_slot_name_path(game, slot, path, sizeof path);
    if (!path[0]) return;
    FILE *file = fopen(path, "rb");
    if (!file) return;
    if (fgets(line, sizeof line, file)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) {
            size_t j = 0;
            for (const unsigned char *p = (const unsigned char *)line; *p && j + 1 < out_size; p++) {
                unsigned char ch = *p;
                if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                    (ch >= '0' && ch <= '9') || ch == ' ' || ch == '_' || ch == '-')
                    out[j++] = (char)ch;
            }
            out[j] = 0;
            if (!out[0]) snprintf(out, out_size, "%s", default_name);
        }
    }
    fclose(file);
}

int launcher_shared_slot_name_set(int game, int slot, const char *name)
{
    char path[1200], temp[1240], clean[32];
    size_t used = 0;
    if (game < 0 || game >= N_GAMES || slot < 0 || slot >= NETPLAY_SHARED_SLOT_COUNT || !name) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && used + 1 < sizeof clean; p++) {
        unsigned char ch = *p;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == ' ' || ch == '_' || ch == '-')
            clean[used++] = (char)ch;
    }
    while (used && clean[used - 1] == ' ') used--;
    clean[used] = 0;
    if (!used) return 0;
    shared_slot_name_path(game, slot, path, sizeof path);
    if (!path[0] || snprintf(temp, sizeof temp, "%s.tmp", path) >= (int)sizeof temp) return 0;
    FILE *file = fopen(temp, "wb");
    if (!file) return 0;
    size_t length = strlen(clean);
    int ok = fwrite(clean, 1, length, file) == length &&
             fwrite("\n", 1, 1, file) == 1;
    if (fclose(file) != 0) ok = 0;
    if (!ok) { remove(temp); return 0; }
    remove(path);
    if (rename(temp, path) != 0) { remove(temp); return 0; }
    return 1;
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

/* Render the selected ROM's cached preview into the shared CPU pixel buffer.
 * The caller must consume the pointer before the next render_build/reset. */
int launcher_preview_pixels(int game, const uint32_t **pixels, int *w, int *h)
{
    if (game < 0 || game >= N_GAMES || !prev_ok[game] || games[game].external_player)
        return 0;
    ensure_pack(game);
    render_build(&prev[game], game, 0);
    return render_pixels(pixels, w, h);
}

static void set_rom(int g, const char *path)
{
    if (g < 0 || g >= N_GAMES || !path) return;
    snprintf(settings.g[g].rom_path, sizeof settings.g[g].rom_path, "%s", path);
    rom_check(g);
    if (rs[g].ok) make_preview(g);
    else prev_ok[g] = 0;
}

static void romhack_title_from_path(const char *path, char *out, size_t n)
{
    if (!out || !n) return;
    out[0] = 0;
    const char *base = path_base(path ? path : "");
    if (!base || !base[0]) base = "Untitled ROM Hack";
    size_t len = strlen(base);
    const char *dot = strrchr(base, '.');
    if (dot && dot > base) len = (size_t)(dot - base);
    if (len >= n) len = n - 1;
    size_t w = 0;
    for (size_t i = 0; i < len && w + 1 < n; i++) {
        char ch = base[i];
        if (ch == '_') ch = ' ';
        out[w++] = ch;
    }
    out[w] = 0;
    while (w && out[w - 1] == ' ') out[--w] = 0;
    if (!w) snprintf(out, n, "Untitled ROM Hack");
}

static void romhack_migrate_legacy(int g)
{
    GameCfg *c = &settings.g[g];
    if (!c->hack_path[0]) return;
    for (int i = 0; i < c->romhack_count && i < MAX_ROMHACKS; i++) {
        if (!strcmp(c->romhacks[i].path, c->hack_path)) {
            c->romhack_selected = i;
            c->hack_path[0] = 0;
            return;
        }
    }
    if (c->romhack_count >= MAX_ROMHACKS) {
        c->hack_path[0] = 0;
        c->romhack_selected = -1;
        return;
    }
    int index = c->romhack_count++;
    snprintf(c->romhacks[index].path, sizeof c->romhacks[index].path, "%s", c->hack_path);
    romhack_title_from_path(c->hack_path, c->romhacks[index].title, sizeof c->romhacks[index].title);
    c->romhack_selected = index;
    c->hack_path[0] = 0;
}

static void romhack_add(int g, const char *path)
{
    if (g < 0 || g >= N_GAMES || !path || !path[0]) return;
    GameCfg *c = &settings.g[g];
    if (!rs[g].ok || !c->rom_path[0]) {
        launcher_toast("Choose a working original ROM on the Game tab first.");
        return;
    }
    for (int i = 0; i < c->romhack_count && i < MAX_ROMHACKS; i++) {
        if (c->romhacks[i].path[0] && !strcmp(c->romhacks[i].path, path)) {
            c->romhack_selected = i;
            sub[g] = SUB_ROMHACKS;
            make_preview(g);
            settings_save();
            launcher_toast("That ROM hack is already in this library.");
            return;
        }
    }
    if (c->romhack_count >= MAX_ROMHACKS) {
        launcher_toast("This game already has the maximum of 16 ROM hacks.");
        return;
    }

    RomStatus st;
    if (rom_load(g, c->rom_path, &st) != 0 || rom_apply_hack(path, &st) != 0) {
        hack_ok[g] = 0;
        snprintf(hack_msg[g], sizeof hack_msg[g], "%s", st.msg);
        launcher_toast(st.msg[0] ? st.msg : "Couldn't validate that ROM hack.");
        return;
    }

    int index = c->romhack_count++;
    RomHackEntry *entry = &c->romhacks[index];
    memset(entry, 0, sizeof *entry);
    snprintf(entry->path, sizeof entry->path, "%s", path);
    romhack_title_from_path(path, entry->title, sizeof entry->title);
    c->romhack_selected = index;
    sub[g] = SUB_ROMHACKS;
    settings_save();
    clear_romhack_thumbs_game(g);
    snprintf(hack_msg[g], sizeof hack_msg[g], "%s", st.msg);
    hack_ok[g] = 1;
    make_preview(g);
    launcher_toast("ROM hack added as a separate game profile. The original ROM file is unchanged.");
}

static void romhack_remove(int g, int index)
{
    if (g < 0 || g >= N_GAMES) return;
    GameCfg *c = &settings.g[g];
    if (index < 0 || index >= c->romhack_count || index >= MAX_ROMHACKS) return;
    if (romhack_title_editing_game == g && romhack_title_editing_index == index) {
        romhack_title_editing_game = romhack_title_editing_index = -1;
        SDL_StopTextInput();
    }
    if (c->romhack_selected == index) c->romhack_selected = -1;
    else if (c->romhack_selected > index) c->romhack_selected--;
    if (index + 1 < c->romhack_count)
        memmove(&c->romhacks[index], &c->romhacks[index + 1],
                (size_t)(c->romhack_count - index - 1) * sizeof c->romhacks[0]);
    c->romhack_count--;
    memset(&c->romhacks[c->romhack_count], 0, sizeof c->romhacks[0]);
    if (c->romhack_count == 0) c->romhack_selected = -1;
    clear_romhack_thumbs_game(g);
    settings_save();
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
    netplay_init();
    clear_state_thumbs();
    clear_romhack_thumbs();
    int migrated_legacy_hacks = 0;
    for (int g = 0; g < N_GAMES; g++) {
        if (settings.g[g].hack_path[0]) migrated_legacy_hacks = 1;
        romhack_migrate_legacy(g);
    }
    if (migrated_legacy_hacks) settings_save();
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
    clear_romhack_thumbs();
    for (int g = 0; g < N_GAMES; g++) rom_check(g);
    for (int g = 0; g < N_GAMES; g++) if (rs[g].ok && !prev_ok[g]) make_preview(g);
    loaded_pack_game = -1;
    bg_for_game = -1;
    audio_menu_apply();
    audio_menu_music(1);
    if (settings.menu_music) menu_music_notice_start();
}

void launcher_shutdown(void) { netplay_shutdown(); clear_state_thumbs(); clear_romhack_thumbs(); tex_collect_save(); }

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
    if (ui_scale < 0.75f) {
        float vx = ui_view_x0(), vw = ui_view_w();
        float left = vx + 170.0f;
        float nav_w = vw - 190.0f;
        if (nav_w < 620.0f) nav_w = 620.0f;
        if (tab < N_GAMES) {
            float w = nav_w / (float)N_GAMES;
            if (w > 220.0f) w = 220.0f;
            launcher_controller_x = left + tab * w + (w - 5.0f) * 0.5f;
            launcher_controller_y = 24.0f;
        } else {
            int count = N_TABS - TAB_MULTIPLAYER;
            float w = nav_w / (float)count;
            if (w > 220.0f) w = 220.0f;
            launcher_controller_x = left + (tab - TAB_MULTIPLAYER) * w + (w - 5.0f) * 0.5f;
            launcher_controller_y = 64.0f;
        }
        return;
    }
    /* Start on the selected sidebar entry on the desktop layout. */
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
    float view_x = ui_view_x0(), view_y = ui_view_y0();
    float view_w = ui_view_w(), view_h = ui_view_h();
    if (launcher_controller_x < view_x + 6.0f) launcher_controller_x = view_x + 6.0f;
    if (launcher_controller_x > view_x + view_w - 6.0f) launcher_controller_x = view_x + view_w - 6.0f;
    if (launcher_controller_y < view_y + 6.0f) launcher_controller_y = view_y + 6.0f;
    if (launcher_controller_y > view_y + view_h - 6.0f) launcher_controller_y = view_y + view_h - 6.0f;

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
    if ((c->scaling == SCALE_STRETCH || c->scaling == SCALE_PIXEL_STRETCH) && use_cfg_shape) { pw = w - 16; ph = h - 16; }
    if (ph > h - 16) { ph = h - 16; pw = ph * ar; }
    SDL_Rect r = to_px(x + (w - pw) * 0.5f, y + (h - ph) * 0.5f, pw, ph);
    render_draw(&r, (c->scaling == SCALE_PIXEL || c->scaling == SCALE_PIXEL_STRETCH) ? c->scaling : SCALE_SMOOTH);
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

static void romhack_select_profile(int g, int index)
{
    if (g < 0 || g >= N_GAMES) return;
    GameCfg *c = &settings.g[g];
    if (index < -1 || index >= c->romhack_count || index >= MAX_ROMHACKS) return;
    c->romhack_selected = index;
    settings_save();

    if (index < 0) {
        hack_ok[g] = 0;
        hack_msg[g][0] = 0;
        make_preview(g);
        return;
    }
    if (games[g].external_player) {
        RomStatus st;
        if (rom_load(g, c->rom_path, &st) ||
            rom_apply_hack(c->romhacks[index].path, &st)) {
            hack_ok[g] = 0;
            snprintf(hack_msg[g], sizeof hack_msg[g], "%s", st.msg);
            launcher_toast(st.msg);
        } else {
            hack_ok[g] = 1;
            snprintf(hack_msg[g], sizeof hack_msg[g], "%s", st.msg);
        }
        return;
    }
    make_preview(g);
    if (!hack_ok[g] && hack_msg[g][0]) launcher_toast(hack_msg[g]);
}

static void draw_romhack_thumb(int g, int index, float x, float y, float w, float h)
{
    ui_rrect(x, y, w, h, 9, HEX(0x0A0D16));
    SDL_Texture *t = index >= 0 ? romhack_thumb_get(g, index) : NULL;
    if (t) {
        int tw = 0, th = 0;
        SDL_QueryTexture(t, NULL, NULL, &tw, &th);
        if (tw > 0 && th > 0) draw_cover(t, tw, th, x + 3, y + 3, w - 6, h - 6);
        return;
    }
    uint32_t accent = HEX(index < 0 ? games[g].accent : ui_accent);
    ui_rrect(x + 4, y + 4, w - 8, h - 8, 7, (accent & 0xFFFFFF00u) | 45u);
    ui_text_c(F_BOLD, 14, x + w * 0.5f, y + h * 0.5f - 9,
              index < 0 ? HEX(games[g].accent) : C_TEXT,
              index < 0 ? "ORIGINAL" : "ROM HACK");
    ui_text_c(F_REG, 10, x + w * 0.5f, y + h * 0.5f + 12, C_DIM,
              index < 0 ? "BASE ROM" : "ADD THUMBNAIL");
}

static void sub_romhacks(int g, float x, float y)
{
    GameCfg *c = &settings.g[g];
    const float lw = 356.0f, gap = 14.0f, rw = 808.0f - lw - gap;
    float rx = x + lw + gap;
    card(x, y, lw, 476, "ROM HACK LIBRARY");
    ui_text(F_REG, 11, x + 18, y + 34, C_DIM,
            "Choose the original or a separate patched profile.");
    if (ui_button(x + lw - 105, y + 10, 88, 30, "Add hack +", B_PRIMARY,
                  rs[g].ok && c->romhack_count < MAX_ROMHACKS)) {
        char path[1100];
        if (dlg_pick(DLG_PATCH, "Add a ROM hack or patched ROM", path, sizeof path))
            romhack_add(g, path);
    }

    const float list_x = x + 10, list_y = y + 58;
    const float list_w = lw - 20, list_h = 350.0f;
    int count = c->romhack_count;
    if (count < 0) count = 0;
    if (count > MAX_ROMHACKS) count = MAX_ROMHACKS;
    float row_h = 68.0f, row_gap = 7.0f, row_step = row_h + row_gap;
    float content_h = (count + 1) * row_step;
    float max_scroll = content_h > list_h ? content_h - list_h : 0.0f;
    if (ui_hover(list_x, list_y, list_w, list_h) && ui_mouse.wheel != 0)
        romhack_scroll[g] -= ui_mouse.wheel * 52.0f;
    if (romhack_scroll[g] < 0) romhack_scroll[g] = 0;
    if (romhack_scroll[g] > max_scroll) romhack_scroll[g] = max_scroll;

    ui_clip(list_x, list_y, list_w, list_h);
    for (int row = 0; row <= count; row++) {
        int index = row - 1; /* Row zero is always the untouched original game. */
        if (index >= 0 && !c->romhacks[index].path[0]) continue;
        float ry = list_y + row * row_step - romhack_scroll[g];
        if (ry + row_h < list_y || ry > list_y + list_h) continue;

        int selected = c->romhack_selected == index;
        int over = 0;
        int clicked = clickable(list_x + 2, ry, list_w - 4, row_h, &over);
        uint32_t bg = selected ? ((HEX(ui_accent) & 0xFFFFFF00u) | 50u)
                               : (over ? C_BTN_H : C_PANEL2);
        ui_rrect(list_x + 2, ry, list_w - 4, row_h, 9, bg);
        if (selected) ui_stroke(list_x + 2, ry, list_w - 4, row_h, 9, 1.5f, HEX(ui_accent));
        draw_romhack_thumb(g, index, list_x + 8, ry + 7, 72, 54);
        const char *title = index < 0 ? "Original game" :
            (c->romhacks[index].title[0] ? c->romhacks[index].title : "Untitled ROM Hack");
        ui_text_fit_tail(F_BOLD, 12, list_x + 90, ry + 11, list_w - 104, C_TEXT, title);
        ui_text_fit_tail(F_REG, 10, list_x + 90, ry + 34, list_w - 104, C_DIM,
                         index < 0 ? "Unmodified base ROM" : path_base(c->romhacks[index].path));
        if (clicked) romhack_select_profile(g, index);
    }
    ui_unclip();
    if (content_h > list_h) {
        float track_x = x + lw - 7, track_y = list_y;
        float track_h = list_h;
        float thumb_h = track_h * list_h / content_h;
        if (thumb_h < 26) thumb_h = 26;
        float thumb_y = track_y + (track_h - thumb_h) * romhack_scroll[g] / max_scroll;
        ui_rrect(track_x, track_y, 3, track_h, 2, C_LINE);
        ui_rrect(track_x - 1, thumb_y, 5, thumb_h, 3, HEX(ui_accent));
    }
    ui_text_fit(F_REG, 10, x + 14, y + 438, lw - 28, C_DIM,
                "Scroll to browse. The original ROM file is never modified.");

    card(rx, y, rw, 476, "SELECTED VERSION");
    int index = c->romhack_selected;
    if (index < 0 || index >= count || !c->romhacks[index].path[0]) {
        draw_romhack_thumb(g, -1, rx + 18, y + 50, 164, 122);
        ui_text_fit_tail(F_BOLD, 18, rx + 198, y + 54, rw - 216, C_TEXT, games[g].name);
        ui_text(F_REG, 12, rx + 198, y + 83, C_OK, "Original game");
        ui_text_wrap(F_REG, 12, rx + 198, y + 108, rw - 216, C_DIM,
                     "This uses the main ROM file with no hack applied.", 3);
        label(rx + 18, y + 190, "MAIN ROM");
        path_box(rx + 18, y + 212, rw - 36, c->rom_path, "Choose the original ROM on the Game tab");
        status_line(rx + 18, y + 262, rw - 36, rs[g].ok ? 1 : 3,
                    rs[g].ok ? "Ready to play unchanged." : rs[g].msg);
        ui_text_wrap(F_REG, 12, rx + 18, y + 305, rw - 36, C_DIM,
                     "Selecting a ROM hack creates a separate play profile. Your base ROM stays as-is.", 3);
    } else {
        RomHackEntry *entry = &c->romhacks[index];
        draw_romhack_thumb(g, index, rx + 18, y + 50, 164, 122);
        int editing = romhack_title_editing_game == g &&
                      romhack_title_editing_index == index;
        if (editing) ui_stroke(rx + 194, y + 49, rw - 212, 31, 6, 1.5f, C_WARN);
        ui_text_fit_tail(F_BOLD, 17, rx + 200, y + 54, rw - 216, C_TEXT,
                         entry->title[0] ? entry->title : "Untitled ROM Hack");
        ui_text_fit_tail(F_REG, 11, rx + 200, y + 82, rw - 216, C_DIM,
                         path_base(entry->path));
        int exists = file_exists(entry->path);
        status_line(rx + 198, y + 110, rw - 214, exists ? (hack_ok[g] ? 1 : 2) : 3,
                    !exists ? "Hack file is missing." :
                    (hack_ok[g] ? "Profile selected and ready." :
                     (hack_msg[g][0] ? hack_msg[g] : "Select profile to validate.")));

        label(rx + 18, y + 190, "ROM HACK FILE");
        path_box(rx + 18, y + 212, rw - 36, entry->path, "No file selected");
        float by = y + 264;
        if (ui_button(rx + 18, by, 94, 32, editing ? "Editing…" : "Rename", B_NORMAL, !editing)) {
            romhack_title_editing_game = g;
            romhack_title_editing_index = index;
            snprintf(romhack_title_before, sizeof romhack_title_before, "%s", entry->title);
            SDL_StartTextInput();
            launcher_toast("Type the title; press Enter to save or Esc to cancel.");
        }
        if (ui_button(rx + 120, by, 132, 32, "Choose thumbnail", B_NORMAL, !editing)) {
            char path[1100];
            if (dlg_pick(DLG_IMAGE, "Choose this ROM hack's thumbnail", path, sizeof path)) {
                snprintf(entry->thumbnail, sizeof entry->thumbnail, "%s", path);
                clear_romhack_thumbs_game(g);
                settings_save();
            }
        }
        if (ui_button(rx + 260, by, 80, 32, "Remove", B_DANGER, !editing)) {
            romhack_remove(g, index);
            launcher_toast("ROM hack removed from the library.");
        }
        if (editing) ui_text(F_REG, 10, rx + 18, y + 305, C_WARN,
                             "Typing changes the title shown in the library.");
        else ui_text_wrap(F_REG, 11, rx + 18, y + 305, rw - 36, C_DIM,
                          "This profile is applied at launch. The base ROM and its save remain separate.", 3);
    }
    if (!rs[g].ok) {
        ui_text_wrap(F_REG, 11, rx + 18, y + 400, rw - 36, C_WARN,
                     "Add a valid original ROM on the Game tab before playing a hack.", 2);
    } else {
        ui_text_fit_tail(F_REG, 10, rx + 18, y + 442, rw - 36, C_DIM,
                         index < 0 ? "Play launches the original game." : "Play launches the selected ROM hack.");
    }
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
    /* ROM hack profiles live separately from the source ROM path. */
    label(rx + 18, y + 150, "ROM hack profiles");
    ui_text_wrap(F_REG, 12, rx + 18, y + 172, rw - 36, C_DIM,
                 "Add patches or already-patched ROMs on the ROM Hacks tab. Each entry is a separate selectable game profile; the original ROM file is never changed.", 3);
    char profile_label[128];
    if (c->romhack_selected >= 0 && c->romhack_selected < c->romhack_count)
        snprintf(profile_label, sizeof profile_label, "Selected: %s",
                 c->romhacks[c->romhack_selected].title[0] ? c->romhacks[c->romhack_selected].title : "ROM hack");
    else
        snprintf(profile_label, sizeof profile_label, "Selected: Original game");
    ui_text_fit_tail(F_REG, 12, rx + 18, y + 224, rw - 36, C_TEXT, profile_label);
    if (ui_button(rx + 18, y + 250, rw - 36, 34, "Manage ROM Hacks…", B_NORMAL, 1))
        sub[g] = SUB_ROMHACKS;
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
    hint_area(x + 18, yy, 500, 60, "Pixel-perfect keeps integer scaling. Smooth blends. Stretch fills the window; pixel-perfect stretch fills it without smoothing.");
    yy += 66;
    label(x + 18, yy, "Starting window size"); ui_seg(x + 18, yy + 22, 500, 36, size_names, N_SIZE, &c->size);

    float rx = x + 548, rw = 260;
    const float look_compact = 0.0f; /* Dual-screen placement is a launcher-wide Android option. */
    card(rx, y, rw, 476, "LOOK");
    draw_preview(g, rx + 16, y + 40, rw - 32, 190 - look_compact, 1);
    label(rx + 18, y + 246 - look_compact, "Background picture");
    path_box(rx + 18, y + 268 - look_compact, rw - 36, c->bg_path, "None (solid colour)");
    if (ui_button(rx + 18, y + 312 - look_compact, 110, 34, "Choose…", B_NORMAL, 1)) {
        char p[1100];
        if (dlg_pick(DLG_IMAGE, "Choose a background picture", p, sizeof p)) { snprintf(c->bg_path, sizeof c->bg_path, "%s", p); ensure_background(g); if (bg_msg[0]) launcher_toast(bg_msg); }
    }
    if (ui_button(rx + 136, y + 312 - look_compact, 106, 34, "Remove", B_GHOST, c->bg_path[0] != 0)) { c->bg_path[0] = 0; ensure_background(g); }
    char t[64];
    if (bg_frames() > 1) snprintf(t, sizeof t, "Animated, %d frames", bg_frames());
    else snprintf(t, sizeof t, "PNG, JPG, BMP, TGA or animated GIF");
    ui_text_fit(F_REG, 11, rx + 18, y + 354 - look_compact, rw - 36, C_DIM, t);
    label(rx + 18, y + 380 - look_compact, "Darken background"); snprintf(t, sizeof t, "%d%%", c->bg_dim); ui_text_r(F_REG, 13, rx + rw - 18, y + 380 - look_compact, C_TEXT, t);
    ui_slider(rx + 22, y + 402 - look_compact, rw - 44, &c->bg_dim, 0, 80);
    if (games[g].wide_l + games[g].wide_r > 0) {
        label(rx + 18, y + 428 - look_compact, "Widescreen");
        snprintf(t, sizeof t, "%d%%", c->wide); ui_text_r(F_REG, 13, rx + rw - 18, y + 428 - look_compact, C_TEXT, t);
        if (ui_slider(rx + 22, y + 450 - look_compact, rw - 44, &c->wide, 0, 100)) wide_dirty = g + 1;
        hint_area(rx + 18, y + 424 - look_compact, rw - 36, 40, "Shows more of the level on the sides. 0% is the original 10:9 screen.");
        if (wide_note[g]) ui_text_fit(F_REG, 11, rx + 18, y + 468 - 6 - look_compact, rw - 36, C_WARN, "This ROM version isn't supported for widescreen.");
    } else {
        label(rx + 18, y + 428 - look_compact, "Widescreen");
        ui_text(F_REG, 12, rx + 18, y + 450 - look_compact, C_DIM, "Not available for this game");
    }
    /* Dual-screen and Auto Pipe placement moved to Launcher Display settings. */
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

static int *mp_rgb_ptr(GameCfg *c, int player_number)
{
    if (!c) return NULL;
    if (player_number == 2) return &c->p2_rgb;
    if (player_number == 3) return &c->p3_rgb;
    if (player_number == 4) return &c->p4_rgb;
    return NULL;
}

static int *mp_custom_ptr(GameCfg *c, int player_number)
{
    if (!c) return NULL;
    if (player_number == 2) return &c->p2_custom;
    if (player_number == 3) return &c->p3_custom;
    if (player_number == 4) return &c->p4_custom;
    return NULL;
}

static uint32_t mp_effective_player_rgb(const GameCfg *c, int player_number)
{
    if (!c) return 0x2CA83Du;
    int custom = player_number == 2 ? c->p2_custom :
                 (player_number == 3 ? c->p3_custom : c->p4_custom);
    int rgb = player_number == 2 ? c->p2_rgb :
              (player_number == 3 ? c->p3_rgb : c->p4_rgb);
    if (custom) return (uint32_t)rgb & 0xFFFFFFu;
    return luigi_colors[mp_color_index(c, player_number)].swatch;
}

static void mp_reset_player_name(GameCfg *c, int player_number)
{
    char *name = mp_name_ptr(c, player_number);
    if (name) snprintf(name, 32, "%s", mp_default_name(player_number));
}

static void mp_reset_player_color(GameCfg *c, int player_number)
{
    if (!c) return;
    if (player_number == 2) {
        c->p2_color = LUIGI_GREEN;
        c->p2_rgb = 0x2CA83D;
        c->p2_custom = 0;
    } else if (player_number == 3) {
        c->p3_color = LUIGI_BLUE;
        c->p3_rgb = 0x2676D9;
        c->p3_custom = 0;
    } else if (player_number == 4) {
        c->p4_color = LUIGI_YELLOW;
        c->p4_rgb = 0xD8B51A;
        c->p4_custom = 0;
    }
}

static void mp_rgb_to_hsv(uint32_t rgb, float *h, float *s, float *v)
{
    float r = ((rgb >> 16) & 255u) / 255.0f;
    float g = ((rgb >> 8) & 255u) / 255.0f;
    float b = (rgb & 255u) / 255.0f;
    float hi = fmaxf(r, fmaxf(g, b)), lo = fminf(r, fminf(g, b)), d = hi - lo;
    *v = hi;
    *s = hi > 0.0001f ? d / hi : 0.0f;
    *h = 0.0f;
    if (d > 0.0001f) {
        if (hi == r) *h = fmodf((g - b) / d, 6.0f) / 6.0f;
        else if (hi == g) *h = ((b - r) / d + 2.0f) / 6.0f;
        else *h = ((r - g) / d + 4.0f) / 6.0f;
        if (*h < 0.0f) *h += 1.0f;
    }
}

static uint32_t mp_hsv_to_rgb(float h, float s, float v)
{
    float c = v * s, hp = h * 6.0f;
    float z = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    if (hp < 1) { r = c; g = z; }
    else if (hp < 2) { r = z; g = c; }
    else if (hp < 3) { g = c; b = z; }
    else if (hp < 4) { g = z; b = c; }
    else if (hp < 5) { r = z; b = c; }
    else { r = c; b = z; }
    float m = v - c;
    unsigned ri = (unsigned)lrintf((r + m) * 255.0f);
    unsigned gi = (unsigned)lrintf((g + m) * 255.0f);
    unsigned bi = (unsigned)lrintf((b + m) * 255.0f);
    return ((ri & 255u) << 16) | ((gi & 255u) << 8) | (bi & 255u);
}

static void mp_draw_color_editor(GameCfg *c, float x, float y)
{
    static const char *player_labels[3] = {"Player 2", "Player 3", "Player 4"};
    if (mp_color_selected < 0 || mp_color_selected > 2) mp_color_selected = 0;
    ui_rect(x + 18, y + 270, 772, 1, C_LINE);
    label(x + 18, y + 281, "Character color");
    ui_seg(x + 150, y + 276, 390, 32, player_labels, 3, &mp_color_selected);

    int player = mp_color_selected + 2;
    int *rgb_store = mp_rgb_ptr(c, player);
    int *custom_store = mp_custom_ptr(c, player);
    if (!rgb_store || !custom_store) return;
    uint32_t rgb = mp_effective_player_rgb(c, player);
    const float wx = x + 90.0f, wy = y + 365.0f, wr = 62.0f;
    ui_color_wheel(wx, wy, wr, rgb);
    ui_text_c(F_REG, 10, wx, wy + wr + 3, C_DIM, "Hue + saturation");

    float dx = ui_mouse.x - wx, dy = ui_mouse.y - wy;
    float dist = sqrtf(dx * dx + dy * dy);
    if (ui_mouse.down && dist <= wr) {
        float hue = atan2f(-dy, dx) / 6.28318530718f;
        if (hue < 0.0f) hue += 1.0f;
        float sat = dist / wr, val;
        float old_h, old_s;
        mp_rgb_to_hsv(rgb, &old_h, &old_s, &val);
        (void)old_h; (void)old_s;
        if (val < 0.20f) val = 0.20f;
        *rgb_store = (int)mp_hsv_to_rgb(hue, sat, val);
        *custom_store = 1;
        rgb = (uint32_t)*rgb_store;
    }

    ui_text(F_BOLD, 13, x + 190, y + 320, C_TEXT, player_labels[mp_color_selected]);
    ui_rrect(x + 190, y + 345, 70, 36, 8, HEX(rgb));
    ui_stroke(x + 190, y + 345, 70, 36, 8, 1, RGBA(255,255,255,120));
    char hex[32];
    snprintf(hex, sizeof hex, "#%06X", (unsigned)(rgb & 0xFFFFFFu));
    ui_text(F_BOLD, 15, x + 276, y + 354, C_TEXT, hex);
    ui_text(F_REG, 11, x + 190, y + 388, C_MUTED, "Brightness");
    float hue, sat, val;
    mp_rgb_to_hsv(rgb, &hue, &sat, &val);
    int brightness = (int)lrintf(val * 100.0f);
    if (brightness < 20) brightness = 20;
    if (brightness > 100) brightness = 100;
    int bright_changed = ui_slider(x + 268, y + 386, 260, &brightness, 20, 100);
    char pct[16];
    snprintf(pct, sizeof pct, "%d%%", brightness);
    ui_text_r(F_REG, 12, x + 568, y + 388, C_TEXT, pct);
    if (bright_changed) {
        *rgb_store = (int)mp_hsv_to_rgb(hue, sat, brightness / 100.0f);
        *custom_store = 1;
        rgb = (uint32_t)*rgb_store;
    }
    ui_text_wrap(F_REG, 11, x + 190, y + 414, 410, C_DIM,
                 "Drag on the wheel to pick a hue and saturation; use brightness for darker shades.", 2);
    if (ui_button(x + 620, y + 344, 148, 36, "Reset color", B_NORMAL, 1)) {
        mp_reset_player_color(c, player);
        settings_save();
        launcher_toast("Character color reset to default.");
    }
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

        char file_id[128];
        launcher_game_file_id(g, file_id, sizeof file_id);
        snprintf(path, sizeof path, "%sstates/%s.slot%d.pcs", settings_dir(), file_id, slot);
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

    char file_id[128];
    launcher_game_file_id(g, file_id, sizeof file_id);
    snprintf(path, sizeof path, "%sstates/%s.suspend.pcs", settings_dir(), file_id);
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
    ui_rrect(x, y, w, 32, 8, active ? mixc(C_BTN, HEX(ui_accent), 0.38f) :
             (over ? C_BTN_H : C_BTN));
    const char *current = name[0] ? name : (active ? "Type a name…" : "Click to rename");
    ui_text_fit(F_REG, 13, x + 12, y + 8, w - 42, name[0] ? C_TEXT : C_DIM, current);
    ui_rrect(x + w - 25, y + 6, 14, 20, 5, HEX(mp_effective_player_rgb(c, player_number)));
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
        mp_controller_view = 0;
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
        mp_name_field(c, p, x + 100, ry, 260);
        if (ui_button(x + 370, ry + 1, 72, 30, "Reset", B_GHOST, 1)) {
            mp_reset_player_name(c, p);
            if (multiplayer_name_editing == p) {
                multiplayer_name_editing = 0;
                SDL_StopTextInput();
            }
            settings_save();
            launcher_toast("Player name reset to default.");
        }
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

    mp_draw_color_editor(c, x, y);
    ui_rect(x + 18, y + 442, 772, 1, C_LINE);
    ui_text_wrap(F_REG, 11, x + 18, y + 451, 772, C_DIM,
                 "Use Multiplayer Controllers for independent controller assignments and respawn bindings. These color settings apply to local co-op and can also be used by online player profiles.", 2);
}
 
/* ------------------------------------------------------------------ game tab: DualSense */
static void tab_mp_controllers(float x, float y)
{
    GameCfg *c = &settings.g[mp_config_game];
    /* SML2 has genuine per-character sound output now; its old custom
     * overlay is obsolete. SML1 keeps the fallback bank for its clone path. */
    if (mp_config_game == GAME_SML2) mp_controller_view = 0;
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
    if (mp_config_game == GAME_SML &&
        ui_button(x + 206, y + 100, 180, 30, "Character sounds",
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
                HEX(mp_effective_player_rgb(c, player + 1));
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
            "Jump", "Fireball", "Power up", "Power down", "Death", "Respawn", "Coin"
        };
        static const int players[3] = {2, 3, 4};
        for (int col = 0; col < 3; col++) {
            int player = players[col];
            float bx = x + 18 + col * 258.0f;
            float cw = 250.0f;
            ui_rrect(bx, y + 164, cw, 24, 7, RGBA(255,255,255,8));
            char player_title[56];
            char *name = mp_name_ptr(c, player);
            snprintf(player_title, sizeof player_title, "P%d  %.18s", player,
                     name && name[0] ? name : mp_default_name(player));
            ui_text_fit(F_BOLD, 12, bx + 8, y + 169, cw - 16,
                        HEX(mp_effective_player_rgb(c, player)), player_title);

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
/* ------------------------------------------------------------------ menu music playlist + animated preview */
static void menu_music_title_from_path(const char *path, char *out, size_t n)
{
    const char *base;
    size_t len, w = 0;
    if (!out || !n) return;
    out[0] = 0;
    if (!path || !path[0]) {
        snprintf(out, n, "PipeClean Chiptune");
        return;
    }
    base = path_base(path);
    if (!base || !base[0]) base = path;
    len = strlen(base);
    const char *dot = strrchr(base, '.');
    if (dot && dot > base) len = (size_t)(dot - base);
    for (size_t i = 0; i < len && w + 1 < n; i++) {
        char ch = base[i];
        if (ch == '_') ch = ' ';
        out[w++] = ch;
    }
    out[w] = 0;
    while (w && out[w - 1] == ' ') out[--w] = 0;
    if (!w) snprintf(out, n, "Untitled track");
}

static void menu_music_notice_start(void)
{
    if (!settings.menu_music) return;
    music_notice_serial++;
    if (!music_notice_serial) music_notice_serial = 1;
    music_notice_age = 0.0f;
    music_notice_deferred = tab == TAB_AUDIO;
    snprintf(music_notice_path, sizeof music_notice_path, "%s", settings.menu_music_path);
}

int launcher_music_notification_serial(void)
{
    /* If a track changed on Audio & Menu, wait until the user leaves that
       page before exposing the new serial to the Android Companion Deck. */
    if (tab != TAB_AUDIO && music_notice_age >= 0.0f && music_notice_age <= 2.75f)
        music_notice_announced_serial = music_notice_serial;
    return (int)music_notice_announced_serial;
}

int launcher_music_notification_allowed(void)
{
    return tab != TAB_AUDIO;
}

static void menu_music_set_builtin(void)
{
    settings.menu_music_path[0] = 0;
    settings.menu_music = 1;
    audio_menu_apply();
    audio_menu_music(1);
    audio_music_preview();
    menu_music_notice_start();
    settings_save();
}

void launcher_menu_music_select(int index)
{
    if (index < 0 || index >= settings.menu_playlist_count ||
        index >= MAX_MENU_TRACKS || !settings.menu_playlist[index][0]) return;
    settings.menu_playlist_selected = index;
    snprintf(settings.menu_music_path, sizeof settings.menu_music_path, "%s",
             settings.menu_playlist[index]);
    settings.menu_music = 1;
    audio_menu_apply();
    audio_menu_music(1);
    audio_music_preview();
    menu_music_notice_start();
    settings_save();
}

void launcher_menu_music_step(int delta)
{
    int count = settings.menu_playlist_count;
    if (count <= 0) {
        menu_music_set_builtin();
        return;
    }
    int current = settings.menu_playlist_selected;
    if (current < 0 || current >= count) current = 0;
    current = (current + (delta < 0 ? -1 : 1) + count) % count;
    launcher_menu_music_select(current);
}

void launcher_menu_music_toggle(void)
{
    settings.menu_music = !settings.menu_music;
    audio_menu_apply();
    audio_menu_music(1);
    if (settings.menu_music) {
        audio_music_preview();
        menu_music_notice_start();
    }
    settings_save();
}

static void menu_playlist_add_path(const char *path)
{
    if (!path || !path[0]) return;
    if (strlen(path) >= sizeof settings.menu_playlist[0]) {
        launcher_toast("That music path is too long to save.");
        return;
    }
    for (int i = 0; i < settings.menu_playlist_count && i < MAX_MENU_TRACKS; i++) {
        if (!strcmp(settings.menu_playlist[i], path)) {
            launcher_menu_music_select(i);
            launcher_toast("That track is already in the playlist.");
            return;
        }
    }
    if (settings.menu_playlist_count >= MAX_MENU_TRACKS) {
        launcher_toast("Playlist is full. Remove a track before adding another.");
        return;
    }
    int i = settings.menu_playlist_count++;
    snprintf(settings.menu_playlist[i], sizeof settings.menu_playlist[i], "%s", path);
    menu_playlist_page = i / 3;
    launcher_menu_music_select(i);
    launcher_toast("Track added to the menu playlist.");
}

static void menu_playlist_remove_selected(void)
{
    int count = settings.menu_playlist_count;
    int index = settings.menu_playlist_selected;
    if (count <= 0 || index < 0 || index >= count) {
        launcher_toast("Select a playlist track to remove.");
        return;
    }
    for (int i = index; i + 1 < count; i++)
        snprintf(settings.menu_playlist[i], sizeof settings.menu_playlist[i], "%s",
                 settings.menu_playlist[i + 1]);
    settings.menu_playlist[--count][0] = 0;
    settings.menu_playlist_count = count;
    if (count == 0) {
        settings.menu_playlist_selected = 0;
        menu_music_set_builtin();
    } else {
        if (index >= count) index = count - 1;
        settings.menu_playlist_selected = index;
        launcher_menu_music_select(index);
    }
    menu_playlist_page = count > 0 ? (count - 1) / 3 : 0;
    settings_save();
    launcher_toast("Track removed from the menu playlist.");
}

static void menu_music_notice_draw(float vx, float vy)
{
    if (music_notice_age < 0.0f || music_notice_age > 2.75f || tab == TAB_AUDIO)
        return;
#if defined(__ANDROID__)
    if (settings.launcher_dual_screen) return;
#endif
    const float w = 330.0f, h = 66.0f;
    const float in_time = 0.30f, out_start = 2.34f, out_time = 0.36f;
    float offset = 0.0f;
    if (music_notice_age < in_time) {
        float p = music_notice_age / in_time;
        float e = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);
        offset = -w * (1.0f - e);
    } else if (music_notice_age > out_start) {
        float p = (music_notice_age - out_start) / out_time;
        if (p > 1.0f) p = 1.0f;
        float e = p * p * (3.0f - 2.0f * p);
        offset = -w * e;
    }
    float x = vx + 16.0f + offset;
    float y = vy + 98.0f;
    char title[512];
    menu_music_title_from_path(music_notice_path, title, sizeof title);
    float cx = x + 36.0f, cy = y + h * 0.5f;
    ui_shadow(x, y, w, h, 13.0f, 9.0f, RGBA(0, 0, 0, 115));
    ui_rrect(x, y, w, h, 13.0f, RGBA(17, 23, 34, 245));
    ui_stroke(x, y, w, h, 13.0f, 1.0f, RGBA(92, 111, 147, 230));
    ui_rrect(cx - 24, cy - 24, 48, 48, 24, HEX(0x080A10));
    ui_stroke(cx - 22, cy - 22, 44, 44, 22, 1, HEX(0x495166));
    ui_stroke(cx - 16, cy - 16, 32, 32, 16, 1, HEX(0x30394C));
    ui_stroke(cx - 10, cy - 10, 20, 20, 10, 1, HEX(0x30394C));
    float angle = anim_clock * 5.2f;
    ui_line(cx, cy, cx + cosf(angle) * 17.0f, cy + sinf(angle) * 17.0f,
            2, HEX(0xE9C66D));
    ui_rrect(cx - 6, cy - 6, 12, 12, 6, HEX(ui_accent));
    ui_rrect(cx - 1.5f, cy - 1.5f, 3, 3, 1.5f, HEX(0x0A0D14));
    ui_text(F_BOLD, 8, x + 68, y + 10, C_OK, "NOW PLAYING");
    ui_text_fit(F_BOLD, 13, x + 68, y + 24, w - 82, C_TEXT, title);
    ui_text_fit(F_REG, 9, x + 68, y + 45, w - 82, C_MUTED,
                music_notice_path[0] ? "PipeClean menu playlist" : "PipeClean built-in menu music");
}



static void menu_playlist_import_dialog(void)
{
    char path[1100];
    if (!dlg_pick(DLG_AUDIO, "Add a track to the menu playlist", path, sizeof path)) return;
    menu_playlist_add_path(path);
}

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
    if (ui_toggle(x + lw - 18 - 44, y + 252, &s->menu_music)) {
        audio_menu_apply();
        audio_menu_music(1);
        if (s->menu_music) {
            audio_music_preview();
            menu_music_notice_start();
        }
        settings_save();
    }
    label(x + 18, y + 296, "Volume");
    if (ui_slider(x + 100, y + 294, lw - 100 - 70, &s->menu_music_vol, 0, 100)) {
        audio_menu_apply();
        settings_save();
    }
    snprintf(t, sizeof t, "%d%%", s->menu_music_vol); ui_text_r(F_REG, 13, x + lw - 18, y + 296, C_TEXT, t);
    ui_text(F_BOLD, 10, x + 18, y + 321, C_TEXT, "PLAYLIST");
    snprintf(t, sizeof t, "%02d / %02d", s->menu_playlist_count, MAX_MENU_TRACKS);
    ui_text_r(F_REG, 9, x + lw - 122, y + 322, C_DIM, t);
    int max_page = s->menu_playlist_count > 0 ? (s->menu_playlist_count - 1) / 3 : 0;
    if (menu_playlist_page > max_page) menu_playlist_page = max_page;
    if (menu_playlist_page < 0) menu_playlist_page = 0;
    if (ui_button(x + lw - 92, y + 316, 36, 21, "Prev", B_GHOST, max_page > 0))
        menu_playlist_page = (menu_playlist_page + max_page) % (max_page + 1);
    if (ui_button(x + lw - 50, y + 316, 32, 21, "Next", B_GHOST, max_page > 0))
        menu_playlist_page = (menu_playlist_page + 1) % (max_page + 1);

    for (int row = 0; row < 3; row++) {
        int index = menu_playlist_page * 3 + row;
        float ry = y + 340 + row * 24.0f;
        if (index < s->menu_playlist_count && index < MAX_MENU_TRACKS) {
            char title[512];
            menu_music_title_from_path(s->menu_playlist[index], title, sizeof title);
            int active = s->menu_music_path[0] && !strcmp(s->menu_music_path, s->menu_playlist[index]);
            if (ui_button(x + 18, ry, lw - 36, 22, title,
                          active ? B_PRIMARY : B_GHOST, 1))
                launcher_menu_music_select(index);
        } else {
            ui_rrect(x + 18, ry, lw - 36, 22, 6, HEX(0x171D2A));
            ui_text_fit(F_REG, 9, x + 28, ry + 5, lw - 56, C_DIM,
                        s->menu_playlist_count ? "Empty playlist slot" : "Add a WAV, MP3, OGG or FLAC track");
        }
    }

    float action_w = (lw - 36 - 18) / 4.0f;
    float action_y = y + 421;
    if (ui_button(x + 18, action_y, action_w, 28, "Add track", B_NORMAL, 1))
        menu_playlist_import_dialog();
    if (ui_button(x + 18 + action_w + 6, action_y, action_w, 28, "Remove", B_GHOST,
                  s->menu_playlist_count > 0))
        menu_playlist_remove_selected();
    if (ui_button(x + 18 + (action_w + 6) * 2, action_y, action_w, 28, "Built-in", B_GHOST,
                  s->menu_music_path[0] != 0))
        menu_music_set_builtin();
    if (ui_button(x + 18 + (action_w + 6) * 3, action_y, action_w, 28, "Restart", B_GHOST, 1)) {
        settings.menu_music = 1;
        audio_menu_apply();
        audio_music_preview();
        audio_menu_music(1);
        menu_music_notice_start();
        settings_save();
    }

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

/* ------------------------------------------------------------------ online session/profile tab */
static const char *online_game_name(int game)
{
    if (game == GAME_SML) return games[GAME_SML].name;
    if (game == GAME_SML2) return games[GAME_SML2].name;
    return "Choose a game";
}

static void online_report_local_rom(int game)
{
    char err[256];
    const CartInfo *ci;
    online_rom_reported_game = game;
    if (launcher_prepare(game, err, sizeof err)) {
        (void)netplay_lobby_report_rom(game, 0, 0);
        launcher_toast(err[0] ? err : "Choose a valid ROM for this game first.");
        return;
    }
    ci = cart_info();
    if (!ci || !ci->size || !ci->crc) {
        (void)netplay_lobby_report_rom(game, 0, 0);
        launcher_toast("Couldn't calculate the selected ROM checksum.");
        return;
    }
    if (!netplay_lobby_report_rom(game, ci->crc, 1)) {
        online_rom_reported_game = -1;
        launcher_toast("Couldn't submit the ROM check. Try again.");
        return;
    }
    launcher_toast("ROM checksum sent. Waiting for the other player to finish checking.");
}

static void online_commit_shared_name(void)
{
    int game = netplay_lobby_get_game();
    int slot = online_save_name_slot;
    if (slot < 0 || slot >= NETPLAY_SHARED_SLOT_COUNT ||
        (game != GAME_SML && game != GAME_SML2) || !online_save_name_buffer[0]) {
        online_save_name_editing = 0;
        online_save_name_slot = -1;
        SDL_StopTextInput();
        return;
    }
    if (!launcher_shared_slot_name_set(game, slot, online_save_name_buffer)) {
        launcher_toast("That save-slot name is invalid.");
    } else if (!netplay_shared_slot_rename(slot, online_save_name_buffer)) {
        launcher_toast("Name saved locally, but couldn't sync it to the player.");
    } else {
        launcher_toast("Shared save-slot name updated.");
    }
    online_save_name_editing = 0;
    online_save_name_slot = -1;
    SDL_StopTextInput();
}

static void online_begin_shared_name_edit(int game, int slot)
{
    if (slot < 0 || slot >= NETPLAY_SHARED_SLOT_COUNT || !netplay_is_host()) return;
    online_save_name_slot = slot;
    netplay_shared_slot_name(slot, online_save_name_buffer, sizeof online_save_name_buffer);
    online_save_name_editing = 1;
    online_name_editing = online_code_editing = online_server_editing = online_chat_editing = 0;
    SDL_StartTextInput();
}

static void tab_online_lobby(float x, float y, const NetplayInfo *np)
{
    const int connected = np->state == NETPLAY_CONNECTED && np->peer[0];
    const int selected_game = netplay_lobby_get_game();
    if (selected_game == GAME_SML || selected_game == GAME_SML2) {
        for (int slot = 0; slot < NETPLAY_SHARED_SLOT_COUNT; slot++) {
            char state_path[1200], marker_path[1200], name[32];
            launcher_shared_slot_paths(selected_game, slot, state_path, sizeof state_path,
                                       marker_path, sizeof marker_path);
            launcher_shared_slot_name_get(selected_game, slot, name, sizeof name);
            netplay_shared_slot_configure(slot, state_path, marker_path, name,
                                          launcher_shared_slot_is_saved(selected_game, slot));
        }
    }
    NetplayChatMessage messages[7];
    int message_count = netplay_chat_snapshot(messages, 7);
    char host_name[32], guest_name[32], line[128];

    snprintf(host_name, sizeof host_name, "%s",
             np->is_host ? settings.online_name : (np->peer[0] ? np->peer : "Host"));
    snprintf(guest_name, sizeof guest_name, "%s",
             np->is_host ? (np->peer[0] ? np->peer : "") :
                           (np->state == NETPLAY_CONNECTED ? settings.online_name : ""));

    card(x, y, 808, 512, "ONLINE SESSION LOBBY");
    ui_text_fit(F_REG, 11, x + 20, y + 41, 100, C_MUTED, "INVITE CODE");
    ui_text(F_BOLD, 18, x + 20, y + 56, C_TEXT, np->code[0] ? np->code : "Creating room…");
    if (np->code[0] && ui_button(x + 218, y + 43, 74, 30, "Copy code", B_GHOST, 1)) {
        if (SDL_SetClipboardText(np->code) == 0) launcher_toast("Invite code copied.");
        else launcher_toast("Couldn't copy the invite code.");
    }
    ui_text_fit(F_REG, 11, x + 310, y + 52, 320, C_DIM,
                connected ? "Share the code with one other player." : "Your room is open while you wait for a player.");
    if (ui_button(x + 680, y + 43, 92, 30, "Disconnect", B_GHOST, 1)) {
        netplay_disconnect();
        online_chat_editing = 0;
        online_save_name_editing = 0;
        online_save_name_slot = -1;
        SDL_StopTextInput();
        launcher_toast("Disconnected from the online session.");
        return;
    }

    ui_rrect(x + 18, y + 82, 344, 328, 12, C_PANEL);
    ui_text(F_BOLD, 13, x + 34, y + 94, C_TEXT, "PLAYERS");
    ui_rrect(x + 30, y + 118, 320, 48, 9, RGBA(66, 126, 92, 32));
    ui_text(F_BOLD, 12, x + 44, y + 127, C_TEXT, host_name[0] ? host_name : "Host");
    ui_text(F_REG, 10, x + 44, y + 145, C_OK, "HOST");
    ui_rrect(x + 30, y + 176, 320, 48, 9,
             connected ? RGBA(66, 126, 92, 32) : RGBA(237, 180, 81, 18));
    ui_text(F_BOLD, 12, x + 44, y + 185, C_TEXT,
            connected ? guest_name : (np->is_host ? "Waiting for player…" : "Joining room…"));
    ui_text(F_REG, 10, x + 44, y + 203, connected ? C_OK : C_MUTED,
            connected ? (np->is_host ? "PLAYER 2" : "YOU") : "NOT CONNECTED");
    ui_rect(x + 34, y + 238, 312, 1, C_LINE);
    ui_text(F_BOLD, 12, x + 34, y + 250, C_TEXT, "GAME FOR THIS SESSION");
    if (np->is_host) {
        int enabled = np->state == NETPLAY_HOSTING || connected;
        if (ui_button(x + 34, y + 270, 148, 34,
                      "Super Mario Land", selected_game == GAME_SML ? B_PRIMARY : B_GHOST,
                      enabled)) {
            online_selected_game = GAME_SML;
            if (!netplay_lobby_set_game(GAME_SML))
                launcher_toast("Couldn't update the selected game.");
        }
        if (ui_button(x + 190, y + 270, 156, 34,
                      "Super Mario Land 2", selected_game == GAME_SML2 ? B_PRIMARY : B_GHOST,
                      enabled)) {
            online_selected_game = GAME_SML2;
            if (!netplay_lobby_set_game(GAME_SML2))
                launcher_toast("Couldn't update the selected game.");
        }
    } else {
        ui_text_fit(F_BOLD, 13, x + 34, y + 274, 310, C_TEXT,
                    online_game_name(selected_game));
    }
    ui_text_wrap(F_REG, 10, x + 34, y + 316, 312, C_DIM,
                 "Both players need the same game and matching ROM. More games can be added to online play later.", 3);
    uint32_t rom_status_color = np->rom_check_state == NETPLAY_ROM_MATCH ? C_OK :
        (np->rom_check_state == NETPLAY_ROM_MISMATCH ||
         np->rom_check_state == NETPLAY_ROM_INVALID ? C_WARN : C_MUTED);
    ui_text_fit(F_REG, 10, x + 34, y + 356, 310, rom_status_color,
                np->rom_check_status[0] ? np->rom_check_status : "ROM check hasn't started yet.");
    if (connected && ui_button(x + 190, y + 378, 156, 25, "Recheck ROM", B_GHOST, 1)) {
        online_rom_reported_game = -1;
        online_report_local_rom(selected_game);
    }

    ui_rrect(x + 374, y + 82, 416, 328, 12, C_PANEL);
    if (ui_button(x + 390, y + 108, 82, 26, "Chat",
                  online_lobby_panel == 0 ? B_PRIMARY : B_GHOST, 1))
        online_lobby_panel = 0;
    if (ui_button(x + 480, y + 108, 120, 26, "Shared Saves",
                  online_lobby_panel == 1 ? B_PRIMARY : B_GHOST, connected))
        online_lobby_panel = 1;

    if (online_lobby_panel == 0) {
        ui_text(F_BOLD, 13, x + 390, y + 94, C_TEXT, "SESSION CHAT");
        if (message_count == 0) {
            ui_text_fit(F_REG, 11, x + 390, y + 153, 380, C_DIM,
                        connected ? "No messages yet. Say hello!" : "Chat becomes available when a player joins.");
        } else {
            float row_y = y + 151 + (7 - message_count) * 25.0f;
            for (int i = 0; i < message_count; i++) {
                snprintf(line, sizeof line, "%.22s: %.80s", messages[i].name, messages[i].text);
                ui_text_fit(F_REG, 11, x + 390, row_y + i * 25.0f, 380, C_TEXT, line);
            }
        }
        int chat_over = 0;
        int chat_clicked = connected ? clickable(x + 390, y + 365, 286, 32, &chat_over) : 0;
        ui_rrect(x + 390, y + 365, 286, 32, 7,
                 online_chat_editing ? mixc(C_BTN, HEX(ui_accent), 0.38f) :
                 (chat_over ? C_BTN_H : C_BTN));
        const char *chat_value = online_chat_buffer[0] ? online_chat_buffer :
                                 (online_chat_editing ? "Type a message…" : "Message (Enter to send)");
        ui_text_fit(F_REG, 11, x + 401, y + 374, 264,
                    online_chat_buffer[0] ? C_TEXT : C_DIM, chat_value);
        if (chat_clicked) {
            online_name_editing = online_code_editing = online_server_editing = 0;
            online_save_name_editing = 0;
            online_chat_editing = 1;
            SDL_StartTextInput();
        }
        if (ui_button(x + 686, y + 365, 88, 32, "Send", B_NORMAL,
                      connected && online_chat_buffer[0])) {
            if (netplay_chat_send(online_chat_buffer)) {
                online_chat_buffer[0] = 0;
                online_chat_editing = 0;
                SDL_StopTextInput();
            } else launcher_toast("Couldn't send that chat message.");
        }
    } else {
        ui_text(F_BOLD, 13, x + 390, y + 94, C_TEXT, "SHARED SAVE SLOTS");
        int active_slot = netplay_shared_slot_active();
        int load_slot = netplay_shared_slot_load();
        ui_text_fit(F_REG, 10, x + 390, y + 139, 278, C_DIM,
                    np->is_host
                      ? "Choose a slot. Load on Start restores the same state."
                      : "Host manages slots; in-game Save/Load syncs the active slot.");
        if (ui_button(x + 677, y + 136, 97, 22, "Start Fresh", B_GHOST,
                      connected && np->is_host && load_slot >= 0)) {
            if (!netplay_shared_slot_select(active_slot, -1))
                launcher_toast("Couldn't clear the starting save selection.");
            else
                launcher_toast("Next session will start a new game.");
        }
        for (int slot = 0; slot < NETPLAY_SHARED_SLOT_COUNT; slot++) {
            float row_y = y + 158 + slot * 21.0f;
            char slot_name[32], status_text[20];
            int local_saved = 0, peer_saved = -1, pending = 0;
            netplay_shared_slot_name(slot, slot_name, sizeof slot_name);
            netplay_shared_slot_status(slot, &local_saved, &peer_saved, &pending);
            uint32_t status_color = C_MUTED;
            if (pending) { snprintf(status_text, sizeof status_text, "SYNCING"); status_color = C_WARN; }
            else if (local_saved && peer_saved == 1) { snprintf(status_text, sizeof status_text, "SYNCED"); status_color = C_OK; }
            else if (peer_saved < 0) { snprintf(status_text, sizeof status_text, local_saved ? "CHECKING" : "EMPTY"); }
            else if (local_saved || peer_saved) { snprintf(status_text, sizeof status_text, "UNSYNCED"); status_color = C_WARN; }
            else { snprintf(status_text, sizeof status_text, "EMPTY"); }

            ui_text_fit(F_REG, 10, x + 390, row_y + 4, 98,
                        slot == active_slot ? HEX(ui_accent) : C_TEXT, slot_name);
            ui_text_fit(F_REG, 9, x + 488, row_y + 5, 63, status_color, status_text);
            if (ui_button(x + 554, row_y, 52, 19,
                          slot == active_slot ? "Active" : "Use",
                          slot == active_slot ? B_PRIMARY : B_GHOST,
                          connected && np->is_host)) {
                if (!netplay_shared_slot_select(slot, -1))
                    launcher_toast("Couldn't select that shared save slot.");
            }
            if (ui_button(x + 610, row_y, 58, 19,
                          slot == load_slot ? "On Start" : "Load",
                          slot == load_slot ? B_PRIMARY : B_GHOST,
                          connected && np->is_host && netplay_shared_slot_is_synced(slot))) {
                if (!netplay_shared_slot_select(slot, slot))
                    launcher_toast("That shared slot is not synchronized yet.");
            }
        }
        ui_text_fit(F_REG, 9, x + 390, y + 371, 112, C_DIM, "Rename active slot");
        int name_over = 0;
        int name_clicked = clickable(x + 500, y + 368, 177, 27, &name_over);
        ui_rrect(x + 500, y + 368, 177, 27, 6,
                 online_save_name_editing ? mixc(C_BTN, HEX(ui_accent), 0.38f) :
                 (name_over ? C_BTN_H : C_BTN));
        char current_name[32];
        netplay_shared_slot_name(active_slot, current_name, sizeof current_name);
        const char *shown_name = online_save_name_editing
            ? online_save_name_buffer : current_name;
        ui_text_fit(F_REG, 10, x + 508, y + 376, 161, C_TEXT,
                    shown_name[0] ? shown_name : "Slot name");
        if (name_clicked && connected && np->is_host && !online_save_name_editing)
            online_begin_shared_name_edit(selected_game, active_slot);
        if (ui_button(x + 682, y + 368, 92, 27,
                      online_save_name_editing ? "Editing…" : "Rename",
                      B_GHOST, connected && np->is_host && !online_save_name_editing))
            online_begin_shared_name_edit(selected_game, active_slot);
        if (online_save_name_editing)
            ui_text_fit(F_REG, 9, x + 500, y + 397, 260, C_DIM, "Enter saves · Esc cancels");
    }

    ui_text_fit(F_REG, 11, x + 24, y + 426, 540, C_DIM,
                np->status[0] ? np->status : "Connected to the PipeClean relay.");
    if (np->is_host) {
        int load_slot = netplay_shared_slot_load();
        int can_play = connected &&
            (selected_game == GAME_SML || selected_game == GAME_SML2) &&
            np->rom_check_state == NETPLAY_ROM_MATCH &&
            (load_slot < 0 || netplay_shared_slot_is_synced(load_slot));
        if (ui_button(x + 592, y + 428, 180, 44, "Play for Everyone",
                      B_PRIMARY, can_play)) {
            if (!netplay_lobby_start_game(selected_game))
                launcher_toast("Couldn't start the shared game.");
        }
    } else {
        ui_text_fit(F_BOLD, 12, x + 560, y + 443, 210, C_TEXT,
                    connected ? "Waiting for host to press Play…" : "Waiting for host…");
    }
}

static void tab_online(float x, float y)
{
    NetplayInfo np;
    netplay_get_info(&np);
    if (np.state == NETPLAY_HOSTING || np.state == NETPLAY_CONNECTED) {
        tab_online_lobby(x, y, &np);
        return;
    }
    const int can_start = np.state == NETPLAY_OFFLINE || np.state == NETPLAY_ERROR;

    card(x, y, 808, 512, "ONLINE PLAY");
    ui_text_fit(F_REG, 12, x + 18, y + 42, 772, C_DIM,
                settings.online_connection_mode == 0
                    ? "Direct peer-to-peer mode is not implemented in this build. Choose Traversal Server to play online."
                    : "Host a room, choose a supported game, and share its invite code with another player.");

    ui_rrect(x + 18, y + 70, 354, 422, 12, C_PANEL);
    ui_text(F_BOLD, 14, x + 34, y + 82, C_TEXT, "PLAYER PROFILE");
    ui_text(F_REG, 11, x + 34, y + 112, C_MUTED, "Display name");
    if (ui_button(x + 280, y + 106, 76, 22, "Reset", B_GHOST, 1)) {
        online_name_editing = online_server_editing = online_chat_editing = 0;
        SDL_StopTextInput();
        snprintf(settings.online_name, sizeof settings.online_name, "Player");
        settings_save();
        launcher_toast("Online display name reset.");
    }
    int name_over = 0;
    int name_clicked = clickable(x + 34, y + 132, 322, 36, &name_over);
    ui_rrect(x + 34, y + 132, 322, 36, 8,
             online_name_editing ? mixc(C_BTN, HEX(ui_accent), 0.38f) : (name_over ? C_BTN_H : C_BTN));
    const char *name = settings.online_name[0] ? settings.online_name :
                       (online_name_editing ? "Type a name…" : "Player");
    ui_text_fit(F_REG, 13, x + 46, y + 141, 292, online_name_editing ? C_DIM : C_TEXT, name);
    if (name_clicked) {
        online_code_editing = online_server_editing = online_chat_editing = 0;
        online_name_editing = 1;
        settings.online_name[0] = 0;
        SDL_StartTextInput();
    }

    uint32_t rgb = (uint32_t)settings.online_color & 0xFFFFFFu;
    if (!rgb) rgb = 0x2CA83Du;
    const float wx = x + 106.0f, wy = y + 286.0f, wr = 62.0f;
    ui_text(F_REG, 11, x + 34, y + 180, C_MUTED, "Profile color");
    ui_color_wheel(wx, wy, wr, rgb);
    ui_text_c(F_REG, 10, wx, wy + wr + 3, C_DIM, "Hue + saturation");

    float dx = ui_mouse.x - wx, dy = ui_mouse.y - wy;
    float dist = sqrtf(dx * dx + dy * dy);
    if (ui_mouse.down && dist <= wr) {
        float hue = atan2f(-dy, dx) / 6.28318530718f;
        if (hue < 0.0f) hue += 1.0f;
        float h0, s0, val;
        mp_rgb_to_hsv(rgb, &h0, &s0, &val);
        (void)h0; (void)s0;
        if (val < 0.20f) val = 0.20f;
        settings.online_color = (int)mp_hsv_to_rgb(hue, dist / wr, val);
        rgb = (uint32_t)settings.online_color;
    }
    ui_rrect(x + 204, y + 226, 62, 34, 8, HEX(rgb));
    ui_stroke(x + 204, y + 226, 62, 34, 8, 1, RGBA(255,255,255,120));
    char rgb_text[24];
    snprintf(rgb_text, sizeof rgb_text, "#%06X", (unsigned)(rgb & 0xFFFFFFu));
    ui_text(F_BOLD, 13, x + 278, y + 235, C_TEXT, rgb_text);
    float hue, sat, val;
    mp_rgb_to_hsv(rgb, &hue, &sat, &val);
    int brightness = (int)lrintf(val * 100.0f);
    if (brightness < 20) brightness = 20;
    if (brightness > 100) brightness = 100;
    ui_text(F_REG, 11, x + 204, y + 278, C_MUTED, "Brightness");
    int brightness_changed = ui_slider(x + 204, y + 300, 138, &brightness, 20, 100);
    char brightness_text[16];
    snprintf(brightness_text, sizeof brightness_text, "%d%%", brightness);
    ui_text_r(F_REG, 11, x + 342, y + 322, C_TEXT, brightness_text);
    if (brightness_changed) {
        settings.online_color = (int)mp_hsv_to_rgb(hue, sat, brightness / 100.0f);
        rgb = (uint32_t)settings.online_color;
    }
    if (ui_button(x + 204, y + 354, 138, 30, "Reset color", B_NORMAL, 1)) {
        settings.online_color = 0x2CA83D;
        settings_save();
        launcher_toast("Online profile color reset.");
    }
    if (ui_toggle(x + 34, y + 440, &settings.touch_controls)) settings_save();
    ui_text(F_REG, 11, x + 90, y + 443, C_TEXT, "On-screen touch controls");
    ui_text_fit(F_REG, 10, x + 34, y + 472, 322, C_DIM,
                "Touch controls are useful on Android devices.");

    ui_rrect(x + 390, y + 70, 400, 422, 12, C_PANEL);
    ui_text(F_BOLD, 14, x + 408, y + 84, C_TEXT, "HOST A SESSION");
    ui_text_wrap(F_REG, 11, x + 408, y + 110, 360, C_DIM,
                 "Choose the game before creating the room. Invite code is generated automatically.", 2);

    ui_text_fit(F_REG, 10, x + 408, y + 143, 360, C_MUTED, "Connection mode");
    int mode_over = 0;
    int mode_clicked = clickable(x + 408, y + 155, 360, 30, &mode_over);
    ui_rrect(x + 408, y + 155, 360, 30, 7,
             online_mode_menu ? mixc(C_BTN, HEX(ui_accent), 0.38f) : (mode_over ? C_BTN_H : C_BTN));
    ui_text_fit(F_REG, 12, x + 420, y + 163, 320, C_TEXT,
                settings.online_connection_mode == 0 ? "Direct Connection" : "Traversal Server");
    ui_tri(x + 748, y + 165, x + 758, y + 165, x + 753, y + 172, C_MUTED);
    if (mode_clicked) online_mode_menu = !online_mode_menu;
    if (online_mode_menu) {
        ui_shadow(x + 408, y + 89, 360, 66, 10, 6, RGBA(0, 0, 0, 100));
        ui_rrect(x + 408, y + 89, 360, 66, 8, C_BG2);
        for (int mode = 0; mode < 2; mode++) {
            float my = y + 91 + mode * 32;
            int over = 0;
            int pick = clickable(x + 412, my, 352, 30, &over);
            int selected = settings.online_connection_mode == mode;
            ui_rrect(x + 412, my, 352, 30, 6,
                     selected ? HEX(ui_accent) : (over ? C_BTN_H : C_BG2));
            ui_text_fit(F_REG, 12, x + 424, my + 8, 324,
                        selected ? HEX(0xFFFFFF) : C_TEXT,
                        mode == 0 ? "Direct Connection" : "Traversal Server");
            if (pick) {
                settings.online_connection_mode = mode;
                settings_save();
                online_mode_menu = 0;
                online_server_editing = online_chat_editing = 0;
                online_name_editing = online_code_editing = 0;
                SDL_StopTextInput();
            }
        }
    }

    ui_text_fit(F_REG, 10, x + 408, y + 192, 360, C_MUTED, "Choose the game to host");
    if (ui_button(x + 408, y + 204, 172, 32, "Super Mario Land",
                  online_selected_game == GAME_SML ? B_PRIMARY : B_GHOST, can_start)) {
        online_selected_game = GAME_SML;
        (void)netplay_lobby_set_game(GAME_SML);
    }
    if (ui_button(x + 590, y + 204, 178, 32, "Super Mario Land 2",
                  online_selected_game == GAME_SML2 ? B_PRIMARY : B_GHOST, can_start)) {
        online_selected_game = GAME_SML2;
        (void)netplay_lobby_set_game(GAME_SML2);
    }

    ui_text_fit(F_REG, 10, x + 408, y + 242, 360, C_MUTED, "Traversal server address");
    int server_over = 0;
    int server_clicked = clickable(x + 408, y + 254, 360, 30, &server_over);
    ui_rrect(x + 408, y + 254, 360, 30, 7,
             online_server_editing ? mixc(C_BTN, HEX(ui_accent), 0.38f) : (server_over ? C_BTN_H : C_BTN));
    const char *server_text = settings.online_server[0] ? settings.online_server :
                              (online_server_editing ? "Type host:port or wss://service.onrender.com…" : PIPECLEAN_DEFAULT_RELAY_ENDPOINT);
    ui_text_fit(F_REG, 12, x + 420, y + 262, 336,
                (online_server_editing && !settings.online_server[0]) ? C_DIM : C_TEXT,
                server_text);
    if (server_clicked) {
        online_name_editing = online_code_editing = online_chat_editing = 0;
        snprintf(online_server_before, sizeof online_server_before, "%s", settings.online_server);
        settings.online_server[0] = 0;
        online_server_editing = 1;
        SDL_StartTextInput();
    }
    if (ui_button(x + 408, y + 290, 360, 36, "Host Room — Generate Invite Code", B_PRIMARY, can_start)) {
        if (settings.online_connection_mode == 0)
            launcher_toast("Direct peer-to-peer networking is not implemented yet. Select Traversal Server.");
        else {
            (void)netplay_lobby_set_game(online_selected_game);
            if (netplay_host(settings.online_server, settings.online_name) == 0)
                launcher_toast("Connecting to traversal server to create a room…");
            else launcher_toast("Disconnect from the current session first.");
        }
    }

    ui_rect(x + 408, y + 336, 364, 1, C_LINE);
    ui_text(F_BOLD, 13, x + 408, y + 344, C_TEXT, "JOIN A ROOM");
    int code_over = 0;
    int code_clicked = clickable(x + 408, y + 365, 228, 32, &code_over);
    ui_rrect(x + 408, y + 365, 228, 32, 8,
             online_code_editing ? mixc(C_BTN, HEX(ui_accent), 0.38f) : (code_over ? C_BTN_H : C_BTN));
    ui_text_fit(F_BOLD, 13, x + 420, y + 373, 204, online_join_code[0] ? C_TEXT : C_DIM,
                online_join_code[0] ? online_join_code : "Enter invite code");
    if (code_clicked) {
        online_name_editing = online_server_editing = online_chat_editing = 0;
        online_code_editing = 1;
        SDL_StartTextInput();
    }
    if (ui_button(x + 644, y + 365, 124, 32, "Join Room", B_NORMAL, can_start)) {
        if (settings.online_connection_mode == 0)
            launcher_toast("Direct peer-to-peer networking is not implemented yet. Select Traversal Server.");
        else if (!online_join_code[0]) launcher_toast("Enter the invite code first.");
        else if (netplay_join(settings.online_server, online_join_code, settings.online_name) == 0)
            launcher_toast("Connecting to the traversal room…");
        else launcher_toast("Disconnect from the current session first.");
    }

    ui_rrect(x + 408, y + 405, 364, 78, 9,
             np.state == NETPLAY_ERROR ? RGBA(206,75,74,25) :
             (np.state == NETPLAY_OFFLINE ? RGBA(237,180,81,18) : RGBA(54,160,112,20)));
    ui_text(F_BOLD, 10, x + 422, y + 413, C_MUTED, "CONNECTION STATUS");
    ui_text_fit(F_REG, 11, x + 422, y + 433, 330, np.state == NETPLAY_ERROR ? C_WARN : C_TEXT,
                np.status[0] ? np.status : "Not connected");
}


#if defined(__ANDROID__) || defined(PIPECLEAN_DEV_BUILD)
/* Companion Deck layout editor: only SML1 and SML2 have live widgets today.
 * Rows for the other games stay visible so the editor can grow without a UI redesign. */
enum {
    DECK_PICK_BACKGROUND = 1, DECK_PICK_BUTTON_TEXTURE, DECK_PICK_ITEM_TEXTURE,
    DECK_PICK_DECORATION, DECK_PICK_SOUND_SAVE, DECK_PICK_SOUND_LOAD,
    DECK_PICK_SOUND_PAUSE, DECK_PICK_SOUND_REWIND, DECK_PICK_SOUND_BACKGROUND,
    DECK_PICK_SOUND_CLOSE, DECK_PICK_ITEM_SOUND, DECK_PICK_EMPTY_SOUND,
    DECK_PICK_IMPORT, DECK_PICK_EXPORT,
    DECK_PICK_ITEM_MUSHROOM = 15, DECK_PICK_ITEM_SPECIAL = 16,
    DECK_PICK_ITEM_CARROT = 17,
    DECK_PICK_HEADER_TEXTURE = 18, DECK_PICK_STATUS_TEXTURE = 19,
    DECK_PICK_BUTTON_SAVE_TEXTURE = 20, DECK_PICK_BUTTON_LOAD_TEXTURE = 21,
    DECK_PICK_BUTTON_PAUSE_TEXTURE = 22, DECK_PICK_BUTTON_REWIND_TEXTURE = 23,
    DECK_PICK_BUTTON_BACKGROUND_TEXTURE = 24, DECK_PICK_BUTTON_CLOSE_TEXTURE = 25
};
enum {
    DECK_ELEM_HEADER, DECK_ELEM_ITEM, DECK_ELEM_CONTROLS, DECK_ELEM_TIMELINE,
    DECK_ELEM_STATUS, DECK_ELEM_DECORATION, DECK_ELEM_DECORATION_2, DECK_ELEM_DECORATION_3,
    DECK_ELEM_DECORATION_4, DECK_ELEM_DECORATION_5, DECK_ELEM_DECORATION_6,
    DECK_ELEM_DECORATION_7, DECK_ELEM_DECORATION_8, DECK_ELEM_DECORATION_9,
    DECK_ELEM_DECORATION_10, DECK_ELEM_DECORATION_11, DECK_ELEM_DECORATION_12,
    DECK_ELEM_DECORATION_13, DECK_ELEM_DECORATION_14, DECK_ELEM_DECORATION_15,
    DECK_ELEM_DECORATION_16, N_DECK_ELEM
};
static const char *deck_element_names[N_DECK_ELEM] = {
    "Header", "Reserve item", "Control buttons", "Save timeline", "Adventure status",
    "Decoration 1", "Decoration 2", "Decoration 3", "Decoration 4",
    "Decoration 5", "Decoration 6", "Decoration 7", "Decoration 8",
    "Decoration 9", "Decoration 10", "Decoration 11", "Decoration 12",
    "Decoration 13", "Decoration 14", "Decoration 15", "Decoration 16"
};
static int companion_editor_pending_decoration_slot;
static char *deck_decoration_path(CompanionLayoutCfg *d, int slot)
{
    if (!d || slot < 0 || slot >= N_DECK_DECORATIONS) return NULL;
    return slot == 0 ? d->decoration_path : d->decoration_extra_path[slot - 1];
}
static const char *deck_decoration_path_const(const CompanionLayoutCfg *d, int slot)
{
    if (!d || slot < 0 || slot >= N_DECK_DECORATIONS) return "";
    return slot == 0 ? d->decoration_path : d->decoration_extra_path[slot - 1];
}
static int deck_decoration_asset_index(int slot)
{
    if (slot < 0 || slot >= N_DECK_DECORATIONS) return -1;
    if (slot == 0) return 3;                 /* legacy first decoration */
    if (slot <= 7) return 20 + slot;         /* stable indices 21..27 */
    return 22 + slot;                        /* new layers occupy indices 30..37 */
}
static int companion_editor_open, companion_editor_page;
static int companion_editor_game = GAME_SML;
static int companion_editor_element = DECK_ELEM_CONTROLS;
static int companion_editor_preview_item = 1;
static int companion_editor_sound_selection;
static int companion_editor_dragging, companion_editor_resizing;
static float companion_editor_last_x, companion_editor_last_y;
static int companion_editor_preview_hover;
static CompanionLayoutCfg companion_undo[16], companion_redo[16];
static int companion_undo_count, companion_redo_count;

static int deck_game_ready(int game)
{
    return game == GAME_SML || game == GAME_SML2;
}

static void deck_layout_defaults(CompanionLayoutCfg *d)
{
    if (!d) return;
    memset(d, 0, sizeof *d);
    d->header_scale = d->item_scale = d->controls_scale =
        d->timeline_scale = d->status_scale = d->decoration_scale = 100;
    for (int i = 0; i < N_DECK_DECORATIONS - 1; i++) d->decoration_extra_scale[i] = d->decoration_extra_scale_y[i] = 100;
d->header_scale_y = d->item_scale_y = d->controls_scale_y = d->timeline_scale_y = d->status_scale_y = d->decoration_scale_y = 100;
    d->sounds_enabled = 1;
}

static void deck_editor_history_clear(void)
{
    companion_undo_count = companion_redo_count = 0;
    companion_editor_dragging = companion_editor_resizing = 0;
}

static void deck_editor_push(CompanionLayoutCfg stack[16], int *count,
                             const CompanionLayoutCfg *d)
{
    if (*count >= 16) {
        memmove(stack, stack + 1, 15 * sizeof stack[0]);
        *count = 15;
    }
    stack[(*count)++] = *d;
}

static void deck_editor_snapshot(void)
{
    if (companion_editor_game < 0 || companion_editor_game >= N_GAMES) return;
    deck_editor_push(companion_undo, &companion_undo_count,
                     &settings.deck_layout[companion_editor_game]);
    companion_redo_count = 0;
}

static int *deck_element_x(CompanionLayoutCfg *d, int element)
{
    switch (element) {
    case DECK_ELEM_HEADER: return &d->header_dx;
    case DECK_ELEM_ITEM: return &d->item_dx;
    case DECK_ELEM_CONTROLS: return &d->controls_dx;
    case DECK_ELEM_TIMELINE: return &d->timeline_dx;
    case DECK_ELEM_STATUS: return &d->status_dx;
    case DECK_ELEM_DECORATION: return &d->decoration_dx;
    default: return element >= DECK_ELEM_DECORATION_2 && element < N_DECK_ELEM
        ? &d->decoration_extra_dx[element - DECK_ELEM_DECORATION_2] : &d->decoration_dx;
    }
}
static int *deck_element_y(CompanionLayoutCfg *d, int element)
{
    switch (element) {
    case DECK_ELEM_HEADER: return &d->header_dy;
    case DECK_ELEM_ITEM: return &d->item_dy;
    case DECK_ELEM_CONTROLS: return &d->controls_dy;
    case DECK_ELEM_TIMELINE: return &d->timeline_dy;
    case DECK_ELEM_STATUS: return &d->status_dy;
    case DECK_ELEM_DECORATION: return &d->decoration_dy;
    default: return element >= DECK_ELEM_DECORATION_2 && element < N_DECK_ELEM
        ? &d->decoration_extra_dy[element - DECK_ELEM_DECORATION_2] : &d->decoration_dy;
    }
}
static int *deck_element_scale(CompanionLayoutCfg *d, int element)
{
    switch (element) {
    case DECK_ELEM_HEADER: return &d->header_scale;
    case DECK_ELEM_ITEM: return &d->item_scale;
    case DECK_ELEM_CONTROLS: return &d->controls_scale;
    case DECK_ELEM_TIMELINE: return &d->timeline_scale;
    case DECK_ELEM_STATUS: return &d->status_scale;
    case DECK_ELEM_DECORATION: return &d->decoration_scale;
    default: return element >= DECK_ELEM_DECORATION_2 && element < N_DECK_ELEM
        ? &d->decoration_extra_scale[element - DECK_ELEM_DECORATION_2] : &d->decoration_scale;
    }
}

static int *deck_element_scale_y(CompanionLayoutCfg *d, int element)
{
    switch (element) {
    case DECK_ELEM_HEADER: return &d->header_scale_y;
    case DECK_ELEM_ITEM: return &d->item_scale_y;
    case DECK_ELEM_CONTROLS: return &d->controls_scale_y;
    case DECK_ELEM_TIMELINE: return &d->timeline_scale_y;
    case DECK_ELEM_STATUS: return &d->status_scale_y;
    case DECK_ELEM_DECORATION: return &d->decoration_scale_y;
    default: return element >= DECK_ELEM_DECORATION_2 && element < N_DECK_ELEM
        ? &d->decoration_extra_scale_y[element - DECK_ELEM_DECORATION_2] : &d->decoration_scale_y;
    }
}

static void deck_editor_element_base(int element, float *x, float *y, float *w, float *h)
{
    switch (element) {
    case DECK_ELEM_HEADER:    *x = 18;  *y = 7;   *w = 758; *h = 55;  break;
    case DECK_ELEM_ITEM:      *x = 250; *y = 14;  *w = 300; *h = 300; break;
    case DECK_ELEM_CONTROLS:  *x = 94;  *y = 320; *w = 612; *h = 46;  break;
    case DECK_ELEM_TIMELINE:  *x = 24;  *y = 365; *w = 752; *h = 83;  break;
    case DECK_ELEM_STATUS:    *x = 620; *y = 76;  *w = 156; *h = 128; break;
    case DECK_ELEM_DECORATION: *x = 280; *y = 153; *w = 240; *h = 144; break;
    case DECK_ELEM_DECORATION_2: *x = 20; *y = 78; *w = 170; *h = 120; break;
    case DECK_ELEM_DECORATION_3: *x = 610; *y = 78; *w = 170; *h = 120; break;
    case DECK_ELEM_DECORATION_4: *x = 315; *y = 245; *w = 170; *h = 120; break;
    case DECK_ELEM_DECORATION_5: *x = 26; *y = 218; *w = 140; *h = 92; break;
    case DECK_ELEM_DECORATION_6: *x = 634; *y = 218; *w = 140; *h = 92; break;
    case DECK_ELEM_DECORATION_7: *x = 190; *y = 85; *w = 122; *h = 86; break;
    default: *x = 488; *y = 260; *w = 122; *h = 86; break;
    }
}

static void deck_editor_element_rect(CompanionLayoutCfg *d, int element,
                                     float px, float py, float pw, float ph,
                                     float *rx, float *ry, float *rw, float *rh)
{
    float x, y, w, h;
    deck_editor_element_base(element, &x, &y, &w, &h);
    float scale_x = (float)*deck_element_scale(d, element) / 100.0f;
    float scale_y = (float)*deck_element_scale_y(d, element) / 100.0f;
    x += (float)*deck_element_x(d, element);
    y += (float)*deck_element_y(d, element);
    *rx = px + (x + w * 0.5f - w * scale_x * 0.5f) * (pw / 800.0f);
    *ry = py + (y + h * 0.5f - h * scale_y * 0.5f) * (ph / 450.0f);
    *rw = w * (pw / 800.0f) * scale_x;
    *rh = h * (ph / 450.0f) * scale_y;
}

static const char *deck_editor_preview_asset_path(const CompanionLayoutCfg *d, int index)
{
    if (!d) return "";
    if (index == 0) return d->background_path;
    if (index == 1) return d->button_texture_path;
    if (index == 2) return d->item_texture_path;
    if (index == 3) return d->decoration_path;
    if (index >= 12 && index <= 14) return d->item_texture_by_id[index - 11];
    if (index == 15) return d->header_texture_path;
    if (index == 16) return d->status_texture_path;
    if (index >= 17 && index <= 20) return d->button_texture_by_action[index - 17];
    if (index >= 21 && index < 28) return d->decoration_extra_path[index - 21];
    if (index == 28) return d->button_texture_by_action[4];
    if (index == 29) return d->button_texture_by_action[5];
    return "";
}

static void deck_editor_preview_asset(const CompanionLayoutCfg *d, int asset_index,
                                     const char *path, float x, float y, float w, float h)
{
    if (!path || !path[0] || w <= 0 || h <= 0) return;
    if (d && asset_index >= 0 && asset_index < N_DECK_ASSET_PATHS &&
        d->asset_stretch[asset_index])
        ui_image_file_stretch(path, x, y, w, h);
    else
        ui_image_file_fit(path, x, y, w, h);
}

static void deck_editor_preview(CompanionLayoutCfg *d, float x, float y, float w, float h)
{
    ui_shadow(x, y, w, h, 11, 5, RGBA(0, 0, 0, 110));
    ui_rrect(x, y, w, h, 10, RGBA(12, 20, 34, 255));
    ui_vgrad(x + 2, y + 2, w - 4, h - 4,
             companion_editor_game == GAME_SML ? RGBA(52, 87, 62, 255) :
             companion_editor_game == GAME_SML2 ? RGBA(67, 49, 88, 255) : RGBA(24, 36, 58, 255),
             RGBA(16, 22, 38, 255));
    ui_clip(x + 2, y + 2, w - 4, h - 4);
    deck_editor_preview_asset(d, 0, d->background_path, x + 2, y + 2, w - 4, h - 4);
    /* Background and all inserted artwork are fit-within by default; no asset is silently cropped. */

    for (int e = DECK_ELEM_DECORATION; e < N_DECK_ELEM; e++) {
        float rx, ry, rw, rh;
        deck_editor_element_rect(d, e, x, y, w, h, &rx, &ry, &rw, &rh);
        int slot = e - DECK_ELEM_DECORATION;
        const char *asset = deck_decoration_path_const(d, slot);
        if (asset[0]) {
            deck_editor_preview_asset(d, slot == 0 ? 3 : 20 + slot, asset, rx, ry, rw, rh);
            if (e == companion_editor_element)
                ui_stroke(rx, ry, rw, rh, 5, 1.5f, RGBA(108, 218, 255, 255));
        } else {
            ui_rrect(rx, ry, rw, rh, 5,
                e == companion_editor_element ? RGBA(56, 135, 210, 135) : RGBA(69, 91, 124, 88));
            ui_text_fit(F_BOLD, 7, rx + 4, ry + rh * .45f, rw - 8,
                RGBA(210, 220, 240, 230), "EMPTY LAYER");
        }
    }

    /* Header layer */
    {
        float rx, ry, rw, rh;
        deck_editor_element_rect(d, DECK_ELEM_HEADER, x, y, w, h, &rx, &ry, &rw, &rh);
        ui_rrect(rx, ry, rw, rh, 5, RGBA(28, 40, 58, 210));
        deck_editor_preview_asset(d, 15, d->header_texture_path, rx + 1, ry + 1, rw - 2, rh - 2);
        ui_text_fit(F_BOLD, 6.8f, rx + 7, ry + rh * .24f, rw - 14,
                    RGBA(127, 220, 236, 255), "PIPE CLEAN / POCKET QUEST");
        ui_text_fit(F_BOLD, 9, rx + 7, ry + rh * .52f, rw - 14,
                    RGBA(247, 249, 255, 255), companion_editor_game == GAME_SML ? "SUPER MARIO LAND" : "SUPER MARIO LAND 2");
        if (DECK_ELEM_HEADER == companion_editor_element)
            ui_stroke(rx, ry, rw, rh, 5, 1.5f, RGBA(108, 218, 255, 255));
    }

    /* The top-right quick actions have independent textures too, and follow the header's placement transform. */
    {
        float sx = (float)(d->header_scale > 0 ? d->header_scale : 100) / 100.f;
        float sy = (float)(d->header_scale_y > 0 ? d->header_scale_y : 100) / 100.f;
        float bx = 593, by = 13, bw = 105, bh = 42;
        bx = 400.f + (bx - 400.f) * sx + d->header_dx;
        by = 31.5f + (by - 31.5f) * sy + d->header_dy;
        bw *= sx; bh *= sy;
        const char *path = d->button_texture_by_action[4][0]
            ? d->button_texture_by_action[4] : d->button_texture_path;
        ui_rrect(x + w * bx / 800.f, y + h * by / 450.f,
                 w * bw / 800.f, h * bh / 450.f, 4, RGBA(34, 48, 67, 240));
        deck_editor_preview_asset(d, path == d->button_texture_path ? 1 : 28, path,
            x + w * bx / 800.f + 1, y + h * by / 450.f + 1,
            w * bw / 800.f - 2, h * bh / 450.f - 2);
        ui_text_c(F_BOLD, 5.4f, x + w * (bx + bw * .5f) / 800.f,
                  y + h * (by + bh * .43f) / 450.f, RGBA(246, 249, 255, 255), "BACKGROUND");

        float cx = 704, cy = 13, cw = 72, ch = 42;
        cx = 400.f + (cx - 400.f) * sx + d->header_dx;
        cy = 31.5f + (cy - 31.5f) * sy + d->header_dy;
        cw *= sx; ch *= sy;
        path = d->button_texture_by_action[5][0]
            ? d->button_texture_by_action[5] : d->button_texture_path;
        ui_rrect(x + w * cx / 800.f, y + h * cy / 450.f,
                 w * cw / 800.f, h * ch / 450.f, 4, RGBA(34, 48, 67, 240));
        deck_editor_preview_asset(d, path == d->button_texture_path ? 1 : 29, path,
            x + w * cx / 800.f + 1, y + h * cy / 450.f + 1,
            w * cw / 800.f - 2, h * ch / 450.f - 2);
        ui_text_c(F_BOLD, 5.4f, x + w * (cx + cw * .5f) / 800.f,
                  y + h * (cy + ch * .43f) / 450.f, RGBA(246, 249, 255, 255), "CLOSE");
    }

    /* Reserve item — use its exact assigned texture, or the legacy generic fallback. */
    {
        float rx, ry, rw, rh;
        deck_editor_element_rect(d, DECK_ELEM_ITEM, x, y, w, h, &rx, &ry, &rw, &rh);
        int item_id = companion_editor_preview_item;
        const char *item_path = item_id >= 1 && item_id <= 3
            ? deck_editor_preview_asset_path(d, 11 + item_id) : "";
        if (!item_path[0]) item_path = d->item_texture_path;
        float inset_x = rw * .16f, inset_y = rh * .13f;
        if (item_path[0]) deck_editor_preview_asset(d, item_path == d->item_texture_path ? 2 : 11 + item_id, item_path, rx + inset_x, ry + inset_y, rw - inset_x * 2, rh - inset_y * 2);
        else {
            ui_rrect(rx + rw * .25f, ry + rh * .25f, rw * .5f, rh * .48f, 8, RGBA(255, 205, 75, 245));
            ui_text_c(F_BOLD, 13, rx + rw * .5f, ry + rh * .36f, RGBA(61, 43, 20, 255), "?");
        }
        ui_text_c(F_BOLD, 5.6f, rx + rw * .5f, ry + rh * .81f,
                  RGBA(248, 245, 230, 255),
                  item_id == 1 ? "MUSHROOM" :
                  item_id == 2 ? (companion_editor_game == GAME_SML ? "SUPERBALL FLOWER" : "FIRE FLOWER") :
                  item_id == 3 ? "CARROT" : "ITEM PREVIEW");
        if (DECK_ELEM_ITEM == companion_editor_element)
            ui_stroke(rx, ry, rw, rh, 8, 1.5f, RGBA(108, 218, 255, 255));
    }

    /* Adventure Status is a first-class transformable element, just like the header. */
    {
        float sx, sy, sw, sh;
        deck_editor_element_rect(d, DECK_ELEM_STATUS, x, y, w, h, &sx, &sy, &sw, &sh);
        ui_rrect(sx, sy, sw, sh, 7, RGBA(34, 44, 62, 240));
        deck_editor_preview_asset(d, 16, d->status_texture_path, sx + 1, sy + 1, sw - 2, sh - 2);
        ui_text_fit(F_BOLD, 6, sx + 5, sy + 6, sw - 10, RGBA(191, 202, 222, 255), "ADVENTURE STATUS");
        ui_text_fit(F_BOLD, 8, sx + 5, sy + sh * .45f, sw - 10, RGBA(250, 240, 220, 255), "LIVES  × 03");
        ui_text_fit(F_BOLD, 7, sx + 5, sy + sh * .66f, sw - 10, RGBA(112, 225, 182, 255),
                    companion_editor_game == GAME_SML ? "WORLD 1 - 1" : "STAGE 01");
        if (DECK_ELEM_STATUS == companion_editor_element)
            ui_stroke(sx, sy, sw, sh, 7, 1.5f, RGBA(108, 218, 255, 255));
    }

    /* Separate images for Save, Load, Pause and Rewind. */
    {
        float rx, ry, rw, rh;
        deck_editor_element_rect(d, DECK_ELEM_CONTROLS, x, y, w, h, &rx, &ry, &rw, &rh);
        static const char *labels[4] = {"SAVE", "LOAD", "PAUSE", "REWIND"};
        for (int b = 0; b < 4; b++) {
            float bx = rx + b * (rw / 4.f), by = ry, bw = rw / 4.f - 2.f, bh = rh;
            int texture_index = 17 + b;
            const char *custom = deck_editor_preview_asset_path(d, texture_index);
            if (!custom[0]) texture_index = 1, custom = d->button_texture_path;
            ui_rrect(bx, by, bw, bh, 4, RGBA(29, 42, 60, 240));
            deck_editor_preview_asset(d, texture_index, custom, bx + 1, by + 1, bw - 2, bh - 2);
            ui_stroke(bx, by, bw, bh, 4, .7f, RGBA(105, 137, 174, 230));
            ui_text_c(F_BOLD, 5.8f, bx + bw * .5f, by + bh * .39f, RGBA(246, 249, 255, 255), labels[b]);
        }
        if (DECK_ELEM_CONTROLS == companion_editor_element)
            ui_stroke(rx, ry, rw, rh, 5, 1.5f, RGBA(108, 218, 255, 255));
    }

    /* Timeline is a wide strip; the editor intentionally has no duplicate save-preview card. */
    {
        float rx, ry, rw, rh;
        deck_editor_element_rect(d, DECK_ELEM_TIMELINE, x, y, w, h, &rx, &ry, &rw, &rh);
        ui_rrect(rx, ry, rw, rh, 5, RGBA(24, 34, 51, 235));
        ui_text_fit(F_BOLD, 6.5f, rx + 5, ry + 4, rw - 10,
                    RGBA(218, 228, 245, 255), "SAVE-STATE TIMELINE");
        for (int slot = 0; slot < 10; slot++) {
            float tile_w = (rw - 18) / 10.f;
            float tx = rx + 3 + slot * (tile_w + 1);
            ui_rrect(tx, ry + rh * .28f, tile_w, rh * .54f, 2,
                     slot == 0 ? RGBA(86, 180, 200, 190) : RGBA(66, 81, 106, 200));
        }
        if (DECK_ELEM_TIMELINE == companion_editor_element)
            ui_stroke(rx, ry, rw, rh, 5, 1.5f, RGBA(108, 218, 255, 255));
    }

    ui_unclip();
    float sx, sy, sw, sh;
    deck_editor_element_rect(d, companion_editor_element, x, y, w, h, &sx, &sy, &sw, &sh);
    ui_stroke(sx, sy, sw, sh, 5, 2, RGBA(117, 213, 255, 255));
    /* Large, high-contrast handles are sized for touchscreens as well as a mouse. */
    /* Oversized grab handles are designed for touch input on handheld displays. */
    ui_rrect(sx + sw - 26, sy + sh * .5f - 13, 26, 26, 7, RGBA(250, 252, 255, 255));
    ui_rrect(sx + sw * .5f - 13, sy + sh - 26, 26, 26, 7, RGBA(250, 252, 255, 255));
    ui_rrect(sx + sw - 26, sy + sh - 26, 26, 26, 7, RGBA(93, 207, 255, 255));
}

static void deck_editor_begin_drag(float x, float y, float preview_x, float preview_y,
                                   float preview_w, float preview_h)
{
    CompanionLayoutCfg *d = &settings.deck_layout[companion_editor_game];
    int order[N_DECK_ELEM];
    int n = 0;
    for (int e = N_DECK_ELEM - 1; e >= DECK_ELEM_DECORATION; e--) order[n++] = e;
    order[n++] = DECK_ELEM_STATUS;
    order[n++] = DECK_ELEM_ITEM;
    order[n++] = DECK_ELEM_CONTROLS;
    order[n++] = DECK_ELEM_TIMELINE;
    order[n++] = DECK_ELEM_HEADER;
    for (int i = 0; i < n; i++) {
        int e = order[i];
        float rx, ry, rw, rh;
        deck_editor_element_rect(d, e, preview_x, preview_y, preview_w, preview_h,
                                 &rx, &ry, &rw, &rh);
        if (x < rx || y < ry || x >= rx + rw || y >= ry + rh) continue;
        companion_editor_element = e;
        int right_handle = x >= rx + rw - 30 && fabsf(y - (ry + rh * .5f)) <= 30;
        int bottom_handle = y >= ry + rh - 30 && fabsf(x - (rx + rw * .5f)) <= 30;
        int corner_handle = x >= rx + rw - 30 && y >= ry + rh - 30;
        companion_editor_resizing = corner_handle ? 1 : right_handle ? 2 : bottom_handle ? 3 : 0;
        companion_editor_dragging = 1;
        companion_editor_last_x = x;
        companion_editor_last_y = y;
        deck_editor_snapshot();
        return;
    }
}

static void deck_editor_drag(float x, float y, float preview_w, float preview_h)
{
    if (!companion_editor_dragging || companion_editor_game < 0 ||
        companion_editor_game >= N_GAMES) return;
    CompanionLayoutCfg *d = &settings.deck_layout[companion_editor_game];
    float dx = x - companion_editor_last_x, dy = y - companion_editor_last_y;
    if (companion_editor_resizing) {
        int *scale_x = deck_element_scale(d, companion_editor_element);
        int *scale_y = deck_element_scale_y(d, companion_editor_element);
        if (companion_editor_resizing == 1 || companion_editor_resizing == 2)
            *scale_x += (int)((dx / (preview_w / 800.0f)) * 0.35f);
        if (companion_editor_resizing == 1 || companion_editor_resizing == 3)
            *scale_y += (int)((dy / (preview_h / 450.0f)) * 0.35f);
        if (*scale_x < 25) *scale_x = 25;
        if (*scale_x > 250) *scale_x = 250;
        if (*scale_y < 25) *scale_y = 25;
        if (*scale_y > 250) *scale_y = 250;
    } else {
        int *ox = deck_element_x(d, companion_editor_element);
        int *oy = deck_element_y(d, companion_editor_element);
        *ox += (int)(dx / (preview_w / 800.0f));
        *oy += (int)(dy / (preview_h / 450.0f));
        if (*ox < -400) *ox = -400;
        if (*ox > 400) *ox = 400;
        if (*oy < -400) *oy = -400;
        if (*oy > 400) *oy = 400;
    }
    companion_editor_last_x = x;
    companion_editor_last_y = y;
}

static void deck_editor_open_decoration_picker(int slot)
{
    if (slot < 0 || slot >= N_DECK_DECORATIONS) return;
    companion_editor_pending_decoration_slot = slot;
    deck_editor_snapshot();
    launcher_open_companion_layout_picker(DECK_PICK_DECORATION, companion_editor_game);
}

static void deck_editor_open_picker(int kind)
{
    if (kind == DECK_PICK_DECORATION) {
        CompanionLayoutCfg *d = &settings.deck_layout[companion_editor_game];
        for (int slot = 0; slot < N_DECK_DECORATIONS; slot++) {
            const char *path = deck_decoration_path_const(d, slot);
            if (!path[0]) {
                companion_editor_pending_decoration_slot = slot;
                deck_editor_snapshot();
                launcher_open_companion_layout_picker(kind, companion_editor_game);
                return;
            }
        }
        launcher_toast("All eight image layers are full. Remove a layer to add another.");
        return;
    }
    if (kind != DECK_PICK_EXPORT) deck_editor_snapshot();
    launcher_open_companion_layout_picker(kind, companion_editor_game);
}

static void deck_editor_undo(void)
{
    if (!companion_undo_count) { launcher_toast("Nothing to undo."); return; }
    deck_editor_push(companion_redo, &companion_redo_count,
                     &settings.deck_layout[companion_editor_game]);
    settings.deck_layout[companion_editor_game] = companion_undo[--companion_undo_count];
}
static void deck_editor_redo(void)
{
    if (!companion_redo_count) { launcher_toast("Nothing to redo."); return; }
    deck_editor_push(companion_undo, &companion_undo_count,
                     &settings.deck_layout[companion_editor_game]);
    settings.deck_layout[companion_editor_game] = companion_redo[--companion_redo_count];
}

static void deck_editor_list(float x, float y)
{
    ui_text(F_BOLD, 15, x + 36, y + 86, C_TEXT, "COMPANION DECK LAYOUTS");
    ui_text_fit(F_REG, 10, x + 36, y + 109, 708, C_MUTED,
                "Choose a game layout. Unreleased layouts are shown here but cannot be edited yet.");
    ui_rect(x + 36, y + 128, 728, 1, C_LINE);
    for (int g = 0; g < N_GAMES; g++) {
        float ry = y + 134 + g * 39.5f;
        int ready = deck_game_ready(g);
        if (ready) ui_rrect(x + 32, ry - 2, 734, 35, 7, RGBA(255,255,255, g == companion_editor_game ? 14 : 5));
        ui_text_fit(F_BOLD, 11, x + 40, ry + 5, 190, ready ? C_TEXT : C_DIM, games[g].name);
        ui_text_fit(F_REG, 9, x + 238, ry + 6, 105, ready ? C_OK : C_DIM,
                    ready ? "READY TO EDIT" : "COMING SOON");
        if (ui_button(x + 468, ry, 76, 29, "Edit", B_PRIMARY, ready)) {
            companion_editor_game = g;
            companion_editor_page = 1;
            companion_editor_element = DECK_ELEM_CONTROLS;
            deck_editor_history_clear();
        }
        if (ui_button(x + 550, ry, 76, 29, "Import", B_GHOST, ready)) {
            companion_editor_game = g;
            deck_editor_history_clear();
            deck_editor_open_picker(DECK_PICK_IMPORT);
        }
        if (ui_button(x + 632, ry, 76, 29, "Export", B_GHOST, ready)) {
            companion_editor_game = g;
            deck_editor_open_picker(DECK_PICK_EXPORT);
        }
    }
    if (ui_button(x + 36, y + 435, 156, 28, "Back to Display", B_NORMAL, 1))
        companion_editor_open = 0;
    ui_text_fit(F_REG, 9, x + 210, y + 442, 540, C_DIM,
                "Bundles include the layout config and any assigned images and sounds.");
}

static void deck_editor_layout(float x, float y)
{
    CompanionLayoutCfg *d = &settings.deck_layout[companion_editor_game];
    ui_text(F_BOLD, 15, x + 36, y + 82, C_TEXT, "COMPANION DECK STUDIO");
    ui_text_fit(F_BOLD, 9, x + 360, y + 86, 392, RGBA(116, 222, 255, 255),
                companion_editor_game == GAME_SML ? "SUPER MARIO LAND  /  LIVE CANVAS" : "SUPER MARIO LAND 2  /  LIVE CANVAS");

    if (ui_button(x + 36, y + 105, 58, 34, "Back", B_GHOST, 1)) {
        companion_editor_page = 0;
        companion_editor_dragging = 0;
        deck_editor_history_clear();
        return;
    }
    if (ui_button(x + 100, y + 105, 68, 34, "Save", B_PRIMARY, 1)) {
        settings_save();
        launcher_toast("Companion Deck layout saved.");
    }
    if (ui_button(x + 174, y + 105, 58, 34, "Undo", B_GHOST, companion_undo_count > 0)) deck_editor_undo();
    if (ui_button(x + 238, y + 105, 58, 34, "Redo", B_GHOST, companion_redo_count > 0)) deck_editor_redo();
    if (ui_button(x + 302, y + 105, 66, 34, "Reset", B_GHOST, 1)) {
        deck_editor_snapshot();
        deck_layout_defaults(d);
    }

    const char *tab_labels[] = {"Canvas", "Assets", "Items", "Sounds", "Layers"};
    const int tab_pages[] = {1, 4, 3, 2, 5};
    for (int t = 0; t < 5; t++) {
        float tx = x + 36 + t * 83;
        if (ui_button(tx, y + 146, 78, 32, tab_labels[t],
                      t == 0 ? B_PRIMARY : B_GHOST, 1)) {
            companion_editor_page = tab_pages[t];
            companion_editor_dragging = 0;
            return;
        }
    }
    if (ui_button(x + 580, y + 146, 176, 32, "+ Add image layer", B_NORMAL, 1)) {
        deck_editor_open_picker(DECK_PICK_DECORATION);
        return;
    }

    /* A larger landscape work surface makes hit targets usable with a finger,
       while the compact inspector keeps all scale and selection controls visible. */
    float px = x + 36, py = y + 194, pw = 380, ph = 213.75f;
    ui_rrect(px - 7, py - 7, pw + 14, ph + 14, 12, RGBA(7, 13, 24, 255));
    ui_stroke(px - 1, py - 1, pw + 2, ph + 2, 8, 1.0f, RGBA(66, 102, 148, 225));
    deck_editor_preview(d, px, py, pw, ph);
    companion_editor_preview_hover = ui_hover(px, py, pw, ph);
    if (ui_mouse.pressed && companion_editor_preview_hover)
        deck_editor_begin_drag(ui_mouse.x, ui_mouse.y, px, py, pw, ph);
    if (ui_mouse.down && companion_editor_dragging)
        deck_editor_drag(ui_mouse.x, ui_mouse.y, pw, ph);
    if (ui_mouse.released || !ui_mouse.down) companion_editor_dragging = 0;
    ui_text_fit(F_BOLD, 8.2f, px, py + ph + 10, pw,
                RGBA(112, 225, 255, 245), "16:9  •  FIT = NO CROP  •  DRAG HANDLES TO RESIZE");

    float rx = x + 438;
    ui_text(F_BOLD, 10, rx, y + 190, C_TEXT, "LAYERS & TRANSFORM");
    ui_text_fit(F_REG, 8, rx + 148, y + 190, 180, C_MUTED,
                "Select, move, reshape");
    for (int e = 0; e < N_DECK_ELEM; e++) {
        int col = e % 2, row = e / 2;
        float by = y + 204 + row * 21.0f;
        if (ui_button(rx + col * 163, by, 157, 19,
                      deck_element_names[e],
                      e == companion_editor_element ? B_PRIMARY : B_GHOST, 1))
            companion_editor_element = e;
    }

    int *scale_x = deck_element_scale(d, companion_editor_element);
    int *scale_y = deck_element_scale_y(d, companion_editor_element);
    char scale_text[48];
    snprintf(scale_text, sizeof scale_text, "W %d%%", *scale_x);
    ui_text(F_BOLD, 9, rx, y + 380, C_TEXT, scale_text);
    if (ui_button(rx + 62, y + 367, 30, 30, "−", B_GHOST, *scale_x > 25)) {
        deck_editor_snapshot(); *scale_x -= 5; if (*scale_x < 25) *scale_x = 25;
    }
    if (ui_button(rx + 96, y + 367, 30, 30, "+", B_GHOST, *scale_x < 250)) {
        deck_editor_snapshot(); *scale_x += 5; if (*scale_x > 250) *scale_x = 250;
    }
    snprintf(scale_text, sizeof scale_text, "H %d%%", *scale_y);
    ui_text(F_BOLD, 9, rx + 137, y + 380, C_TEXT, scale_text);
    if (ui_button(rx + 202, y + 367, 30, 30, "−", B_GHOST, *scale_y > 25)) {
        deck_editor_snapshot(); *scale_y -= 5; if (*scale_y < 25) *scale_y = 25;
    }
    if (ui_button(rx + 236, y + 367, 30, 30, "+", B_GHOST, *scale_y < 250)) {
        deck_editor_snapshot(); *scale_y += 5; if (*scale_y > 250) *scale_y = 250;
    }
    if (ui_button(rx, y + 405, 105, 28, "Center layer", B_GHOST, 1)) {
        deck_editor_snapshot();
        *deck_element_x(d, companion_editor_element) = 0;
        *deck_element_y(d, companion_editor_element) = 0;
    }
    ui_text_fit(F_REG, 8, rx + 112, y + 412, 210, C_MUTED,
                "Width and height are independent.");
    ui_text_fit(F_REG, 8, rx, y + 441, 324, C_DIM,
                "Fit keeps every pixel visible; Stretch fills the layer box.");
}

static void deck_editor_sounds(float x, float y)
{
    CompanionLayoutCfg *d = &settings.deck_layout[companion_editor_game];
    ui_text(F_BOLD, 15, x + 36, y + 86, C_TEXT, "COMPANION SOUNDS");
    if (ui_button(x + 36, y + 107, 65, 28, "Back", B_GHOST, 1))
        companion_editor_page = 1;
    ui_text(F_REG, 11, x + 114, y + 114, C_TEXT, "Enable custom action sounds");
    if (ui_mouse.pressed && ui_hover(x + 320, y + 110, 52, 24))
        deck_editor_snapshot();
    (void)ui_toggle(x + 320, y + 110, &d->sounds_enabled);
    static const char *labels[N_DECK_BUTTON_SOUNDS] = {
        "Save button", "Load button", "Pause button", "Rewind button",
        "Background button", "Close button"
    };
    for (int i = 0; i < N_DECK_BUTTON_SOUNDS; i++) {
        int col = i % 2, row = i / 2;
        float bx = x + 36 + col * 365, by = y + 151 + row * 65;
        ui_rrect(bx, by, 350, 54, 8, C_PANEL);
        ui_text_fit(F_BOLD, 10, bx + 12, by + 9, 155, C_TEXT, labels[i]);
        ui_text_fit(F_REG, 8, bx + 12, by + 28, 225, C_DIM,
                    d->button_sound_path[i][0] ? d->button_sound_path[i] : "Use built-in / silent default");
        char b[32]; snprintf(b, sizeof b, "Choose…");
        if (ui_button(bx + 266, by + 13, 72, 29, b, B_GHOST, 1))
            deck_editor_open_picker(DECK_PICK_SOUND_SAVE + i);
    }
    ui_rrect(x + 36, y + 352, 350, 56, 8, C_PANEL);
    ui_text_fit(F_BOLD, 10, x + 48, y + 362, 170, C_TEXT, "Item used");
    ui_text_fit(F_REG, 8, x + 48, y + 381, 220, C_DIM,
                d->item_sound_path[0] ? d->item_sound_path : "Choose a sound for a usable item");
    if (ui_button(x + 302, y + 366, 70, 28, "Choose…", B_GHOST, 1))
        deck_editor_open_picker(DECK_PICK_ITEM_SOUND);
    ui_rrect(x + 402, y + 352, 360, 56, 8, C_PANEL);
    ui_text_fit(F_BOLD, 10, x + 414, y + 362, 180, C_TEXT, "Empty item slot");
    ui_text_fit(F_REG, 8, x + 414, y + 381, 220, C_DIM,
                d->empty_item_sound_path[0] ? d->empty_item_sound_path : "Choose a sound when the slot is empty");
    if (ui_button(x + 674, y + 366, 74, 28, "Choose…", B_GHOST, 1))
        deck_editor_open_picker(DECK_PICK_EMPTY_SOUND);
}

static void deck_editor_items(float x, float y)
{
    CompanionLayoutCfg *d = &settings.deck_layout[companion_editor_game];
    const int item_ids[3] = {1, 2, 3};
    const int pick_kinds[3] = {DECK_PICK_ITEM_MUSHROOM, DECK_PICK_ITEM_SPECIAL, DECK_PICK_ITEM_CARROT};
    const char *labels[3];
    labels[0] = "Mushroom";
    labels[1] = companion_editor_game == GAME_SML ? "Superball Flower" : "Fire Flower";
    labels[2] = "Carrot";
    ui_text(F_BOLD, 15, x + 36, y + 86, C_TEXT, "RESERVE ITEM TEXTURES");
    if (ui_button(x + 36, y + 107, 65, 28, "Back", B_GHOST, 1)) {
        companion_editor_page = 1;
        return;
    }
    ui_text_wrap(F_REG, 10, x + 114, y + 108, 640, C_MUTED,
                 "Choose a separate transparent PNG or GIF for each power-up. An empty per-item path falls back to the legacy generic item texture.", 2);
    for (int row = 0; row < 3; row++) {
        int item = item_ids[row];
        float ry = y + 153 + row * 82;
        int ready = item != 3 || companion_editor_game == GAME_SML2;
        ui_rrect(x + 36, ry, 728, 70, 8, C_PANEL);
        ui_text_fit(F_BOLD, 11, x + 50, ry + 12, 195, ready ? C_TEXT : C_DIM, labels[row]);
        const char *asset_path = d->item_texture_by_id[item];
        int asset_index = 11 + item;
        ui_text_fit(F_REG, 8, x + 50, ry + 34, 260, asset_path[0] ? C_MUTED : C_DIM,
                    asset_path[0] ? path_base(asset_path) :
                    d->item_texture_path[0] ? "Using shared fallback texture" : "Using built-in item art");
        ui_rrect(x + 320, ry + 8, 60, 54, 6, RGBA(7, 13, 24, 255));
        ui_stroke(x + 320, ry + 8, 60, 54, 6, 1.0f, RGBA(77, 111, 149, 180));
        const char *preview_path = asset_path[0] ? asset_path : d->item_texture_path;
        if (preview_path[0])
            ui_image_file_fit(preview_path, x + 324, ry + 11, 52, 48);
        else
            ui_text_fit(F_BOLD, 7, x + 324, ry + 30, 52, C_DIM, "BUILT-IN");
        if (!ready) {
            ui_text_fit(F_REG, 8, x + 394, ry + 39, 110, C_DIM, "SML2 ONLY");
            continue;
        }
        if (ui_button(x + 394, ry + 18, 70, 32,
                      companion_editor_preview_item == item ? "Preview*" : "Preview",
                      companion_editor_preview_item == item ? B_PRIMARY : B_GHOST, 1)) {
            companion_editor_preview_item = item;
        }
        if (ui_button(x + 470, ry + 18, 72, 32,
                      d->asset_stretch[asset_index] ? "STRETCH" : "FIT / NO CROP",
                      d->asset_stretch[asset_index] ? B_PRIMARY : B_NORMAL, 1)) {
            deck_editor_snapshot();
            d->asset_stretch[asset_index] = !d->asset_stretch[asset_index];
            settings_save();
        }
        if (ui_button(x + 548, ry + 18, 82, 32, "Choose…", B_GHOST, 1))
            deck_editor_open_picker(pick_kinds[row]);
        if (ui_button(x + 636, ry + 18, 112, 32, "Clear texture", B_DANGER, asset_path[0] != 0)) {
            deck_editor_snapshot();
            d->item_texture_by_id[item][0] = 0;
            d->asset_stretch[asset_index] = 0;
            settings_save();
            launcher_toast("Item texture cleared.");
        }
    }
}

static void deck_editor_assets(float x, float y)
{
    CompanionLayoutCfg *d = &settings.deck_layout[companion_editor_game];
    ui_text(F_BOLD, 15, x + 36, y + 86, C_TEXT, "DECK ASSETS");
    if (ui_button(x + 36, y + 107, 65, 28, "Back", B_GHOST, 1)) {
        companion_editor_page = 1;
        return;
    }
    ui_text_wrap(F_REG, 10, x + 114, y + 108, 490, C_MUTED,
        "Each region and action button has its own artwork. Fit shows the whole image; Stretch fills its box without cropping.", 2);
    if (ui_button(x + 638, y + 107, 126, 28, "Decorations…", B_GHOST, 1)) {
        companion_editor_page = 5;
        return;
    }

    struct AssetRow { const char *label; char *path; int kind; int asset_index; } rows[] = {
        {"Background image", d->background_path, DECK_PICK_BACKGROUND, 0},
        {"Header plate", d->header_texture_path, DECK_PICK_HEADER_TEXTURE, 15},
        {"Adventure status", d->status_texture_path, DECK_PICK_STATUS_TEXTURE, 16},
        {"Save button", d->button_texture_by_action[0], DECK_PICK_BUTTON_SAVE_TEXTURE, 17},
        {"Load button", d->button_texture_by_action[1], DECK_PICK_BUTTON_LOAD_TEXTURE, 18},
        {"Pause / Resume", d->button_texture_by_action[2], DECK_PICK_BUTTON_PAUSE_TEXTURE, 19},
        {"Rewind button", d->button_texture_by_action[3], DECK_PICK_BUTTON_REWIND_TEXTURE, 20},
        {"Background button", d->button_texture_by_action[4], DECK_PICK_BUTTON_BACKGROUND_TEXTURE, 28},
        {"Close Game button", d->button_texture_by_action[5], DECK_PICK_BUTTON_CLOSE_TEXTURE, 29}
    };
    for (int i = 0; i < (int)(sizeof rows / sizeof rows[0]); i++) {
        float ry = y + 151 + i * 31.0f;
        ui_rrect(x + 36, ry, 728, 28, 6,
                 i == 0 ? RGBA(44, 85, 117, 180) : C_PANEL);
        ui_text_fit(F_BOLD, 9, x + 49, ry + 9, 164, C_TEXT, rows[i].label);
        const char *name = rows[i].path[0] ? path_base(rows[i].path) : "Built-in appearance";
        ui_text_fit(F_REG, 7.5f, x + 218, ry + 9, 140,
                    rows[i].path[0] ? C_MUTED : C_DIM, name);
        ui_rrect(x + 366, ry + 3, 56, 22, 5, RGBA(7, 13, 24, 255));
        ui_stroke(x + 366, ry + 3, 56, 22, 5, 1.0f, RGBA(77, 111, 149, 180));
        if (rows[i].path[0])
            ui_image_file_fit(rows[i].path, x + 369, ry + 4, 50, 20);
        else
            ui_text_fit(F_BOLD, 6.5f, x + 369, ry + 9, 50, C_DIM, "DEFAULT");
        int stretch = d->asset_stretch[rows[i].asset_index];
        if (ui_button(x + 450, ry + 3, 78, 22, stretch ? "STRETCH" : "FIT / NO CROP",
                      stretch ? B_PRIMARY : B_NORMAL, 1)) {
            deck_editor_snapshot();
            d->asset_stretch[rows[i].asset_index] = !stretch;
            settings_save();
        }
        if (ui_button(x + 536, ry + 3, 82, 22, "Choose…", B_GHOST, 1))
            deck_editor_open_picker(rows[i].kind);
        if (ui_button(x + 626, ry + 3, 124, 22, "Clear texture", B_DANGER, rows[i].path[0] != 0)) {
            deck_editor_snapshot();
            rows[i].path[0] = 0;
            d->asset_stretch[rows[i].asset_index] = 0;
            settings_save();
            launcher_toast("Asset cleared.");
        }
    }
}

static void deck_editor_decorations(float x, float y)
{
    CompanionLayoutCfg *d = &settings.deck_layout[companion_editor_game];
    ui_text(F_BOLD, 15, x + 36, y + 86, C_TEXT, "IMAGE LAYERS");
    if (ui_button(x + 36, y + 107, 65, 30, "Back", B_GHOST, 1)) {
        companion_editor_page = 1;
        return;
    }
    ui_text_wrap(F_REG, 9, x + 114, y + 110, 620, C_MUTED,
        "Eight independent layers. Fit / no crop preserves every pixel; Stretch fills the box. Remove clears only that layer.", 2);
    for (int slot = 0; slot < N_DECK_DECORATIONS; slot++) {
        char *asset_path = deck_decoration_path(d, slot);
        float ry = y + 151 + slot * 35.5f;
        ui_rrect(x + 36, ry, 728, 32, 6,
                 slot == companion_editor_element - DECK_ELEM_DECORATION
                    ? RGBA(65, 130, 190, 46) : C_PANEL);
        char label[32];
        snprintf(label, sizeof label, "LAYER %02d", slot + 1);
        ui_text(F_BOLD, 8.5f, x + 48, ry + 6, C_TEXT, label);
        ui_text_fit(F_REG, 7.5f, x + 48, ry + 18, 318,
                    asset_path[0] ? C_MUTED : C_DIM,
                    asset_path[0] ? path_base(asset_path) : "EMPTY — READY FOR AN IMAGE");
        ui_rrect(x + 372, ry + 3, 54, 26, 5, RGBA(7, 13, 24, 255));
        ui_stroke(x + 372, ry + 3, 54, 26, 5, 1.0f, RGBA(77, 111, 149, 180));
        if (asset_path[0])
            ui_image_file_fit(asset_path, x + 375, ry + 4, 48, 24);
        else
            ui_text_fit(F_BOLD, 6.5f, x + 375, ry + 12, 48, C_DIM, "EMPTY");
        int asset_index = slot == 0 ? 3 : 20 + slot;
        int stretch = d->asset_stretch[asset_index];
        if (ui_button(x + 438, ry + 2, 82, 28, stretch ? "STRETCH" : "FIT / NO CROP",
                      stretch ? B_PRIMARY : B_NORMAL, 1)) {
            deck_editor_snapshot();
            d->asset_stretch[asset_index] = !stretch;
            settings_save();
        }
        if (ui_button(x + 526, ry + 2, 82, 28, "Choose…", B_GHOST, 1))
            deck_editor_open_decoration_picker(slot);
        if (ui_button(x + 614, ry + 2, 136, 28, "Remove layer", B_DANGER, asset_path[0] != 0)) {
            deck_editor_snapshot();
            asset_path[0] = 0;
            d->asset_stretch[asset_index] = 0;
            if (companion_editor_element == DECK_ELEM_DECORATION + slot)
                companion_editor_element = DECK_ELEM_DECORATION;
            settings_save();
            launcher_toast("Image layer removed.");
        }
        if (ui_mouse.pressed && ui_hover(x + 36, ry, 520, 32) && asset_path[0]) {
            companion_editor_element = DECK_ELEM_DECORATION + slot;
        }
    }
}

static void deck_editor_draw(float x, float y)
{
    /* Shared studio frame: depth, restrained neon accent, and a header rail
       make the individual editor pages feel like one finished tool. */
    ui_shadow(x + 18, y + 70, 772, 404, 14, 7, RGBA(0, 0, 0, 112));
    ui_rrect(x + 18, y + 70, 772, 404, 12, RGBA(15, 22, 35, 255));
    ui_vgrad(x + 20, y + 72, 768, 48,
             RGBA(43, 68, 96, 255), RGBA(22, 31, 47, 255));
    ui_rect(x + 30, y + 72, 748, 2, RGBA(99, 220, 255, 230));
    ui_stroke(x + 18, y + 70, 772, 404, 12, 1.0f, RGBA(77, 111, 149, 190));
    if (companion_editor_page == 0) deck_editor_list(x, y);
    else if (companion_editor_page == 2) deck_editor_sounds(x, y);
    else if (companion_editor_page == 3) deck_editor_items(x, y);
    else if (companion_editor_page == 4) deck_editor_assets(x, y);
    else if (companion_editor_page == 5) deck_editor_decorations(x, y);
    else deck_editor_layout(x, y);
}

int launcher_companion_layout_target_game(int selected_game)
{
    if (companion_editor_open && companion_editor_game >= 0 && companion_editor_game < N_GAMES)
        return companion_editor_game;
    return selected_game;
}

int launcher_companion_layout_preview_enabled(void)
{
    return companion_editor_open && companion_editor_page != 0;
}

int launcher_companion_layout_preview_item(void)
{
    if (!companion_editor_open || companion_editor_page != 3) return 0;
    if (companion_editor_preview_item < 1 ||
        companion_editor_preview_item >= N_DECK_ITEM_TEXTURE_IDS) return 0;
    if (companion_editor_preview_item == 3 && companion_editor_game != GAME_SML2) return 0;
    return companion_editor_preview_item;
}

static int deck_bundle_path_is_relative(const char *value)
{
    if (!value || !value[0] || value[0] == '/' || value[0] == '\\' ||
        strchr(value, '\\') || (strlen(value) > 1 && value[1] == ':'))
        return 0;
    const char *segment = value;
    for (const char *p = value; ; p++) {
        if (*p == '/' || *p == 0) {
            size_t length = (size_t)(p - segment);
            if (length == 0 || (length == 1 && segment[0] == '.') ||
                (length == 2 && segment[0] == '.' && segment[1] == '.'))
                return 0;
            if (!*p) break;
            segment = p + 1;
        }
    }
    return 1;
}

static void deck_set_path(char *dst, size_t cap, const char *value,
                          const char *base_dir)
{
    if (!dst || cap == 0) return;
    dst[0] = 0;
    if (!value || !value[0]) return;
    /* Bundles may reference only assets inside their own extracted directory. */
    if (!deck_bundle_path_is_relative(value)) return;
    if (base_dir && base_dir[0])
        snprintf(dst, cap, "%s%s%s", base_dir,
                 (base_dir[strlen(base_dir) - 1] == '/' ||
                  base_dir[strlen(base_dir) - 1] == '\\') ? "" : "/", value);
}

static int deck_manifest_apply(int game, const char *path)
{
    if (game < 0 || game >= N_GAMES || !path || !path[0]) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char directory[1024];
    snprintf(directory, sizeof directory, "%s", path);
    char *slash = strrchr(directory, '/');
#ifdef _WIN32
    char *backslash = strrchr(directory, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
#endif
    if (slash) slash[1] = 0; else directory[0] = 0;
    CompanionLayoutCfg fresh;
    deck_layout_defaults(&fresh);
    char line[1400];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = 0;
        size_t n = strlen(eq);
        while (n && (eq[n - 1] == '\n' || eq[n - 1] == '\r')) eq[--n] = 0;
        int v = atoi(eq);
        if (!strcmp(line, "header_dx")) fresh.header_dx = v;
        else if (!strcmp(line, "header_dy")) fresh.header_dy = v;
        else if (!strcmp(line, "header_scale")) fresh.header_scale = v;
        else if (!strcmp(line, "header_scale_y")) fresh.header_scale_y = v;
        else if (!strcmp(line, "item_dx")) fresh.item_dx = v;
        else if (!strcmp(line, "item_dy")) fresh.item_dy = v;
        else if (!strcmp(line, "item_scale")) fresh.item_scale = v;
        else if (!strcmp(line, "item_scale_y")) fresh.item_scale_y = v;
        else if (!strcmp(line, "controls_dx")) fresh.controls_dx = v;
        else if (!strcmp(line, "controls_dy")) fresh.controls_dy = v;
        else if (!strcmp(line, "controls_scale")) fresh.controls_scale = v;
        else if (!strcmp(line, "controls_scale_y")) fresh.controls_scale_y = v;
        else if (!strcmp(line, "timeline_dx")) fresh.timeline_dx = v;
        else if (!strcmp(line, "timeline_dy")) fresh.timeline_dy = v;
        else if (!strcmp(line, "timeline_scale")) fresh.timeline_scale = v;
        else if (!strcmp(line, "timeline_scale_y")) fresh.timeline_scale_y = v;
        else if (!strcmp(line, "status_dx")) fresh.status_dx = v;
        else if (!strcmp(line, "status_dy")) fresh.status_dy = v;
        else if (!strcmp(line, "status_scale")) fresh.status_scale = v;
        else if (!strcmp(line, "status_scale_y")) fresh.status_scale_y = v;
        else if (!strcmp(line, "decoration_dx")) fresh.decoration_dx = v;
        else if (!strcmp(line, "decoration_dy")) fresh.decoration_dy = v;
        else if (!strcmp(line, "decoration_scale")) fresh.decoration_scale = v;
        else if (!strcmp(line, "decoration_scale_y")) fresh.decoration_scale_y = v;
        else if (!strcmp(line, "decoration_2_dx")) fresh.decoration_extra_dx[0] = v;
        else if (!strcmp(line, "decoration_2_dy")) fresh.decoration_extra_dy[0] = v;
        else if (!strcmp(line, "decoration_2_scale")) fresh.decoration_extra_scale[0] = v;
        else if (!strcmp(line, "decoration_2_scale_y")) fresh.decoration_extra_scale_y[0] = v;
        else if (!strcmp(line, "decoration_3_dx")) fresh.decoration_extra_dx[1] = v;
        else if (!strcmp(line, "decoration_3_dy")) fresh.decoration_extra_dy[1] = v;
        else if (!strcmp(line, "decoration_3_scale")) fresh.decoration_extra_scale[1] = v;
        else if (!strcmp(line, "decoration_3_scale_y")) fresh.decoration_extra_scale_y[1] = v;
        else if (!strcmp(line, "decoration_4_dx")) fresh.decoration_extra_dx[2] = v;
        else if (!strcmp(line, "decoration_4_dy")) fresh.decoration_extra_dy[2] = v;
        else if (!strcmp(line, "decoration_4_scale")) fresh.decoration_extra_scale[2] = v;
        else if (!strcmp(line, "decoration_4_scale_y")) fresh.decoration_extra_scale_y[2] = v;
        else if (!strcmp(line, "decoration_5_dx")) fresh.decoration_extra_dx[3] = v;
        else if (!strcmp(line, "decoration_5_dy")) fresh.decoration_extra_dy[3] = v;
        else if (!strcmp(line, "decoration_5_scale")) fresh.decoration_extra_scale[3] = v;
        else if (!strcmp(line, "decoration_5_scale_y")) fresh.decoration_extra_scale_y[3] = v;
        else if (!strcmp(line, "decoration_6_dx")) fresh.decoration_extra_dx[4] = v;
        else if (!strcmp(line, "decoration_6_dy")) fresh.decoration_extra_dy[4] = v;
        else if (!strcmp(line, "decoration_6_scale")) fresh.decoration_extra_scale[4] = v;
        else if (!strcmp(line, "decoration_6_scale_y")) fresh.decoration_extra_scale_y[4] = v;
        else if (!strcmp(line, "decoration_7_dx")) fresh.decoration_extra_dx[5] = v;
        else if (!strcmp(line, "decoration_7_dy")) fresh.decoration_extra_dy[5] = v;
        else if (!strcmp(line, "decoration_7_scale")) fresh.decoration_extra_scale[5] = v;
        else if (!strcmp(line, "decoration_7_scale_y")) fresh.decoration_extra_scale_y[5] = v;
        else if (!strcmp(line, "decoration_8_dx")) fresh.decoration_extra_dx[6] = v;
        else if (!strcmp(line, "decoration_8_dy")) fresh.decoration_extra_dy[6] = v;
        else if (!strcmp(line, "decoration_8_scale")) fresh.decoration_extra_scale[6] = v;
        else if (!strcmp(line, "decoration_8_scale_y")) fresh.decoration_extra_scale_y[6] = v;
        else if (!strcmp(line, "sounds_enabled")) fresh.sounds_enabled = v ? 1 : 0;
        else if (!strcmp(line, "background_path")) deck_set_path(fresh.background_path, sizeof fresh.background_path, eq, directory);
        else if (!strcmp(line, "button_texture_path")) deck_set_path(fresh.button_texture_path, sizeof fresh.button_texture_path, eq, directory);
        else if (!strcmp(line, "item_texture_path")) deck_set_path(fresh.item_texture_path, sizeof fresh.item_texture_path, eq, directory);
        else if (!strcmp(line, "item_texture_path_1")) deck_set_path(fresh.item_texture_by_id[1], sizeof fresh.item_texture_by_id[1], eq, directory);
        else if (!strcmp(line, "item_texture_path_2")) deck_set_path(fresh.item_texture_by_id[2], sizeof fresh.item_texture_by_id[2], eq, directory);
        else if (!strcmp(line, "item_texture_path_3")) deck_set_path(fresh.item_texture_by_id[3], sizeof fresh.item_texture_by_id[3], eq, directory);
        else if (!strcmp(line, "decoration_path")) deck_set_path(fresh.decoration_path, sizeof fresh.decoration_path, eq, directory);
        else if (!strcmp(line, "decoration_path_1")) deck_set_path(fresh.decoration_extra_path[0], sizeof fresh.decoration_extra_path[0], eq, directory);
        else if (!strcmp(line, "decoration_path_2")) deck_set_path(fresh.decoration_extra_path[1], sizeof fresh.decoration_extra_path[1], eq, directory);
        else if (!strcmp(line, "decoration_path_3")) deck_set_path(fresh.decoration_extra_path[2], sizeof fresh.decoration_extra_path[2], eq, directory);
        else if (!strcmp(line, "decoration_path_4")) deck_set_path(fresh.decoration_extra_path[3], sizeof fresh.decoration_extra_path[3], eq, directory);
        else if (!strcmp(line, "decoration_path_5")) deck_set_path(fresh.decoration_extra_path[4], sizeof fresh.decoration_extra_path[4], eq, directory);
        else if (!strcmp(line, "decoration_path_6")) deck_set_path(fresh.decoration_extra_path[5], sizeof fresh.decoration_extra_path[5], eq, directory);
        else if (!strcmp(line, "decoration_path_7")) deck_set_path(fresh.decoration_extra_path[6], sizeof fresh.decoration_extra_path[6], eq, directory);
        else if (!strcmp(line, "header_texture_path")) deck_set_path(fresh.header_texture_path, sizeof fresh.header_texture_path, eq, directory);
        else if (!strcmp(line, "status_texture_path")) deck_set_path(fresh.status_texture_path, sizeof fresh.status_texture_path, eq, directory);
        else if (!strcmp(line, "button_texture_save_path")) deck_set_path(fresh.button_texture_by_action[0], sizeof fresh.button_texture_by_action[0], eq, directory);
        else if (!strcmp(line, "button_texture_load_path")) deck_set_path(fresh.button_texture_by_action[1], sizeof fresh.button_texture_by_action[1], eq, directory);
        else if (!strcmp(line, "button_texture_pause_path")) deck_set_path(fresh.button_texture_by_action[2], sizeof fresh.button_texture_by_action[2], eq, directory);
        else if (!strcmp(line, "button_texture_rewind_path")) deck_set_path(fresh.button_texture_by_action[3], sizeof fresh.button_texture_by_action[3], eq, directory);
        else if (!strcmp(line, "button_texture_background_path")) deck_set_path(fresh.button_texture_by_action[4], sizeof fresh.button_texture_by_action[4], eq, directory);
        else if (!strcmp(line, "button_texture_close_path")) deck_set_path(fresh.button_texture_by_action[5], sizeof fresh.button_texture_by_action[5], eq, directory);
        else if (!strcmp(line, "item_sound_path")) deck_set_path(fresh.item_sound_path, sizeof fresh.item_sound_path, eq, directory);
        else if (!strcmp(line, "empty_item_sound_path")) deck_set_path(fresh.empty_item_sound_path, sizeof fresh.empty_item_sound_path, eq, directory);
        else if (!strncmp(line, "asset_stretch_", 13)) {
            char *end = NULL;
            long asset = strtol(line + 13, &end, 10);
            if (end && *end == 0 && asset >= 0 && asset < N_DECK_ASSET_PATHS)
                fresh.asset_stretch[asset] = v ? 1 : 0;
        }
        else {
            for (int b = 0; b < N_DECK_BUTTON_SOUNDS; b++) {
                char key[48];
                snprintf(key, sizeof key, "button_sound_path%d", b);
                if (!strcmp(line, key)) {
                    deck_set_path(fresh.button_sound_path[b], sizeof fresh.button_sound_path[b], eq, directory);
                    break;
                }
            }
        }
    }
    fclose(f);
    if (fresh.header_dx < -400) fresh.header_dx = -400; if (fresh.header_dx > 400) fresh.header_dx = 400;
    if (fresh.header_dy < -400) fresh.header_dy = -400; if (fresh.header_dy > 400) fresh.header_dy = 400;
    if (fresh.item_dx < -400) fresh.item_dx = -400; if (fresh.item_dx > 400) fresh.item_dx = 400;
    if (fresh.item_dy < -400) fresh.item_dy = -400; if (fresh.item_dy > 400) fresh.item_dy = 400;
    if (fresh.controls_dx < -400) fresh.controls_dx = -400; if (fresh.controls_dx > 400) fresh.controls_dx = 400;
    if (fresh.controls_dy < -400) fresh.controls_dy = -400; if (fresh.controls_dy > 400) fresh.controls_dy = 400;
    if (fresh.timeline_dx < -400) fresh.timeline_dx = -400; if (fresh.timeline_dx > 400) fresh.timeline_dx = 400;
    if (fresh.timeline_dy < -400) fresh.timeline_dy = -400; if (fresh.timeline_dy > 400) fresh.timeline_dy = 400;
    if (fresh.status_dx < -400) fresh.status_dx = -400; if (fresh.status_dx > 400) fresh.status_dx = 400;
    if (fresh.status_dy < -400) fresh.status_dy = -400; if (fresh.status_dy > 400) fresh.status_dy = 400;
    if (fresh.decoration_dx < -400) fresh.decoration_dx = -400; if (fresh.decoration_dx > 400) fresh.decoration_dx = 400;
    if (fresh.decoration_dy < -400) fresh.decoration_dy = -400; if (fresh.decoration_dy > 400) fresh.decoration_dy = 400;
    if (fresh.header_scale < 25) fresh.header_scale = 25; if (fresh.header_scale > 250) fresh.header_scale = 250;
    if (fresh.item_scale < 25) fresh.item_scale = 25; if (fresh.item_scale > 250) fresh.item_scale = 250;
    if (fresh.controls_scale < 25) fresh.controls_scale = 25; if (fresh.controls_scale > 250) fresh.controls_scale = 250;
    if (fresh.timeline_scale < 25) fresh.timeline_scale = 25; if (fresh.timeline_scale > 250) fresh.timeline_scale = 250;
    if (fresh.status_scale < 25) fresh.status_scale = 25; if (fresh.status_scale > 250) fresh.status_scale = 250;
    if (fresh.status_scale_y < 25) fresh.status_scale_y = 25; if (fresh.status_scale_y > 250) fresh.status_scale_y = 250;
    if (fresh.decoration_scale < 25) fresh.decoration_scale = 25; if (fresh.decoration_scale > 250) fresh.decoration_scale = 250;
    settings.deck_layout[game] = fresh;
    return 1;
}

void launcher_companion_layout_picker_result(int kind, int game, const char *path)
{
    if (game < 0 || game >= N_GAMES) return;
    if (!path || !path[0]) return; /* user canceled the Android picker */
    CompanionLayoutCfg *d = &settings.deck_layout[game];
    if (kind == DECK_PICK_IMPORT) {
        if (deck_manifest_apply(game, path)) {
            companion_editor_game = game;
            companion_editor_page = 1;
            companion_editor_open = 1;
            companion_editor_dragging = companion_editor_resizing = 0;
            companion_redo_count = 0;
            settings_save();
            launcher_toast("Companion Deck bundle imported.");
        } else launcher_toast("That bundle has no valid layout manifest.");
        return;
    }
    if (kind == DECK_PICK_EXPORT) {
        launcher_toast("Companion Deck bundle exported.");
        return;
    }
    if (kind == DECK_PICK_BACKGROUND) snprintf(d->background_path, sizeof d->background_path, "%s", path);
    else if (kind == DECK_PICK_BUTTON_TEXTURE) snprintf(d->button_texture_path, sizeof d->button_texture_path, "%s", path);
    else if (kind == DECK_PICK_ITEM_TEXTURE) snprintf(d->item_texture_path, sizeof d->item_texture_path, "%s", path);
    else if (kind == DECK_PICK_ITEM_MUSHROOM) snprintf(d->item_texture_by_id[1], sizeof d->item_texture_by_id[1], "%s", path);
    else if (kind == DECK_PICK_ITEM_SPECIAL) snprintf(d->item_texture_by_id[2], sizeof d->item_texture_by_id[2], "%s", path);
    else if (kind == DECK_PICK_ITEM_CARROT) snprintf(d->item_texture_by_id[3], sizeof d->item_texture_by_id[3], "%s", path);
    else if (kind == DECK_PICK_HEADER_TEXTURE) snprintf(d->header_texture_path, sizeof d->header_texture_path, "%s", path);
    else if (kind == DECK_PICK_STATUS_TEXTURE) snprintf(d->status_texture_path, sizeof d->status_texture_path, "%s", path);
    else if (kind == DECK_PICK_BUTTON_SAVE_TEXTURE) snprintf(d->button_texture_by_action[0], sizeof d->button_texture_by_action[0], "%s", path);
    else if (kind == DECK_PICK_BUTTON_LOAD_TEXTURE) snprintf(d->button_texture_by_action[1], sizeof d->button_texture_by_action[1], "%s", path);
    else if (kind == DECK_PICK_BUTTON_PAUSE_TEXTURE) snprintf(d->button_texture_by_action[2], sizeof d->button_texture_by_action[2], "%s", path);
    else if (kind == DECK_PICK_BUTTON_REWIND_TEXTURE) snprintf(d->button_texture_by_action[3], sizeof d->button_texture_by_action[3], "%s", path);
    else if (kind == DECK_PICK_BUTTON_BACKGROUND_TEXTURE) snprintf(d->button_texture_by_action[4], sizeof d->button_texture_by_action[4], "%s", path);
    else if (kind == DECK_PICK_BUTTON_CLOSE_TEXTURE) snprintf(d->button_texture_by_action[5], sizeof d->button_texture_by_action[5], "%s", path);
    else if (kind == DECK_PICK_DECORATION) {
        int slot = companion_editor_pending_decoration_slot;
        if (slot < 0 || slot >= N_DECK_DECORATIONS) return;
        char *target = deck_decoration_path(d, slot);
        if (!target) return;
        snprintf(target, 512, "%s", path);
        companion_editor_element = DECK_ELEM_DECORATION + slot;
    }
    else if (kind >= DECK_PICK_SOUND_SAVE && kind <= DECK_PICK_SOUND_CLOSE)
        snprintf(d->button_sound_path[kind - DECK_PICK_SOUND_SAVE],
                 sizeof d->button_sound_path[0], "%s", path);
    else if (kind == DECK_PICK_ITEM_SOUND) snprintf(d->item_sound_path, sizeof d->item_sound_path, "%s", path);
    else if (kind == DECK_PICK_EMPTY_SOUND) snprintf(d->empty_item_sound_path, sizeof d->empty_item_sound_path, "%s", path);
    settings_save();
    launcher_toast("Companion Deck asset assigned.");
}
#endif /* Android or developer build */

static void tab_display(float x, float y)
{
#if defined(__ANDROID__) || defined(PIPECLEAN_DEV_BUILD)
    if (companion_editor_open) {
        deck_editor_draw(x, y);
        return;
    }
#endif
    ui_rrect(x + 18, y + 70, 772, 404, 12, C_PANEL);
    ui_text(F_BOLD, 15, x + 36, y + 86, C_TEXT, "LAUNCHER DISPLAY");
    ui_text_wrap(F_REG, 12, x + 36, y + 112, 724, C_MUTED,
                 "These options affect the launcher on desktop and Android. Per-game image shape and scaling remain in each game's Display page.", 3);

    ui_text(F_REG, 13, x + 36, y + 165, C_TEXT, "Interface size");
    int scale_changed = ui_slider(x + 210, y + 160, 340,
                                  &settings.launcher_ui_scale, 75, 125);
    char scale_label[24];
    snprintf(scale_label, sizeof scale_label, "%d%%", settings.launcher_ui_scale);
    ui_text_r(F_BOLD, 13, x + 610, y + 164, C_TEXT, scale_label);
    if (scale_changed) {
        ui_set_scale_preference(settings.launcher_ui_scale);
        settings_save();
    }
    ui_text_fit(F_REG, 11, x + 36, y + 190, 700, C_DIM,
                "Use 75–90% when more of the interface needs to fit. Larger values improve readability but may crop the edges.");

    ui_rect(x + 36, y + 226, 728, 1, C_LINE);
#if defined(__ANDROID__)
    ui_text(F_REG, 13, x + 36, y + 245, C_TEXT, "Layout behavior");
    ui_text(F_REG, 13, x + 210, y + 245, C_OK, "Adapts to device");
    ui_text_fit(F_REG, 11, x + 36, y + 287, 700, C_DIM,
                "Android sizes the app to your device. Interface size still applies; rotate or resize the app window if your device supports it.");

    ui_text(F_REG, 13, x + 36, y + 327, C_TEXT, "Second-screen Companion Deck");
    if (ui_toggle(x + 310, y + 323, &settings.launcher_dual_screen))
        settings_save();
    ui_text_fit(F_REG, 10, x + 36, y + 352, 710, C_DIM,
                settings.launcher_dual_screen
                    ? "Track changes slide a vinyl/song banner onto the bottom of the Companion Deck. Playlist stays in Audio & Menu."
                    : "Track changes slide a vinyl/song banner into the launcher’s top-left. Playlist stays in Audio & Menu.");

    ui_text(F_REG, 13, x + 36, y + 380, C_TEXT, "Auto Pipe Swap (SML1)");
    if (ui_toggle(x + 310, y + 376, &settings.launcher_pipe_screen_swap))
        settings_save();
    ui_text_fit(F_REG, 10, x + 36, y + 405, 710, C_DIM,
                "When enabled, Super Mario Land swaps gameplay to the lower display in pipe and bonus rooms. Requires the Companion Deck.");
#else
    ui_text(F_REG, 13, x + 36, y + 245, C_TEXT, "Desktop window size");
    static const char *sizes[] = {"Compact", "Balanced", "Large"};
    int size = settings.launcher_window_size;
    if (ui_seg(x + 210, y + 237, 400, 38, sizes, 3, &size)) {
        settings.launcher_window_size = size;
        settings_save();
    }
    ui_text(F_REG, 13, x + 36, y + 297, C_TEXT, "Fullscreen launcher");
    if (ui_toggle(x + 210, y + 293, &settings.launcher_fullscreen))
        settings_save();
    ui_text_fit(F_REG, 11, x + 36, y + 330, 700, C_DIM,
                "Window size is restored when fullscreen is turned off. The game's own Display settings can still change game window size.");
    ui_text(F_REG, 13, x + 36, y + 369, C_TEXT, "Companion Deck simulator");
    if (ui_toggle(x + 310, y + 365, &settings.launcher_dual_screen)) {
        settings_save();
        launcher_toast(settings.launcher_dual_screen
            ? "Companion Deck simulator enabled. A second window will open."
            : "Companion Deck simulator disabled.");
    }
    ui_text_fit(F_REG, 10, x + 36, y + 396, 710, C_DIM,
                "Opens a resizable second window for testing deck layouts and game state without a physical second display.");
#endif
    if (ui_button(x + 36, y + 434, 180, 30, "Reset display options", B_NORMAL, 1)) {
        settings.launcher_ui_scale = 100;
        settings.launcher_window_size = 1;
        settings.launcher_fullscreen = 0;
        settings.launcher_dual_screen = 0;
        settings.launcher_pipe_screen_swap = 0;
        ui_set_scale_preference(100);
        settings_save();
        launcher_toast("Launcher display options reset.");
    }
#if defined(__ANDROID__) || defined(PIPECLEAN_DEV_BUILD)
    if (settings.launcher_dual_screen) {
        if (ui_button(x + 230, y + 434, 280, 30, "Companion Deck layout editor", B_PRIMARY, 1)) {
            companion_editor_open = 1;
            companion_editor_page = 0;
            companion_editor_game = GAME_SML;
            deck_editor_history_clear();
        }
    } else {
        ui_text_fit(F_REG, 10, x + 230, y + 441, 520, C_DIM,
                    "Enable the Companion Deck to edit its per-game layouts.");
    }
#endif
}

/* ------------------------------------------------------------------ frame */

/* ------------------------------------------------------------------ frame */
static const char *tab_labels[N_TABS] = {
    [TAB_MULTIPLAYER] = "Multiplayer",
    [TAB_MP_CONTROLLERS] = "MP Controllers",
    [TAB_FILTERS] = "Filters",
    [TAB_AUDIO] = "Audio & menu",
    [TAB_ONLINE] = "Online",
    [TAB_DISPLAY] = "Display"
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
    NetplayInfo online_frame_info;
    netplay_get_info(&online_frame_info);
    if ((online_frame_info.state == NETPLAY_HOSTING ||
         online_frame_info.state == NETPLAY_CONNECTED) &&
        online_last_netplay_state != NETPLAY_HOSTING &&
        online_last_netplay_state != NETPLAY_CONNECTED)
        tab = TAB_ONLINE;
    online_last_netplay_state = online_frame_info.state;
    {
        int connected_peer = online_frame_info.state == NETPLAY_CONNECTED &&
                             online_frame_info.peer[0] != 0;
        int selected_game = netplay_lobby_get_game();
        if (!connected_peer) {
            online_rom_reported_game = -1;
        } else if ((selected_game == GAME_SML || selected_game == GAME_SML2) &&
                   online_rom_reported_game != selected_game) {
            online_report_local_rom(selected_game);
        }
        int shared_start = netplay_take_start_request();
        if (shared_start >= 0) res.play = shared_start;
    }
    launcher_controller_update(dt);
    anim_clock += dt;
    if (tab != TAB_AUDIO && music_notice_deferred && music_notice_age >= 0.0f) {
        /* Re-enter the animation at the start when leaving the management tab. */
        music_notice_age = 0.0f;
        music_notice_deferred = 0;
    }
    if (music_notice_age >= 0.0f) {
        music_notice_age += dt;
        if (music_notice_age > 2.75f) {
            music_notice_age = -1.0f;
            music_notice_deferred = 0;
        }
    }
    int g = (tab == TAB_MULTIPLAYER || tab == TAB_MP_CONTROLLERS) ? mp_config_game : launcher_current_game();
    if (tab < N_GAMES) last_game_tab = tab;
    ui_accent = tab < N_GAMES ? games[tab].accent : ((tab == TAB_MULTIPLAYER || tab == TAB_MP_CONTROLLERS) ? games[mp_config_game].accent : 0x4C8DFF);
    ensure_background(g);
    bg_update(dt);

    /* backdrop */
    float vx = ui_view_x0(), vy = ui_view_y0(), vw = ui_view_w(), vh = ui_view_h();
#if defined(__ANDROID__)
    /* Android uses the two-row touch-first navigation on every screen size,
       instead of showing the desktop sidebar on high-resolution phones. */
    int compact_layout = 1;
#else
    int compact_layout = ui_scale < 0.75f;
#endif
    ui_vgrad(vx, vy, vw, vh, C_BG2, C_BG);
    int bw, bh;
    SDL_Texture *bt = bg_texture(&bw, &bh);
    if (bt) {
        draw_cover(bt, bw, bh, vx, vy, vw, vh);
        ui_rect(vx, vy, vw, vh, RGBA(8, 9, 14, (int)(60 + settings.g[g].bg_dim * 2.4f)));
    }

    /* Use only the supplied transparent PipeClean logo in the header. */
    SDL_Texture *brand_logo = branding_logo_texture();
    if (compact_layout) {
        float nav_left = vx + 170.0f;
        float nav_w = vw - 190.0f;
        if (nav_w < 620.0f) nav_w = 620.0f;
        if (brand_logo) ui_image(brand_logo, NULL, vx + 14, 10, 138, 68);
        /* Small windows move navigation into the header instead of squeezing
           the content beside a permanent sidebar. */
#if defined(__ANDROID__)
        const float game_nav_y = 4.0f, game_nav_h = 40.0f, game_label_y = 14.0f;
        const float global_nav_y = 46.0f, global_nav_h = 39.0f, global_label_y = 56.0f;
        const float nav_divider_y = 92.0f;
#else
        const float game_nav_y = 8.0f, game_nav_h = 32.0f, game_label_y = 17.0f;
        const float global_nav_y = 48.0f, global_nav_h = 32.0f, global_label_y = 57.0f;
        const float nav_divider_y = 90.0f;
#endif
        float game_w = nav_w / (float)N_GAMES;
        if (game_w > 220.0f) game_w = 220.0f;
        for (int i = 0; i < N_GAMES; i++) {
            float x = nav_left + i * game_w;
            if (tab_button(x, game_nav_y, game_w - 5, game_nav_h, tab == i, games[i].accent)) tab = i;
            ui_text_fit(F_BOLD, 13, x + 8, game_label_y, game_w - 20,
                        tab == i ? C_TEXT : C_MUTED, games[i].name);
        }
        int global_count = N_TABS - TAB_MULTIPLAYER;
        float global_w = nav_w / (float)global_count;
        if (global_w > 220.0f) global_w = 220.0f;
        for (int i = TAB_MULTIPLAYER; i < N_TABS; i++) {
            float x = nav_left + (i - TAB_MULTIPLAYER) * global_w;
            uint32_t accent = i == TAB_MULTIPLAYER ? games[mp_config_game].accent : 0x4C8DFF;
            if (tab_button(x, global_nav_y, global_w - 5, global_nav_h, tab == i, accent)) tab = i;
            ui_text_fit(F_BOLD, 12, x + 7, global_label_y, global_w - 17,
                        tab == i ? C_TEXT : C_MUTED, tab_labels[i]);
        }
        ui_rect(vx + 12, nav_divider_y, vw - 24, 1, C_LINE);
    } else {
        if (brand_logo) ui_image(brand_logo, NULL, 20, 1, 170, 83);
        ui_rect(20, 90, UI_W - 40, 1, C_LINE);

        /* Desktop sidebar */
        float sx = 24, sy = 108;
        int compact_games = N_GAMES > 4;
        float game_row = compact_games ? 46.0f : 76.0f;
        float game_h = compact_games ? 42.0f : 68.0f;
        for (int i = 0; i < N_GAMES; i++) {
            float y = sy + i * game_row;
            if (tab_button(sx, y, 224, game_h, tab == i, games[i].accent)) tab = i;
            if (compact_games) {
                ui_text_fit(F_BOLD, 13, sx + 14, y + 4, 202, tab == i ? C_TEXT : C_MUTED, games[i].name);
                int kind = rs[i].ok ? 1 : (settings.g[i].rom_path[0] ? 3 : 2);
                const char *msg = rs[i].ok ? "Ready to play" : (settings.g[i].rom_path[0] ? "ROM issue" : "ROM needed");
                status_line(sx + 14, y + 23, 202, kind, msg);
            } else {
                ui_text_fit(F_BOLD, 15, sx + 18, y + 11, 196, tab == i ? C_TEXT : C_MUTED, games[i].name);
                const GameCfg *gc = &settings.g[i];
                const char *ready = !rs[i].ok ? "ROM needed" :
                    (gc->romhack_selected >= 0 ? (hack_ok[i] ? "Ready with ROM hack" : "ROM hack selected") : "Ready to play");
                status_line(sx + 18, y + 38, 196, rs[i].ok ? 1 : 2, ready);
            }
        }
        float y2 = sy + N_GAMES * game_row + 8;
        ui_rect(sx + 8, y2, 208, 1, C_LINE);
        float nav_y = y2 + 8;
        int nav_count = N_TABS - TAB_MULTIPLAYER;
        float nav_available = UI_H - 52.0f - nav_y;
        float nav_step = nav_available / (float)nav_count;
        if (nav_step > 43.0f) nav_step = 43.0f;
        float nav_h = nav_step - 3.0f;
        if (nav_h > 38.0f) nav_h = 38.0f;
        for (int i = TAB_MULTIPLAYER; i < N_TABS; i++) {
            float y = nav_y + (i - TAB_MULTIPLAYER) * nav_step;
            uint32_t accent = i == TAB_MULTIPLAYER ? games[mp_config_game].accent : 0x4C8DFF;
            if (tab_button(sx, y, 224, nav_h, tab == i, accent)) tab = i;
            ui_text_fit(F_BOLD, nav_h < 32.0f ? 12 : 14, sx + 18,
                        y + (nav_h - 21.0f) * 0.5f, 194,
                        tab == i ? C_TEXT : C_MUTED, tab_labels[i]);
        }
    }

    float cx = compact_layout ? vx + (vw - 808.0f) * 0.5f : 268.0f, cy = 108;
    if (tab < N_GAMES) {
        GameCfg *c = &settings.g[tab];
        ui_accent = games[tab].accent;
        ui_text(F_BOLD, 28, cx, cy - 4, C_TEXT, games[tab].name);
        ui_text(F_REG, 13, cx, cy + 34, C_MUTED, games[tab].sub);
        int can = rs[tab].ok;
        const char *play_label = c->romhack_selected >= 0 ? "Play Hack" : "Play";
        if (ui_button(cx + 808 - 176, cy + 2, 176, 52, can ? play_label : "Needs ROM", B_PRIMARY, can)) res.play = tab;
        if (!can) hint_area(cx + 808 - 176, cy + 2, 176, 52, "Choose your ROM on the Game tab (or drop it on this window)");
        float y = cy + 118;
        pad_set_context(tab, 0);
        if (games[tab].external_player) {
            const char *external_sub_names[2] = {"Game", "ROM Hacks"};
            int external_sub = sub[tab] == SUB_ROMHACKS ? 1 : 0;
            ui_seg(cx, cy + 66, 808, 38, external_sub_names, 2, &external_sub);
            sub[tab] = external_sub ? SUB_ROMHACKS : SUB_GAME;
            if (sub[tab] == SUB_ROMHACKS) sub_romhacks(tab, cx, y);
            else sub_external_game(tab, cx, y);
        } else {
            ui_seg(cx, cy + 66, 808, 38, sub_names, N_SUB, &sub[tab]);
            switch (sub[tab]) {
        case SUB_GAME:
            sub_game(tab, cx, y);
            break;
        case SUB_ROMHACKS:
            sub_romhacks(tab, cx, y);
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
    } else if (tab == TAB_AUDIO) {
        ui_text(F_BOLD, 28, cx, cy - 4, C_TEXT, "Audio & menu");
        ui_text(F_REG, 13, cx, cy + 34, C_MUTED, "Game volume, the launcher's own music and the little click sounds.");
        tab_audio(cx, cy + 58);
    } else if (tab == TAB_ONLINE) {
        ui_text(F_BOLD, 28, cx, cy - 4, C_TEXT, "Online");
        ui_text(F_REG, 13, cx, cy + 34, C_MUTED, "Session invites, player identity, and cross-platform play.");
        tab_online(cx, cy + 58);
    } else {
        ui_text(F_BOLD, 28, cx, cy - 4, C_TEXT, "Display & layout");
        ui_text(F_REG, 13, cx, cy + 34, C_MUTED, "Adjust the launcher's size on desktop and Android.");
        tab_display(cx, cy + 58);
    }

    /* footer */
    const char *h = ui_hint_text();
    const char *footer = launcher_controller_cursor_active && !cap_kind && !multiplayer_name_editing
        ? "Controller: D-pad/left stick move | A select | B cancel | Move mouse to switch back"
        : (h && h[0] ? h : "Drag and drop a ROM, patch, picture, sound or texture folder anywhere on this window.");
    ui_text_fit(F_REG, 12, compact_layout ? vx + 14 : 28, compact_layout ? vy + vh - 26 : UI_H - 26,
                compact_layout ? vw - 28 : UI_W - 56, C_DIM, footer);

    /* toast */
    if (toast_t > 0) {
        toast_t -= dt;
        float a = toast_t < 0.4f ? toast_t / 0.4f : 1.0f;
        float tw = ui_text_w(F_BOLD, 14, toast_msg) + 40;
        float tx = (compact_layout ? vx + vw * 0.5f : UI_W * 0.5f) - tw * 0.5f;
        float ty = compact_layout ? vy + vh - 74 : UI_H - 74;
        ui_shadow(tx, ty, tw, 40, 12, 10, RGBA(0, 0, 0, (int)(90 * a)));
        ui_rrect(tx, ty, tw, 40, 12, RGBA(40, 46, 66, (int)(250 * a)));
        ui_text_c(F_BOLD, 14, compact_layout ? vx + vw * 0.5f : UI_W * 0.5f, ty + 10, RGBA(237, 239, 246, (int)(255 * a)), toast_msg);
    }

    /* Cancel an older notification when entering Audio & Menu, but retain a
       fresh track change made inside that page so it can show after leaving. */
    if (tab == TAB_AUDIO && !music_notice_deferred)
        music_notice_age = -1.0f;
    /* If navigation left Audio & Menu during this frame, start the deferred
       notification now so both the launcher and Companion Deck slide in together. */
    if (tab != TAB_AUDIO && music_notice_deferred && music_notice_age >= 0.0f) {
        music_notice_age = 0.0f;
        music_notice_deferred = 0;
    }
    /* Music status is a temporary overlay, never part of the Audio & Menu page. */
    menu_music_notice_draw(vx, vy);

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

    if (romhack_title_editing_game >= 0)
        ui_hint("Type a title for this ROM hack. Enter saves; Esc cancels.");
    if (wide_dirty && !ui_mouse.down) { make_preview(wide_dirty - 1); wide_dirty = 0; }
    if (ui_mouse.released) click_id = 0;
    settings.last_tab = tab;
    save_timer += dt;
    if (save_timer > 2.0f && !ui_mouse.down && !multiplayer_name_editing &&
        romhack_title_editing_game < 0) { save_timer = 0; settings_save(); }

    /* pad light follows the tab while in the launcher */
    pad_frame(dt);
    return res;
}

/* ------------------------------------------------------------------ events */
void launcher_event(const SDL_Event *e)
{
    if (e && (e->type == SDL_MOUSEMOTION || e->type == SDL_MOUSEBUTTONDOWN ||
              e->type == SDL_MOUSEWHEEL || e->type == SDL_FINGERDOWN ||
              e->type == SDL_FINGERMOTION || e->type == SDL_FINGERUP)) {
        launcher_leave_controller_cursor();
    }
    if (romhack_title_editing_game >= 0) {
        int g = romhack_title_editing_game, index = romhack_title_editing_index;
        if (g < 0 || g >= N_GAMES || index < 0 ||
            index >= settings.g[g].romhack_count || index >= MAX_ROMHACKS) {
            romhack_title_editing_game = romhack_title_editing_index = -1;
            SDL_StopTextInput();
            return;
        }
        RomHackEntry *entry = &settings.g[g].romhacks[index];
        if (e->type == SDL_TEXTINPUT) {
            size_t have = strlen(entry->title), add = strlen(e->text.text);
            if (have + add < sizeof entry->title &&
                (unsigned char)e->text.text[0] >= 32)
                memcpy(entry->title + have, e->text.text, add + 1);
            return;
        }
        if (e->type == SDL_KEYDOWN && !e->key.repeat) {
            SDL_Keycode k = e->key.keysym.sym;
            if (k == SDLK_ESCAPE) {
                snprintf(entry->title, sizeof entry->title, "%s", romhack_title_before);
                romhack_title_editing_game = romhack_title_editing_index = -1;
                SDL_StopTextInput();
                settings_save();
                return;
            }
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_TAB) {
                if (!entry->title[0])
                    romhack_title_from_path(entry->path, entry->title, sizeof entry->title);
                romhack_title_editing_game = romhack_title_editing_index = -1;
                SDL_StopTextInput();
                settings_save();
                return;
            }
            if (k == SDLK_BACKSPACE || k == SDLK_DELETE) {
                size_t len = strlen(entry->title);
                if (len) {
                    len--;
                    while (len && (((unsigned char)entry->title[len] & 0xC0u) == 0x80u)) len--;
                    entry->title[len] = 0;
                }
                return;
            }
            if ((e->key.keysym.mod & KMOD_CTRL) && k == SDLK_a) {
                entry->title[0] = 0;
                return;
            }
        }
        if ((e->type == SDL_WINDOWEVENT && e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) ||
            (e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_FINGERDOWN)) {
            if (!entry->title[0])
                romhack_title_from_path(entry->path, entry->title, sizeof entry->title);
            romhack_title_editing_game = romhack_title_editing_index = -1;
            SDL_StopTextInput();
            settings_save();
        } else {
            return;
        }
    }
    if (online_save_name_editing) {
        if (e->type == SDL_TEXTINPUT) {
            size_t have = strlen(online_save_name_buffer);
            for (const unsigned char *p = (const unsigned char *)e->text.text; *p; p++) {
                unsigned char ch = *p;
                if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                      (ch >= '0' && ch <= '9') || ch == ' ' || ch == '_' || ch == '-'))
                    continue;
                if (have + 1 >= sizeof online_save_name_buffer) break;
                online_save_name_buffer[have++] = (char)ch;
                online_save_name_buffer[have] = 0;
            }
            return;
        }
        if (e->type == SDL_KEYDOWN && !e->key.repeat) {
            SDL_Keycode key = e->key.keysym.sym;
            if (key == SDLK_ESCAPE) {
                online_save_name_editing = 0;
                online_save_name_slot = -1;
                SDL_StopTextInput();
                return;
            }
            if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_TAB) {
                online_commit_shared_name();
                return;
            }
            if (key == SDLK_BACKSPACE || key == SDLK_DELETE) {
                size_t length = strlen(online_save_name_buffer);
                if (length) online_save_name_buffer[length - 1] = 0;
                return;
            }
            if ((e->key.keysym.mod & KMOD_CTRL) && key == SDLK_a) {
                online_save_name_buffer[0] = 0;
                return;
            }
        }
        if ((e->type == SDL_WINDOWEVENT && e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) ||
            (e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_FINGERDOWN)) {
            online_commit_shared_name();
        }
        return;
    }
    if (online_name_editing || online_code_editing || online_server_editing || online_chat_editing) {
        int editing_name = online_name_editing;
        int editing_server = online_server_editing;
        int editing_chat = online_chat_editing;
        char *dst = editing_name ? settings.online_name :
                    (editing_server ? settings.online_server :
                     (editing_chat ? online_chat_buffer : online_join_code));
        size_t cap = editing_name ? sizeof settings.online_name :
                     (editing_server ? sizeof settings.online_server :
                      (editing_chat ? sizeof online_chat_buffer : sizeof online_join_code));
        const char *fallback = editing_name ? "Player" :
                              (editing_server ? PIPECLEAN_DEFAULT_RELAY_ENDPOINT : "");
        if (!dst || cap == 0) {
            online_name_editing = online_code_editing = online_server_editing = online_chat_editing = 0;
            SDL_StopTextInput();
            return;
        }
        if (e->type == SDL_TEXTINPUT) {
            if (editing_name) {
                size_t have = strlen(dst), add = strlen(e->text.text);
                if (have + add < cap && have + add <= 24 &&
                    (unsigned char)e->text.text[0] >= 32)
                    memcpy(dst + have, e->text.text, add + 1);
            } else if (editing_server) {
                size_t have = strlen(dst);
                for (const unsigned char *p = (const unsigned char *)e->text.text;
                     *p && have + 1 < cap && have < 150; ++p) {
                    unsigned char ch = *p;
                    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' ||
                        ch == '_' || ch == ':') {
                        dst[have++] = (char)ch;
                        dst[have] = 0;
                    }
                }
            } else if (editing_chat) {
                size_t have = strlen(dst), add = strlen(e->text.text);
                int valid = add > 0 && have + add < cap && have + add <= 88;
                for (size_t i = 0; valid && i < add; i++) {
                    unsigned char ch = (unsigned char)e->text.text[i];
                    if (ch < 32 || ch == 127) valid = 0;
                }
                if (valid) memcpy(dst + have, e->text.text, add + 1);
            } else {
                size_t have = strlen(dst);
                for (const unsigned char *p = (const unsigned char *)e->text.text;
                     *p && have + 1 < cap; ++p) {
                    unsigned char ch = *p;
                    if (ch >= 'a' && ch <= 'z') ch = (unsigned char)(ch - 'a' + 'A');
                    if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-') {
                        dst[have++] = (char)ch;
                        dst[have] = 0;
                    }
                }
            }
            return;
        }
        if (e->type == SDL_KEYDOWN && !e->key.repeat) {
            SDL_Keycode k = e->key.keysym.sym;
            if (k == SDLK_ESCAPE) {
                if (editing_server)
                    snprintf(settings.online_server, sizeof settings.online_server, "%s", online_server_before);
                online_name_editing = online_code_editing = online_server_editing = online_chat_editing = 0;
                SDL_StopTextInput();
                if (editing_name && !dst[0]) snprintf(dst, cap, "%s", fallback);
                return;
            }
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_TAB) {
                if (editing_chat) {
                    if (dst[0]) {
                        if (netplay_chat_send(dst)) dst[0] = 0;
                        else launcher_toast("Couldn't send that chat message.");
                    }
                }
                online_name_editing = online_code_editing = online_server_editing = online_chat_editing = 0;
                SDL_StopTextInput();
                if (!editing_chat && !dst[0]) snprintf(dst, cap, "%s", fallback);
                if (editing_name || editing_server) settings_save();
                return;
            }
            if (k == SDLK_BACKSPACE || k == SDLK_DELETE) {
                size_t len = strlen(dst);
                if (len) {
                    if (editing_name || editing_chat) {
                        len--;
                        while (len && (((unsigned char)dst[len] & 0xC0u) == 0x80u)) len--;
                    } else len--;
                    dst[len] = 0;
                }
                return;
            }
            if ((e->key.keysym.mod & KMOD_CTRL) && k == SDLK_a) {
                dst[0] = 0;
                return;
            }
        }
        if ((e->type == SDL_WINDOWEVENT && e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) ||
            (e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_FINGERDOWN)) {
            online_name_editing = online_code_editing = online_server_editing = online_chat_editing = 0;
            SDL_StopTextInput();
            if (!editing_chat && !dst[0]) snprintf(dst, cap, "%s", fallback);
            if (editing_name || editing_server) settings_save();
        }
        return;
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
            (e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_FINGERDOWN)) {
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
    int is_patch = ends_with(path, ".ips") || ends_with(path, ".bps") || ends_with(path, ".ups");
    int is_gb_rom = ends_with(path, ".gb") || ends_with(path, ".gbc");

    /* On the ROM Hacks tab, a supported full patched ROM is a secondary
     * profile, not a replacement for the game's configured base ROM. */
    if (tab < N_GAMES && sub[tab] == SUB_ROMHACKS && (is_patch || is_gb_rom)) {
        romhack_add(tab, path);
        return;
    }

    int id = rom_identify_file(path);
    if (id >= 0) {
        snprintf(settings.g[id].rom_path, sizeof settings.g[id].rom_path, "%s", path);
        rom_check(id);
        if (rs[id].ok) make_preview(id);
        tab = id;
        launcher_toast(rs[id].ok ? "ROM added." : rs[id].msg);
        return;
    }
    if (is_patch) {
        if (tab >= N_GAMES) tab = g;
        if (tab >= 0 && tab < N_GAMES) {
            romhack_add(tab, path);
            sub[tab] = SUB_ROMHACKS;
        } else launcher_toast("Choose a game's tab before adding a ROM hack.");
        return;
    }
    if (ends_with(path, ".png") || ends_with(path, ".jpg") || ends_with(path, ".jpeg") || ends_with(path, ".bmp") || ends_with(path, ".gif") || ends_with(path, ".tga")) {
        if (tab < N_GAMES && sub[tab] == SUB_ROMHACKS &&
            settings.g[tab].romhack_selected >= 0 &&
            settings.g[tab].romhack_selected < settings.g[tab].romhack_count) {
            RomHackEntry *entry = &settings.g[tab].romhacks[settings.g[tab].romhack_selected];
            snprintf(entry->thumbnail, sizeof entry->thumbnail, "%s", path);
            clear_romhack_thumbs_game(tab);
            settings_save();
            launcher_toast("ROM hack thumbnail changed.");
            return;
        }
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
    if (is_gb_rom) { launcher_toast("That ROM isn't one of the supported games."); return; }
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