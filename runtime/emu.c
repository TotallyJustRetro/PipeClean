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
static char save_path[1100];
static int save_tick;
static int force_interp_flag;
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
void emu_input(uint8_t b, uint8_t d) { input_word = (uint32_t)b | ((uint32_t)d << 8); }
void emu_set_paused(int p) { paused = p; audio_game_set_paused(p); }
void emu_set_turbo(int t) { turbo = t; }
void emu_set_save_path(const char *p) { snprintf(save_path, sizeof save_path, "%s", p ? p : ""); }
int emu_running(void) { return thr != NULL; }

static void run_core(int force_interp)
{
    if (force_interp || rom_needs_interpreter()) run_interpreter();
    else recomp_run();
}

static void publish(void)
{
    SDL_LockMutex(fmx);
    memcpy(pub.shade, ppu_shade, sizeof pub.shade);
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
    uint8_t invincibility;   /* C0D3 */
    uint8_t superball_ttl;   /* C0A9 */
    uint8_t death_y;         /* C0DD */
    uint8_t super_status;    /* FF99 */
    uint8_t superball;       /* FFB5 */
    uint8_t timer0, timer1;  /* FFA6-FFA7 */
} MpPlayerState;

static int mp_active;
static uint8_t mp_buttons, mp_dpad;
static int mp_ready;
static uint8_t mp_state[GB_STATE_BYTES];
static MpPlayerState mp_p2_state;
static Frame *mp_frame_out;
static int16_t *mp_audio_out;
static int mp_audio_max, mp_audio_n;


static int mp_active;
static uint8_t mp_buttons, mp_dpad;
static int mp_ready;
static uint8_t mp_state[GB_STATE_BYTES];
static MpPlayerState mp_p2_state;
static Frame *mp_frame_out;
static int16_t *mp_audio_out;
static int mp_audio_max, mp_audio_n;

static void mp_player_save(MpPlayerState *s)
{
    if (!s) return;
    for (int i = 0; i < 0x10; i++) s->mario[i] = rd8((uint16_t)(0xC200 + i));
    s->invincibility = rd8(0xC0D3);
    s->superball_ttl = rd8(0xC0A9);
    s->death_y = rd8(0xC0DD);
    s->super_status = rd8(0xFF99);
    s->superball = rd8(0xFFB5);
    s->timer0 = rd8(0xFFA6);
    s->timer1 = rd8(0xFFA7);
}

static void mp_player_load(const MpPlayerState *s)
{
    if (!s) return;
    for (int i = 0; i < 0x10; i++) wr8((uint16_t)(0xC200 + i), s->mario[i]);
    wr8(0xC0D3, s->invincibility);
    wr8(0xC0A9, s->superball_ttl);
    wr8(0xC0DD, s->death_y);
    wr8(0xFF99, s->super_status);
    wr8(0xFFB5, s->superball);
    wr8(0xFFA6, s->timer0);
    wr8(0xFFA7, s->timer1);
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
    memcpy(f->layer, ppu_layer, sizeof f->layer);
    memcpy(f->bguv, ppu_bguv, sizeof f->bguv);
    memcpy(f->spruv, ppu_spruv, sizeof f->spruv);
    memcpy(f->bgtile, ppu_bgtile, sizeof f->bgtile);
    memcpy(f->sprtile, ppu_sprtile, sizeof f->sprtile);
    memcpy(f->tiles, vram, sizeof f->tiles);
    memcpy(f->bg_map, &vram[0x1800], sizeof f->bg_map);
    memcpy(f->mario_oam, &oam[0x0C], sizeof f->mario_oam);
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

int emu_mp_begin(void)
{
    if (thr || mp_ready || rom_loaded_game() != GAME_SML) return -1;
    if (!fmx) fmx = SDL_CreateMutex();
    gb_reset();
    if (save_path[0]) cart_load_save(save_path);
    if (gb_state_save(mp_state, GB_STATE_BYTES)) return -1;

    mp_player_save(&mp_p2_state);
    /* Start the second Mario a comfortable distance beside Player 1. */
    mp_p2_state.mario[2] = (uint8_t)(mp_p2_state.mario[2] + 24);

    mp_ready = 1;
    return 0;
}

int emu_mp_step(int player, uint8_t buttons, uint8_t dpad, Frame *frame, int16_t *audio, int audio_max)
{
    if (!mp_ready || (player != 0 && player != 1) || !frame) return -1;

    if (gb_state_load(mp_state, GB_STATE_BYTES)) return -1;

    if (player == 1) {
        uint8_t p1_scroll = rd8(0xFFA4);
        mp_player_load(&mp_p2_state);
        mp_buttons = buttons; mp_dpad = dpad;
        mp_frame_out = frame; mp_audio_out = audio; mp_audio_max = audio_max; mp_audio_n = 0;
        mp_active = 1;
        if (setjmp(stop_jmp) == 0) run_core(0);
        mp_active = 0;

        /* P2 may have moved SML1's camera while trying to stay centered.
         * Convert its screen X/OAM back into Player 1's camera coordinates. */
        int dx = mp_scroll_delta(rd8(0xFFA4), p1_scroll);
        if (dx) {
            uint8_t x = rd8(0xC202);
            wr8(0xC202, (uint8_t)(x + dx));
        }
        mp_capture_frame(frame, dx);
        mp_player_save(&mp_p2_state);
        gb_state_load(mp_state, GB_STATE_BYTES);
        return mp_audio_n;
    }

    mp_buttons = buttons; mp_dpad = dpad;
    mp_frame_out = frame; mp_audio_out = audio; mp_audio_max = audio_max; mp_audio_n = 0;
    mp_active = 1;
    if (setjmp(stop_jmp) == 0) run_core(0);
    mp_active = 0;
    if (gb_state_save(mp_state, GB_STATE_BYTES)) return -1;
    mp_capture_frame(frame, 0);
    return mp_audio_n;
}

void emu_mp_end(void)
{
    if (mp_ready && save_path[0]) cart_write_save(save_path);
    mp_active = 0;
    mp_ready = 0;
    mp_frame_out = NULL;
    mp_audio_out = NULL;
    mp_audio_max = mp_audio_n = 0;
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

void frame_hook(void)
{
    static int16_t abuf[4096 * 2];
    if (previewing) {
        if (frame_count >= preview_target) longjmp(stop_jmp, 1);
        apu_drain(abuf, 4096);
        return;
    }
    if (mp_active) {
        mp_capture_frame(mp_frame_out, 0);
        mp_audio_n = mp_audio_out && mp_audio_max > 0 ? apu_drain(mp_audio_out, mp_audio_max) : 0;
        gb_set_input(mp_buttons, mp_dpad);
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

int emu_start(int force_interp)
{
    if (thr) return 0;
    if (!fmx) fmx = SDL_CreateMutex();
    force_interp_flag = force_interp;
    abort_flag = 0; paused = 0; turbo = 0; save_tick = 0; pace_next = 0;
    gb_reset();
    if (save_path[0]) cart_load_save(save_path);
    memset(&pub, 0, sizeof pub);
    apu_set_volume(settings.volume / 100.0f);
    audio_game_begin();
    events_begin();
    thr = SDL_CreateThread(thread_main, "emulation", NULL);
    if (!thr) { audio_game_end(); events_end(); return -1; }
    return 0;
}

void emu_stop(void)
{
    if (!thr) return;
    abort_flag = 1;
    audio_game_abort();
    SDL_WaitThread(thr, NULL);
    thr = NULL;
    audio_game_end();
    events_end();
}

void emu_preview(int frames)
{
    if (!fmx) fmx = SDL_CreateMutex();
    previewing = 1; preview_target = frames;
    gb_reset();
    if (setjmp(stop_jmp) == 0) run_core(0);
    previewing = 0;
}

void emu_run_blocking(int force_interp)
{
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
