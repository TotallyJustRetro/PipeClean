#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdarg.h>
#include <ctype.h>
#include "gb.h"
#include "widescreen.h"
#include "rom.h"
#include "settings.h"
#include "cart.h"
#include "emu.h"
#include "events.h"
#include "audio.h"
#include "app.h"

static void serial_print(uint8_t c) { putchar(c); fflush(stdout); }

static void usage(const char *a0)
{
    fprintf(stderr,
        "usage: %s [game.gb]\n"
        "  (no arguments)    open the launcher\n"
        "  game.gb           start that game right away (Dr. Mario, Super Mario Land, Super Mario Land 2)\n"
        "  --hack FILE       apply a romhack (.ips/.bps/.ups or patched .gb)\n"
        "developer options (run without a window):\n"
        "  --headless        run the game without video/audio\n"
        "  --frames N        stop after N video frames\n"
        "  --interp          run everything in the interpreter instead of the recompiled code\n"
        "  --fuzz SEED       press random buttons (deterministic)\n"
        "  --script FILE     lines of '<frame> <hexmask>': A=1 B=2 Sel=4 Start=8 R=10 L=20 U=40 D=80\n"
        "  --mp-script FILE  SML2 co-op inputs: '<frame> <P1-mask> <P2-mask>' (hex masks)\n"
        "  --mp-report FILE write per-frame multiplayer diagnostics as JSONL\n"
        "  --hash            print frame-hash chain + CPU state at exit\n"
        "  --hash-log FILE   per-frame hashes\n"
        "  --dump-ppm FILE   write final frame\n"
        "  --misses FILE     write ROM addresses that ran outside the recompiled set\n"
        "  --serial          echo serial port output (test ROMs)\n"
        "  --test-status     print the result a test ROM leaves in cart RAM\n"
        "  --watch LO-HI     log writes to a RAM range (hex)\n"
        "  --dump-ram FILE   write WRAM+HRAM at exit\n"
        "  --log-apu         print every sound-channel trigger\n"
        "  --no-crc-check    run any ROM (interpreter only)\n", a0);
}

static int scan_mode, scan_counter;
static int inc1[0x10000], other[0x10000];
static int inc_first[0x10000];
static int scan_last[0x10000], scan_cnt[0x10000];
static uint8_t scan_val[0x10000];
static uint8_t scan_seen[0x10000][32];
void dev_scan_cb(uint16_t a, uint8_t o, uint8_t n)
{
    if (o == n) return;
    if (scan_counter) { if ((uint8_t)(o + 1) == n || (o == 0x09 && n == 0x10) || ((o & 0x0F) == 9 && n == o + 7)) { if (!inc1[a]) inc_first[a] = frame_count; inc1[a]++; } else other[a]++; return; }
    if (n != 0) { scan_last[a] = frame_count; scan_val[a] = n; return; }
    if (scan_last[a] && frame_count - scan_last[a] <= 3 && o == scan_val[a]) {
        scan_cnt[a]++;
        scan_seen[a][o >> 3] |= (uint8_t)(1 << (o & 7));
    }
}
void dev_watch_cb(uint16_t a, uint8_t o, uint8_t n) { if (o != n) fprintf(stderr, "[wr] frame %d %04X: %02X -> %02X  (pc %04X scx %02X) stk %04X %04X %04X\n", frame_count, a, o, n, cpu.pc, ppu_read(0x43), rd8(cpu.sp) | rd8(cpu.sp + 1) << 8, rd8(cpu.sp + 2) | rd8(cpu.sp + 3) << 8, rd8(cpu.sp + 4) | rd8(cpu.sp + 5) << 8); }

static void dump_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    static const uint8_t pal[4][3] = {{0xE0, 0xF8, 0xD0}, {0x88, 0xC0, 0x70}, {0x34, 0x68, 0x56}, {0x08, 0x18, 0x20}};
    fprintf(f, "P6\n%d %d\n255\n", ppu_w, GB_H);
    for (int y = 0; y < GB_H; y++)
        for (int x = 0; x < ppu_w; x++) fwrite(pal[ppu_shade[y][x] & 3], 1, 3, f);
    fclose(f);
}

typedef struct {
    int frame;
    uint8_t p1_mask, p2_mask;
} MpTestEvent;

#define MP_TEST_MAX_EVENTS 4096

static int load_mp_test_script(const char *path, MpTestEvent events[MP_TEST_MAX_EVENTS],
                               int *count_out)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }

    char line[512];
    int line_no = 0, count = 0, previous_frame = -1;
    while (fgets(line, sizeof line, f)) {
        line_no++;
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!*p) continue;

        int frame;
        unsigned p1, p2;
        char extra;
        int fields = sscanf(p, "%d %x %x %c", &frame, &p1, &p2, &extra);
        if (fields != 3 || frame < 0 || frame < previous_frame ||
            p1 > 0xFFu || p2 > 0xFFu || count >= MP_TEST_MAX_EVENTS) {
            fprintf(stderr, "%s:%d: expected ordered '<frame> <P1-mask> <P2-mask>' hex values\n",
                    path, line_no);
            fclose(f);
            return -1;
        }
        events[count++] = (MpTestEvent){ frame, (uint8_t)p1, (uint8_t)p2 };
        previous_frame = frame;
    }

    if (ferror(f)) {
        fprintf(stderr, "Error reading multiplayer script: %s\n", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    *count_out = count;
    return 0;
}

static uint32_t mp_test_frame_hash(const Frame *frame)
{
    uint32_t hash = 2166136261u;
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

static int run_mp_test(const char *script_path, const char *report_path, int frames)
{
    MpTestEvent events[MP_TEST_MAX_EVENTS];
    int event_count = 0;
    if (load_mp_test_script(script_path, events, &event_count)) return 1;
    if (frames <= 0) {
        fprintf(stderr, "--mp-script requires --frames N with N greater than zero\n");
        return 1;
    }
    if (emu_dev.script) {
        fprintf(stderr, "--script and --mp-script cannot be used together\n");
        return 1;
    }
    if (rom_loaded_game() != GAME_SML2) {
        fprintf(stderr, "--mp-script currently requires a recognized Super Mario Land 2 ROM\n");
        return 1;
    }

    FILE *report = !strcmp(report_path, "-") ? stdout : fopen(report_path, "w");
    if (!report) { perror(report_path); return 1; }
    Frame *frame = (Frame *)calloc(1, sizeof *frame);
    if (!frame) {
        fprintf(stderr, "Could not allocate multiplayer test frame\n");
        if (report != stdout) fclose(report);
        return 1;
    }

    /* Test mode always drives exactly two players, independent of saved UI settings. */
    settings.g[GAME_SML2].multiplayer_players = 2;
    emu_dev.max_frames = -1; /* The harness owns the logical frame limit. */
    if (emu_mp_begin()) {
        fprintf(stderr, "Could not initialize SML2 multiplayer test mode\n");
        free(frame);
        if (report != stdout) fclose(report);
        return 1;
    }

    int event_index = 0;
    uint8_t p1_mask = 0, p2_mask = 0;
    int result = 0;
    for (int logical_frame = 0; logical_frame < frames; logical_frame++) {
        while (event_index < event_count &&
               events[event_index].frame <= logical_frame) {
            p1_mask = events[event_index].p1_mask;
            p2_mask = events[event_index].p2_mask;
            event_index++;
        }

        int rc = emu_mp_step(0, p1_mask & 0x0Fu, p1_mask >> 4,
                             frame, NULL, 0);
        if (rc < 0) {
            fprintf(stderr, "Player 1 failed at scripted frame %d\n", logical_frame);
            result = 1;
            break;
        }
        rc = emu_mp_step(1, p2_mask & 0x0Fu, p2_mask >> 4,
                         frame, NULL, 0);
        if (rc < 0) {
            fprintf(stderr, "Player 2 failed at scripted frame %d\n", logical_frame);
            result = 1;
            break;
        }

        emu_mp_frame_refresh(frame);
        EmuMpTestSnapshot snap;
        if (emu_mp_test_snapshot(&snap)) {
            fprintf(stderr, "Could not read multiplayer diagnostics at frame %d\n",
                    logical_frame);
            result = 1;
            break;
        }

        if (fprintf(report,
            "{\"schema\":1,\"frame\":%d,\"p1_input\":%u,\"p2_input\":%u,"
            "\"game_mode\":%u,\"level\":%u,\"level_bank\":%u,"
            "\"camera_x\":%u,\"camera_y\":%u,"
            "\"p1_world_x\":%u,\"p1_world_y\":%u,"
            "\"p1_screen_x\":%u,\"p1_screen_y\":%u,"
            "\"p1_grounded\":%u,\"p1_in_air\":%u,\"p1_lives\":%u,"
            "\"p2_spawned\":%u,\"p2_world_x\":%u,\"p2_world_y\":%u,"
            "\"p2_screen_x\":%u,\"p2_screen_y\":%u,"
            "\"p2_grounded\":%u,\"p2_in_air\":%u,\"p2_lives\":%u,"
            "\"p2_sprite_count\":%u,\"coins_low\":%u,\"coins_high\":%u,"
            "\"mp_initialized\":%u,\"stable_frames\":%u,\"tile_patch_count\":%u,"
            "\"bg_map_hash\":\"%08X\",\"level_ram_hash\":\"%08X\","
            "\"actor_region_hash\":\"%08X\",\"render_hash\":\"%08X\"}\n",
            logical_frame, (unsigned)p1_mask, (unsigned)p2_mask,
            (unsigned)snap.game_mode, (unsigned)snap.level, (unsigned)snap.level_bank,
            (unsigned)snap.camera_x, (unsigned)snap.camera_y,
            (unsigned)snap.p1_world_x, (unsigned)snap.p1_world_y,
            (unsigned)snap.p1_screen_x, (unsigned)snap.p1_screen_y,
            (unsigned)snap.p1_grounded, (unsigned)snap.p1_in_air,
            (unsigned)snap.p1_lives, (unsigned)snap.p2_spawned,
            (unsigned)snap.p2_world_x, (unsigned)snap.p2_world_y,
            (unsigned)snap.p2_screen_x, (unsigned)snap.p2_screen_y,
            (unsigned)snap.p2_grounded, (unsigned)snap.p2_in_air,
            (unsigned)snap.p2_lives, (unsigned)frame->mp_player_sprite_count[1],
            (unsigned)snap.coins_low, (unsigned)snap.coins_high,
            (unsigned)snap.multiplayer_initialized, (unsigned)snap.stable_gameplay_frames,
            (unsigned)snap.tile_patch_count, (unsigned)snap.bg_map_hash,
            (unsigned)snap.level_ram_hash, (unsigned)snap.actor_region_hash,
            (unsigned)mp_test_frame_hash(frame)) < 0) {
            fprintf(stderr, "Could not write multiplayer report: %s\n", report_path);
            result = 1;
            break;
        }
        if ((logical_frame & 31) == 31 && fflush(report) != 0) {
            fprintf(stderr, "Could not flush multiplayer report: %s\n", report_path);
            result = 1;
            break;
        }
    }

    if (fflush(report) != 0) result = 1;
    emu_mp_end();
    free(frame);
    if (report != stdout && fclose(report) != 0) result = 1;
    if (!result)
        fprintf(stderr, "Multiplayer test complete: %d logical frames; report: %s\n",
                frames, report_path);
    return result;
}

static int widepct;
int main(int argc, char **argv)
{
    const char *path = NULL, *hack = NULL, *ppm = NULL, *misses = NULL, *ramdump = NULL, *tiles_arg = NULL;
    const char *mp_script = NULL, *mp_report_path = NULL;
    int wlo = -1, whi = 0;
    int headless = 0, interp = 0, ser = 0, crc_check = 1, status = 0, dev = 0;
    settings_load();
    emu_dev.max_frames = -1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--headless")) headless = 1;
        else if (!strcmp(a, "--frames") && i + 1 < argc) { emu_dev.max_frames = atoi(argv[++i]); headless = 1; }
        else if (!strcmp(a, "--interp")) { interp = 1; dev = 1; }
        else if (!strcmp(a, "--hack") && i + 1 < argc) hack = argv[++i];
        else if (!strcmp(a, "--fuzz") && i + 1 < argc) { emu_dev.fuzz = 1; emu_dev.seed = (uint32_t)atoi(argv[++i]); headless = 1; }
        else if (!strcmp(a, "--script") && i + 1 < argc) { emu_dev.script = argv[++i]; headless = 1; }
        else if (!strcmp(a, "--mp-script") && i + 1 < argc) { mp_script = argv[++i]; headless = 1; }
        else if (!strcmp(a, "--mp-report") && i + 1 < argc) { mp_report_path = argv[++i]; headless = 1; }
        else if (!strcmp(a, "--hash")) { emu_dev.hash = 1; headless = 1; }
        else if (!strcmp(a, "--hash-log") && i + 1 < argc) { emu_dev.hash_log = argv[++i]; headless = 1; }
        else if (!strcmp(a, "--dump-ppm") && i + 1 < argc) { ppm = argv[++i]; headless = 1; }
        else if (!strcmp(a, "--misses") && i + 1 < argc) { misses = argv[++i]; headless = 1; }
        else if (!strcmp(a, "--dump-ram") && i + 1 < argc) { ramdump = argv[++i]; headless = 1; }
        else if (!strcmp(a, "--wide") && i + 1 < argc) { widepct = atoi(argv[++i]); }
        else if (!strcmp(a, "--watch") && i + 1 < argc) { sscanf(argv[++i], "%x-%x", &wlo, &whi); headless = 1; }
        else if (!strcmp(a, "--poke") && i + 1 < argc) { unsigned ad, v; int fr; if (sscanf(argv[++i], "%x=%x@%d", &ad, &v, &fr) == 3) emu_dev_poke(fr, (uint16_t)ad, (uint8_t)v); headless = 1; }
        else if (!strcmp(a, "--mailbox-scan")) { scan_mode = 1; headless = 1; }
        else if (!strcmp(a, "--region") && i + 1 < argc) { emu_dev.region_on = sscanf(argv[++i], "%d,%d,%d,%d", &emu_dev.region[0], &emu_dev.region[1], &emu_dev.region[2], &emu_dev.region[3]) == 4; headless = 1; }
        else if (!strcmp(a, "--counter-scan")) { scan_mode = 1; scan_counter = 1; headless = 1; }
        else if (!strcmp(a, "--tiles") && i + 1 < argc) { tiles_arg = argv[++i]; headless = 1; }
        else if (!strcmp(a, "--serial")) { ser = 1; headless = 1; }
        else if (!strcmp(a, "--test-status")) { status = 1; headless = 1; }
        else if (!strcmp(a, "--log-apu")) { events_log = 1; headless = 1; }
        else if (!strcmp(a, "--no-crc-check")) { crc_check = 0; dev = 1; }
        else if (a[0] == '-') { usage(argv[0]); return 1; }
        else path = a;
    }
    (void)dev;

    if (!headless) return app_run(path, hack);      /* windowed: launcher or direct start */

    if (!path) { usage(argv[0]); return 1; }
    if (ser) gb_serial_hook(serial_print);
    RomStatus st;
    memset(&st, 0, sizeof st);
    if (!crc_check || rom_identify_file(path) < 0) {
        if (rom_load_raw(path)) { fprintf(stderr, "Cannot load %s\n", path); return 1; }
        interp = 1;
    } else {
        int g = rom_identify_file(path);
        if (rom_load(g, path, &st)) { fprintf(stderr, "%s: %s\n", path, st.msg); return 1; }
        if (hack) {
            if (rom_apply_hack(hack, &st)) { fprintf(stderr, "%s: %s\n", hack, st.msg); return 1; }
            fprintf(stderr, "%s\n", st.msg);
        }
    }
    if (widepct > 0) {
        int g = rom_loaded_game(), l = 0, r = 0;
        if (g < 0) g = rom_identify_file(path);
        wide_dims(g, widepct, &l, &r);
        if (g >= 0 && (l + r) > 0 && wide_install(g, l, r))
            ppu_set_wide(l, r, games[g].hud_lines, games[g].hud_window, games[g].wide_gate);
        else
            fprintf(stderr, "widescreen not available\n");
    }
    if (mp_script) {
        if (!mp_report_path) {
            fprintf(stderr, "--mp-script requires --mp-report FILE\n");
            return 1;
        }
        if (emu_dev_init()) return 1;
        int mp_frames = emu_dev.max_frames;
        int mp_result = run_mp_test(mp_script, mp_report_path, mp_frames);
        emu_dev_close();
        fflush(stdout);
        return mp_result;
    }

    if (emu_dev_init()) return 1;
    if (scan_mode) gb_watch_range(0xC000, 0xFFFE, dev_watch_cb);
    if (wlo >= 0) { extern void dev_watch_cb(uint16_t, uint8_t, uint8_t); gb_watch_range((uint16_t)wlo, (uint16_t)whi, dev_watch_cb); }
    gb_reset();
    emu_run_blocking(interp);
    emu_dev_report();
    if (tiles_arg) { int ty, x0, x1; if (sscanf(tiles_arg, "%d,%d,%d", &ty, &x0, &x1) == 3) { for (int x = x0; x < x1; x += 8) printf("%d ", ppu_bgtile[ty][x]); printf("\n"); } }
    if (scan_counter) { for (int a = 0xC000; a < 0xFFFF; a++) if (inc1[a] >= 2 && inc1[a] <= 80 && other[a] <= inc1[a] / 2 + 3) printf("%04X inc=%d other=%d first=%d\n", a, inc1[a], other[a], inc_first[a]); } else if (scan_mode) for (int a = 0xC000; a < 0xFFFF; a++) if (scan_cnt[a] >= 3) { int nd = 0; for (int v = 0; v < 256; v++) nd += (scan_seen[a][v >> 3] >> (v & 7)) & 1; printf("%04X n=%d distinct=%d:", a, scan_cnt[a], nd); for (int v = 0; v < 256 && nd <= 24; v++) if ((scan_seen[a][v >> 3] >> (v & 7)) & 1) printf(" %02X", v); printf("\n"); }
    if (ppm) dump_ppm(ppm);
    if (status && cart_ram_on) {
        printf("test-status=%02X sig=%s text=", cart_ram[0], (cart_ram[1] == 0xDE && cart_ram[2] == 0xB0 && cart_ram[3] == 0x61) ? "ok" : "none");
        for (int i = 4; i < 0x400 && cart_ram[i]; i++) putchar(cart_ram[i] == '\n' ? '|' : cart_ram[i]);
        putchar('\n');
    }
    if (ramdump) {
        FILE *f = fopen(ramdump, "wb");
        if (f) { for (int a = 0xC000; a < 0xE000; a++) fputc(rd8((uint16_t)a), f); for (int a = 0xFF80; a < 0xFFFF; a++) fputc(rd8((uint16_t)a), f); for (int a = 0xA000; a < 0xC000; a++) fputc(rd8((uint16_t)a), f); fclose(f); }
    }
    if (misses) gb_dump_misses(misses);
    emu_dev_close();
    fflush(stdout);
    return 0;
}