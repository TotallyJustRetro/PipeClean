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

static int mp_active;
static uint8_t mp_buttons, mp_dpad;
static int mp_ready;
static int mp_p2_spawned;
static uint8_t mp_state[GB_STATE_BYTES];
static MpPlayerState mp_p2_state;
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
static uint8_t mp_p2_jump_before;
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
    uint8_t reserved[2];
} MpStateExtra;

#define EMU_STATE_MAGIC 0x50534353u /* "PCSS" */
#define EMU_STATE_VERSION 2u
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
static void rewind_free(void);
static void rewind_init(void);
static int emu_start_internal(int force_interp, int reset);

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

static void mp_respawn_p2_from_p1(void)
{
    mp_player_save(&mp_p2_state);
    mp_p2_state.game_state = 0;
    mp_p2_state.timer = 0;
    mp_p2_state.death_anim_counter = 0;
    mp_p2_state.invincibility = 0;
    mp_p2_state.superball_ttl = 0;
    memset(mp_p2_state.projectile_status, 0, sizeof mp_p2_state.projectile_status);
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
    int x = p1x;
    if (x <= 0x78) x += 24;
    else if (x >= 0x28) x -= 24;
    else x = 0x50;
    if (x < 0x18) x = 0x18;
    if (x > 0xC8) x = 0xC8;
    memcpy(mp_p2_state.mario_oam, &oam[0x0C], sizeof mp_p2_state.mario_oam);
    mp_oam_offset_x(mp_p2_state.mario_oam, x - p1x);
    mp_p2_state.mario[2] = (uint8_t)x;
    mp_p2_state.mario[1] = rd8(0xC201);
    memcpy(mp_p2_last_oam, mp_p2_state.mario_oam, sizeof mp_p2_last_oam);
}

static void mp_player_save(MpPlayerState *s)
{
    if (!s) return;
    for (int i = 0; i < 0x10; i++) s->mario[i] = rd8((uint16_t)(0xC200 + i));
    for (int i = 0; i < 16; i++) s->mario_oam[i] = rd8((uint16_t)(0xC00C + i));
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
static int mp_merge_tilemap_local_edits(int allow_full)
{
    (void)allow_full;
    int changed = 0;
    for (int col = 0; col < 32; col++) {
        int n = 0;
        for (int row = 0; row < 32; row++) {
            int idx = row * 32 + col;
            if (mp_tilemap_before[idx] != mp_tilemap_after[idx]) n++;
        }
        if (n == 0 || n > 4) continue;
        for (int row = 0; row < 32; row++) {
            int idx = row * 32 + col;
            uint8_t before = mp_tilemap_before[idx];
            uint8_t after = mp_tilemap_after[idx];
            if (before == after) continue;
            /* Block collisions are synchronized by their explicit VBlank event. */
            if (before >= 0x60 && before != 0xF4 && after == 0x20)
                continue;
            vram[0x1800 + idx] = after;
            changed = 1;
        }
    }
    return changed;
}

static int mp_merge_block_vblank_event(uint8_t event, uint16_t addr,
                                       uint8_t before, uint8_t after,
                                       uint8_t p2_scroll, uint8_t p1_scroll)
{
    (void)before;
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
    case 0x02:
        /*
         * This is the start of the normal SML1 bump animation. The game's
         * VBlank handler temporarily hides the block while the item emerges,
         * but the shared world must NOT permanently import that blank tile.
         */
        mp_pending_block = 1;
        mp_pending_block_idx = (uint16_t)p1idx;
        return 0;
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
    memcpy(f->bg_map, &vram[0x1800], sizeof f->bg_map);
    memcpy(f->mario_oam, &oam[0x0C], sizeof f->mario_oam);
    memset(f->mario_oam2, 0, sizeof f->mario_oam2);
    memset(f->luigi_mask, 0, sizeof f->luigi_mask);
    f->p1_lives = rd8(0xDA15);
    f->p2_lives = mp_p2_lives;
    f->p2_game_state = mp_p2_state.game_state;
    f->p2_visible = (uint8_t)(mp_p2_spawned && mp_p2_lives > 0 &&
                               (mp_p2_state.game_state == 0 ||
                                mp_p2_state.game_state == 3 ||
                                mp_p2_state.game_state == 4));
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
}

void emu_mp_frame_refresh(Frame *frame)
{
    if (!mp_ready || !frame) return;
    mp_capture_frame(frame, 0);
    if (mp_p2_spawned && mp_p2_lives > 0)
        memcpy(frame->mario_oam2, mp_p2_state.mario_oam, sizeof frame->mario_oam2);
}

int emu_mp_begin(void)
{
    if (thr || mp_ready || rom_loaded_game() != GAME_SML) return -1;
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
    mp_p2_spawned = 0;
    mp_p1_lives_seen = (uint8_t)mp_bcd_to_int(rd8(0xDA15));
    mp_p2_lives = mp_p1_lives_seen;
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
    mp_p2_jump_before = 0;
    mp_vblank_waiting = 0;
    memset(mp_p2_last_oam, 0, sizeof mp_p2_last_oam);
    gb_mp_vblank_watch = 0;

    mp_ready = 1;
    rewind_init();
    return 0;
}

int emu_mp_step(int player, uint8_t buttons, uint8_t dpad, Frame *frame, int16_t *audio, int audio_max)
{
    if (!mp_ready || (player != 0 && player != 1) || !frame) return -1;

    if (gb_state_load(mp_state, GB_STATE_BYTES)) return -1;

    if (player == 1) {
        if (!mp_p2_spawned || mp_p2_lives == 0) {
            memset(frame->mario_oam2, 0, sizeof frame->mario_oam2);
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
        int p2_world_lives_before = mp_bcd_to_int(rd8(0xDA15));
        uint8_t score_before[3];
        uint8_t coins_before = rd8(0xFFFA);
        uint8_t p1_square_sfx_before = rd8(0xDFE0);
        uint8_t p1_noise_sfx_before = rd8(0xDFF8);
        for (int i = 0; i < 3; i++) score_before[i] = rd8((uint16_t)(0xC0A0 + i));

        mp_capture_shared_world_before();
        mp_player_load(&mp_p2_state);
        mp_p2_jump_before = rd8(0xC207);
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
        uint8_t p2_square_sfx = mp_vblank_square_sfx ? mp_vblank_square_sfx : rd8(0xDFE0);
        uint8_t p2_noise_sfx = mp_vblank_noise_sfx ? mp_vblank_noise_sfx : rd8(0xDFF8);
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
        int p2_jump_event = (mp_p2_jump_before == 0 && rd8(0xC207) != 0);

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
        mp_copy_mario_oam_buffer(p2_oam_saved, dx);
        memcpy(mp_p2_last_oam, p2_oam_saved, sizeof mp_p2_last_oam);

        mp_player_save(&mp_p2_state);
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
        if (rd8(0xDFE0) == 0) {
            if (coins_after != coins_before)
                wr8(0xDFE0, 0x05); /* SFX_COIN */
            else if (enemy_sound_event)
                wr8(0xDFE0, 0x03); /* SFX_STOMP */
            else if (p1_square_sfx_before == 0 && p2_square_sfx)
                wr8(0xDFE0, p2_square_sfx);
            else if (p2_jump_event)
                wr8(0xDFE0, 0x01); /* SFX_JUMP */
        }

        int p2_block_hit = (mp_vblank_collision == 0x01 ||
                            mp_vblank_collision == 0x02 ||
                            mp_vblank_collision == 0x04);
        if (rd8(0xDFF8) == 0) {
            if (p2_block_hit)
                wr8(0xDFF8, 0x02); /* SML1 block-hit noise */
            else if (p1_noise_sfx_before == 0 && p2_noise_sfx)
                wr8(0xDFF8, p2_noise_sfx);
        }

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
                mp_vblank_collision_before,
                (mp_vblank_collision_addr >= 0x9800 && mp_vblank_collision_addr < 0x9C00)
                    ? mp_tilemap_after[mp_vblank_collision_addr - 0x9800]
                    : 0,
                p2_scroll_after,
                p1_scroll);

            if (mp_vblank_collision != 0x01 &&
                mp_vblank_collision != 0x02 &&
                mp_vblank_collision != 0x04)
                tile_changed |= mp_merge_tilemap_local_edits(dx == 0);

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
        frame->p2_sound_event = (uint8_t)((coins_after != coins_before) || enemy_sound_event);

        /*
         * Fatal small-Mario deaths are the original SML1 states 3 -> 4 -> 1.
         * Let the original code draw those four falling OAM objects. When the
         * life is finally consumed, respawn from the authoritative P1 state.
         */
        if (life_delta < 0 || p2_game_state == 0x39 || p2_game_state == 0x3A) {
            frame->p1_lives = rd8(0xDA15);
            frame->p2_lives = mp_p2_lives;
            frame->p2_game_state = p2_game_state;
            memset(frame->mario_oam2, 0, sizeof frame->mario_oam2);
            memset(frame->luigi_mask, 0, sizeof frame->luigi_mask);
            frame->p2_visible = 0;
            if (mp_p2_lives > 0) {
                mp_respawn_p2_from_p1();
            }
            return 0;
        }

        frame->p2_game_state = p2_game_state;
        frame->p2_lives = mp_p2_lives;
        frame->p2_visible = (uint8_t)(p2_state_before == 0 ||
                                      p2_game_state == 3 ||
                                      p2_game_state == 4);
        return 0;
    }

    /*
     * Player 1 is the camera leader. When Luigi is too close to the left edge,
     * block a rightward camera advance until Luigi has moved farther right.
     */
    if (mp_p2_spawned && mp_p2_lives > 0 && mp_p2_state.game_state == 0 &&
        (dpad & 0x01) && rd8(0xC202) >= 0x50 &&
        mp_p2_state.mario[2] < 0x20) {
        dpad &= (uint8_t)~0x01;
    }

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

    /*
     * Mirror every life awarded to Mario onto Luigi. We only mirror upward
     * changes; losing a Mario life does not silently remove one from Luigi.
     */
    uint8_t p1_lives_now = (uint8_t)mp_bcd_to_int(rd8(0xDA15));
    if (p1_lives_now > mp_p1_lives_seen) {
        int gained = (int)p1_lives_now - (int)mp_p1_lives_seen;
        while (gained-- > 0 && mp_p2_lives < 99) mp_p2_lives++;
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
    if (mp_p2_spawned && mp_p2_lives > 0 && p1_camera_dx) {
        int x = (int)mp_p2_state.mario[2] - p1_camera_dx;
        if (x < 0) x += 256;
        if (x > 255) x -= 256;
        mp_p2_state.mario[2] = (uint8_t)x;
        mp_oam_offset_x(mp_p2_state.mario_oam, -p1_camera_dx);
        mp_oam_offset_x(mp_p2_last_oam, -p1_camera_dx);
    }
    frame->p1_lives = rd8(0xDA15);
    frame->p2_lives = mp_p2_lives;
    frame->p2_game_state = mp_p2_state.game_state;
    frame->p2_visible = (uint8_t)(mp_p2_spawned && mp_p2_lives > 0 &&
                                  mp_p2_state.game_state == 0);

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

    return mp_audio_n;
}


void emu_mp_end(void)
{
    if (mp_ready && save_path[0]) cart_write_save(save_path);
    mp_active = 0;
    mp_ready = 0;
    mp_p2_spawned = 0;
    mp_p2_lives = 0;
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
        memcpy((uint8_t *)dst + sizeof h + h.core_bytes, &x, sizeof x);
    }
    return 0;
}

static int emu_state_load_blob(const void *src, size_t n)
{
    if (!src || n < sizeof(EmuStateHeader)) return -1;
    EmuStateHeader h;
    memcpy(&h, src, sizeof h);
    if (h.magic != EMU_STATE_MAGIC || h.version != EMU_STATE_VERSION) return -1;
    if ((int)h.game_id != rom_loaded_game()) return -1;
    int mp_context = mp_ready || mp_active;
    if (((h.flags & EMU_STATE_FLAG_MP) != 0) != (mp_context != 0)) return -1;
    if (h.core_bytes != gb_state_data_size() || h.extra_bytes > sizeof(MpStateExtra)) return -1;
    if (sizeof h + h.core_bytes + h.extra_bytes > n) return -1;
    if (gb_state_load((const uint8_t *)src + sizeof h, h.core_bytes)) return -1;
    if (mp_context) {
        if (h.extra_bytes != sizeof(MpStateExtra)) return -1;
        MpStateExtra x;
        memcpy(&x, (const uint8_t *)src + sizeof h + h.core_bytes, sizeof x);
        mp_p2_state = x.p2;
        mp_p2_lives = x.p2_lives;
        mp_p2_spawned = x.p2_spawned != 0;
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
        /*
         * PPU reaches frame_hook at the VBlank boundary, before the SML1
         * VBlank interrupt executes. Let that ISR run to completion so
         * Call_1B86, UpdateLives, DMA and the rest of the original frame are
         * processed. gb_mp_vblank_done() yields immediately after RETI.
         */
        if (!mp_vblank_waiting) {
            mp_vblank_waiting = 1;
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
