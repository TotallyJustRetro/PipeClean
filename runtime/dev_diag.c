#include "dev_diag.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef PIPECLEAN_DEV_BUILD

void dev_diag_begin(int game, int player_count, SDL_Window *game_window)
{ (void)game; (void)player_count; (void)game_window; }
void dev_diag_event(const char *message) { (void)message; }
void dev_diag_frame(const Frame *frame, int game, int player_count,
                    const uint8_t buttons[MAX_MP_PLAYERS],
                    const uint8_t dpad[MAX_MP_PLAYERS], int paused)
{
    (void)frame; (void)game; (void)player_count;
    (void)buttons; (void)dpad; (void)paused;
}
void dev_diag_end(const char *reason) { (void)reason; }
void dev_diag_capture_screen(SDL_Renderer *game_renderer) { (void)game_renderer; }
int dev_diag_handle_event(const SDL_Event *event) { (void)event; return 0; }

#else

#include <SDL_image.h>
#include <time.h>
#include <ctype.h>
#include "games.h"
#include "settings.h"
#include "util.h"

#define DIAG_WIDTH 960
#define DIAG_HEIGHT 640
#define DIAG_MAX_EVENTS 7
#define DIAG_SCALE 2

typedef struct {
    uint64_t frame;
    uint32_t render_hash;
    EmuMpTestSnapshot sml2;
    int has_sml2;
    int game, player_count, paused;
    uint8_t buttons[MAX_MP_PLAYERS], dpad[MAX_MP_PLAYERS];
    uint8_t visible[MAX_MP_PLAYERS], sprites[MAX_MP_PLAYERS];
    int pos_x[MAX_MP_PLAYERS], pos_y[MAX_MP_PLAYERS];
    int screen_x[MAX_MP_PLAYERS], screen_y[MAX_MP_PLAYERS];
    int grounded[MAX_MP_PLAYERS], in_air[MAX_MP_PLAYERS], lives[MAX_MP_PLAYERS];
} DiagSample;

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    Uint32 window_id;
    FILE *log;
    char log_path[1400];
    char game_name[64];
    uint64_t frame_count;
    const Frame *current_frame;
    Uint32 last_draw_ticks;
    uint32_t last_render_hash;
    int player_count;
    int has_sample;
    int movement_stall[MAX_MP_PLAYERS];
    int movement_warned[MAX_MP_PLAYERS];
    int sprite_warned[MAX_MP_PLAYERS];
    int last_level, last_bank;
    int last_lives[MAX_MP_PLAYERS];
    uint32_t last_coins;
    uint32_t last_map_hash;
    uint16_t last_camera_x, last_camera_y;
    uint64_t last_dump_frame, last_screenshot_frame, last_periodic_frame;
    uint64_t last_bg_snapshot_frame;
    int screenshot_pending;
    char screenshot_reason[80];
    char recent[DIAG_MAX_EVENTS][128];
    int recent_head, recent_count;
    DiagSample sample;
} DiagState;

static DiagState diag;


static void write_json_string(FILE *file, const char *value)
{
    if (!file) return;
    fputc('"', file);
    if (value) {
        for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
            if (*p == '"' || *p == '\\') fputc('\\', file);
            if (*p >= 32) fputc(*p, file);
        }
    }
    fputc('"', file);
}

static void write_hex_bytes(FILE *file, const uint8_t *bytes, size_t count)
{
    if (!file || !bytes) return;
    for (size_t i = 0; i < count; i++)
        fprintf(file, "%02X", bytes[i]);
}

static void log_memory_snapshot(const char *reason)
{
    if (!diag.log) return;

    EmuMpTestSnapshot state;
    int have_sml2 = emu_mp_test_snapshot(&state) == 0;
    uint8_t player_ram[0xE0];
    uint8_t world_ram[0x1800];
    int have_ram = have_sml2 &&
        emu_mp_test_read_ram(0xA200, player_ram, sizeof player_ram) == 0 &&
        emu_mp_test_read_ram(0xA800, world_ram, sizeof world_ram) == 0;

    fprintf(diag.log, "{\"type\":\"incident_snapshot\",\"frame\":%llu,\"reason\":",
            (unsigned long long)diag.frame_count);
    write_json_string(diag.log, reason ? reason : "unknown");
    fputs(",\"game\":", diag.log);
    write_json_string(diag.log, diag.game_name);
    fprintf(diag.log, ",\"players\":%d,\"sml2_available\":%s",
            diag.player_count, have_sml2 ? "true" : "false");
    if (have_sml2) {
        fprintf(diag.log,
            ",\"state\":{\"p1_world\":[%u,%u],\"p2_world\":[%u,%u],"
            "\"p1_screen\":[%u,%u],\"p2_screen\":[%u,%u],"
            "\"camera\":[%u,%u],\"level\":[%u,%u],\"mode\":%u,"
            "\"lives\":[%u,%u],\"spawned\":%u,\"grounded\":[%u,%u],"
            "\"in_air\":[%u,%u],\"coins\":[%u,%u],"
            "\"hashes\":[\"%08X\",\"%08X\",\"%08X\",\"%08X\"],"
            "\"tile_patches\":%u}",
            state.p1_world_x, state.p1_world_y, state.p2_world_x, state.p2_world_y,
            state.p1_screen_x, state.p1_screen_y, state.p2_screen_x, state.p2_screen_y,
            state.camera_x, state.camera_y, state.level, state.level_bank, state.game_mode,
            state.p1_lives, state.p2_lives, state.p2_spawned,
            state.p1_grounded, state.p2_grounded, state.p1_in_air, state.p2_in_air,
            state.coins_low, state.coins_high, state.bg_map_hash, state.level_ram_hash,
            state.actor_region_hash, diag.last_render_hash, state.tile_patch_count);
    }
    if (have_ram) {
        fputs(",\"ram_hex\":{\"A200_A2DF\":\"", diag.log);
        write_hex_bytes(diag.log, player_ram, sizeof player_ram);
        fputs("\",\"A800_BFFF\":\"", diag.log);
        write_hex_bytes(diag.log, world_ram, sizeof world_ram);
        fputc('"', diag.log);
        fputc('}', diag.log);
    } else {
        fputs(",\"ram_hex_available\":false", diag.log);
    }
    if (have_sml2 && diag.current_frame) {
        const Frame *f = diag.current_frame;
        const uint8_t *p2_tiles = emu_mp_player_sprite_tiles(1, 0);
        const uint8_t *p2_tiles_cgb1 = emu_mp_player_sprite_tiles(1, 1);
        fputs(",\"visual_hex\":{\"bg_map_9800_9BFF\":\"", diag.log);
        write_hex_bytes(diag.log, f->bg_map, sizeof f->bg_map);
        fputs("\",\"vram_tiles_8000_97FF\":\"", diag.log);
        write_hex_bytes(diag.log, f->tiles, sizeof f->tiles);
        fputs("\",\"p1_oam\":\"", diag.log);
        write_hex_bytes(diag.log, f->mario_oam, sizeof f->mario_oam);
        fputs("\",\"p2_oam\":\"", diag.log);
        write_hex_bytes(diag.log, f->mp_player_oam[1], sizeof f->mp_player_oam[1]);
        fprintf(diag.log, "\",\"p2_sprite_count\":%u", f->mp_player_sprite_count[1]);
        if (p2_tiles) {
            fputs(",\"p2_sprite_tiles_bank0\":\"", diag.log);
            write_hex_bytes(diag.log, p2_tiles, 0x1000);
            fputc('"', diag.log);
        }
        if (p2_tiles_cgb1) {
            fputs(",\"p2_sprite_tiles_bank1\":\"", diag.log);
            write_hex_bytes(diag.log, p2_tiles_cgb1, 0x1000);
            fputc('"', diag.log);
        }
        fputc('}', diag.log);
    }
    fputs("}\n", diag.log);
    fflush(diag.log);
}

static int diag_event_warrants_capture(const char *message)
{
    if (!message) return 0;
    return strstr(message, "PLAYER 2 NOT SPAWNED") ||
           strstr(message, "PLAYER 2 SPAWNED") ||
           strstr(message, "PLAYER 2 VISIBLE WITH FEW SPRITES") ||
           strstr(message, "INPUT HELD BUT POSITION STATIC") ||
           strstr(message, "LIVES ") ||
           strstr(message, "RESPAWN REQUESTED") ||
           strstr(message, "LEVEL CHANGED") ||
           strstr(message, "CO-OP TETHER ACTIVE");
}

static void request_incident_capture(const char *reason, int periodic)
{
    uint64_t frame = diag.frame_count;
    int bg_change = reason && strstr(reason, "BG MAP CHANGED WITH STATIC CAMERA");
    if (bg_change) {
        if (diag.last_bg_snapshot_frame &&
            frame < diag.last_bg_snapshot_frame + 600u) return;
        diag.last_bg_snapshot_frame = frame;
    }
    if (diag.last_dump_frame == frame) return;

    diag.last_dump_frame = frame;
    diag.screenshot_pending = 1;
    snprintf(diag.screenshot_reason, sizeof diag.screenshot_reason, "%s",
             reason ? reason : (periodic ? "periodic" : "incident"));
    log_memory_snapshot(diag.screenshot_reason);
}

static void diag_event_capture_request(const char *message)
{
    if (diag_event_warrants_capture(message) ||
        (message && strstr(message, "BG MAP CHANGED WITH STATIC CAMERA")))
        request_incident_capture(message, 0);
}

/* Compact built-in 3x5 font; avoids extra DLLs and is readable in a small
 * always-available developer window. Bits 2..0 are the pixels in each row. */
static const char font_chars[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:-_./=+[]#?%";
static const uint8_t font_rows[][5] = {
    {0,0,0,0,0}, {2,5,7,5,5}, {6,5,6,5,6}, {3,4,4,4,3},
    {6,5,5,5,6}, {7,4,6,4,7}, {7,4,6,4,4}, {3,4,5,5,3},
    {5,5,7,5,5}, {7,2,2,2,7}, {1,1,1,5,2}, {5,5,6,5,5},
    {4,4,4,4,7}, {5,7,7,5,5}, {5,7,7,7,5}, {2,5,5,5,2},
    {6,5,6,4,4}, {2,5,5,7,3}, {6,5,6,5,5}, {3,4,2,1,6},
    {7,2,2,2,2}, {5,5,5,5,7}, {5,5,5,5,2}, {5,5,7,7,5},
    {5,5,2,5,5}, {5,5,2,2,2}, {7,1,2,4,7},
    {7,5,5,5,7}, {2,6,2,2,7}, {6,1,2,4,7},
    {6,1,2,1,6}, {5,5,7,1,1}, {7,4,6,1,6},
    {3,4,6,5,2}, {7,1,2,2,2}, {2,5,2,5,2},
    {2,5,3,1,6}, {0,2,0,2,0}, {0,0,7,0,0},
    {0,0,0,0,7}, {0,0,0,0,2}, {1,1,2,4,4},
    {0,7,0,7,0}, {0,2,7,2,0}, {6,4,4,4,6},
    {3,1,1,1,3}, {5,7,5,7,5}, {6,1,2,0,2}, {5,1,2,4,5}
};

static uint32_t frame_hash(const Frame *frame)
{
    uint32_t hash = 2166136261u;
    if (!frame) return 0;
    for (int y = 0; y < GB_H; y++) {
        for (int x = 0; x < GB_WMAX; x++) {
            hash ^= frame->shade[y][x];
            hash *= 16777619u;
            hash ^= frame->layer[y][x];
            hash *= 16777619u;
        }
    }
    return hash;
}

static void draw_text(int x, int y, const char *text, SDL_Color color)
{
    if (!diag.renderer || !text) return;
    SDL_SetRenderDrawColor(diag.renderer, color.r, color.g, color.b, color.a);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++, x += 4 * DIAG_SCALE) {
        unsigned char c = (unsigned char)toupper(*p);
        const char *at = strchr(font_chars, (int)c);
        if (!at) at = strchr(font_chars, '?');
        size_t index = at ? (size_t)(at - font_chars) : 0;
        if (index >= sizeof font_rows / sizeof font_rows[0]) index = 0;
        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 3; col++) {
                if (!(font_rows[index][row] & (1u << (2 - col)))) continue;
                SDL_Rect pixel = { x + col * DIAG_SCALE, y + row * DIAG_SCALE,
                                   DIAG_SCALE, DIAG_SCALE };
                SDL_RenderFillRect(diag.renderer, &pixel);
            }
        }
    }
}

static void diag_event_log(const char *message)
{
    if (!message) message = "";
    snprintf(diag.recent[diag.recent_head], sizeof diag.recent[0], "%s", message);
    diag.recent_head = (diag.recent_head + 1) % DIAG_MAX_EVENTS;
    if (diag.recent_count < DIAG_MAX_EVENTS) diag.recent_count++;
    if (diag.log) {
        fprintf(diag.log, "{\"type\":\"event\",\"frame\":%llu,\"message\":\"",
                (unsigned long long)diag.frame_count);
        for (const unsigned char *p = (const unsigned char *)message; *p; p++) {
            if (*p == '"' || *p == '\\') fputc('\\', diag.log);
            if (*p >= 32) fputc(*p, diag.log);
        }
        fputs("\"}\n", diag.log);
        fflush(diag.log);
    }
    diag_event_capture_request(message);
}

void dev_diag_event(const char *message)
{
    if (!diag.log && !diag.window) return;
    diag_event_log(message);
}

static void make_log_path(void)
{
    char dir[1200];
    snprintf(dir, sizeof dir, "%slogs/", settings_dir());
    if (mkdir_u(dir) == 0) {
        time_t now = time(NULL);
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &now);
#else
        localtime_r(&now, &tmv);
#endif
        char stamp[48];
        strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);
        snprintf(diag.log_path, sizeof diag.log_path,
                 "%sPipeClean-dev-multiplayer-%s-%u.jsonl",
                 dir, stamp, (unsigned)SDL_GetTicks());
    } else {
        snprintf(diag.log_path, sizeof diag.log_path,
                 "%sPipeClean-dev-multiplayer-%u.jsonl",
                 settings_dir(), (unsigned)SDL_GetTicks());
    }
}

void dev_diag_begin(int game, int player_count, SDL_Window *game_window)
{
    memset(&diag, 0, sizeof diag);
    diag.player_count = player_count;
    diag.last_level = -1;
    diag.last_bank = -1;
    for (int i = 0; i < MAX_MP_PLAYERS; i++) diag.last_lives[i] = -1;
    snprintf(diag.game_name, sizeof diag.game_name, "%s",
             (game >= 0 && game < N_GAMES) ? games[game].name : "UNKNOWN GAME");
    make_log_path();
    diag.log = fopen(diag.log_path, "w");
    diag.window = SDL_CreateWindow(
        "PipeClean Dev - Multiplayer Diagnostics",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        DIAG_WIDTH, DIAG_HEIGHT, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_ALWAYS_ON_TOP);
    if (diag.window) {
        diag.renderer = SDL_CreateRenderer(diag.window, -1, SDL_RENDERER_SOFTWARE);
        if (!diag.renderer) {
            SDL_DestroyWindow(diag.window);
            diag.window = NULL;
        } else {
            diag.window_id = SDL_GetWindowID(diag.window);
            SDL_SetRenderDrawBlendMode(diag.renderer, SDL_BLENDMODE_NONE);
            SDL_ShowWindow(diag.window);
            if (game_window) SDL_SetWindowInputFocus(game_window);
        }
    }

    if (diag.log) {
        fprintf(diag.log,
                "{\"type\":\"session_start\",\"game\":\"%s\","
                "\"players\":%d,\"log_file\":\"logs/%s\","
                "\"build\":\"PIPECLEAN_DEV_BUILD\"}\n",
                diag.game_name, player_count, path_base(diag.log_path));
        fflush(diag.log);
    }
    char event[120];
    snprintf(event, sizeof event, "SESSION STARTED - %s", diag.game_name);
    diag_event_log(event);
    if (!diag.log) fprintf(stderr, "PipeClean Dev: couldn't create multiplayer log at %s\n",
                           diag.log_path);
}

static void sample_current(const Frame *frame, int game, int player_count,
                           const uint8_t buttons[MAX_MP_PLAYERS],
                           const uint8_t dpad[MAX_MP_PLAYERS], int paused,
                           DiagSample *sample)
{
    memset(sample, 0, sizeof *sample);
    sample->frame = ++diag.frame_count;
    sample->game = game;
    sample->player_count = player_count;
    sample->paused = paused;
    sample->render_hash = frame_hash(frame);
    for (int i = 0; i < player_count && i < MAX_MP_PLAYERS; i++) {
        sample->buttons[i] = buttons ? buttons[i] : 0;
        sample->dpad[i] = dpad ? dpad[i] : 0;
        sample->visible[i] = i == 0 ? 1 : frame->mp_player_visible[i];
        sample->sprites[i] = i == 0 ? 4 : frame->mp_player_sprite_count[i];
    }
    if (game == GAME_SML2 && emu_mp_test_snapshot(&sample->sml2) == 0) {
        sample->has_sml2 = 1;
        sample->pos_x[0] = sample->sml2.p1_world_x;
        sample->pos_y[0] = sample->sml2.p1_world_y;
        sample->pos_x[1] = sample->sml2.p2_world_x;
        sample->pos_y[1] = sample->sml2.p2_world_y;
        sample->screen_x[0] = sample->sml2.p1_screen_x;
        sample->screen_y[0] = sample->sml2.p1_screen_y;
        sample->screen_x[1] = sample->sml2.p2_screen_x;
        sample->screen_y[1] = sample->sml2.p2_screen_y;
        sample->grounded[0] = sample->sml2.p1_grounded;
        sample->in_air[0] = sample->sml2.p1_in_air;
        sample->grounded[1] = sample->sml2.p2_grounded;
        sample->in_air[1] = sample->sml2.p2_in_air;
        sample->lives[0] = sample->sml2.p1_lives;
        sample->lives[1] = sample->sml2.p2_lives;
        sample->visible[1] = sample->sml2.p2_spawned;
    } else if (frame) {
        sample->pos_x[0] = frame->player_x;
        sample->pos_y[0] = frame->player_y;
        sample->screen_x[0] = frame->player_x;
        sample->screen_y[0] = frame->player_y;
        sample->screen_x[1] = frame->mario_oam2[1];
        sample->screen_y[1] = frame->mario_oam2[0];
        sample->lives[0] = frame->p1_lives;
        sample->lives[1] = frame->p2_lives;
    }
}

static void update_events(const DiagSample *s)
{
    if (s->has_sml2) {
        const EmuMpTestSnapshot *m = &s->sml2;
        if (diag.last_level >= 0 &&
            (diag.last_level != m->level || diag.last_bank != m->level_bank)) {
            char text[120];
            snprintf(text, sizeof text, "LEVEL CHANGED TO %02X BANK %02X",
                     m->level, m->level_bank);
            diag_event_log(text);
        }
        diag.last_level = m->level;
        diag.last_bank = m->level_bank;

        if (diag.last_lives[0] >= 0 && diag.last_lives[0] != m->p1_lives) {
            char text[120];
            snprintf(text, sizeof text, "P1 LIVES %d -> %d",
                     diag.last_lives[0], m->p1_lives);
            diag_event_log(text);
        }
        if (diag.last_lives[1] >= 0 && diag.last_lives[1] != m->p2_lives) {
            char text[120];
            snprintf(text, sizeof text, "P2 LIVES %d -> %d",
                     diag.last_lives[1], m->p2_lives);
            diag_event_log(text);
        }
        uint32_t current_coins = ((uint32_t)m->coins_high << 8) | m->coins_low;
        if (diag.last_lives[0] >= 0 && diag.last_coins != current_coins)
            diag_event_log("COIN COUNTER CHANGED");
        if (diag.last_lives[0] >= 0 && diag.last_map_hash != m->bg_map_hash &&
            diag.last_camera_x == m->camera_x && diag.last_camera_y == m->camera_y)
            diag_event_log("BG MAP CHANGED WITH STATIC CAMERA");
        diag.last_coins = current_coins;
        diag.last_map_hash = m->bg_map_hash;
        diag.last_camera_x = m->camera_x;
        diag.last_camera_y = m->camera_y;
        diag.last_lives[0] = m->p1_lives;
        diag.last_lives[1] = m->p2_lives;
    }

    for (int i = 0; i < s->player_count && i < MAX_MP_PLAYERS; i++) {
        int changed = !diag.has_sample ||
            diag.sample.pos_x[i] != s->pos_x[i] ||
            diag.sample.pos_y[i] != s->pos_y[i];
        if (i > 0 && !diag.has_sample && s->visible[i]) {
            diag_event_log("PLAYER 2 SPAWNED");
        }
        if (i > 0 && diag.has_sample &&
            diag.sample.visible[i] != s->visible[i]) {
            diag_event_log(s->visible[i] ? "PLAYER 2 SPAWNED" : "PLAYER 2 NOT SPAWNED");
        }
        if (s->dpad[i] && !changed) {
            diag.movement_stall[i]++;
            if (diag.movement_stall[i] > 90 && !diag.movement_warned[i]) {
                char text[120];
                snprintf(text, sizeof text, "P%d INPUT HELD BUT POSITION STATIC", i + 1);
                diag_event_log(text);
                diag.movement_warned[i] = 1;
            }
        } else {
            diag.movement_stall[i] = 0;
            diag.movement_warned[i] = 0;
        }
        if (i > 0 && s->visible[i] && s->sprites[i] < 2 && !diag.sprite_warned[i]) {
            diag_event_log("PLAYER 2 VISIBLE WITH FEW SPRITES");
            diag.sprite_warned[i] = 1;
        } else if (i > 0 && s->sprites[i] >= 2) {
            diag.sprite_warned[i] = 0;
        }
    }
    diag.has_sample = 1;
}

static void log_frame(const DiagSample *s)
{
    if (!diag.log) return;
    if (s->has_sml2) {
        const EmuMpTestSnapshot *m = &s->sml2;
        fprintf(diag.log,
            "{\"type\":\"frame\",\"n\":%llu,\"paused\":%d,"
            "\"inputs\":[[%u,%u],[%u,%u],[%u,%u],[%u,%u]],"
            "\"p1\":[%d,%d,%d,%d,%d,%d,%u],"
            "\"p2\":[%d,%d,%d,%d,%d,%d,%u,%u,%u],"
            "\"camera\":[%u,%u],\"level\":[%u,%u],\"mode\":%u,"
            "\"coins\":[%u,%u],\"hashes\":[\"%08X\",\"%08X\",\"%08X\",\"%08X\"],"
            "\"tile_patches\":%u}\n",
            (unsigned long long)s->frame, s->paused,
            s->buttons[0], s->dpad[0], s->buttons[1], s->dpad[1],
            s->buttons[2], s->dpad[2], s->buttons[3], s->dpad[3],
            m->p1_world_x, m->p1_world_y, m->p1_screen_x, m->p1_screen_y,
            m->p1_grounded, m->p1_in_air, m->p1_lives,
            m->p2_world_x, m->p2_world_y, m->p2_screen_x, m->p2_screen_y,
            m->p2_grounded, m->p2_in_air, m->p2_lives, m->p2_spawned,
            s->sprites[1], m->camera_x, m->camera_y, m->level, m->level_bank,
            m->game_mode, m->coins_low, m->coins_high, m->bg_map_hash,
            m->level_ram_hash, m->actor_region_hash, s->render_hash,
            m->tile_patch_count);
    } else {
        fprintf(diag.log,
            "{\"type\":\"frame\",\"n\":%llu,\"paused\":%d,"
            "\"inputs\":[[%u,%u],[%u,%u]],\"p1_screen\":[%d,%d],"
            "\"p2_visible\":%u,\"p2_screen\":[%d,%d],\"p2_sprites\":%u,"
            "\"coins\":[%u,%u],\"scroll_x\":%u,\"render_hash\":\"%08X\"}\n",
            (unsigned long long)s->frame, s->paused,
            s->buttons[0], s->dpad[0], s->buttons[1], s->dpad[1],
            s->screen_x[0], s->screen_y[0], s->visible[1],
            s->screen_x[1], s->screen_y[1], s->sprites[1],
            s->has_sml2 ? s->sml2.coins_low : 0,
            s->has_sml2 ? s->sml2.coins_high : 0,
            0u, s->render_hash);
    }
    if ((s->frame % 60u) == 0) fflush(diag.log);
}

static void draw_dashboard(void)
{
    if (!diag.window || !diag.renderer || !diag.has_sample) return;
    int w, h;
    SDL_GetWindowSize(diag.window, &w, &h);
    SDL_SetRenderDrawColor(diag.renderer, 15, 19, 28, 255);
    SDL_RenderClear(diag.renderer);
    const SDL_Color white = {226,232,242,255};
    const SDL_Color cyan = {82,195,255,255};
    const SDL_Color muted = {145,160,180,255};
    const SDL_Color warn = {255,177,80,255};
    int x=18, y=16, step=22;
    char line[256];
    draw_text(x,y,"PIPE CLEAN DEV - LIVE MULTIPLAYER DIAGNOSTICS",cyan); y+=step+4;
    snprintf(line,sizeof line,"GAME: %s  PLAYERS: %d  FRAME: %llu",
             diag.game_name,diag.player_count,(unsigned long long)diag.frame_count);
    draw_text(x,y,line,white); y+=step;
    snprintf(line,sizeof line,"STATUS: RUNNING  PAUSED: %s",
             diag.sample.paused ? "YES" : "NO");
    draw_text(x,y,line,white); y+=step;
    {
        const char *log_name = diag.log_path[0] ? diag.log_path : "(LOG FILE UNAVAILABLE)";
        size_t length = strlen(log_name);
        if (length > 106) log_name += length - 106;
        snprintf(line,sizeof line,"LOG: %s",log_name);
        draw_text(x,y,line,muted); y+=step+3;
    }
    if(diag.sample.has_sml2) {
        const EmuMpTestSnapshot *m=&diag.sample.sml2;
        snprintf(line,sizeof line,"LEVEL: %02X BANK: %02X MODE: %02X CAMERA: %u,%u",
                 m->level,m->level_bank,m->game_mode,m->camera_x,m->camera_y);
        draw_text(x,y,line,cyan); y+=step;
        snprintf(line,sizeof line,"P1 WORLD: %u,%u SCREEN: %u,%u GROUND:%u AIR:%u LIVES:%u",
                 m->p1_world_x,m->p1_world_y,m->p1_screen_x,m->p1_screen_y,
                 m->p1_grounded,m->p1_in_air,m->p1_lives);
        draw_text(x,y,line,white); y+=step;
        snprintf(line,sizeof line,"P1 INPUT: B=%02X D=%02X",
                 diag.sample.buttons[0],diag.sample.dpad[0]);
        draw_text(x,y,line,muted); y+=step;
        snprintf(line,sizeof line,"P2 SPAWNED:%u WORLD:%u,%u SCREEN:%u,%u SPRITES:%u",
                 m->p2_spawned,m->p2_world_x,m->p2_world_y,
                 m->p2_screen_x,m->p2_screen_y,diag.sample.sprites[1]);
        draw_text(x,y,line,m->p2_spawned?white:warn); y+=step;
        snprintf(line,sizeof line,"P2 GROUND:%u AIR:%u LIVES:%u INPUT B=%02X D=%02X",
                 m->p2_grounded,m->p2_in_air,m->p2_lives,
                 diag.sample.buttons[1],diag.sample.dpad[1]);
        draw_text(x,y,line,white); y+=step;
        snprintf(line,sizeof line,"COINS: %02X + %02X TILE PATCHES: %u",
                 m->coins_low,m->coins_high,m->tile_patch_count);
        draw_text(x,y,line,white); y+=step;
        snprintf(line,sizeof line,"BG MAP HASH: %08X",m->bg_map_hash);
        draw_text(x,y,line,muted); y+=step;
        snprintf(line,sizeof line,"LEVEL RAM HASH: %08X ACTOR HASH: %08X",
                 m->level_ram_hash,m->actor_region_hash);
        draw_text(x,y,line,muted); y+=step;
    } else {
        snprintf(line,sizeof line,"P1 SCREEN: %d,%d  P2 VISIBLE:%u SCREEN:%d,%d SPRITES:%u",
                 diag.sample.screen_x[0],diag.sample.screen_y[0],
                 diag.sample.visible[1],diag.sample.screen_x[1],
                 diag.sample.screen_y[1],diag.sample.sprites[1]);
        draw_text(x,y,line,white); y+=step;
        draw_text(x,y,"DETAILED MEMORY TELEMETRY IS CURRENTLY AVAILABLE FOR SML2",muted); y+=step;
    }
    snprintf(line,sizeof line,"RENDER HASH: %08X",diag.sample.render_hash);
    draw_text(x,y,line,muted); y+=step+3;
    draw_text(x,y,"RECENT EVENTS",cyan); y+=step;
    for(int j=0;j<diag.recent_count && y+12<h;j++) {
        int idx=(diag.recent_head-diag.recent_count+j+DIAG_MAX_EVENTS)%DIAG_MAX_EVENTS;
        draw_text(x,y,diag.recent[idx],warn);
        y+=step;
    }
    if(h>0) {
        SDL_SetRenderDrawColor(diag.renderer, 45, 57, 75, 255);
        SDL_RenderDrawLine(diag.renderer, 12, h-30, w-12, h-30);
        draw_text(18,h-22,"CLOSE THIS WINDOW TO HIDE IT; CLOSE GAME TO FINALIZE THE LOG",muted);
    }
    SDL_RenderPresent(diag.renderer);
}

void dev_diag_frame(const Frame *frame, int game, int player_count,
                    const uint8_t buttons[MAX_MP_PLAYERS],
                    const uint8_t dpad[MAX_MP_PLAYERS], int paused)
{
    if (!diag.log && !diag.window) return;
    DiagSample sample;
    diag.current_frame = frame;
    sample_current(frame,game,player_count,buttons,dpad,paused,&sample);
    update_events(&sample);
    diag.sample=sample;
    diag.last_render_hash=sample.render_hash;
    log_frame(&sample);
    if (diag.frame_count > 0 && diag.frame_count % 900u == 0 &&
        diag.last_periodic_frame != diag.frame_count) {
        diag.last_periodic_frame = diag.frame_count;
        request_incident_capture("periodic", 1);
    }
    Uint32 now=SDL_GetTicks();
    if(diag.window && (Uint32)(now-diag.last_draw_ticks)>=100u) {
        diag.last_draw_ticks=now;
        draw_dashboard();
    }
}

void dev_diag_capture_screen(SDL_Renderer *game_renderer)
{
    if (!game_renderer || !diag.screenshot_pending) return;

    int width = 0, height = 0;
    if (SDL_GetRendererOutputSize(game_renderer, &width, &height) != 0 ||
        width <= 0 || height <= 0) {
        diag_event_log("SCREENSHOT FAILED - INVALID RENDERER SIZE");
        diag.screenshot_pending = 0;
        return;
    }

    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
        0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) {
        diag_event_log("SCREENSHOT FAILED - SURFACE ALLOCATION");
        diag.screenshot_pending = 0;
        return;
    }
    if (SDL_RenderReadPixels(game_renderer, NULL, SDL_PIXELFORMAT_ARGB8888,
                             surface->pixels, surface->pitch) != 0) {
        SDL_FreeSurface(surface);
        diag_event_log("SCREENSHOT FAILED - RENDER READBACK");
        diag.screenshot_pending = 0;
        return;
    }

    char folder[1200];
    snprintf(folder, sizeof folder, "%s", diag.log_path);
    char *slash = strrchr(folder, '/');
    char *backslash = strrchr(folder, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    if (slash) slash[1] = 0;
    else snprintf(folder, sizeof folder, "%s", settings_dir());

    char slug[64];
    size_t used = 0;
    const char *reason = diag.screenshot_reason[0]
        ? diag.screenshot_reason : "incident";
    for (const unsigned char *p = (const unsigned char *)reason;
         *p && used + 1 < sizeof slug; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9')) {
            slug[used++] = (char)*p;
        } else if (used && slug[used - 1] != '-' && used + 1 < sizeof slug) {
            slug[used++] = '-';
        }
    }
    while (used && slug[used - 1] == '-') used--;
    if (!used) { memcpy(slug, "incident", 9); used = 8; }
    slug[used] = 0;

    char path[1500];
    snprintf(path, sizeof path, "%sPipeClean-screen-%06llu-%s.png",
             folder, (unsigned long long)diag.frame_count, slug);
    int saved = IMG_SavePNG(surface, path);
    SDL_FreeSurface(surface);

    if (saved == 0) {
        if (diag.log) {
            fprintf(diag.log, "{\"type\":\"screenshot\",\"frame\":%llu,\"reason\":",
                    (unsigned long long)diag.frame_count);
            write_json_string(diag.log, reason);
            fputs(",\"path\":", diag.log);
            write_json_string(diag.log, path_base(path));
            fprintf(diag.log, ",\"width\":%d,\"height\":%d}\n", width, height);
            fflush(diag.log);
        }
        diag.last_screenshot_frame = diag.frame_count;
    } else if (diag.log) {
        fprintf(diag.log, "{\"type\":\"screenshot_error\",\"frame\":%llu,\"error\":",
                (unsigned long long)diag.frame_count);
        write_json_string(diag.log, IMG_GetError());
        fputs("}\n", diag.log);
        fflush(diag.log);
    }
    diag.screenshot_pending = 0;
}

void dev_diag_end(const char *reason)
{
    if (!diag.log && !diag.window) return;
    char line[160];
    snprintf(line,sizeof line,"SESSION END - %s",reason?reason:"UNKNOWN");
    diag_event_log(line);
    if(diag.log) {
        fprintf(diag.log,
            "{\"type\":\"session_end\",\"frames\":%llu,\"reason\":\"%s\","
            "\"last_render_hash\":\"%08X\"}\n",
            (unsigned long long)diag.frame_count,reason?reason:"unknown",
            diag.last_render_hash);
        fflush(diag.log);
        fclose(diag.log);
    }
    diag.log=NULL;
    if(diag.renderer) SDL_DestroyRenderer(diag.renderer);
    if(diag.window) SDL_DestroyWindow(diag.window);
    diag.renderer=NULL;
    diag.window=NULL;
    diag.window_id=0;
    diag.log_path[0]=0;
}

int dev_diag_handle_event(const SDL_Event *event)
{
    if(!event || !diag.window || !diag.window_id) return 0;
    Uint32 id=0;
    switch(event->type) {
    case SDL_WINDOWEVENT:
        id=event->window.windowID;
        if(id!=diag.window_id) return 0;
        if(event->window.event==SDL_WINDOWEVENT_CLOSE) {
            diag_event_log("DIAGNOSTICS WINDOW CLOSED - LOGGING CONTINUES");
            SDL_DestroyRenderer(diag.renderer);
            SDL_DestroyWindow(diag.window);
            diag.renderer=NULL;
            diag.window=NULL;
            diag.window_id=0;
        }
        return 1;
    case SDL_KEYDOWN: case SDL_KEYUP:
        id=event->key.windowID;
        return id==diag.window_id;
    case SDL_TEXTINPUT:
        id=event->text.windowID;
        return id==diag.window_id;
    case SDL_MOUSEMOTION:
        id=event->motion.windowID;
        return id==diag.window_id;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
        id=event->button.windowID;
        return id==diag.window_id;
    case SDL_MOUSEWHEEL:
        id=event->wheel.windowID;
        return id==diag.window_id;
    default:
        return 0;
    }
}

#endif
