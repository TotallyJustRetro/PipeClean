/* Emulation driver: the frame hook that connects the CPU/PPU/APU core to the outside world.
 * In windowed play the core runs on its own thread, paced by the audio device (the audio clock is
 * the master clock), and hands finished frames to the main thread. */
#include <SDL.h>
#include <setjmp.h>
#include <stdlib.h>
#include "emu.h"
#include "audio.h"
#include "rom.h"
#include "cart.h"
#include "events.h"
#include "settings.h"

EmuDev emu_dev = {.max_frames = -1};

static jmp_buf stop_jmp;
static SDL_Thread *thr;
static SDL_mutex *fmx;
static Frame pub;                         /* latest published frame */
static volatile int abort_flag, paused, turbo, thread_mode, preview_target, previewing;
static volatile uint32_t input_word;     /* buttons | dpad << 8 */
static SDL_mutex *rewind_sync_mx;
static SDL_cond *rewind_sync_cv;
static volatile int rewind_step_request;
static volatile int rewind_step_done;
static int rewind_step_result;
static char save_path[1100];
static int save_tick;
static int force_interp_flag;
static volatile int core_faulted;
static volatile uint8_t core_fault_opcode;
static volatile uint16_t core_fault_pc;
static Uint64 pace_next;

static uint64_t chain = 1469598103934665603ull;
static FILE *hash_log;
typedef struct { int frame; uint8_t mask; } ScriptEv;
static ScriptEv script[4096];
static int n_script;
static uint32_t rng = 1;
static struct { int frame; uint16_t addr; uint8_t val; } pokes[64];
static int n_pokes;
void emu_dev_poke(int frame, uint16_t addr, uint8_t val) { if (n_pokes < 64) { pokes[n_pokes].frame = frame; pokes[n_pokes].addr = addr; pokes[n_pokes].val = val; n_pokes++; } }
static int fuzz_next;
static uint8_t fz_b, fz_d;

static uint32_t xr(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

uint64_t emu_frames(void) { return pub.seq; }
int emu_cpu_faulted(void) { return core_faulted; }
void emu_cpu_fault_info(uint8_t *opcode, uint16_t *pc)
{
    if (opcode) *opcode = core_fault_opcode;
    if (pc) *pc = core_fault_pc;
}

static void abort_on_cpu_fault(uint8_t opcode, uint16_t pc)
{
    core_fault_opcode = opcode;
    core_fault_pc = pc;
    core_faulted = 1;
    longjmp(stop_jmp, 4);
}
void emu_input(uint8_t b, uint8_t d) { input_word = (uint32_t)b | ((uint32_t)d << 8); }
void emu_set_paused(int p) { paused = p; audio_game_set_paused(p); }
void emu_set_turbo(int t) { turbo = t; }
void emu_set_save_path(const char *p) { snprintf(save_path, sizeof save_path, "%s", p ? p : ""); }
int emu_running(void) { return thr != NULL; }

static void run_core(int force_interp)
{
    gb_set_cpu_fault_hook(abort_on_cpu_fault);
    if (force_interp || rom_needs_interpreter()) run_interpreter();
    else recomp_run();
}

static void publish(void)
{
    SDL_LockMutex(fmx);
    memcpy(pub.shade, ppu_shade, sizeof pub.shade);
    memcpy(pub.rgb, ppu_rgb, sizeof pub.rgb);
    pub.cgb_mode = (uint8_t)ppu_cgb_mode_enabled();
    memcpy(pub.layer, ppu_layer, sizeof pub.layer);
    memcpy(pub.bguv, ppu_bguv, sizeof pub.bguv);
    memcpy(pub.spruv, ppu_spruv, sizeof pub.spruv);
    memcpy(pub.bgtile, ppu_bgtile, sizeof pub.bgtile);
    memcpy(pub.sprtile, ppu_sprtile, sizeof pub.sprtile);
    memcpy(pub.tiles, vram, sizeof pub.tiles);
    pub.w = ppu_w; pub.xoff = ppu_xoff;
    pub.player_x = rd8(0xC202); pub.player_y = rd8(0xC201);
    pub.scroll_x = rd8(0xFFA4); pub.game_state = rd8(0xFFB3);
    pub.obp0 = ppu_read(0x48); pub.obp1 = ppu_read(0x49); pub.sprite_size16 = (uint8_t)((ppu_read(0x40) & 0x04) != 0);
    memcpy(pub.mario_oam, &oam[0x0C], sizeof pub.mario_oam);
    memcpy(pub.bg_map, &vram[0x1800], sizeof pub.bg_map);
    pub.lcd_on = ppu_lcd_is_on();
    pub.seq++;
    SDL_UnlockMutex(fmx);
}

int emu_frame_get(Frame *f)
{
    int got = 0;
    SDL_LockMutex(fmx);
    if (pub.seq != f->seq) { memcpy(f, &pub, sizeof *f); got = 1; }
    SDL_UnlockMutex(fmx);
    return got;
}

/* SML1 multiplayer uses the original game code for both players.
 * Player 1 owns the authoritative world/camera. For Player 2 we clone the
 * current Player 1 world, restore only Player 2's character state, execute one
 * real SML1 frame with Player 2's input, keep the resulting character state,
 * and discard the world's changes. This gives P2 the game's actual movement,
 * collision and animation code while keeping one shared world on screen. */
typedef struct {
    uint8_t mario[0x10];     /* C200-C20F: position, animation, momentum, etc. */
    uint8_t mario_oam[16];   /* C00C-C01B: Mario's last rendered four OAM entries */
    uint8_t effect_oam[52];  /* C01C-C04F: P2-owned block/item effects, OAM slots 7-19 */
    uint8_t projectile_oam[12]; /* C000-C00B: three private projectile OAM entries */
    uint8_t invincibility;   /* C0D3 */
    uint8_t superball_ttl;   /* C0A9 */
    uint8_t projectile_status[3]; /* FFA9-FFAB: active Superball projectile state */
    uint8_t death_y;         /* C0DD */
    uint8_t super_status;    /* FF99 */
    uint8_t superball;       /* FFB5 */
    uint8_t timer;           /* FFA6: SML1 death/injury timer */
    uint8_t death_anim_counter; /* C0AC: SML1 state-4 death animation index */
    uint8_t game_state;      /* FFB3: private SML1 state machine */
    uint8_t joy_held;        /* FF80 */
    uint8_t joy_pressed;     /* FF81 */
} MpPlayerState;

typedef struct {
    MpPlayerState state;
    uint8_t lives;
    uint8_t spawned;
    uint8_t respawn_requested;
    uint16_t invulnerability_frames;
    uint8_t last_oam[16];
    uint8_t a_was_down;
    uint8_t pending_square_sfx, pending_noise_sfx;
} MpExtraPlayer;

/* SML2 keeps Mario's motion/animation state in cartridge RAM, not SML1's
 * work-RAM structure. Preserve the known character block and movement scratch. */
typedef struct {
    uint8_t ram[0xDF]; /* sparse whitelist from A200 through A2DE */
    uint8_t keys_held, keys_pressed;
    uint8_t h_c0, h_c1, h_c2, h_c3, h_c4, h_c5, h_c6, h_c7;
    uint8_t oam[16];
    uint16_t invulnerability_frames;
    uint8_t lives; /* BCD at A22C */
    uint8_t spawned, respawn_requested, previous_a, sfx_events;
} MpSml2Player;

static int mp_active;
static int mp_game = GAME_SML;
static MpSml2Player mp_sml2_players[MAX_MP_PLAYERS];
/* Exact visible OAM pieces captured from the active SML2 character mapping.
 * Kept separately so multiplayer save-state layouts remain backward compatible. */
static uint8_t mp_sml2_render_oam[MAX_MP_PLAYERS][MP_MAX_OAM_SPRITES * 4];
static uint8_t mp_sml2_render_oam_count[MAX_MP_PLAYERS];
/* Each simulated clone has its own transient VRAM contents. Save the tile
 * patterns used by its OAM before restoring Player 1's complete GB state. */
static uint8_t mp_sml2_render_tiles[MAX_MP_PLAYERS][0x1000];
static uint8_t mp_sml2_render_tiles_cgb1[MAX_MP_PLAYERS][0x1000];
static uint8_t mp_sml2_render_tiles_valid[MAX_MP_PLAYERS];
static int mp_sml2_initialized;
static unsigned mp_sml2_stable_frames;
static uint8_t mp_sml2_stable_level;
static uint8_t mp_sml2_stable_bank;
static uint8_t mp_sml2_vram_before[0x400];
static uint8_t mp_sml2_vram_after[0x400];

#define MP_SML2_MAX_TILE_PATCHES 1024
typedef struct {
    uint16_t world_x, world_y; /* tile-aligned world pixel coordinates */
    uint8_t level, bank, tile;
} MpSml2TilePatch;
static MpSml2TilePatch mp_sml2_tile_patches[MP_SML2_MAX_TILE_PATCHES];
static unsigned mp_sml2_tile_patch_count;
static uint8_t mp_sml2_tile_patch_level = 0xFF;
static uint8_t mp_sml2_tile_patch_bank = 0xFF;

static uint8_t mp_buttons, mp_dpad;
static int mp_ready;
static int mp_player_count = 2;
static int mp_current_player;
static int mp_p2_spawned;
static int mp_p2_respawn_requested;
static uint16_t mp_p2_invulnerability_frames;
static uint8_t mp_pending_square_sfx, mp_pending_noise_sfx;
static uint8_t mp_state[GB_STATE_BYTES];
static MpPlayerState mp_p2_state;
static MpExtraPlayer mp_extra_players[MAX_MP_PLAYERS - 2];
static uint8_t mp_tilemap_before[0x400];
static uint8_t mp_tilemap_after[0x400];
static uint8_t mp_enemy_before[0xA0];
static uint8_t mp_enemy_after[0xA0];
static uint8_t mp_enemy_merge[0xA0];
static uint8_t mp_enemy_merge_mask[10];
static uint8_t mp_p1_lives_seen;
static uint8_t mp_p2_lives;
static uint8_t mp_p2_last_oam[16];
static uint8_t mp_vblank_life_event;
static uint8_t mp_vblank_collision;
static uint16_t mp_vblank_collision_addr;
static uint8_t mp_vblank_collision_before;
static uint8_t mp_vblank_square_sfx;
static uint8_t mp_vblank_noise_sfx;
static uint8_t mp_vblank_floaty_control;
static uint8_t mp_vblank_floaty_x;
static uint8_t mp_vblank_floaty_y;
static uint8_t mp_pending_block;
static uint16_t mp_pending_block_idx;
static uint8_t mp_p2_a_was_down;
static int mp_vblank_waiting;
static Frame *mp_frame_out;
static int16_t *mp_audio_out;
static int mp_audio_max, mp_audio_n;

#define REWIND_SLOTS 96
#define REWIND_CAPTURE_INTERVAL 2

typedef struct {
    uint32_t magic, version, flags, game_id, core_bytes, extra_bytes;
} EmuStateHeader;

typedef struct {
    MpPlayerState p2;
    uint8_t p2_lives;
    uint8_t p2_spawned;
    uint16_t p2_invulnerability_frames;
    uint8_t p2_last_oam[16];
    uint8_t p2_a_was_down;
    uint8_t pending_square_sfx, pending_noise_sfx;
    MpExtraPlayer extra[MAX_MP_PLAYERS - 2];
} MpStateExtraV6;

typedef struct {
    uint8_t ram[0x54];
    uint8_t keys_held, keys_pressed;
    uint8_t h_c0, h_c1, h_c2, h_c3, h_c4, h_c5, h_c6, h_c7;
    uint8_t oam[16];
    uint16_t invulnerability_frames;
    uint8_t lives;
    uint8_t spawned, respawn_requested, previous_a, sfx_events;
} MpSml2PlayerV7;

typedef struct {
    uint8_t ram[0xB3];
    uint8_t keys_held, keys_pressed;
    uint8_t h_c0, h_c1, h_c2, h_c3, h_c4, h_c5, h_c6, h_c7;
    uint8_t oam[16];
    uint16_t invulnerability_frames;
    uint8_t lives;
    uint8_t spawned, respawn_requested, previous_a, sfx_events;
} MpSml2PlayerV8;

typedef struct {
    MpPlayerState p2;
    uint8_t p2_lives;
    uint8_t p2_spawned;
    uint16_t p2_invulnerability_frames;
    uint8_t p2_last_oam[16];
    uint8_t p2_a_was_down;
    uint8_t pending_square_sfx, pending_noise_sfx;
    MpExtraPlayer extra[MAX_MP_PLAYERS - 2];
    MpSml2PlayerV8 sml2[MAX_MP_PLAYERS];
    uint8_t sml2_initialized;
} MpStateExtraV8;

typedef struct {
    MpPlayerState p2;
    uint8_t p2_lives;
    uint8_t p2_spawned;
    uint16_t p2_invulnerability_frames;
    uint8_t p2_last_oam[16];
    uint8_t p2_a_was_down;
    uint8_t pending_square_sfx, pending_noise_sfx;
    MpExtraPlayer extra[MAX_MP_PLAYERS - 2];
    MpSml2PlayerV7 sml2[MAX_MP_PLAYERS];
    uint8_t sml2_initialized;
} MpStateExtraV7;

typedef struct {
    MpPlayerState p2;
    uint8_t p2_lives;
    uint8_t p2_spawned;
    uint16_t p2_invulnerability_frames;
    uint8_t p2_last_oam[16];
    uint8_t p2_a_was_down;
    uint8_t pending_square_sfx, pending_noise_sfx;
    MpExtraPlayer extra[MAX_MP_PLAYERS - 2];
    MpSml2Player sml2[MAX_MP_PLAYERS];
    uint8_t sml2_initialized;
} MpStateExtra;

typedef struct {
    MpPlayerState p2;
    uint8_t p2_lives;
    uint8_t p2_spawned;
    uint16_t p2_invulnerability_frames;
    uint8_t pending_square_sfx, pending_noise_sfx;
} MpStateExtraV5;

#define EMU_STATE_MAGIC 0x50534353u /* "PCSS" */
#define EMU_STATE_VERSION 9u
#define EMU_STATE_FLAG_MP 1u

static uint8_t *rewind_data;
static size_t rewind_stride;
static int rewind_head = -1;
static int rewind_count;
static int rewind_oldest = -1;
static int rewind_cursor = -1;
static int rewind_last_loaded = -1;
static int rewind_capture_skip;
static int rewind_mode;
static int rewind_pending;

static void mp_player_save(MpPlayerState *s);
static void mp_capture_mp_player_data(Frame *frame);
static void rewind_free(void);
static void rewind_init(void);
static int emu_start_internal(int force_interp, int reset);
static void mp_sml2_capture_frame(Frame *frame);
static int mp_sml2_gameplay_active(void);

static int mp_bcd_to_int(uint8_t b)
{
    int n = ((b >> 4) & 0x0F) * 10 + (b & 0x0F);
    return n > 99 ? 99 : n;
}

static void mp_oam_offset_x(uint8_t oam16[16], int dx)
{
    if (!oam16 || !dx) return;
    for (int i = 0; i < 4; i++) {
        int x = (int)oam16[i * 4 + 1] + dx;
        while (x < 0) x += 256;
        while (x > 255) x -= 256;
        oam16[i * 4 + 1] = (uint8_t)x;
    }
}

static void mp_respawn_p2_from_p1(int player)
{
    mp_player_save(&mp_p2_state);
    mp_p2_state.game_state = 0;
    mp_p2_state.timer = 0;
    mp_p2_state.death_anim_counter = 0;
    mp_p2_state.invincibility = 0;
    mp_p2_state.superball_ttl = 0;
    memset(mp_p2_state.projectile_status, 0, sizeof mp_p2_state.projectile_status);
    memset(mp_p2_state.projectile_oam, 0, sizeof mp_p2_state.projectile_oam);
    memset(mp_p2_state.effect_oam, 0, sizeof mp_p2_state.effect_oam);
    mp_p2_state.super_status = 0;
    mp_p2_state.superball = 0;
    mp_p2_state.joy_held = 0;
    mp_p2_state.joy_pressed = 0;
    mp_p2_state.mario[0] = 0;  /* visible */
    mp_p2_state.mario[7] = 0;  /* C207: grounded */
    mp_p2_state.mario[11] = 0; /* C20B: animation counter */
    mp_p2_state.mario[12] = 0; /* C20C: momentum */
    mp_p2_state.mario[13] = 0; /* C20D: direction */
    mp_p2_state.mario[14] = 2; /* C20E: walking */

    /* Start Luigi from Mario's current location, then let SML1 rebuild his
     * small-Mario OAM on the next real gameplay frame. */
    int p1x = rd8(0xC202);
    int offset = player <= 1 ? 24 : (player == 2 ? 40 : 56);
    int x = p1x + (p1x <= 0xC8 - offset ? offset : -offset);
    if (x < 0x18) x = 0x18;
    if (x > 0xC8) x = 0xC8;
    memcpy(mp_p2_state.mario_oam, &oam[0x0C], sizeof mp_p2_state.mario_oam);
    mp_oam_offset_x(mp_p2_state.mario_oam, x - p1x);
    mp_p2_state.mario[2] = (uint8_t)x;
    mp_p2_state.mario[1] = rd8(0xC201);
    memcpy(mp_p2_last_oam, mp_p2_state.mario_oam, sizeof mp_p2_last_oam);
    mp_p2_spawned = 1;
    mp_p2_respawn_requested = 0;
    mp_p2_invulnerability_frames = 120; /* 2 seconds at 60 FPS, with visible flicker */
}

static void mp_player_save(MpPlayerState *s)
{
    if (!s) return;
    for (int i = 0; i < 0x10; i++) s->mario[i] = rd8((uint16_t)(0xC200 + i));
    for (int i = 0; i < 16; i++) s->mario_oam[i] = rd8((uint16_t)(0xC00C + i));
    for (int i = 0; i < 52; i++) s->effect_oam[i] = rd8((uint16_t)(0xC01C + i));
    for (int i = 0; i < 12; i++) s->projectile_oam[i] = rd8((uint16_t)(0xC000 + i));
    s->invincibility = rd8(0xC0D3);
    s->superball_ttl = rd8(0xC0A9);
    s->projectile_status[0] = rd8(0xFFA9);
    s->projectile_status[1] = rd8(0xFFAA);
    s->projectile_status[2] = rd8(0xFFAB);
    s->death_y = rd8(0xC0DD);
    s->super_status = rd8(0xFF99);
    s->superball = rd8(0xFFB5);
    s->timer = rd8(0xFFA6);
    s->death_anim_counter = rd8(0xC0AC);
    s->game_state = rd8(0xFFB3);
    s->joy_held = rd8(0xFF80);
    s->joy_pressed = rd8(0xFF81);
}

static void mp_player_load(const MpPlayerState *s)
{
    if (!s) return;
    for (int i = 0; i < 0x10; i++) wr8((uint16_t)(0xC200 + i), s->mario[i]);
    for (int i = 0; i < 16; i++) wr8((uint16_t)(0xC00C + i), s->mario_oam[i]);
    for (int i = 0; i < 52; i++) wr8((uint16_t)(0xC01C + i), s->effect_oam[i]);
    for (int i = 0; i < 12; i++) wr8((uint16_t)(0xC000 + i), s->projectile_oam[i]);
    wr8(0xC0D3, s->invincibility);
    wr8(0xC0A9, s->superball_ttl);
    wr8(0xFFA9, s->projectile_status[0]);
    wr8(0xFFAA, s->projectile_status[1]);
    wr8(0xFFAB, s->projectile_status[2]);
    wr8(0xC0DD, s->death_y);
    wr8(0xFF99, s->super_status);
    wr8(0xFFB5, s->superball);
    wr8(0xFFA6, s->timer);
    wr8(0xC0AC, s->death_anim_counter);
    wr8(0xFFB3, s->game_state);
    wr8(0xFF80, s->joy_held);
    wr8(0xFF81, s->joy_pressed);
}

static void mp_working_to_extra(MpExtraPlayer *slot)
{
    if (!slot) return;
    slot->state = mp_p2_state;
    slot->lives = mp_p2_lives;
    slot->spawned = (uint8_t)mp_p2_spawned;
    slot->respawn_requested = (uint8_t)mp_p2_respawn_requested;
    slot->invulnerability_frames = mp_p2_invulnerability_frames;
    memcpy(slot->last_oam, mp_p2_last_oam, sizeof slot->last_oam);
    slot->a_was_down = mp_p2_a_was_down;
    slot->pending_square_sfx = mp_pending_square_sfx;
    slot->pending_noise_sfx = mp_pending_noise_sfx;
}

static void mp_extra_to_working(const MpExtraPlayer *slot)
{
    if (!slot) return;
    mp_p2_state = slot->state;
    mp_p2_lives = slot->lives;
    mp_p2_spawned = slot->spawned != 0;
    mp_p2_respawn_requested = slot->respawn_requested != 0;
    mp_p2_invulnerability_frames = slot->invulnerability_frames;
    memcpy(mp_p2_last_oam, slot->last_oam, sizeof mp_p2_last_oam);
    mp_p2_a_was_down = slot->a_was_down;
    mp_pending_square_sfx = slot->pending_square_sfx;
    mp_pending_noise_sfx = slot->pending_noise_sfx;
}

static void mp_spawn_extra_from_p1(int player)
{
    if (player < 2 || player >= MAX_MP_PLAYERS) return;
    MpExtraPlayer saved;
    mp_working_to_extra(&saved);
    mp_extra_to_working(&mp_extra_players[player - 2]);
    if (!mp_p2_spawned && rd8(0xFFB3) == 0) {
        if (mp_p2_lives == 0) {
            mp_p2_lives = (uint8_t)mp_bcd_to_int(rd8(0xDA15));
            if (mp_p2_lives == 0) mp_p2_lives = 1;
        }
        mp_respawn_p2_from_p1(player);
        mp_p2_invulnerability_frames = 0;
        mp_p2_respawn_requested = 0;
        mp_p2_a_was_down = 0;
    }
    mp_working_to_extra(&mp_extra_players[player - 2]);
    mp_extra_to_working(&saved);
}

static void mp_copy_mario_oam_buffer(uint8_t out[16], int screen_dx)
{
    /*
     * SML1 builds Mario's current four OAM entries in wOAMBuffer at C000.
     * The hardware OAM is still the previous frame when frame_hook fires at
     * the start of VBlank, so multiplayer must read the freshly-built buffer.
     * Mario starts at OAM entry 3, hence C00C.
     */
    if (!out) return;
    for (int i = 0; i < 16; i++) out[i] = rd8((uint16_t)(0xC00C + i));
    if (screen_dx) {
        for (int i = 0; i < 4; i++) {
            int x = (int)out[i * 4 + 1] + screen_dx;
            out[i * 4 + 1] = (uint8_t)x;
        }
    }
}

static void mp_copy_projectile_oam_buffer(uint8_t out[12], int screen_dx)
{
    if (!out) return;
    for (int i = 0; i < 12; i++) out[i] = rd8((uint16_t)(0xC000 + i));
    /* Projectile X lives in the second byte of each four-byte OAM entry. */
    for (int i = 0; i < 3; i++) {
        int x = (int)out[i * 4 + 1] + screen_dx;
        out[i * 4 + 1] = (uint8_t)x;
    }
}

static void mp_copy_effect_oam_buffer(uint8_t out[52], int screen_dx)
{
    if (!out) return;
    /* OAM slots 7-19 are reserved for block debris, bump sprites and floaties.
     * Enemy sprites live in later slots and are deliberately not overlaid. */
    for (int i = 0; i < 52; i++) out[i] = rd8((uint16_t)(0xC01C + i));
    /* OAM slot 11 is SML1's temporary block-picture sprite. The actual block
     * is already in the authoritative tilemap, so copying tile $82 draws a clone. */
    uint8_t block_picture_tile = out[4 * (11 - 7) + 2];
    if (block_picture_tile >= 0x80 && block_picture_tile <= 0x83)
        memset(&out[4 * (11 - 7)], 0, 4);
    for (int i = 0; i < 13; i++) {
        int x = (int)out[i * 4 + 1] + screen_dx;
        out[i * 4 + 1] = (uint8_t)x;
    }
}

static void mp_queue_sfx(uint16_t address, uint8_t *pending, uint8_t sfx)
{
    if (!sfx || !pending) return;
    if (rd8(address) == 0) {
        wr8(address, sfx);
        *pending = 0;
    } else if (!*pending) {
        *pending = sfx;
    }
}

static int mp_is_enemy_stomp_transition(uint8_t before_type, uint8_t after_type)
{
    if (before_type == 0xFF || before_type == after_type) return 0;
    /*
     * Call_2A01 indexes Data_3186 by enemy type and uses its first byte as
     * the stomp transition. cart_lo is bank 0, where Data_3186 lives at
     * ROM address $3186 in the original SML1 image.
     */
    if (cart_lo && (0x3186u + (unsigned)before_type * 5u) < 0x4000u) {
        uint8_t expected = cart_lo[0x3186u + (unsigned)before_type * 5u];
        if (expected != 0 && after_type == expected) return 1;
    }
    return after_type == 0xFF;
}

static int mp_is_pickup(uint8_t type)
{
    switch (type) {
    case 0x28: /* MUSHROOM_IN_FLIGHT */
    case 0x29: /* MUSHROOM */
    case 0x2A: /* HEART_IN_FLIGHT */
    case 0x2B: /* HEART */
    case 0x2D: /* FLOWER_GROWING */
    case 0x2E: /* FLOWER */
    case 0x34: /* STAR */
        return 1;
    default:
        return 0;
    }
}

static void mp_capture_shared_world_before(void)
{
    memcpy(mp_tilemap_before, &vram[0x1800], sizeof mp_tilemap_before);
    for (int i = 0; i < 0xA0; i++) mp_enemy_before[i] = rd8((uint16_t)(0xD100 + i));
}

static void mp_capture_shared_world_after(void)
{
    memcpy(mp_tilemap_after, &vram[0x1800], sizeof mp_tilemap_after);
    for (int i = 0; i < 0xA0; i++) mp_enemy_after[i] = rd8((uint16_t)(0xD100 + i));
}

/*
 * Merge local level edits made by Luigi without importing the columns that
 * were rewritten only because his isolated simulation scrolled its private
 * camera. A broken block/collected coin normally changes only a few cells in
 * one 32-byte column, while DrawColumn replaces a whole column of level data.
 */
static int mp_merge_tilemap_local_edits(int allow_full, uint8_t p2_scroll, uint8_t p1_scroll)
{
    int changed = 0;
    int p2_col0 = (int)(p2_scroll >> 3);
    int p1_col0 = (int)(p1_scroll >> 3);
    for (int col = 0; col < 32; col++) {
        int n = 0;
        for (int row = 0; row < 32; row++) {
            int idx = row * 32 + col;
            if (mp_tilemap_before[idx] != mp_tilemap_after[idx]) n++;
        }
        /* With matching cameras the complete tilemap is aligned. Otherwise
         * skip full DrawColumn refreshes and merge only local edits, translated
         * back into Player 1's camera. */
        if (n == 0 || (n > 4 && !allow_full)) continue;
        int world_col = (p2_col0 + col) & 31;
        int p1_col = (world_col - p1_col0) & 31;
        for (int row = 0; row < 32; row++) {
            int idx = row * 32 + col;
            uint8_t before = mp_tilemap_before[idx];
            uint8_t after = mp_tilemap_after[idx];
            if (before == after) continue;
            /* A temporary empty tile during a block-bump animation is not the
             * final state. Guard it only when camera alignment is ambiguous. */
            if (before >= 0x60 && before != 0xF4 && after == 0x20)
                continue;
            vram[0x1800 + row * 32 + p1_col] = after;
            changed = 1;
        }
    }
    return changed;
}

static int mp_merge_block_vblank_event(uint8_t event, uint16_t addr,
                                       uint8_t after,
                                       uint8_t p2_scroll, uint8_t p1_scroll)
{
    if (addr < 0x9800 || addr >= 0x9C00) return 0;

    int p2idx = (int)addr - 0x9800;
    int row = p2idx / 32;
    int col = p2idx & 31;
    int world_col = ((int)(p2_scroll >> 3) + col) & 31;
    int p1_col = (world_col - (int)(p1_scroll >> 3)) & 31;
    int p1idx = row * 32 + p1_col;

    switch (event) {
    case 0x01:
        /* A real breakable block is destroyed immediately. */
        vram[0x1800 + p1idx] = after;
        return 1;
    case 0x02: {
        /*
         * FFEE=02 is a bump, not always a block destruction. The map tile at
         * the saved collision address tells us which block was hit; C02E is an
         * OAM sprite tile, not the world-map block ID. Item/mystery blocks
         * (80/81) become the game's used-block tile (7F). A breakable brick
         * (82) remains intact when only bumped by small Mario. Other terrain is
         * restored exactly as it was, avoiding accidental holes in the level.
         */
        /* The interrupt watch sees the tile AFTER the cloned game's VBlank
         * logic has temporarily blanked it. Read the pre-simulation shared map
         * instead, translated into P1's authoritative tilemap coordinates. */
        uint8_t original_tile = mp_tilemap_before[p1idx];
        uint8_t final_tile = original_tile;
        if (original_tile == 0x80 || original_tile == 0x81) final_tile = 0x7F;
        if (original_tile == 0x82) final_tile = 0x82;
        vram[0x1800 + p1idx] = final_tile;
        mp_pending_block = 0;
        mp_pending_block_idx = 0;
        return final_tile != original_tile;
    }
    case 0x04:
        /*
         * The animation has finished. SML1 has now decided whether this is
         * the normal block again or the spent/mystery block, so import exactly
         * the final tile produced by the original game.
         */
        if (!mp_pending_block) return 0;
        if (mp_pending_block_idx < 0x400)
            vram[0x1800 + mp_pending_block_idx] = after;
        mp_pending_block = 0;
        mp_pending_block_idx = 0;
        return 1;
    default:
        return 0;
    }
}

static int mp_scroll_delta(uint8_t now, uint8_t old)
{
    int d = (int)now - (int)old;
    if (d > 127) d -= 256;
    if (d < -127) d += 256;
    return d;
}

static void mp_capture_frame(Frame *f, int screen_dx)
{
    memcpy(f->shade, ppu_shade, sizeof f->shade);
    memcpy(f->rgb, ppu_rgb, sizeof f->rgb);
    f->cgb_mode = (uint8_t)ppu_cgb_mode_enabled();
    memcpy(f->layer, ppu_layer, sizeof f->layer);
    memcpy(f->bguv, ppu_bguv, sizeof f->bguv);
    memcpy(f->spruv, ppu_spruv, sizeof f->spruv);
    memcpy(f->bgtile, ppu_bgtile, sizeof f->bgtile);
    memcpy(f->sprtile, ppu_sprtile, sizeof f->sprtile);
    memcpy(f->tiles, vram, sizeof f->tiles);
    ppu_vram_bank1_copy(f->tiles_cgb1);
    memcpy(f->bg_map, &vram[0x1800], sizeof f->bg_map);
    memcpy(f->mario_oam, &oam[0x0C], sizeof f->mario_oam);
    memset(f->mario_oam2, 0, sizeof f->mario_oam2);
    memset(f->luigi_mask, 0, sizeof f->luigi_mask);
    memset(f->p2_projectile_oam, 0, sizeof f->p2_projectile_oam);
    memset(f->p2_effect_oam, 0, sizeof f->p2_effect_oam);
    ppu_cgb_obj_palette_copy(f->cgb_obj_palette);
    f->p1_lives = rd8(0xDA15);
    f->p2_lives = mp_p2_lives;
    f->p2_game_state = mp_p2_state.game_state;
    f->p2_visible = (uint8_t)(mp_p2_spawned && mp_p2_lives > 0);
    f->p2_blink_hidden = (uint8_t)(mp_p2_invulnerability_frames > 0 &&
                                   ((frame_count / 4) & 1));
    if (screen_dx) {
        for (int i = 0; i < 4; i++) {
            int x = (int)f->mario_oam[i * 4 + 1] + screen_dx;
            f->mario_oam[i * 4 + 1] = (uint8_t)x;
        }
    }
    f->w = ppu_w; f->xoff = ppu_xoff;
    f->player_x = rd8(0xC202); f->player_y = rd8(0xC201);
    f->scroll_x = rd8(0xFFA4); f->game_state = rd8(0xFFB3);
    f->obp0 = ppu_read(0x48); f->obp1 = ppu_read(0x49);
    f->sprite_size16 = (uint8_t)((ppu_read(0x40) & 0x04) != 0);
    f->lcd_on = ppu_lcd_is_on();
    f->seq = (uint64_t)frame_count;
    mp_capture_mp_player_data(f);
}

static void mp_capture_mp_player_data(Frame *frame)
{
    if (!frame) return;
    memset(frame->mp_player_oam, 0, sizeof frame->mp_player_oam);
    memset(frame->mp_player_sprite_count, 0, sizeof frame->mp_player_sprite_count);
    frame->mp_player_count = (uint8_t)mp_player_count;
    frame->mp_player_lives[0] = frame->p1_lives;
    frame->mp_player_game_state[0] = frame->game_state;
    frame->mp_player_visible[0] = 1;
    frame->mp_player_blink_hidden[0] = 0;
    frame->mp_player_sprite_count[0] = 4;
    memcpy(frame->mp_player_oam[0], frame->mario_oam, sizeof frame->mario_oam);

    frame->mp_player_lives[1] = mp_p2_lives;
    frame->mp_player_game_state[1] = mp_p2_state.game_state;
    frame->mp_player_visible[1] = (uint8_t)(mp_p2_spawned && mp_p2_lives > 0);
    frame->mp_player_blink_hidden[1] = (uint8_t)(mp_p2_invulnerability_frames > 0 && ((frame_count / 4) & 1));
    frame->mp_player_sprite_count[1] = 4;
    memcpy(frame->mp_player_oam[1], mp_p2_state.mario_oam, sizeof mp_p2_state.mario_oam);
    memcpy(frame->mp_player_projectile_oam[1], mp_p2_state.projectile_oam, sizeof mp_p2_state.projectile_oam);
    memcpy(frame->mp_player_effect_oam[1], mp_p2_state.effect_oam, sizeof mp_p2_state.effect_oam);

    for (int player = 2; player < MAX_MP_PLAYERS; player++) {
        const MpExtraPlayer *extra = &mp_extra_players[player - 2];
        frame->mp_player_lives[player] = extra->lives;
        frame->mp_player_game_state[player] = extra->state.game_state;
        frame->mp_player_visible[player] = (uint8_t)(extra->spawned && extra->lives > 0);
        frame->mp_player_blink_hidden[player] = (uint8_t)(extra->invulnerability_frames > 0 && ((frame_count / 4) & 1));
        frame->mp_player_sprite_count[player] = 4;
        memcpy(frame->mp_player_oam[player], extra->state.mario_oam, sizeof extra->state.mario_oam);
        memcpy(frame->mp_player_projectile_oam[player], extra->state.projectile_oam, sizeof extra->state.projectile_oam);
        memcpy(frame->mp_player_effect_oam[player], extra->state.effect_oam, sizeof extra->state.effect_oam);
    }
    /* Keep the legacy Player 2 fields populated for existing callers. */
    frame->p2_lives = mp_p2_lives;
    frame->p2_game_state = mp_p2_state.game_state;
    frame->p2_visible = (uint8_t)(mp_p2_spawned && mp_p2_lives > 0);
    frame->p2_blink_hidden = frame->mp_player_blink_hidden[1];
    memcpy(frame->mario_oam2, frame->mp_player_oam[1], sizeof frame->mario_oam2);
    memcpy(frame->p2_projectile_oam, frame->mp_player_projectile_oam[1], sizeof frame->p2_projectile_oam);
    memcpy(frame->p2_effect_oam, frame->mp_player_effect_oam[1], sizeof frame->p2_effect_oam);
}

const uint8_t *emu_mp_player_sprite_tiles(int player, int bank)
{
    if (mp_game != GAME_SML2 || player < 1 || player >= MAX_MP_PLAYERS ||
        !mp_sml2_render_tiles_valid[player])
        return NULL;
    return bank ? mp_sml2_render_tiles_cgb1[player] : mp_sml2_render_tiles[player];
}

void emu_mp_frame_refresh(Frame *frame)
{
    if (!mp_ready || !frame) return;
    if (mp_game == GAME_SML2) {
        mp_sml2_capture_frame(frame);
        return;
    }
    mp_capture_frame(frame, 0);
    if (mp_p2_spawned && mp_p2_lives > 0) {
        memcpy(frame->mario_oam2, mp_p2_state.mario_oam, sizeof frame->mario_oam2);
        memcpy(frame->p2_projectile_oam, mp_p2_state.projectile_oam, sizeof frame->p2_projectile_oam);
        memcpy(frame->p2_effect_oam, mp_p2_state.effect_oam, sizeof frame->p2_effect_oam);
    }
}

int emu_mp_begin(void)
{
    mp_game = rom_loaded_game();
    if (thr || mp_ready || (mp_game != GAME_SML && mp_game != GAME_SML2)) return -1;
    if (!fmx) fmx = SDL_CreateMutex();
    gb_reset();
    if (save_path[0]) cart_load_save(save_path);
    if (gb_state_save(mp_state, GB_STATE_BYTES)) return -1;

    /*
     * Do not copy Mario state here: the reset state has not reached SML1's
     * level-start initialization yet. Player 2 is spawned from the first real
     * gameplay frame below, after C200-C20F contain a valid Mario.
     */
    memset(&mp_p2_state, 0, sizeof mp_p2_state);
    memset(mp_extra_players, 0, sizeof mp_extra_players);
    mp_player_count = settings.g[mp_game].multiplayer_players;
    if (mp_player_count < 2) mp_player_count = 2;
    if (mp_player_count > MAX_MP_PLAYERS) mp_player_count = MAX_MP_PLAYERS;
    mp_current_player = 0;
    mp_p2_spawned = 0;
    mp_p2_respawn_requested = 0;
    mp_p2_invulnerability_frames = 0;
    mp_pending_square_sfx = mp_pending_noise_sfx = 0;
    mp_p1_lives_seen = (uint8_t)mp_bcd_to_int(rd8(0xDA15));
    mp_p2_lives = mp_p1_lives_seen;
    memset(mp_sml2_players, 0, sizeof mp_sml2_players);
    memset(mp_sml2_render_oam, 0, sizeof mp_sml2_render_oam);
    memset(mp_sml2_render_oam_count, 0, sizeof mp_sml2_render_oam_count);
    memset(mp_sml2_render_tiles_valid, 0, sizeof mp_sml2_render_tiles_valid);
    mp_sml2_initialized = 0;
    mp_sml2_tile_patches_clear();
    mp_sml2_stable_frames = 0;
    mp_sml2_stable_level = 0;
    mp_sml2_stable_bank = 0;
    if (mp_game == GAME_SML2) {
        mp_vblank_waiting = 0;
        gb_mp_vblank_watch = 0;
        mp_frame_out = NULL;
        mp_audio_out = NULL;
        mp_audio_max = mp_audio_n = 0;
        mp_ready = 1;
        rewind_init();
        return 0;
    }
    for (int player = 2; player < MAX_MP_PLAYERS; player++)
        mp_extra_players[player - 2].lives = mp_p1_lives_seen;
    mp_vblank_life_event = 0;
    mp_vblank_collision = 0;
    mp_vblank_collision_addr = 0;
    mp_vblank_collision_before = 0;
    mp_vblank_square_sfx = 0;
    mp_vblank_noise_sfx = 0;
    mp_vblank_floaty_control = 0;
    mp_vblank_floaty_x = 0;
    mp_vblank_floaty_y = 0;
    mp_pending_block = 0;
    mp_pending_block_idx = 0;
    mp_p2_a_was_down = 0;
    mp_vblank_waiting = 0;
    memset(mp_p2_last_oam, 0, sizeof mp_p2_last_oam);
    gb_mp_vblank_watch = 0;

    mp_ready = 1;
    rewind_init();
    return 0;
}


static int mp_sml2_bcd_to_int(uint8_t b)
{
    return ((b >> 4) & 0x0F) * 10 + (b & 0x0F);
}

static int mp_sml2_private_addr(unsigned address)
{
    /* Character movement/animation addresses identified from the SML2
     * disassembly. Do not copy adjacent enemy, level or timer RAM. */
    switch (address) {
    case 0xA200: case 0xA201: case 0xA202:
    case 0xA20C: case 0xA20D: case 0xA20E:
    case 0xA214: case 0xA215: case 0xA216: case 0xA217:
    case 0xA218: case 0xA219: case 0xA220: case 0xA222:
    case 0xA223: case 0xA224:
    case 0xA227: case 0xA228: case 0xA229: case 0xA22A:
    case 0xA22B: case 0xA22C: case 0xA232: case 0xA233:
    case 0xA235: case 0xA236: case 0xA237: case 0xA238:
    case 0xA23B: case 0xA23C: case 0xA23D: case 0xA24F:
    case 0xA25A: case 0xA25C: case 0xA268: case 0xA26B: case 0xA26C:
    case 0xA26D: case 0xA271: case 0xA272: case 0xA279: case 0xA27A:
    case 0xA27C: case 0xA283: case 0xA284: case 0xA285:
    case 0xA291: case 0xA2A0: case 0xA2B2: case 0xA2B3: case 0xA2DE:
        return 1;
    default:
        return 0;
    }
}

/* Track only tiles that change near a player while the player is
 * airborne and neither the game camera nor the PPU tilemap scrolls. The ROM
 * rebuilds the circular BG map from level data as the camera moves; keeping
 * only these local interaction changes lets used/broken blocks survive that
 * rebuild without pinning ordinary scenery or scrolling rows. */
static void mp_sml2_tile_patches_clear(void)
{
    mp_sml2_tile_patch_count = 0;
    mp_sml2_tile_patch_level = 0xFF;
    mp_sml2_tile_patch_bank = 0xFF;
}

static void mp_sml2_tile_patches_prepare(uint8_t level, uint8_t bank)
{
    if (mp_sml2_tile_patch_level != level ||
        mp_sml2_tile_patch_bank != bank) {
        mp_sml2_tile_patch_count = 0;
        mp_sml2_tile_patch_level = level;
        mp_sml2_tile_patch_bank = bank;
    }
}

static uint16_t mp_sml2_read16(unsigned address)
{
    return (uint16_t)rd8((uint16_t)address) |
           ((uint16_t)rd8((uint16_t)(address + 1u)) << 8);
}

static int mp_sml2_signed_delta(uint8_t a, uint8_t b)
{
    int d = (int)a - (int)b;
    if (d > 127) d -= 256;
    if (d < -128) d += 256;
    return d;
}

static int mp_sml2_tile_near_player(int tile_x, int tile_y,
                                    int player_x, int player_y)
{
    int dx = mp_sml2_signed_delta((uint8_t)(tile_x + 4), (uint8_t)player_x);
    int dy = mp_sml2_signed_delta((uint8_t)(tile_y + 4), (uint8_t)player_y);
    /* Head-hit blocks sit above the player; allow a small amount below for
     * used blocks and pickups, but reject unrelated scenery across the view. */
    return dx >= -24 && dx <= 24 && dy >= -36 && dy <= 16;
}

static void mp_sml2_record_tile_patches(uint8_t level, uint8_t bank,
                                        const uint8_t before[0x400],
                                        const uint8_t after[0x400],
                                        int scx, int scy,
                                        uint8_t player_x, uint8_t player_y)
{
    mp_sml2_tile_patches_prepare(level, bank);
    uint16_t origin_x = (uint16_t)(mp_sml2_read16(0xA227) - player_x);
    uint16_t origin_y = (uint16_t)(mp_sml2_read16(0xA229) - player_y);

    for (int i = 0; i < 0x400; i++) {
        if (before[i] == after[i]) continue;
        int map_x = i & 31;
        int map_y = i >> 5;
        int screen_x = (map_x * 8 - scx) & 0xFF;
        int screen_y = (map_y * 8 - scy) & 0xFF;
        if (!mp_sml2_tile_near_player(screen_x, screen_y, player_x, player_y))
            continue;

        uint16_t world_x = (uint16_t)(origin_x + screen_x) & 0xFFF8u;
        uint16_t world_y = (uint16_t)(origin_y + screen_y) & 0xFFF8u;
        int found = 0;
        for (unsigned j = 0; j < mp_sml2_tile_patch_count; j++) {
            MpSml2TilePatch *patch = &mp_sml2_tile_patches[j];
            if (patch->level == level && patch->bank == bank &&
                patch->world_x == world_x && patch->world_y == world_y) {
                /* The same block can be hit more than once; keep its latest
                 * interaction tile, not a prior intermediate animation. */
                patch->tile = after[i];
                found = 1;
                break;
            }
        }
        if (found || mp_sml2_tile_patch_count >= MP_SML2_MAX_TILE_PATCHES)
            continue;
        MpSml2TilePatch *patch =
            &mp_sml2_tile_patches[mp_sml2_tile_patch_count++];
        patch->world_x = world_x;
        patch->world_y = world_y;
        patch->level = level;
        patch->bank = bank;
        patch->tile = after[i];
    }
}

static void mp_sml2_apply_tile_patches(void)
{
    if (!mp_sml2_gameplay_active()) return;
    uint8_t level = rd8(0xA269);
    uint8_t bank = rd8(0xA258);
    mp_sml2_tile_patches_prepare(level, bank);
    if (!mp_sml2_tile_patch_count) return;

    uint16_t origin_x = (uint16_t)(mp_sml2_read16(0xA227) - rd8(0xA23C));
    uint16_t origin_y = (uint16_t)(mp_sml2_read16(0xA229) - rd8(0xA23B));
    int scx = rd8(0xFF43);
    int scy = rd8(0xFF42);
    for (unsigned i = 0; i < mp_sml2_tile_patch_count; i++) {
        const MpSml2TilePatch *patch = &mp_sml2_tile_patches[i];
        if (patch->level != level || patch->bank != bank) continue;
        int dx = (int16_t)(patch->world_x - origin_x);
        int dy = (int16_t)(patch->world_y - origin_y);
        /* Keep the full 16-bit world offset: the visible tilemap spans
         * 256 pixels, so valid positive offsets can exceed 127 pixels. */
        if (dx < 0 || dx >= 256 || dy < 0 || dy >= 256) continue;
        int map_x = (((unsigned)dx + (unsigned)scx) & 0xFFu) >> 3;
        int map_y = (((unsigned)dy + (unsigned)scy) & 0xFFu) >> 3;
        vram[0x1800 + map_y * 32 + map_x] = patch->tile;
    }
}

static int mp_sml2_distance(uint8_t a, int b)
{
    int d = abs((int)a - (b & 0xFF));
    return d > 128 ? 256 - d : d;
}

static void mp_sml2_capture_oam_from(const uint8_t source[0xA0],
                                         uint8_t out[16])
{
    memset(out, 0, 16);
    int bx = rd8(0xA23C), by = rd8(0xA23B);
    const int sy = 8;
    uint8_t used[40] = {0};
    for (int slot = 0; slot < 4; slot++) {
        int ox = (slot & 1) ? 8 : 0;
        int oy = (slot & 2) ? sy : 0;
        int tx = bx + ox, ty = by + oy;
        int best = -1, best_score = 1000;
        for (int i = 0; i < 40; i++) {
            if (used[i]) continue;
            int y = source[i * 4], x = source[i * 4 + 1];
            if (!y || y >= 160 || !x || x >= 168) continue;
            int sx0 = mp_sml2_distance((uint8_t)x, tx);
            int sy0 = mp_sml2_distance((uint8_t)y, ty);
            int sx1 = mp_sml2_distance((uint8_t)x, tx + 8);
            int sy1 = mp_sml2_distance((uint8_t)y, ty + 16);
            int score0 = sx0 + sy0, score1 = sx1 + sy1;
            int score = score0 < score1 ? score0 : score1;
            if (score < best_score) { best_score = score; best = i; }
        }
        if (best < 0 || best_score > 12) continue;
        used[best] = 1;
        memcpy(&out[slot * 4], &source[best * 4], 4);
    }
}

/* Keep Player 1's legacy four-piece capture path unchanged. */
static void mp_sml2_capture_oam(uint8_t out[16])
{
    mp_sml2_capture_oam_from(oam, out);
}

/* SML2's player renderer indexes a pointer table at $4000 in ROM bank 1.
 * Decode that exact mapping (Y offset, X offset, tile, attributes) and match
 * its pieces against hardware OAM, rather than assuming every pose is a 2x2. */
static int mp_sml2_capture_mapping_oam_at(int rom_bank, uint8_t mapping,
                                          int base_x, int base_y,
                                          const uint8_t source[0xA0],
                                          uint8_t out[MP_MAX_OAM_SPRITES * 4],
                                          int *complete_out)
{
    memset(out, 0, MP_MAX_OAM_SPRITES * 4);
    if (complete_out) *complete_out = 0;
    if (mapping >= 0xF2) return 0; /* 242 pointers occupy $4000-$41E3. */

    uint16_t pointer_address = (uint16_t)(0x4000u + (uint16_t)mapping * 2u);
    uint8_t lo = cart_rom_read_bank(rom_bank, pointer_address);
    uint8_t hi = cart_rom_read_bank(rom_bank, (uint16_t)(pointer_address + 1u));
    uint16_t map = (uint16_t)(lo | ((uint16_t)hi << 8));
    if (map < 0x41E4 || map >= 0x8000) return 0;

    uint8_t used[40] = {0};
    int count = 0, expected = 0;
    for (int entry = 0; entry < MP_MAX_OAM_SPRITES; entry++) {
        uint8_t raw_dy = cart_rom_read_bank(rom_bank, map++);
        if (raw_dy == 0x80) break;
        uint8_t raw_dx = cart_rom_read_bank(rom_bank, map++);
        uint8_t tile = cart_rom_read_bank(rom_bank, map++);
        (void)cart_rom_read_bank(rom_bank, map++); /* Runtime OAM owns the attributes. */
        expected++;

        int dy = raw_dy < 0x80 ? (int)raw_dy : (int)raw_dy - 256;
        int dx = raw_dx < 0x80 ? (int)raw_dx : (int)raw_dx - 256;
        uint8_t want_y = (uint8_t)(base_y + dy);
        uint8_t want_x = (uint8_t)(base_x + dx);
        for (int i = 0; i < 40; i++) {
            if (used[i]) continue;
            const uint8_t *src = &source[i * 4];
            if (src[0] != want_y || src[1] != want_x || src[2] != tile) continue;
            memcpy(&out[count * 4], src, 4);
            used[i] = 1;
            count++;
            break;
        }
    }
    if (complete_out) *complete_out = expected > 0 && count == expected;
    /* Return exact matches even for partial maps. The caller prefers complete
     * poses, but can safely keep matching pieces instead of guessing a 2x2. */
    return count;
}

/* The retail ROM keeps Mario mappings in bank 1. SML2 DX v1.8.1
 * relocates its CGB player mapping tables to banks 44/45. */
static int mp_sml2_player_mapping_banks(int banks[2])
{
    const CartInfo *ci = cart_info();
    if (rom_hack_active() && ci && ci->mapper == 5 && ci->rom_banks >= 64) {
        banks[0] = 44;
        banks[1] = 45;
        return 2;
    }
    banks[0] = 1;
    banks[1] = -1;
    return 1;
}

/* The game does not use mapping IDs 0-21 for every Mario pose. Its player
 * animation selector is shifted by +32 for small Mario, +69/+95 for the
 * carrot forms, and +112/+144 for fire forms. During a flash animation it
 * can temporarily use the +32 set regardless of power-up. The early IDs
 * include unrelated effects/enemies, which is why searching only 0-21
 * could assemble an unrelated sprite out of nearby OAM entries. */
static int mp_sml2_player_mapping_id_allowed(int mapping)
{
    int powerup = rd8(0xA216);
    int animation = rd8(0xA217);

    /* The original routine at $5550 explicitly selects mapping 107 ($6B)
     * for the special falling/death pose, bypassing the normal power-up
     * offsets below. Keep it valid regardless of the current power-up or
     * invulnerability-flash animation state. */
    if (mapping == 0x6B) return 1;

    if (animation & 0x04)
        return mapping >= 0x20 && mapping <= 0x35;

    switch (powerup) {
    case 0:
        return mapping >= 0x20 && mapping <= 0x35;
    case 1:
        return mapping <= 0x15;
    case 2:
        return (mapping >= 0x45 && mapping <= 0x5A) ||
               (mapping >= 0x5F && mapping <= 0x74);
    case 3:
        return (mapping >= 0x70 && mapping <= 0x85) ||
               (mapping >= 0x90 && mapping <= 0xA5);
    default:
        /* Keep reasonable player-pose ranges if the ROM uses an unexpected
         * power-up value, rather than falling back to arbitrary world maps. */
        return (mapping <= 0x15) ||
               (mapping >= 0x20 && mapping <= 0x35) ||
               (mapping >= 0x45 && mapping <= 0x5A) ||
               (mapping >= 0x5F && mapping <= 0x85) ||
               (mapping >= 0x90 && mapping <= 0xA5);
    }
}

/* Decode only mapping IDs used by the current player form. At the stable
 * screen origin, an exact mapping match is much safer than guessing a 2x2
 * block from nearby OAM entries. If the ROM omits pieces during clipping,
 * exact partial matches are retained as a last resort. */
static int mp_sml2_capture_best_mapping_oam(uint8_t preferred,
                                             const uint8_t source[0xA0],
                                             uint8_t out[MP_MAX_OAM_SPRITES * 4])
{
    uint8_t candidate[MP_MAX_OAM_SPRITES * 4];
    uint8_t best_complete_oam[MP_MAX_OAM_SPRITES * 4];
    uint8_t best_partial_oam[MP_MAX_OAM_SPRITES * 4];
    int banks[2];
    int bank_count = mp_sml2_player_mapping_banks(banks);
    int best_count = 0, partial_count = 0;
    const int player_x = rd8(0xA23C), player_y = rd8(0xA23B);
    memset(out, 0, MP_MAX_OAM_SPRITES * 4);
    memset(best_complete_oam, 0, sizeof best_complete_oam);
    memset(best_partial_oam, 0, sizeof best_partial_oam);

    /* Prefer the live selector when it refers to one of this form's maps. */
    if (preferred < 0xF2 && mp_sml2_player_mapping_id_allowed(preferred)) {
        for (int bi = 0; bi < bank_count; bi++) {
            int complete = 0;
            int count = mp_sml2_capture_mapping_oam_at(banks[bi], preferred,
                player_x, player_y, source, candidate, &complete);
            if (complete && count > best_count) {
                memcpy(best_complete_oam, candidate, (size_t)count * 4u);
                best_count = count;
            } else if (!complete && count > partial_count) {
                memcpy(best_partial_oam, candidate, (size_t)count * 4u);
                partial_count = count;
            }
        }
    }

    /* Search only the mapping families used by Mario in this form. */
    for (int bi = 0; bi < bank_count; bi++) {
        for (int mapping = 0; mapping < 0xF2; mapping++) {
            if (mapping == preferred ||
                !mp_sml2_player_mapping_id_allowed(mapping))
                continue;
            int complete = 0;
            int count = mp_sml2_capture_mapping_oam_at(banks[bi],
                (uint8_t)mapping, player_x, player_y, source, candidate,
                &complete);
            if (complete && count > best_count) {
                memcpy(best_complete_oam, candidate, (size_t)count * 4u);
                best_count = count;
            } else if (!complete && count > partial_count) {
                memcpy(best_partial_oam, candidate, (size_t)count * 4u);
                partial_count = count;
            }
        }
    }
    if (best_count > 0) {
        memcpy(out, best_complete_oam, (size_t)best_count * 4u);
        return best_count;
    }

    /* Some animation paths move pieces slightly relative to A23B/A23C.
     * Infer a nearby origin from the first tile in each allowed map, then
     * validate the entire pose against exact tile IDs and screen positions. */
    for (int bi = 0; bi < bank_count; bi++) {
        int rom_bank = banks[bi];
        for (int mapping = 0; mapping < 0xF2; mapping++) {
            if (!mp_sml2_player_mapping_id_allowed(mapping)) continue;
            uint16_t pointer_address =
                (uint16_t)(0x4000u + (uint16_t)mapping * 2u);
            uint8_t lo = cart_rom_read_bank(rom_bank, pointer_address);
            uint8_t hi = cart_rom_read_bank(rom_bank,
                                            (uint16_t)(pointer_address + 1u));
            uint16_t map = (uint16_t)(lo | ((uint16_t)hi << 8));
            if (map < 0x41E4 || map >= 0x8000) continue;

            uint8_t raw_dy = cart_rom_read_bank(rom_bank, map);
            if (raw_dy == 0x80) continue;
            uint8_t raw_dx = cart_rom_read_bank(rom_bank, (uint16_t)(map + 1u));
            uint8_t tile0 = cart_rom_read_bank(rom_bank, (uint16_t)(map + 2u));
            int dy = raw_dy < 0x80 ? (int)raw_dy : (int)raw_dy - 256;
            int dx = raw_dx < 0x80 ? (int)raw_dx : (int)raw_dx - 256;

            for (int i = 0; i < 40; i++) {
                const uint8_t *src = &source[i * 4];
                if (src[2] != tile0) continue;
                int base_x = (uint8_t)((int)src[1] - dx);
                int base_y = (uint8_t)((int)src[0] - dy);
                if (mp_sml2_distance((uint8_t)base_x, player_x) > 24 ||
                    mp_sml2_distance((uint8_t)base_y, player_y) > 24)
                    continue;

                int complete = 0;
                int count = mp_sml2_capture_mapping_oam_at(rom_bank,
                    (uint8_t)mapping, base_x, base_y, source, candidate,
                    &complete);
                if (complete && count > best_count) {
                    memcpy(best_complete_oam, candidate, (size_t)count * 4u);
                    best_count = count;
                } else if (!complete && count > partial_count) {
                    memcpy(best_partial_oam, candidate, (size_t)count * 4u);
                    partial_count = count;
                }
            }
        }
    }

    if (best_count > 0) {
        memcpy(out, best_complete_oam, (size_t)best_count * 4u);
        return best_count;
    }
    if (partial_count >= 2) {
        memcpy(out, best_partial_oam, (size_t)partial_count * 4u);
        return partial_count;
    }
    return 0;
}

static void mp_sml2_save_player(MpSml2Player *p, int player)
{
    if (!p) return;
    for (unsigned a = 0xA200; a <= 0xA2DE; a++)
        if (mp_sml2_private_addr(a)) p->ram[a - 0xA200] = rd8((uint16_t)a);
    p->keys_held = rd8(0xFF80); p->keys_pressed = rd8(0xFF81);
    p->h_c0 = rd8(0xFFC0); p->h_c1 = rd8(0xFFC1);
    p->h_c2 = rd8(0xFFC2); p->h_c3 = rd8(0xFFC3);
    p->h_c4 = rd8(0xFFC4); p->h_c5 = rd8(0xFFC5);
    p->h_c6 = rd8(0xFFC6); p->h_c7 = rd8(0xFFC7);
    p->lives = rd8(0xA22C);
    p->spawned = 1;
    uint8_t staged_oam[0xA0];
    /* The ROM composes the frame's sprites in SRAM at $A100 before OAM DMA.
     * Read that current-frame list before using the PPU's latched OAM array,
     * which can lag behind the character's just-updated mapping state. */
    for (int i = 0; i < (int)sizeof staged_oam; i++)
        staged_oam[i] = rd8((uint16_t)(0xA100 + i));
    mp_sml2_capture_oam_from(staged_oam, p->oam);
    if (player > 0 && player < MAX_MP_PLAYERS) {
        /* OAM tile IDs belong to the cloned PPU state. Copy their graphics
         * before the caller reloads Player 1's state, otherwise those IDs
         * are rendered using a different character's/pose's VRAM contents. */
        memcpy(mp_sml2_render_tiles[player], vram,
               sizeof mp_sml2_render_tiles[player]);
        uint8_t bank1[0x1800];
        ppu_vram_bank1_copy(bank1);
        memcpy(mp_sml2_render_tiles_cgb1[player], bank1,
               sizeof mp_sml2_render_tiles_cgb1[player]);
        mp_sml2_render_tiles_valid[player] = 1;

        int mapped = mp_sml2_capture_best_mapping_oam(
            p->h_c6, staged_oam, mp_sml2_render_oam[player]);
        /* Fall back to the stable four-piece matcher if the ROM map cannot
         * be read or doesn't match the live OAM for this game frame. */
        if (mapped >= 2) {
            /* Even a clipped partial map is exact; do not replace it with a
             * guessed 2x2 which can include enemy/effect sprites. */
            mp_sml2_render_oam_count[player] = (uint8_t)mapped;
        } else {
            uint8_t live_player_oam[16];
            mp_sml2_capture_oam_from(oam, live_player_oam);
            int live_pieces = 0;
            for (int i = 0; i < 4; i++) {
                const uint8_t *sprite = &live_player_oam[i * 4];
                if (sprite[0] > 0 && sprite[0] < 160 &&
                    sprite[1] > 0 && sprite[1] < 168)
                    live_pieces++;
            }

            if (live_pieces >= 3) {
                memcpy(mp_sml2_render_oam[player], live_player_oam,
                       sizeof live_player_oam);
                mp_sml2_render_oam_count[player] = 4;
            } else {
                memcpy(mp_sml2_render_oam[player], p->oam, sizeof p->oam);
                mp_sml2_render_oam_count[player] = 4;
            }
        }
    }
}

static void mp_sml2_load_player(const MpSml2Player *p)
{
    if (!p) return;
    for (unsigned a = 0xA200; a <= 0xA2DE; a++)
        if (mp_sml2_private_addr(a)) wr8((uint16_t)a, p->ram[a - 0xA200]);
    wr8(0xFF80, p->keys_held); wr8(0xFF81, p->keys_pressed);
    wr8(0xFFC0, p->h_c0); wr8(0xFFC1, p->h_c1);
    wr8(0xFFC2, p->h_c2); wr8(0xFFC3, p->h_c3);
    wr8(0xFFC4, p->h_c4); wr8(0xFFC5, p->h_c5);
    wr8(0xFFC6, p->h_c6); wr8(0xFFC7, p->h_c7);
}

static void mp_sml2_offset_player(MpSml2Player *p, int dx)
{
    if (!p) return;
    p->ram[0x27] = (uint8_t)(p->ram[0x27] + dx);
    p->ram[0x3C] = (uint8_t)(p->ram[0x3C] + dx);
    p->h_c2 = (uint8_t)(p->h_c2 + dx);
    p->h_c5 = (uint8_t)(p->h_c5 + dx);
    p->ram[0x00] = 0x80; /* A200: stationary horizontal-velocity sentinel */
    p->ram[0x21] = 0;    /* A221: don't inherit Mario's pipe/level transition */
    p->previous_a = 0;
    p->invulnerability_frames = 90;
}

/* Shift only camera-relative data. The saved A227/FFC2 coordinates are the
 * character's world position; A23C/FFC5 and OAM X are cached screen positions.
 * Keep clone sprites aligned with the authoritative Player 1 camera without
 * moving their actual position in the level. */
static void mp_sml2_shift_screen_x(MpSml2Player *p, int dx)
{
    if (!p || !dx) return;
    p->ram[0x3C] = (uint8_t)(p->ram[0x3C] + dx); /* A23C: screen X */
    p->h_c5 = (uint8_t)(p->h_c5 + dx);          /* FFC5: OAM screen X */
    for (int i = 0; i < 4; i++) {
        uint8_t *sprite = &p->oam[i * 4];
        if (sprite[0] && sprite[1])
            sprite[1] = (uint8_t)(sprite[1] + dx);
    }
    int player = (int)(p - mp_sml2_players);
    if (player > 0 && player < MAX_MP_PLAYERS) {
        for (int i = 0; i < mp_sml2_render_oam_count[player]; i++) {
            uint8_t *sprite = &mp_sml2_render_oam[player][i * 4];
            if (sprite[0] && sprite[1])
                sprite[1] = (uint8_t)(sprite[1] + dx);
        }
    }
}

static int mp_sml2_gameplay_candidate(void)
{
    /* FF9B is SML2's game-mode dispatcher: mode 4 is active level play.
     * The mode and timer can look valid for a few frames during level entry,
     * so this is only a candidate; co-op must wait for a stable level. */
    uint16_t timer = (uint16_t)rd8(0xA254) | ((uint16_t)rd8(0xA255) << 8);
    return rd8(0xFF9B) == 4 && timer != 0 && rd8(0xA221) == 0 &&
           rd8(0xA23C) != 0 && rd8(0xA23B) != 0;
}

static void mp_sml2_update_gameplay_stability(void)
{
    if (!mp_sml2_gameplay_candidate()) {
        mp_sml2_stable_frames = 0;
        return;
    }

    uint8_t level = rd8(0xA269);
    uint8_t bank = rd8(0xA258);
    if (mp_sml2_stable_frames == 0 ||
        level != mp_sml2_stable_level ||
        bank != mp_sml2_stable_bank) {
        mp_sml2_stable_level = level;
        mp_sml2_stable_bank = bank;
        mp_sml2_stable_frames = 1;
        return;
    }

    /* Require half a second of consecutive authoritative P1 frames with
     * the same level, level bank and active-play signature before clone
     * simulations or shared-world merging are allowed. This keeps transient
     * level-entry and level-loading states out of the cloned game loop. */
    if (mp_sml2_stable_frames < 30) mp_sml2_stable_frames++;
}

static int mp_sml2_gameplay_active(void)
{
    return mp_sml2_stable_frames >= 30 &&
           mp_sml2_gameplay_candidate() &&
           rd8(0xA269) == mp_sml2_stable_level &&
           rd8(0xA258) == mp_sml2_stable_bank;
}

static void mp_sml2_capture_frame(Frame *frame)
{
    if (!frame) return;
    mp_capture_frame(frame, 0);
    memset(frame->luigi_mask, 0, sizeof frame->luigi_mask);
    frame->mp_player_count = (uint8_t)mp_player_count;
    frame->player_x = rd8(0xA227);
    frame->player_y = rd8(0xA229);
    frame->scroll_x = rd8(0xFFCA);
    /* SML2 maps its character as four 8x8 OAM entries. */
    frame->sprite_size16 = 0;
    frame->game_state = 0;
    frame->p1_lives = rd8(0xA22C);
    frame->mp_player_lives[0] = (uint8_t)mp_sml2_bcd_to_int(rd8(0xA22C));
    frame->mp_player_game_state[0] = 0;
    frame->mp_player_visible[0] = 1;
    frame->mp_player_blink_hidden[0] = 0;
    mp_sml2_capture_oam(frame->mario_oam);
    memcpy(frame->mp_player_oam[0], frame->mario_oam, sizeof frame->mario_oam);
    memset(frame->mp_player_sfx_events, 0, sizeof frame->mp_player_sfx_events);
    memset(frame->mp_player_sound_event, 0, sizeof frame->mp_player_sound_event);
    for (int player = 1; player < MAX_MP_PLAYERS; player++) {
        MpSml2Player *p = &mp_sml2_players[player];
        int lives = mp_sml2_bcd_to_int(p->lives);
        frame->mp_player_lives[player] = (uint8_t)lives;
        frame->mp_player_game_state[player] = 0;
        frame->mp_player_visible[player] = (uint8_t)(p->spawned && lives > 0);
        frame->mp_player_blink_hidden[player] = (uint8_t)(
            p->invulnerability_frames > 0 && ((frame_count / 4) & 1));
        if (mp_sml2_render_oam_count[player] > 0) {
            unsigned bytes = (unsigned)mp_sml2_render_oam_count[player] * 4u;
            memcpy(frame->mp_player_oam[player],
                   mp_sml2_render_oam[player], bytes);
            frame->mp_player_sprite_count[player] =
                mp_sml2_render_oam_count[player];
        } else {
            memcpy(frame->mp_player_oam[player], p->oam, sizeof p->oam);
            frame->mp_player_sprite_count[player] = 4;
        }

        /* If both OAM sources failed to identify the clone's pose, show a
         * translated copy of Player 1's known-visible pose as a final fallback.
         * This only affects rendering; the clone keeps independent RAM/physics.
         * It prevents the overlay from disappearing just because this frame's
         * character-map lookup or DMA list was incomplete. */
        if (p->spawned && lives > 0) {
            int visible_pieces = 0;
            for (int i = 0; i < frame->mp_player_sprite_count[player]; i++) {
                const uint8_t *sprite = &frame->mp_player_oam[player][i * 4];
                if (sprite[0] > 0 && sprite[0] < 160 &&
                    sprite[1] > 0 && sprite[1] < 168)
                    visible_pieces++;
            }
            if (visible_pieces < 2) {
                int dx = (int)(uint8_t)(p->ram[0x3C] - rd8(0xA23C));
                int dy = (int)(uint8_t)(p->ram[0x3B] - rd8(0xA23B));
                if (dx > 127) dx -= 256;
                if (dy > 127) dy -= 256;
                int source_pieces = 0;
                for (int i = 0; i < 4; i++) {
                    const uint8_t *sprite = &frame->mario_oam[i * 4];
                    if (sprite[0] > 0 && sprite[0] < 160 &&
                        sprite[1] > 0 && sprite[1] < 168)
                        source_pieces++;
                }
                if (source_pieces >= 2) {
                    memcpy(frame->mp_player_oam[player], frame->mario_oam,
                           sizeof frame->mario_oam);
                    for (int i = 0; i < 4; i++) {
                        uint8_t *sprite = &frame->mp_player_oam[player][i * 4];
                        sprite[0] = (uint8_t)(sprite[0] + dy);
                        sprite[1] = (uint8_t)(sprite[1] + dx);
                    }
                    frame->mp_player_sprite_count[player] = 4;
                    /* This fallback copied Player 1's OAM, so it must use
                     * Player 1's frame-local tile data as well. The clone's
                     * private tile snapshot can have different tile contents
                     * at the same IDs and would render this pose as stripes. */
                    mp_sml2_render_tiles_valid[player] = 0;
                }
            }
        }
        frame->mp_player_sfx_events[player] = p->sfx_events;
    }
    frame->p1_lives = rd8(0xA22C);
    frame->p2_lives = frame->mp_player_lives[1];
    frame->p2_game_state = 0;
    frame->p2_visible = frame->mp_player_visible[1];
    frame->p2_blink_hidden = frame->mp_player_blink_hidden[1];
    memcpy(frame->mario_oam2, frame->mp_player_oam[1], sizeof frame->mario_oam2);
    frame->p2_sfx_events = mp_current_player > 0
        ? mp_sml2_players[mp_current_player].sfx_events : 0;
    memset(frame->p2_projectile_oam, 0, sizeof frame->p2_projectile_oam);
    memset(frame->p2_effect_oam, 0, sizeof frame->p2_effect_oam);
}

static void mp_sml2_spawn_player(int player)
{
    if (player < 1 || player >= MAX_MP_PLAYERS) return;
    MpSml2Player *p = &mp_sml2_players[player];
    uint8_t lives = p->lives;
    *p = mp_sml2_players[0];
    mp_sml2_offset_player(p, 24 * player);
    memset(mp_sml2_render_oam[player], 0, sizeof mp_sml2_render_oam[player]);
    mp_sml2_render_oam_count[player] = 0;
    mp_sml2_render_tiles_valid[player] = 0;
    if (lives) p->lives = lives;
    p->ram[0x2C] = p->lives;
    p->spawned = 1;
    p->respawn_requested = 0;
    p->sfx_events = 0;
}

static int emu_mp_step_sml2(int player, uint8_t buttons, uint8_t dpad,
                            Frame *frame, int16_t *audio, int audio_max)
{
    if (!mp_ready || player < 0 || player >= mp_player_count || !frame) return -1;
    if (player == 0) {
        if (gb_state_load(mp_state, GB_STATE_BYTES)) return -1;
        mp_sml2_apply_tile_patches();
        uint8_t scroll_before = rd8(0xFFCA);
        uint8_t scroll_y_before = rd8(0xFFC8);
        uint8_t scx_before = rd8(0xFF43), scy_before = rd8(0xFF42);
        uint8_t level_before = rd8(0xA269), level_bank_before = rd8(0xA258);
        uint8_t ground_before = rd8(0xA214), air_before = rd8(0xA215);
        memcpy(mp_sml2_vram_before, &vram[0x1800], sizeof mp_sml2_vram_before);
        gb_set_input(buttons, dpad);
        mp_vblank_waiting = 0;
        gb_mp_vblank_watch = 0;
        mp_active = 1;
        int status = setjmp(stop_jmp);
        if (status == 0) run_core(0);
        mp_active = 0;
        if (status == 4) { mp_frame_out = NULL; mp_audio_out = NULL; return -1; }
        memcpy(mp_sml2_vram_after, &vram[0x1800], sizeof mp_sml2_vram_after);
        int level_unchanged = level_before == rd8(0xA269) &&
                              level_bank_before == rd8(0xA258);
        int camera_unchanged = scroll_before == rd8(0xFFCA) &&
                               scroll_y_before == rd8(0xFFC8) &&
                               scx_before == rd8(0xFF43) &&
                               scy_before == rd8(0xFF42);
        int player_airborne = air_before || !ground_before ||
                              rd8(0xA215) || !rd8(0xA214);
        if (level_unchanged && camera_unchanged && player_airborne &&
            mp_sml2_gameplay_active() &&
            memcmp(mp_sml2_vram_before, mp_sml2_vram_after,
                   sizeof mp_sml2_vram_before) != 0) {
            mp_sml2_record_tile_patches(level_before, level_bank_before,
                mp_sml2_vram_before, mp_sml2_vram_after,
                scx_before, scy_before, rd8(0xA23C), rd8(0xA23B));
        }
        mp_sml2_apply_tile_patches();
        int n = (audio && audio_max > 0) ? apu_drain(audio, audio_max) : 0;
        if (gb_state_save(mp_state, GB_STATE_BYTES)) return -1;
        mp_sml2_save_player(&mp_sml2_players[0], 0);
        mp_sml2_update_gameplay_stability();
        int camera_dx = mp_scroll_delta(rd8(0xFFCA), scroll_before);
        if (camera_dx && mp_sml2_initialized && mp_sml2_gameplay_active()) {
            /* Player 1 owns the camera. Translate each clone's cached screen
             * coordinates and sprite X positions when the authoritative view
             * scrolls, leaving its world coordinates and physics untouched. */
            for (int i = 1; i < mp_player_count; i++)
                mp_sml2_shift_screen_x(&mp_sml2_players[i], -camera_dx);
        }
        if (mp_sml2_gameplay_active()) {
            if (!mp_sml2_initialized) {
                for (int i = 1; i < mp_player_count; i++) mp_sml2_spawn_player(i);
                mp_sml2_initialized = 1;
            }
        } else {
            mp_sml2_initialized = 0;
            for (int i = 1; i < MAX_MP_PLAYERS; i++) mp_sml2_players[i].spawned = 0;
        }
        mp_sml2_capture_frame(frame);
        return n;
    }

    if (gb_state_load(mp_state, GB_STATE_BYTES)) return -1;
    MpSml2Player *p = &mp_sml2_players[player];
    p->sfx_events = 0;
    if (!mp_sml2_initialized || !mp_sml2_gameplay_active()) {
        mp_sml2_capture_frame(frame);
        return 0;
    }
    if (p->respawn_requested) {
        uint8_t lives = p->lives ? p->lives : mp_sml2_players[0].lives;
        mp_sml2_spawn_player(player);
        p->lives = lives;
        p->ram[0x2C] = lives;
        p->sfx_events |= P2_SFX_EVENT_RESPAWN;
    }
    if (!p->spawned) {
        mp_sml2_capture_frame(frame);
        return 0;
    }

    int gameplay_before = mp_sml2_gameplay_active();
    uint8_t level_before = rd8(0xA269);
    uint8_t level_bank_before = rd8(0xA258);
    mp_sml2_load_player(p);
    uint8_t old_a = p->previous_a;
    uint8_t old_ground = rd8(0xA214);
    uint8_t old_air = rd8(0xA215);
    uint8_t old_power = rd8(0xA216);
    uint8_t old_lives = rd8(0xA22C);
    uint8_t coins_low = rd8(0xA262), coins_high = rd8(0xA263);
    uint8_t kills = rd8(0xA28D);
    uint8_t scroll = rd8(0xFFCA);
    uint8_t scroll_y_before = rd8(0xFFC8);
    uint8_t scx_before = rd8(0xFF43), scy_before = rd8(0xFF42);
    memcpy(mp_sml2_vram_before, &vram[0x1800], sizeof mp_sml2_vram_before);
    gb_set_input(buttons, dpad);
    mp_vblank_waiting = 0;
    gb_mp_vblank_watch = 0;
    mp_active = 1;
    int status = setjmp(stop_jmp);
    if (status == 0) run_core(0);
    mp_active = 0;
    if (status == 4) { mp_frame_out = NULL; mp_audio_out = NULL; return -1; }
    uint8_t coins_low_after = rd8(0xA262), coins_high_after = rd8(0xA263);
    uint8_t kills_after = rd8(0xA28D), scroll_after = rd8(0xFFCA);
    memcpy(mp_sml2_vram_after, &vram[0x1800], sizeof mp_sml2_vram_after);
    mp_sml2_save_player(p, player);
    int gameplay_after = mp_sml2_gameplay_active();
    int same_level = level_before == rd8(0xA269) &&
                     level_bank_before == rd8(0xA258);
    int merge_world = gameplay_before && gameplay_after && same_level;
    p->previous_a = (uint8_t)((buttons & 0x01u) != 0);
    if ((buttons & 0x01u) && !old_a && old_ground && !old_air)
        p->sfx_events |= P2_SFX_EVENT_JUMP;
    if (old_power != rd8(0xA216))
        p->sfx_events |= rd8(0xA216) > old_power ? P2_SFX_EVENT_POWER_UP : P2_SFX_EVENT_POWER_DOWN;
    if (old_lives != p->lives &&
        mp_sml2_bcd_to_int(p->lives) < mp_sml2_bcd_to_int(old_lives)) {
        p->sfx_events |= P2_SFX_EVENT_DIE;
        p->spawned = 0;
        if (mp_sml2_bcd_to_int(p->lives) > 0) p->respawn_requested = 1;
    }
    if ((buttons & 0x02u) && rd8(0xA216) == 3)
        p->sfx_events |= P2_SFX_EVENT_FIREBALL;
    int camera_dx = mp_scroll_delta(scroll_after, scroll);
    /* Clone simulations can scroll locally. Do not import that camera into
     * the shared world: transform the captured sprite/cache X positions back
     * into Player 1's camera before the authoritative state is restored. */
    if (camera_dx) mp_sml2_shift_screen_x(p, camera_dx);
    int same_camera = camera_dx == 0 &&
        rd8(0xFFC8) == scroll_y_before &&
        rd8(0xFFCA) == scroll &&
        rd8(0xFF43) == scx_before && rd8(0xFF42) == scy_before;
    int vram_changed = merge_world && same_camera &&
        memcmp(mp_sml2_vram_before, mp_sml2_vram_after, sizeof mp_sml2_vram_before) != 0;
    int player_airborne = old_air || !old_ground ||
                          rd8(0xA215) || !rd8(0xA214);
    if (vram_changed && player_airborne) {
        mp_sml2_record_tile_patches(level_before, level_bank_before,
            mp_sml2_vram_before, mp_sml2_vram_after,
            scx_before, scy_before, rd8(0xA23C), rd8(0xA23B));
    }

    if (gb_state_load(mp_state, GB_STATE_BYTES)) return -1;

    if (vram_changed) {
        for (int i = 0; i < 0x400; i++)
            if (mp_sml2_vram_before[i] != mp_sml2_vram_after[i])
                vram[0x1800 + i] = mp_sml2_vram_after[i];
    }
    if (merge_world && coins_low_after != coins_low) wr8(0xA262, coins_low_after);
    if (merge_world && coins_high_after != coins_high) wr8(0xA263, coins_high_after);
    if (merge_world && kills_after != kills) wr8(0xA28D, kills_after);
    mp_sml2_apply_tile_patches();
    if (vram_changed ||
        (merge_world && (coins_low_after != coins_low ||
                         coins_high_after != coins_high || kills_after != kills))) {
        if (gb_state_save(mp_state, GB_STATE_BYTES)) return -1;
    }
    if (p->invulnerability_frames > 0) p->invulnerability_frames--;
    mp_sml2_capture_frame(frame);
    return 0;
}

static int emu_mp_step_internal(int player, uint8_t buttons, uint8_t dpad, Frame *frame, int16_t *audio, int audio_max)
{
    if (!mp_ready || (player != 0 && player != 1) || !frame) return -1;
    frame->p2_sfx_events = 0;

    if (gb_state_load(mp_state, GB_STATE_BYTES)) return -1;

    if (player == 1) {
        uint8_t p2_a_was_down_before = mp_p2_a_was_down;
        mp_p2_a_was_down = (uint8_t)((buttons & 0x01u) != 0);
        if (mp_p2_respawn_requested && mp_p2_spawned && rd8(0xFFB3) == 0) {
            if (mp_p2_lives == 0) {
                mp_p2_lives = (uint8_t)mp_bcd_to_int(rd8(0xDA15));
                if (mp_p2_lives == 0) mp_p2_lives = 1;
            }
            mp_respawn_p2_from_p1(mp_current_player);
            frame->p2_sfx_events |= P2_SFX_EVENT_RESPAWN;
        }
        if (!mp_p2_spawned || mp_p2_lives == 0) {
            memset(frame->mario_oam2, 0, sizeof frame->mario_oam2);
            memset(frame->p2_projectile_oam, 0, sizeof frame->p2_projectile_oam);
            memset(frame->p2_effect_oam, 0, sizeof frame->p2_effect_oam);
            memset(frame->mario_oam, 0, sizeof frame->mario_oam);
            memset(frame->luigi_mask, 0, sizeof frame->luigi_mask);
            frame->p1_lives = rd8(0xDA15);
            frame->p2_lives = mp_p2_lives;
            frame->p2_game_state = mp_p2_state.game_state;
            frame->p2_visible = 0;
            return 0;
        }

        uint8_t p1_scroll = rd8(0xFFA4);
        uint8_t p2_state_before = mp_p2_state.game_state;
        /* Snapshot prior character values so one-shot sounds follow actual events. */
        uint8_t p2_jump_status_before = mp_p2_state.mario[7];   /* C207: 0 ground, 1 rising, 2 falling */
        uint8_t p2_grounded_before = mp_p2_state.mario[10];     /* C20A: 1 on ground */
        uint8_t p2_super_status_before = mp_p2_state.super_status;
        uint8_t p2_superball_before = mp_p2_state.superball;
        uint8_t p2_projectiles_before[3];
        memcpy(p2_projectiles_before, mp_p2_state.projectile_status, sizeof p2_projectiles_before);
        uint8_t p2_ttl_before = mp_p2_state.superball_ttl;
        int p2_world_lives_before = mp_bcd_to_int(rd8(0xDA15));
        uint8_t score_before[3];
        uint8_t coins_before = rd8(0xFFFA);
        uint8_t p1_square_sfx_before = rd8(0xDFE0);
        uint8_t p1_noise_sfx_before = rd8(0xDFF8);
        for (int i = 0; i < 3; i++) score_before[i] = rd8((uint16_t)(0xC0A0 + i));

        mp_capture_shared_world_before();
        mp_player_load(&mp_p2_state);
        uint8_t p2_floaty_before = rd8(0xFFED);

        /* A death sequence and hurt timer are part of SML1's actual state
         * machine, so keep the original game state instead of substituting a
         * second animation system. */
        mp_buttons = (p2_state_before == 0) ? buttons : 0;
        mp_dpad = (p2_state_before == 0) ? dpad : 0;
        gb_set_input(mp_buttons, mp_dpad);

        mp_frame_out = frame;
        mp_audio_out = audio;
        mp_audio_max = audio_max;
        mp_audio_n = 0;
        mp_vblank_waiting = 0;
        gb_mp_vblank_watch = 0;
        mp_active = 1;
        int mp_run_status = setjmp(stop_jmp);
        if (mp_run_status == 0) run_core(0);
        if (mp_run_status == 4) {
            mp_active = 0; mp_frame_out = NULL; mp_audio_out = NULL;
            mp_audio_max = mp_audio_n = 0;
            return -1;
        }
        mp_active = 0;

        mp_capture_shared_world_after();
        uint8_t p2_game_state = rd8(0xFFB3);
        uint8_t p2_scroll_after = rd8(0xFFA4);
        uint8_t score_after[3];
        uint8_t coins_after = rd8(0xFFFA);
        uint8_t p2_floaty_control = mp_vblank_floaty_control ? mp_vblank_floaty_control : rd8(0xFFED);
        uint8_t p2_floaty_x = mp_vblank_floaty_control ? mp_vblank_floaty_x : rd8(0xFFEB);
        uint8_t p2_floaty_y = mp_vblank_floaty_control ? mp_vblank_floaty_y : rd8(0xFFEC);
        uint8_t p2_square_current = rd8(0xDFE0);
        uint8_t p2_noise_current = rd8(0xDFF8);
        /* Never mistake Mario's already-pending sound for a sound Luigi made. */
        uint8_t p2_square_sfx = mp_vblank_square_sfx ? mp_vblank_square_sfx :
                                (p2_square_current != p1_square_sfx_before ? p2_square_current : 0);
        uint8_t p2_noise_sfx = mp_vblank_noise_sfx ? mp_vblank_noise_sfx :
                               (p2_noise_current != p1_noise_sfx_before ? p2_noise_current : 0);
        for (int i = 0; i < 3; i++) score_after[i] = rd8((uint16_t)(0xC0A0 + i));
        int p2_world_lives_after = mp_bcd_to_int(rd8(0xDA15));
        int life_delta = p2_world_lives_after - p2_world_lives_before;

        /* The VBlank routine consumes wLivesEarnedLost, so frame_hook snapshots
         * it before UpdateLives clears it. This catches a 1UP/100-coin life even
         * when the cloned world's visible lives are already saturated at 99. */
        if (mp_vblank_life_event == 0xFF)
            life_delta = -1;
        else if (mp_vblank_life_event != 0)
            life_delta = 1;

        if (life_delta > 0) {
            for (int i = 0; i < life_delta && mp_p2_lives < 99; i++) mp_p2_lives++;
        } else if (life_delta < 0) {
            for (int i = 0; i < -life_delta && mp_p2_lives > 0; i++) mp_p2_lives--;
        }

        int dx = mp_scroll_delta(p2_scroll_after, p1_scroll);
        int enemy_merged = 0;
        int enemy_sound_event = 0;
        int p2_stomp_event = (p2_square_sfx == 0x03);
        /*
         * Generate one-shot sound flags from real game-state transitions.
         * Pressing A while airborne must stay silent: C207 transitions 0 -> 1
         * and C20A says whether Luigi was grounded before the frame.
         */
        uint8_t p2_sfx_events = 0;
        uint8_t jump_status_after = rd8(0xC207);
        if (p2_state_before == 0 && p2_grounded_before && p2_jump_status_before == 0 &&
            jump_status_after != 0 && (buttons & 0x01u) && !p2_a_was_down_before)
            p2_sfx_events |= P2_SFX_EVENT_JUMP;

        for (int i = 0; i < 3; i++) {
            uint8_t projectile_after = rd8((uint16_t)(0xFFA9 + i));
            if (p2_projectiles_before[i] == 0 && projectile_after != 0)
                p2_sfx_events |= P2_SFX_EVENT_FIREBALL;
        }
        if (p2_ttl_before == 0 && rd8(0xC0A9) != 0)
            p2_sfx_events |= P2_SFX_EVENT_FIREBALL;

        /* These original sound codes mark pickup/injury; RAM changes cover DX variants. */
        uint8_t p2_super_status_after = rd8(0xFF99);
        uint8_t p2_superball_after = rd8(0xFFB5);
        if (p2_square_sfx == 0x04 ||
            (p2_superball_before == 0 && p2_superball_after != 0) ||
            (p2_super_status_before != 1 && p2_super_status_after == 1))
            p2_sfx_events |= P2_SFX_EVENT_POWER_UP;

        if (p2_square_sfx == 0x06 ||
            (p2_super_status_before != 3 && p2_super_status_after == 3) ||
            (p2_superball_before != 0 && p2_superball_after == 0))
            p2_sfx_events |= P2_SFX_EVENT_POWER_DOWN;

        if ((p2_state_before == 0 && p2_game_state == 3) || life_delta < 0)
            p2_sfx_events |= P2_SFX_EVENT_DIE;

        if (p2_sfx_events & P2_SFX_EVENT_DIE)
            p2_sfx_events &= (uint8_t)~P2_SFX_EVENT_POWER_DOWN;
        frame->p2_sfx_events |= p2_sfx_events;

        /*
         * A stomp sound belongs to one collision, not every enemy whose AI
         * happened to change state during this isolated frame. First collect
         * explicit death-state transitions. If this particular enemy uses a
         * less-common transition, use the nearest changed enemy to Luigi as the
         * stomp target.
         */
        int stomp_target = -1;
        if (p2_stomp_event) {
            int changed = 0;
            for (int slot = 0; slot < 10; slot++) {
                uint8_t bt = mp_enemy_before[slot * 0x10];
                uint8_t at = mp_enemy_after[slot * 0x10];
                if (bt != 0xFF && bt != at && !mp_is_pickup(bt) && !mp_is_pickup(at)) {
                    stomp_target = slot;
                    changed++;
                }
            }
            /* Only use the fallback when exactly one enemy changed. */
            if (changed != 1) stomp_target = -1;
        }

        memset(mp_enemy_merge_mask, 0, sizeof mp_enemy_merge_mask);
        for (int slot = 0; slot < 10; slot++) {
            uint8_t before_type = mp_enemy_before[slot * 0x10];
            uint8_t after_type = mp_enemy_after[slot * 0x10];
            int type_changed = before_type != after_type && before_type != 0xFF;
            int enemy_death = type_changed &&
                              (mp_is_enemy_stomp_transition(before_type, after_type) ||
                               after_type == 0xFF ||
                               slot == stomp_target);
            int pickup_consumed = mp_is_pickup(before_type) && before_type != after_type;
            int pickup_spawned = before_type == 0xFF && mp_is_pickup(after_type);
            if (enemy_death || pickup_consumed || pickup_spawned) {
                mp_enemy_merge_mask[slot] = 1;
                memcpy(&mp_enemy_merge[slot * 0x10], &mp_enemy_after[slot * 0x10], 0x10);
                int ex = (int)mp_enemy_merge[slot * 0x10 + 3] + dx;
                mp_enemy_merge[slot * 0x10 + 3] = (uint8_t)ex;
                enemy_merged = 1;
                if (enemy_death) enemy_sound_event = 1;
            }
        }
        frame->p2_sound_event = (uint8_t)((coins_after != coins_before) || enemy_sound_event);

        /*
         * Convert Luigi's local camera back to the authoritative Player 1
         * coordinate system before saving his private character state.
         */
        if (dx) {
            uint8_t x = rd8(0xC202);
            wr8(0xC202, (uint8_t)(x + dx));
        }

        /*
         * Do NOT publish the isolated Luigi world here. Its tilemap contains
         * temporary question-block/coin changes and its camera may have
         * loaded different columns. Only keep Luigi's freshly rendered OAM;
         * the visible frame must be captured from the restored P1 world below.
         */
        uint8_t p2_oam_saved[16];
        uint8_t p2_projectile_oam_saved[12];
        uint8_t p2_effect_oam_saved[52];
        mp_copy_mario_oam_buffer(p2_oam_saved, dx);
        mp_copy_projectile_oam_buffer(p2_projectile_oam_saved, dx);
        mp_copy_effect_oam_buffer(p2_effect_oam_saved, dx);
        memcpy(mp_p2_last_oam, p2_oam_saved, sizeof mp_p2_last_oam);

        mp_player_save(&mp_p2_state);
        memcpy(mp_p2_state.projectile_oam, p2_projectile_oam_saved, sizeof mp_p2_state.projectile_oam);
        memcpy(mp_p2_state.effect_oam, p2_effect_oam_saved, sizeof mp_p2_state.effect_oam);
        mp_p2_state.game_state = p2_game_state;
        memcpy(mp_p2_state.mario_oam, mp_p2_last_oam, sizeof mp_p2_state.mario_oam);

        /*
         * Restore Mario's authoritative world exactly as it was before the
         * isolated Luigi simulation. Luigi's power-up state lives in
         * mp_p2_state and is therefore kept private to his next simulation;
         * it must never be copied into Player 1's RAM.
         */
        if (gb_state_load(mp_state, GB_STATE_BYTES)) return -1;

        /*
         * Transfer Luigi's newly generated SML1 sound requests to the
         * authoritative P1 state. Coin and stomp have stable original IDs;
         * block hits use the game's noise SFX.
         */
        int p2_new_floaty = (p2_floaty_before == 0 && p2_floaty_control != 0);
        uint8_t square_request = 0;
        if (coins_after != coins_before) square_request = 0x05; /* SFX_COIN */
        else if (enemy_sound_event) square_request = 0x03; /* SFX_STOMP */
        else if (p1_square_sfx_before == 0 && p2_square_sfx &&
                 p2_square_sfx != 0x01 && p2_square_sfx != 0x04 && p2_square_sfx != 0x06)
            square_request = p2_square_sfx; /* P2 jump and power sounds are mixed separately. */
        /* Jump sound is synthesized by PipeClean and triggered from the frame flag. */
        mp_queue_sfx(0xDFE0, &mp_pending_square_sfx, square_request);

        int p2_block_hit = (mp_vblank_collision == 0x01 ||
                            mp_vblank_collision == 0x02 ||
                            mp_vblank_collision == 0x04);
        uint8_t noise_request = p2_block_hit ? 0x02 :
            ((p1_noise_sfx_before == 0) ? p2_noise_sfx : 0);
        mp_queue_sfx(0xDFF8, &mp_pending_noise_sfx, noise_request);

        /*
         * FFED is the original score/floaty queue. Preserve only a newly
         * created P2 event, never a pending P1 event inherited by the clone.
         * The normal SML1 code will process it on the authoritative update.
         */
        if (p2_new_floaty && rd8(0xFFED) == 0) {
            wr8(0xFFEB, p2_floaty_x);
            wr8(0xFFEC, p2_floaty_y);
            wr8(0xFFED, p2_floaty_control);
        } else if (enemy_sound_event && rd8(0xFFED) == 0) {
            /* A stomp always awards an original SML1 floaty; this fallback is
             * only used if the transient FFED write was consumed during the
             * cloned frame. */
            wr8(0xFFEB, rd8(0xC202));
            wr8(0xFFEC, (uint8_t)(rd8(0xC201) - 8));
            wr8(0xFFED, 0x01); /* first stomp = 100 points */
        }

        /*
         * Shared level edits. A same-camera frame can safely merge the entire
         * tile map. During a private-camera move, merge only small local edits;
         * the exact tile touched by SML1's VBlank collision routine is handled
         * separately below.
         */
        int tile_changed = 0;
        int block_hit = (mp_vblank_collision == 0x01 ||
                         mp_vblank_collision == 0x02 ||
                         mp_vblank_collision == 0x04);

        if (p2_state_before == 0) {
            tile_changed |= mp_merge_block_vblank_event(
                mp_vblank_collision,
                mp_vblank_collision_addr,
                (mp_vblank_collision_addr >= 0x9800 && mp_vblank_collision_addr < 0x9C00)
                    ? mp_tilemap_after[mp_vblank_collision_addr - 0x9800]
                    : 0,
                p2_scroll_after,
                p1_scroll);

            if (mp_vblank_collision != 0x01 &&
                mp_vblank_collision != 0x02 &&
                mp_vblank_collision != 0x04)
                tile_changed |= mp_merge_tilemap_local_edits(dx == 0, p2_scroll_after, p1_scroll);

            if (mp_vblank_collision == 0xC0 &&
                mp_vblank_collision_addr >= 0x9800 &&
                mp_vblank_collision_addr < 0x9C00) {
                int p2idx = (int)mp_vblank_collision_addr - 0x9800;
                int row = p2idx / 32, col = p2idx & 31;
                int world_col = ((int)(p2_scroll_after >> 3) + col) & 31;
                int p1_col = (world_col - (int)(p1_scroll >> 3)) & 31;
                int p1idx = row * 32 + p1_col;

                if (mp_vblank_collision_before != mp_tilemap_after[p2idx]) {
                    vram[0x1800 + p1idx] = mp_tilemap_after[p2idx];
                    tile_changed = 1;
                }
            }
        }

        int collision_changed = 0;

        /* Coins/score and consumed/spawned powerups are shared world state. */
        if (p2_state_before == 0) {
            for (int i = 0; i < 3; i++)
                if (score_after[i] != score_before[i])
                    wr8((uint16_t)(0xC0A0 + i), score_after[i]);
            if (coins_after != coins_before)
                wr8(0xFFFA, coins_after);

            for (int slot = 0; slot < 10; slot++) {
                if (mp_enemy_merge_mask[slot])
                    for (int i = 0; i < 0x10; i++)
                        wr8((uint16_t)(0xD100 + slot * 0x10 + i), mp_enemy_merge[slot * 0x10 + i]);
            }

            if (tile_changed || collision_changed || enemy_merged ||
                coins_after != coins_before ||
                memcmp(score_before, score_after, sizeof score_before) != 0) {
                if (gb_state_save(mp_state, GB_STATE_BYTES)) return -1;
            }
        }

        /*
         * Capture the frame ONLY after restoring and merging into the
         * authoritative Player 1 world. This prevents Luigi's private
         * temporary block/item map from becoming the visible background.
         */
        mp_capture_frame(frame, 0);
        memcpy(frame->mario_oam2, mp_p2_last_oam, sizeof frame->mario_oam2);
        memcpy(frame->p2_projectile_oam, mp_p2_state.projectile_oam, sizeof frame->p2_projectile_oam);
        memcpy(frame->p2_effect_oam, mp_p2_state.effect_oam, sizeof frame->p2_effect_oam);
        frame->p2_sound_event = (uint8_t)((coins_after != coins_before) || enemy_sound_event);

        /*
         * Fatal small-Mario deaths are the original SML1 states 3 -> 4 -> 1.
         * Let the original code draw those four falling OAM objects. When the
         * life is finally consumed, respawn from the authoritative P1 state.
         */
        if (life_delta < 0) {
            frame->p1_lives = rd8(0xDA15);
            frame->p2_lives = mp_p2_lives;
            frame->p2_game_state = p2_game_state;
            memset(frame->mario_oam2, 0, sizeof frame->mario_oam2);
            memset(frame->p2_projectile_oam, 0, sizeof frame->p2_projectile_oam);
            memset(frame->luigi_mask, 0, sizeof frame->luigi_mask);
            frame->p2_visible = 0;
            if (mp_p2_lives > 0) {
                mp_respawn_p2_from_p1(mp_current_player);
            }
            return 0;
        }

        frame->p2_game_state = p2_game_state;
        frame->p2_lives = mp_p2_lives;
        /* Keep Luigi's own OAM visible through the game's death animation. */
        frame->p2_visible = (uint8_t)(mp_p2_spawned && mp_p2_lives > 0);
        frame->p2_blink_hidden = (uint8_t)(mp_p2_invulnerability_frames > 0 &&
                                           ((frame_count / 4) & 1));
        return 0;
    }

    /* Service deferred P2 sound requests when Mario's SFX register becomes free. */
    if (mp_pending_square_sfx && rd8(0xDFE0) == 0) {
        wr8(0xDFE0, mp_pending_square_sfx);
        mp_pending_square_sfx = 0;
    }
    if (mp_pending_noise_sfx && rd8(0xDFF8) == 0) {
        wr8(0xDFF8, mp_pending_noise_sfx);
        mp_pending_noise_sfx = 0;
    }

    /*
     * Player 1 is the camera leader. When Luigi is too close to the left edge,
     * block a rightward camera advance until Luigi has moved farther right.
     */
    int block_camera_advance = mp_p2_spawned && mp_p2_lives > 0 && mp_p2_state.game_state == 0 &&
        mp_p2_state.mario[2] < 0x20;
    for (int player = 2; player < mp_player_count; player++) {
        const MpExtraPlayer *extra = &mp_extra_players[player - 2];
        if (extra->spawned && extra->lives > 0 && extra->state.game_state == 0 &&
            extra->state.mario[2] < 0x20) block_camera_advance = 1;
    }
    if (block_camera_advance && (dpad & 0x01) && rd8(0xC202) >= 0x50)
        dpad &= (uint8_t)~0x01;

    uint8_t p1_scroll_before = rd8(0xFFA4);
    mp_buttons = buttons;
    mp_dpad = dpad;
    gb_set_input(mp_buttons, mp_dpad);
    mp_frame_out = frame;
    mp_audio_out = audio;
    mp_audio_max = audio_max;
    mp_audio_n = 0;
    mp_vblank_waiting = 0;
    gb_mp_vblank_watch = 0;
    mp_active = 1;
    int mp_run_status = setjmp(stop_jmp);
    if (mp_run_status == 0) run_core(0);
    if (mp_run_status == 4) {
        mp_active = 0; mp_frame_out = NULL; mp_audio_out = NULL;
        mp_audio_max = mp_audio_n = 0;
        return -1;
    }
    mp_active = 0;
    mp_audio_n = (audio && audio_max > 0) ? apu_drain(audio, audio_max) : 0;
    if (gb_state_save(mp_state, GB_STATE_BYTES)) return -1;
    mp_capture_frame(frame, 0);
    if (mp_p2_invulnerability_frames > 0) mp_p2_invulnerability_frames--;

    /*
     * Mirror every life awarded to Mario onto Luigi. We only mirror upward
     * changes; losing a Mario life does not silently remove one from Luigi.
     */
    uint8_t p1_lives_now = (uint8_t)mp_bcd_to_int(rd8(0xDA15));
    if (p1_lives_now > mp_p1_lives_seen) {
        int gained = (int)p1_lives_now - (int)mp_p1_lives_seen;
        int each = gained;
        while (each-- > 0 && mp_p2_lives < 99) mp_p2_lives++;
        for (int player = 2; player < mp_player_count; player++) {
            MpExtraPlayer *extra = &mp_extra_players[player - 2];
            int more = gained;
            while (more-- > 0 && extra->lives < 99) extra->lives++;
        }
    }
    mp_p1_lives_seen = p1_lives_now;
    frame->p1_lives = rd8(0xDA15);
    frame->p2_lives = mp_p2_lives;

    /*
     * Luigi's C202 is a screen coordinate in the previous shared camera.
     * When Mario advances the authoritative camera, shift Luigi's saved
     * screen coordinate by the same amount before Luigi's next physics frame.
     */
    int p1_camera_dx = mp_scroll_delta(frame->scroll_x, p1_scroll_before);
    if (p1_camera_dx) {
        if (mp_p2_spawned && mp_p2_lives > 0) {
            int x = (int)mp_p2_state.mario[2] - p1_camera_dx;
            if (x < 0) x += 256;
            if (x > 255) x -= 256;
            mp_p2_state.mario[2] = (uint8_t)x;
            mp_oam_offset_x(mp_p2_state.mario_oam, -p1_camera_dx);
            mp_oam_offset_x(mp_p2_last_oam, -p1_camera_dx);
        }
        for (int player = 2; player < mp_player_count; player++) {
            MpExtraPlayer *extra = &mp_extra_players[player - 2];
            if (!extra->spawned || extra->lives == 0) continue;
            int x = (int)extra->state.mario[2] - p1_camera_dx;
            if (x < 0) x += 256;
            if (x > 255) x -= 256;
            extra->state.mario[2] = (uint8_t)x;
            mp_oam_offset_x(extra->state.mario_oam, -p1_camera_dx);
            mp_oam_offset_x(extra->last_oam, -p1_camera_dx);
        }
    }
    frame->p1_lives = rd8(0xDA15);
    frame->p2_lives = mp_p2_lives;
    frame->p2_game_state = mp_p2_state.game_state;
    frame->p2_visible = (uint8_t)(mp_p2_spawned && mp_p2_lives > 0);
    frame->p2_blink_hidden = (uint8_t)(mp_p2_invulnerability_frames > 0 &&
                                       ((frame_count / 4) & 1));

    /*
     * Spawn P2 only once SML1 reaches normal gameplay. This guarantees that
     * P2 starts from the game's initialized Mario state rather than reset RAM.
     */
    if (!mp_p2_spawned && rd8(0xFFB3) == 0) {
        if (mp_p2_lives == 0) {
            mp_p2_lives = (uint8_t)mp_bcd_to_int(rd8(0xDA15));
            if (mp_p2_lives == 0) mp_p2_lives = 1;
        }
        mp_player_save(&mp_p2_state);
        mp_p2_state.game_state = 0;
        mp_p2_state.timer = 0;
        mp_p2_state.death_anim_counter = 0;
        mp_p2_state.invincibility = 0;
        mp_p2_state.superball_ttl = 0;
        memset(mp_p2_state.projectile_status, 0, sizeof mp_p2_state.projectile_status);
        memset(mp_p2_state.projectile_oam, 0, sizeof mp_p2_state.projectile_oam);
        memset(mp_p2_state.effect_oam, 0, sizeof mp_p2_state.effect_oam);
        mp_p2_state.super_status = 0;
        mp_p2_state.superball = 0;
        mp_p2_state.joy_held = 0;
        mp_p2_state.joy_pressed = 0;

        int p1x = rd8(0xC202);
        int x = p1x;
        if (x <= 0x70) x += 24;
        else if (x >= 0x30) x -= 24;
        mp_oam_offset_x(mp_p2_state.mario_oam, x - p1x);
        mp_p2_state.mario[2] = (uint8_t)x;
        memcpy(mp_p2_last_oam, mp_p2_state.mario_oam, sizeof mp_p2_last_oam);
        mp_p2_spawned = 1;
        frame->p2_visible = 1;
        frame->p2_lives = mp_p2_lives;
    }

    if (rd8(0xFFB3) == 0) {
        for (int player = 2; player < mp_player_count; player++)
            if (!mp_extra_players[player - 2].spawned) mp_spawn_extra_from_p1(player);
    }
    mp_capture_mp_player_data(frame);
    return mp_audio_n;
}


int emu_mp_step(int player, uint8_t buttons, uint8_t dpad, Frame *frame, int16_t *audio, int audio_max)
{
    if (!mp_ready || !frame || player < 0 || player >= mp_player_count || player >= MAX_MP_PLAYERS)
        return -1;
    mp_current_player = player;
    int result;
    if (mp_game == GAME_SML2) {
        result = emu_mp_step_sml2(player, buttons, dpad, frame, audio, audio_max);
    } else if (player <= 1) {
        result = emu_mp_step_internal(player, buttons, dpad, frame, audio, audio_max);
    } else {
        MpExtraPlayer saved;
        mp_working_to_extra(&saved);
        mp_extra_to_working(&mp_extra_players[player - 2]);
        result = emu_mp_step_internal(1, buttons, dpad, frame, NULL, 0);
        mp_working_to_extra(&mp_extra_players[player - 2]);
        mp_extra_to_working(&saved);
    }
    if (result >= 0) {
        if (mp_game == GAME_SML2) {
            mp_sml2_capture_frame(frame);
            frame->mp_player_sfx_events[player] = frame->p2_sfx_events;
        } else {
            mp_capture_mp_player_data(frame);
            frame->mp_player_sfx_events[player] = frame->p2_sfx_events;
            frame->mp_player_sound_event[player] = frame->p2_sound_event;
        }
    }
    mp_current_player = 0;
    return result;
}

void emu_mp_request_respawn(int player)
{
    if (!mp_ready || player < 1 || player >= mp_player_count) return;
    if (mp_game == GAME_SML2) {
        if (mp_sml2_initialized) mp_sml2_players[player].respawn_requested = 1;
        return;
    }
    if (player == 1) {
        if (mp_p2_spawned) mp_p2_respawn_requested = 1;
        return;
    }
    MpExtraPlayer *extra = &mp_extra_players[player - 2];
    if (extra->spawned) extra->respawn_requested = 1;
}

void emu_mp_end(void)
{
    if (mp_ready && save_path[0]) cart_write_save(save_path);
    mp_active = 0;
    mp_game = GAME_SML;
    mp_sml2_initialized = 0;
    mp_sml2_tile_patches_clear();
    memset(mp_sml2_players, 0, sizeof mp_sml2_players);
    memset(mp_sml2_render_oam, 0, sizeof mp_sml2_render_oam);
    memset(mp_sml2_render_oam_count, 0, sizeof mp_sml2_render_oam_count);
    memset(mp_sml2_render_tiles_valid, 0, sizeof mp_sml2_render_tiles_valid);
    mp_ready = 0;
    mp_player_count = 2;
    mp_current_player = 0;
    memset(mp_extra_players, 0, sizeof mp_extra_players);
    mp_p2_spawned = 0;
    mp_p2_lives = 0;
    mp_p2_respawn_requested = 0;
    mp_p2_invulnerability_frames = 0;
    mp_pending_square_sfx = mp_pending_noise_sfx = 0;
    mp_vblank_life_event = 0;
    mp_vblank_collision = 0;
    mp_vblank_collision_addr = 0;
    mp_vblank_collision_before = 0;
    mp_vblank_square_sfx = 0;
    mp_vblank_noise_sfx = 0;
    mp_vblank_floaty_control = 0;
    mp_vblank_floaty_x = 0;
    mp_vblank_floaty_y = 0;
    mp_pending_block = 0;
    mp_vblank_waiting = 0;
    gb_mp_vblank_watch = 0;
    memset(mp_p2_last_oam, 0, sizeof mp_p2_last_oam);
    mp_p1_lives_seen = 0;
    rewind_free();
    mp_frame_out = NULL;
    mp_audio_out = NULL;
    mp_audio_max = mp_audio_n = 0;
}

void gb_mp_vblank_done(void)
{
    if (!mp_active) return;
    gb_mp_vblank_watch = 0;
    mp_vblank_waiting = 0;
    longjmp(stop_jmp, 3);
}

static void dev_hook(uint8_t *b, uint8_t *d)
{
    if (emu_dev.hash || hash_log) {
        uint64_t h = 1469598103934665603ull;
        for (int y = 0; y < GB_H; y++)
            for (int x = 0; x < GB_W; x++) { h ^= ppu_shade[y][x + ppu_xoff]; h *= 1099511628211ull; }
        chain = (chain ^ h) * 1099511628211ull;
        if (hash_log) fprintf(hash_log, "%d %016llx %04X\n", frame_count, (unsigned long long)h, cpu.pc);
    }
    if (emu_dev.region_on) {
        static uint64_t last; uint64_t h = 1469598103934665603ull;
        for (int y = emu_dev.region[1]; y < emu_dev.region[3]; y++)
            for (int x = emu_dev.region[0]; x < emu_dev.region[2]; x++) { h ^= ppu_shade[y][x + ppu_xoff]; h *= 1099511628211ull; }
        if (h != last) { fprintf(stderr, "[region] frame %d changed\n", frame_count); last = h; }
    }
    if (getenv("GBL_POPIN")) {                 /* dev: sprites that appear out of nowhere inside the picture */
        static uint8_t prevl[GB_H][GB_WMAX];
        static int tot, shown;
        int W = ppu_w;
        for (int y = 0; y < GB_H; y++)
            for (int x = 12; x < W - 12; x++) {
                if (!ppu_layer[y][x]) continue;
                int near = 0;
                for (int dy = -6; dy <= 6 && !near; dy++)
                    for (int dx = -6; dx <= 6; dx++) {
                        int yy = y + dy, xx = x + dx;
                        if (yy >= 0 && yy < GB_H && xx >= 0 && xx < W && prevl[yy][xx]) { near = 1; break; }
                    }
                if (!near && x >= atoi(getenv("GBL_POPIN"))) { tot++; if (shown < 3000) { shown++; fprintf(stderr, "[popin] frame %d x %d y %d (W %d)\n", frame_count, x, y, W); } }
            }
        memcpy(prevl, ppu_layer, sizeof prevl);
        if (emu_dev.max_frames >= 0 && frame_count + 1 >= emu_dev.max_frames) fprintf(stderr, "[popin] total %d\n", tot);
    }
    for (int i = 0; i < n_pokes; i++) if (pokes[i].frame == frame_count) wr8(pokes[i].addr, pokes[i].val);
    if (emu_dev.max_frames >= 0 && frame_count >= emu_dev.max_frames) longjmp(stop_jmp, 2);
    if (emu_dev.fuzz) {
        if (frame_count >= fuzz_next) {
            fuzz_next = frame_count + 3 + (int)(xr() % 25);
            uint32_t q = xr() % 16;
            fz_b = fz_d = 0;
            if (q < 5) fz_b = (uint8_t)(1u << (xr() & 3));
            else if (q < 11) fz_d = (uint8_t)(1u << (xr() & 3));
            else if (q < 13) fz_b = 8;
        }
        *b |= fz_b; *d |= fz_d;
    }
    if (n_script) {
        uint8_t m = 0;
        for (int i = 0; i < n_script; i++) if (script[i].frame <= frame_count) m = script[i].mask;
        *b |= m & 0x0F; *d |= m >> 4;
    }
}

static void pace_without_audio(void)
{
    Uint64 f = SDL_GetPerformanceFrequency();
    Uint64 per = (Uint64)((double)f * CYCLES_PER_FRAME / CPU_HZ);
    Uint64 now = SDL_GetPerformanceCounter();
    if (!pace_next || now > pace_next + per * 4) pace_next = now;
    pace_next += per;
    while ((now = SDL_GetPerformanceCounter()) < pace_next) {
        Uint64 left = pace_next - now;
        if (left * 1000 / f > 2) SDL_Delay(1);
    }
}

static size_t emu_state_blob_size(void)
{
    return sizeof(EmuStateHeader) + gb_state_data_size() + ((mp_ready || mp_active) ? sizeof(MpStateExtra) : 0);
}

static int emu_state_save_blob(void *dst, size_t n)
{
    if (!dst || n < emu_state_blob_size()) return -1;
    int mp_context = mp_ready || mp_active;
    EmuStateHeader h = {EMU_STATE_MAGIC, EMU_STATE_VERSION, mp_context ? EMU_STATE_FLAG_MP : 0u,
                        (uint32_t)rom_loaded_game(), (uint32_t)gb_state_data_size(),
                        mp_context ? (uint32_t)sizeof(MpStateExtra) : 0u};
    memcpy(dst, &h, sizeof h);
    if (gb_state_save((uint8_t *)dst + sizeof h, n - sizeof h)) return -1;
    if (mp_context) {
        MpStateExtra x;
        memset(&x, 0, sizeof x);
        x.p2 = mp_p2_state;
        x.p2_lives = mp_p2_lives;
        x.p2_spawned = (uint8_t)mp_p2_spawned;
        x.p2_invulnerability_frames = mp_p2_invulnerability_frames;
        memcpy(x.p2_last_oam, mp_p2_last_oam, sizeof x.p2_last_oam);
        x.p2_a_was_down = mp_p2_a_was_down;
        memcpy(x.extra, mp_extra_players, sizeof x.extra);
        x.pending_square_sfx = mp_pending_square_sfx;
        x.pending_noise_sfx = mp_pending_noise_sfx;
        memcpy(x.sml2, mp_sml2_players, sizeof x.sml2);
        x.sml2_initialized = (uint8_t)mp_sml2_initialized;
        memcpy((uint8_t *)dst + sizeof h + h.core_bytes, &x, sizeof x);
    }
    return 0;
}

static int emu_state_load_blob(const void *src, size_t n)
{
    if (!src || n < sizeof(EmuStateHeader)) return -1;
    EmuStateHeader h;
    memcpy(&h, src, sizeof h);
    if (h.magic != EMU_STATE_MAGIC || (h.version != EMU_STATE_VERSION && h.version != 8u && h.version != 7u && h.version != 6u && h.version != 5u)) return -1;
    if ((int)h.game_id != rom_loaded_game()) return -1;
    int mp_context = mp_ready || mp_active;
    if (((h.flags & EMU_STATE_FLAG_MP) != 0) != (mp_context != 0)) return -1;
    if (h.core_bytes != gb_state_data_size() || h.extra_bytes > sizeof(MpStateExtra)) return -1;
    if (sizeof h + h.core_bytes + h.extra_bytes > n) return -1;
    if (mp_context) {
        size_t expected = h.version == 5u ? sizeof(MpStateExtraV5) :
                           (h.version == 6u ? sizeof(MpStateExtraV6) :
                            (h.version == 7u ? sizeof(MpStateExtraV7) :
                             (h.version == 8u ? sizeof(MpStateExtraV8) : sizeof(MpStateExtra))));
        if (h.extra_bytes != expected) return -1;
    } else if (h.extra_bytes != 0) return -1;
    if (gb_state_load((const uint8_t *)src + sizeof h, h.core_bytes)) return -1;
    if (mp_context) {
        if (h.version == 5u) {
            MpStateExtraV5 old;
            memcpy(&old, (const uint8_t *)src + sizeof h + h.core_bytes, sizeof old);
            mp_p2_state = old.p2;
            mp_p2_lives = old.p2_lives;
            mp_p2_spawned = old.p2_spawned != 0;
            mp_p2_invulnerability_frames = old.p2_invulnerability_frames;
            memcpy(mp_p2_last_oam, old.p2.mario_oam, sizeof mp_p2_last_oam);
            mp_p2_a_was_down = 0;
            mp_pending_square_sfx = old.pending_square_sfx;
            mp_pending_noise_sfx = old.pending_noise_sfx;
            memset(mp_extra_players, 0, sizeof mp_extra_players);
            memset(mp_sml2_players, 0, sizeof mp_sml2_players);
    memset(mp_sml2_render_oam, 0, sizeof mp_sml2_render_oam);
    memset(mp_sml2_render_oam_count, 0, sizeof mp_sml2_render_oam_count);
    memset(mp_sml2_render_tiles_valid, 0, sizeof mp_sml2_render_tiles_valid);
            mp_sml2_initialized = 0;
        } else if (h.version == 6u) {
            MpStateExtraV6 old;
            memcpy(&old, (const uint8_t *)src + sizeof h + h.core_bytes, sizeof old);
            mp_p2_state = old.p2;
            mp_p2_lives = old.p2_lives;
            mp_p2_spawned = old.p2_spawned != 0;
            mp_p2_invulnerability_frames = old.p2_invulnerability_frames;
            memcpy(mp_p2_last_oam, old.p2_last_oam, sizeof mp_p2_last_oam);
            mp_p2_a_was_down = old.p2_a_was_down;
            memcpy(mp_extra_players, old.extra, sizeof mp_extra_players);
            mp_pending_square_sfx = old.pending_square_sfx;
            mp_pending_noise_sfx = old.pending_noise_sfx;
            memset(mp_sml2_players, 0, sizeof mp_sml2_players);
    memset(mp_sml2_render_oam, 0, sizeof mp_sml2_render_oam);
    memset(mp_sml2_render_oam_count, 0, sizeof mp_sml2_render_oam_count);
    memset(mp_sml2_render_tiles_valid, 0, sizeof mp_sml2_render_tiles_valid);
            mp_sml2_initialized = 0;
        } else if (h.version == 7u) {
            MpStateExtraV7 old;
            memcpy(&old, (const uint8_t *)src + sizeof h + h.core_bytes, sizeof old);
            mp_p2_state = old.p2;
            mp_p2_lives = old.p2_lives;
            mp_p2_spawned = old.p2_spawned != 0;
            mp_p2_invulnerability_frames = old.p2_invulnerability_frames;
            memcpy(mp_p2_last_oam, old.p2_last_oam, sizeof mp_p2_last_oam);
            mp_p2_a_was_down = old.p2_a_was_down;
            memcpy(mp_extra_players, old.extra, sizeof mp_extra_players);
            memset(mp_sml2_players, 0, sizeof mp_sml2_players);
    memset(mp_sml2_render_oam, 0, sizeof mp_sml2_render_oam);
    memset(mp_sml2_render_oam_count, 0, sizeof mp_sml2_render_oam_count);
    memset(mp_sml2_render_tiles_valid, 0, sizeof mp_sml2_render_tiles_valid);
            for (int i = 0; i < MAX_MP_PLAYERS; i++) {
                memcpy(mp_sml2_players[i].ram, old.sml2[i].ram, sizeof old.sml2[i].ram);
                mp_sml2_players[i].keys_held = old.sml2[i].keys_held;
                mp_sml2_players[i].keys_pressed = old.sml2[i].keys_pressed;
                mp_sml2_players[i].h_c0 = old.sml2[i].h_c0;
                mp_sml2_players[i].h_c1 = old.sml2[i].h_c1;
                mp_sml2_players[i].h_c2 = old.sml2[i].h_c2;
                mp_sml2_players[i].h_c3 = old.sml2[i].h_c3;
                mp_sml2_players[i].h_c4 = old.sml2[i].h_c4;
                mp_sml2_players[i].h_c5 = old.sml2[i].h_c5;
                mp_sml2_players[i].h_c6 = old.sml2[i].h_c6;
                mp_sml2_players[i].h_c7 = old.sml2[i].h_c7;
                memcpy(mp_sml2_players[i].oam, old.sml2[i].oam, sizeof old.sml2[i].oam);
                mp_sml2_players[i].invulnerability_frames = old.sml2[i].invulnerability_frames;
                mp_sml2_players[i].lives = old.sml2[i].lives;
                mp_sml2_players[i].spawned = old.sml2[i].spawned;
                mp_sml2_players[i].respawn_requested = old.sml2[i].respawn_requested;
                mp_sml2_players[i].previous_a = old.sml2[i].previous_a;
                mp_sml2_players[i].sfx_events = old.sml2[i].sfx_events;
            }
            mp_sml2_initialized = old.sml2_initialized != 0;
            mp_pending_square_sfx = old.pending_square_sfx;
            mp_pending_noise_sfx = old.pending_noise_sfx;
        } else if (h.version == 8u) {
            MpStateExtraV8 old;
            memcpy(&old, (const uint8_t *)src + sizeof h + h.core_bytes, sizeof old);
            mp_p2_state = old.p2;
            mp_p2_lives = old.p2_lives;
            mp_p2_spawned = old.p2_spawned != 0;
            mp_p2_invulnerability_frames = old.p2_invulnerability_frames;
            memcpy(mp_p2_last_oam, old.p2_last_oam, sizeof mp_p2_last_oam);
            mp_p2_a_was_down = old.p2_a_was_down;
            memcpy(mp_extra_players, old.extra, sizeof mp_extra_players);
            memset(mp_sml2_players, 0, sizeof mp_sml2_players);
    memset(mp_sml2_render_oam, 0, sizeof mp_sml2_render_oam);
    memset(mp_sml2_render_oam_count, 0, sizeof mp_sml2_render_oam_count);
    memset(mp_sml2_render_tiles_valid, 0, sizeof mp_sml2_render_tiles_valid);
            for (int i = 0; i < MAX_MP_PLAYERS; i++) {
                memcpy(mp_sml2_players[i].ram, old.sml2[i].ram, sizeof old.sml2[i].ram);
                mp_sml2_players[i].keys_held = old.sml2[i].keys_held;
                mp_sml2_players[i].keys_pressed = old.sml2[i].keys_pressed;
                mp_sml2_players[i].h_c0 = old.sml2[i].h_c0;
                mp_sml2_players[i].h_c1 = old.sml2[i].h_c1;
                mp_sml2_players[i].h_c2 = old.sml2[i].h_c2;
                mp_sml2_players[i].h_c3 = old.sml2[i].h_c3;
                mp_sml2_players[i].h_c4 = old.sml2[i].h_c4;
                mp_sml2_players[i].h_c5 = old.sml2[i].h_c5;
                mp_sml2_players[i].h_c6 = old.sml2[i].h_c6;
                mp_sml2_players[i].h_c7 = old.sml2[i].h_c7;
                memcpy(mp_sml2_players[i].oam, old.sml2[i].oam, sizeof old.sml2[i].oam);
                mp_sml2_players[i].invulnerability_frames = old.sml2[i].invulnerability_frames;
                mp_sml2_players[i].lives = old.sml2[i].lives;
                mp_sml2_players[i].spawned = old.sml2[i].spawned;
                mp_sml2_players[i].respawn_requested = old.sml2[i].respawn_requested;
                mp_sml2_players[i].previous_a = old.sml2[i].previous_a;
                mp_sml2_players[i].sfx_events = old.sml2[i].sfx_events;
            }
            mp_sml2_initialized = old.sml2_initialized != 0;
            mp_pending_square_sfx = old.pending_square_sfx;
            mp_pending_noise_sfx = old.pending_noise_sfx;
        } else {
            MpStateExtra x;
            memcpy(&x, (const uint8_t *)src + sizeof h + h.core_bytes, sizeof x);
            mp_p2_state = x.p2;
            mp_p2_lives = x.p2_lives;
            mp_p2_spawned = x.p2_spawned != 0;
            mp_p2_invulnerability_frames = x.p2_invulnerability_frames;
            memcpy(mp_p2_last_oam, x.p2_last_oam, sizeof mp_p2_last_oam);
            mp_p2_a_was_down = x.p2_a_was_down;
            memcpy(mp_extra_players, x.extra, sizeof mp_extra_players);
            memcpy(mp_sml2_players, x.sml2, sizeof mp_sml2_players);
            mp_sml2_initialized = x.sml2_initialized != 0;
            mp_pending_square_sfx = x.pending_square_sfx;
            mp_pending_noise_sfx = x.pending_noise_sfx;
        }
        mp_p2_respawn_requested = 0;
        for (int player = 0; player < MAX_MP_PLAYERS - 2; player++)
            mp_extra_players[player].respawn_requested = 0;
        mp_p1_lives_seen = (uint8_t)mp_bcd_to_int(rd8(0xDA15));
        mp_pending_block = 0;
        mp_vblank_waiting = 0;
        gb_mp_vblank_watch = 0;
        if (gb_state_save(mp_state, GB_STATE_BYTES)) return -1;
    }
    return 0;
}

static void rewind_free(void)
{
    if (rewind_sync_mx) SDL_LockMutex(rewind_sync_mx);
    rewind_step_request = 0;
    rewind_step_done = 1;
    if (rewind_sync_cv) SDL_CondBroadcast(rewind_sync_cv);
    if (rewind_sync_mx) SDL_UnlockMutex(rewind_sync_mx);
    free(rewind_data);
    rewind_data = NULL;
    rewind_stride = 0;
    rewind_head = -1;
    rewind_count = 0;
    rewind_oldest = rewind_cursor = rewind_last_loaded = -1;
    rewind_capture_skip = 0;
    rewind_mode = rewind_pending = 0;
}

static void rewind_init(void)
{
    rewind_free();
    if (!rewind_sync_mx) rewind_sync_mx = SDL_CreateMutex();
    if (!rewind_sync_cv) rewind_sync_cv = SDL_CreateCond();
    rewind_stride = emu_state_blob_size();
    rewind_data = (uint8_t *)malloc((size_t)REWIND_SLOTS * rewind_stride);
    if (!rewind_data) { rewind_stride = 0; return; }
    memset(rewind_data, 0, (size_t)REWIND_SLOTS * rewind_stride);
    rewind_head = -1;
    rewind_count = 0;
    rewind_capture_skip = 0;
}

static int state_write_file(const char *path, const void *data, size_t n)
{
    if (!path || !path[0] || !data || !n) return -1;
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    size_t wrote = fwrite(data, 1, n, f);
    if (fclose(f) != 0 || wrote != n) { remove(tmp); return -1; }
    remove(path);
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}

static int state_read_file(const char *path, void **data_out, size_t *n_out)
{
    if (!path || !path[0] || !data_out || !n_out) return -1;
    *data_out = NULL; *n_out = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    void *data = malloc((size_t)sz);
    if (!data) { fclose(f); return -1; }
    if (fread(data, 1, (size_t)sz, f) != (size_t)sz || fclose(f) != 0) {
        free(data); return -1;
    }
    *data_out = data;
    *n_out = (size_t)sz;
    return 0;
}

int emu_state_save_file(const char *path, int resume_after)
{
    int was_running = thr != NULL;
    if (was_running) emu_stop();

    size_t n = emu_state_blob_size();
    void *data = malloc(n);
    int ok = data && emu_state_save_blob(data, n) == 0 && state_write_file(path, data, n) == 0;
    free(data);

    if (was_running && resume_after) {
        if (emu_start_internal(force_interp_flag, 0) != 0) ok = 0;
    }
    return ok ? 0 : -1;
}

int emu_state_load_file(const char *path, int resume_after)
{
    int was_running = thr != NULL;
    if (was_running) emu_stop();

    void *data = NULL;
    size_t n = 0;
    int ok = state_read_file(path, &data, &n) == 0 &&
             emu_state_load_blob(data, n) == 0;
    free(data);

    if (ok) {
        rewind_init();
    } else if (was_running) {
        /* Re-start the game exactly where it was before the failed load. */
        ok = 0;
    }

    if (ok) {
        audio_game_flush();
        events_state_reset();
    }
    if (was_running && resume_after) {
        if (emu_start_internal(force_interp_flag, 0) != 0) ok = 0;
    }
    return ok ? 0 : -1;
}

static void rewind_capture_if_due(void)
{
    if (!rewind_data || !rewind_stride || rewind_mode) return;
    if (++rewind_capture_skip < REWIND_CAPTURE_INTERVAL) return;
    rewind_capture_skip = 0;
    int idx = (rewind_head + 1) % REWIND_SLOTS;
    if (emu_state_save_blob(rewind_data + (size_t)idx * rewind_stride, rewind_stride)) return;
    rewind_head = idx;
    if (rewind_count < REWIND_SLOTS) rewind_count++;
    rewind_oldest = (rewind_head - rewind_count + 1 + REWIND_SLOTS) % REWIND_SLOTS;
}

static int rewind_load_previous(void)
{
    if (!rewind_data || rewind_count < 2 || rewind_cursor < 0) return -1;
    size_t n = emu_state_blob_size();
    if (emu_state_load_blob(rewind_data + (size_t)rewind_cursor * rewind_stride, n)) return -1;
    rewind_last_loaded = rewind_cursor;
    int oldest = rewind_oldest;
    int distance = (rewind_cursor - oldest + REWIND_SLOTS) % REWIND_SLOTS;
    if (distance <= 0) rewind_cursor = -1;
    else rewind_cursor = (rewind_cursor - 1 + REWIND_SLOTS) % REWIND_SLOTS;
    (void)distance;
    return 0;
}

static int rewind_prime(void)
{
    if (!rewind_data || rewind_count < 2) return -1;
    rewind_oldest = (rewind_head - rewind_count + 1 + REWIND_SLOTS) % REWIND_SLOTS;
    rewind_cursor = (rewind_head - 1 + REWIND_SLOTS) % REWIND_SLOTS;
    rewind_last_loaded = -1;
    rewind_mode = 1;
    rewind_pending = 1;
    audio_game_flush();
    events_state_reset();
    audio_game_set_paused(1);
    return 0;
}

int emu_rewind_step(void)
{
    if (!rewind_mode && rewind_prime()) return -1;
    if (mp_ready || mp_active) {
        rewind_pending = 0;
        return rewind_load_previous();
    }
    if (!thr || !rewind_sync_mx || !rewind_sync_cv) return -1;

    SDL_LockMutex(rewind_sync_mx);
    rewind_pending = 0;
    rewind_step_done = 0;
    rewind_step_result = -1;
    rewind_step_request = 1;
    SDL_CondSignal(rewind_sync_cv);
    while (!rewind_step_done && !abort_flag)
        SDL_CondWait(rewind_sync_cv, rewind_sync_mx);
    int result = abort_flag ? -1 : rewind_step_result;
    SDL_UnlockMutex(rewind_sync_mx);
    return result;
}

void emu_rewind_capture(void)
{
    rewind_capture_if_due();
}

void emu_rewind_end(void)
{
    if (!rewind_mode) return;
    if (rewind_last_loaded >= 0 && rewind_oldest >= 0) {
        int new_count = (rewind_last_loaded - rewind_oldest + REWIND_SLOTS) % REWIND_SLOTS + 1;
        rewind_head = rewind_last_loaded;
        rewind_count = new_count;
    }
    rewind_mode = 0;
    rewind_pending = 0;
    audio_game_flush();
    events_state_reset();
    rewind_cursor = -1;
    rewind_last_loaded = -1;
    audio_game_set_paused(0);
}

int emu_rewind_available(void)
{
    return rewind_count >= 2;
}

void frame_hook(void)
{
    static int16_t abuf[4096 * 2];
    if (previewing) {
        if (frame_count >= preview_target) longjmp(stop_jmp, 1);
        apu_drain(abuf, 4096);
        return;
    }
    if (mp_active) {
        if (!mp_vblank_waiting) {
            mp_vblank_waiting = 1;
            if (mp_game == GAME_SML) {
                mp_vblank_life_event = rd8(0xC0A3);
                mp_vblank_collision = rd8(0xFFEE);
                mp_vblank_collision_addr = (uint16_t)(((uint16_t)rd8(0xFFEF) << 8) | rd8(0xFFF0));
                mp_vblank_square_sfx = rd8(0xDFE0);
                mp_vblank_noise_sfx = rd8(0xDFF8);
                mp_vblank_floaty_control = rd8(0xFFED);
                mp_vblank_floaty_x = rd8(0xFFEB);
                mp_vblank_floaty_y = rd8(0xFFEC);
                mp_vblank_collision_before = 0;
                if (mp_vblank_collision_addr >= 0x9800 && mp_vblank_collision_addr < 0x9C00)
                    mp_vblank_collision_before = rd8(mp_vblank_collision_addr);
            }
            gb_mp_vblank_arm();
            return;
        }
        mp_vblank_waiting = 0;
        gb_mp_vblank_watch = 0;
        mp_audio_n = 0;
        longjmp(stop_jmp, 3);
    }
    uint8_t b = (uint8_t)(input_word & 0xFF), d = (uint8_t)(input_word >> 8);
    if (!thread_mode) {                      /* headless / developer run */
        apu_drain(abuf, 4096);
        events_frame();
        dev_hook(&b, &d);
        gb_set_input(b, d);
        return;
    }
    if (abort_flag) longjmp(stop_jmp, 1);
    if (rewind_mode) {
        if (rewind_step_request) {
            int result = rewind_load_previous();
            if (result) rewind_mode = 0;
            SDL_LockMutex(rewind_sync_mx);
            rewind_step_request = 0;
            rewind_step_result = result;
            rewind_step_done = 1;
            SDL_CondBroadcast(rewind_sync_cv);
            SDL_UnlockMutex(rewind_sync_mx);
        }
        publish();
        events_frame();
        apu_drain(abuf, 4096);
        gb_set_input(0, 0);
        SDL_Delay(8);
        return;
    }
    rewind_capture_if_due();
    publish();
    events_frame();
    int n = apu_drain(abuf, 4096);
    if (!turbo) audio_game_push(abuf, n);
    gb_set_input(b, d);
    while (paused && !abort_flag) SDL_Delay(8);
    if (abort_flag) longjmp(stop_jmp, 1);
    if (!turbo) {
        if (audio_ok()) audio_game_wait(audio_game_target());
        else pace_without_audio();
    } else pace_next = 0;
    if (++save_tick >= 600 && save_path[0]) { save_tick = 0; cart_write_save(save_path); }
}

static int thread_main(void *u)
{
    (void)u;
    thread_mode = 1;
    if (setjmp(stop_jmp) == 0) run_core(force_interp_flag);
    thread_mode = 0;
    if (save_path[0]) cart_write_save(save_path);
    return 0;
}

static int emu_start_internal(int force_interp, int reset)
{
    if (thr) return 0;
    if (!fmx) fmx = SDL_CreateMutex();
    force_interp_flag = force_interp;
    core_faulted = 0; core_fault_opcode = 0; core_fault_pc = 0;
    abort_flag = 0; paused = 0; turbo = 0; save_tick = 0; pace_next = 0;
    if (reset) {
        gb_reset();
        if (save_path[0]) cart_load_save(save_path);
    }
    memset(&pub, 0, sizeof pub);
    apu_set_volume(settings.volume / 100.0f);
    audio_game_begin();
    events_begin();
    rewind_init();
    thr = SDL_CreateThread(thread_main, "emulation", NULL);
    if (!thr) { audio_game_end(); events_end(); rewind_free(); return -1; }
    return 0;
}

int emu_start(int force_interp)
{
    return emu_start_internal(force_interp, 1);
}

void emu_stop(void)
{
    if (!thr) return;
    abort_flag = 1;
    if (rewind_sync_mx) {
        SDL_LockMutex(rewind_sync_mx);
        rewind_step_done = 1;
        rewind_step_request = 0;
        if (rewind_sync_cv) SDL_CondBroadcast(rewind_sync_cv);
        SDL_UnlockMutex(rewind_sync_mx);
    }
    audio_game_abort();
    SDL_WaitThread(thr, NULL);
    thr = NULL;
    audio_game_end();
    events_end();
    rewind_free();
    if (rewind_sync_cv) { SDL_DestroyCond(rewind_sync_cv); rewind_sync_cv = NULL; }
    if (rewind_sync_mx) { SDL_DestroyMutex(rewind_sync_mx); rewind_sync_mx = NULL; }
}

void emu_preview(int frames)
{
    if (!fmx) fmx = SDL_CreateMutex();
    core_faulted = 0; core_fault_opcode = 0; core_fault_pc = 0;
    previewing = 1; preview_target = frames;
    gb_reset();
    if (setjmp(stop_jmp) == 0) run_core(0);
    previewing = 0;
}

void emu_run_blocking(int force_interp)
{
    core_faulted = 0; core_fault_opcode = 0; core_fault_pc = 0;
    thread_mode = 0;
    events_begin();
    if (setjmp(stop_jmp) == 0) run_core(force_interp);
    events_end();
}

int emu_dev_init(void)
{
    if (!fmx) fmx = SDL_CreateMutex();
    rng = emu_dev.seed * 2654435761u + 1;
    if (emu_dev.hash_log) hash_log = fopen(emu_dev.hash_log, "w");
    if (emu_dev.script) {
        FILE *f = fopen(emu_dev.script, "r");
        if (!f) { perror("script"); return 1; }
        int fr; unsigned m;
        while (n_script < 4096 && fscanf(f, "%d %x", &fr, &m) == 2) { script[n_script].frame = fr; script[n_script++].mask = (uint8_t)m; }
        fclose(f);
    }
    return 0;
}

void emu_dev_report(void)
{
    if (emu_dev.hash)
        printf("frames=%d cycles=%llu hash=%016llx pc=%04X sp=%04X af=%02X%02X bc=%02X%02X de=%02X%02X hl=%02X%02X\n",
               frame_count, (unsigned long long)total_cycles, (unsigned long long)chain, cpu.pc, cpu.sp,
               cpu.a, cpu.f, cpu.b, cpu.c, cpu.d, cpu.e, cpu.h, cpu.l);
}

void emu_dev_close(void) { if (hash_log) { fclose(hash_log); hash_log = NULL; } }
