#include "gb.h"
#include "rom.h"
#include "cart.h"
#include "patch.h"
#include "settings.h"
#include "recomp_guard.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>

const GameDef games[N_GAMES] = {
    {"drmario",    "Dr. Mario",             "Falling-pill puzzle. Recompiled to native code.",                "DR.MARIO",         0xF0225DD0u, 1, 0xE0584C, 200, 0,  0,  0, 0, 0, 0},
    {"sml",        "Super Mario Land",      "Sarasaland platformer.",                                          "SUPER MARIOLAND",  0,           0, 0x4FA85A, 420, 32, 56, 16, 0, 0, 0},
    {"sml2",       "Super Mario Land 2",    "6 Golden Coins.",                                                 "MARIOLAND2",       0,           0, 0xE8B23A, 700, 49, 33,  0, 0, 0, 0},
    {"wario-sml3", "Wario Land: SML3",      "Wario's first adventure.",                "SUPERMARIOLAND3",  0,           0, 0xE4A144, 420,  0,  0,  0, 0, 0, 0},
    {"wario2-gb",  "Wario Land II (GB)",    "The monochrome Game Boy release.",       "WARIOLAND2",       0,           0, 0xC77B44, 420,  0,  0,  0, 0, 0, 0},
    {"wario3-gbc", "Wario Land 3 (GBC)",    "Wario's color-era adventure.",           "WARIOLAND3",       0,           0, 0x58B5C9, 500,  0,  0,  0, 0, 0, 0},
    {"wario2-gbc", "Wario Land II (GBC)",  "The Game Boy Color release.",             "CGBWARIOLAND2",    0,           0, 0x8A80D4, 420,  0,  0,  0, 0, 0, 0},
};

#define MAX_ROM (8u << 20)

static uint8_t *base_img;
static size_t base_len;
static int base_game = -1, interp_needed, hack_active;

/* Use native code only when it corresponds to this loaded cartridge. */
static int can_run_compiled_drmario(void)
{
    return base_game == GAME_DRMARIO && recomp_rom_matches_build(base_img, base_len);
}

static long read_file(const char *path, uint8_t **buf)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || (unsigned long)n > MAX_ROM) { fclose(f); return -2; }
    *buf = (uint8_t *)malloc((size_t)n + 1);
    if (!*buf) { fclose(f); return -1; }
    size_t got = fread(*buf, 1, (size_t)n, f);
    fclose(f);
    return (long)got;
}

/* Header titles are not consistently terminated on CGB cartridges: some
 * dumps put the manufacturer code immediately after the title. Compare a
 * normalized title prefix, and use the CGB flag to distinguish the two
 * Wario Land II releases, whose titles overlap. */
static void normalize_title(const char *src, char *dst, size_t cap)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == ' ' || c == '\t') continue;
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        if (c < 32 || c > 126) break;
        dst[j++] = (char)c;
    }
    dst[j] = 0;
}

int rom_identify(const uint8_t *img, size_t n)
{
    CartInfo ci;
    if (!cart_parse(img, n, &ci)) return -1;

    char header[32];
    normalize_title(ci.title, header, sizeof header);
    if (!header[0]) return -1;

    /* Wario Land II has both a monochrome GB release and a CGB release.
     * Some CGB headers retain the same visible title as the GB cartridge. */
    if (strstr(header, "WARIOLAND2"))
        return (ci.cgb_flag & 0x80) ? GAME_WARIO_LAND2_GBC : GAME_WARIO_LAND2_GB;

    int best = -1;
    size_t best_len = 0;
    for (int g = 0; g < N_GAMES; g++) {
        if (g == GAME_WARIO_LAND2_GB || g == GAME_WARIO_LAND2_GBC) continue;
        char candidate[32];
        normalize_title(games[g].title, candidate, sizeof candidate);
        size_t len = strlen(candidate);
        if (len > best_len && len && strncmp(header, candidate, len) == 0) {
            best = g;
            best_len = len;
        }
    }
    return best;
}

int rom_identify_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t head[0x150];
    size_t got = fread(head, 1, sizeof head, f);
    fclose(f);
    return got < 0x150 ? -1 : rom_identify(head, got);
}

static int check(int game, const uint8_t *buf, long n, RomStatus *st)
{
    CartInfo ci;
    if (n < 0x150 || !cart_parse(buf, (size_t)n, &ci)) {
        snprintf(st->msg, sizeof st->msg, "That doesn't look like a Game Boy ROM.");
        return 1;
    }
    if (ci.mapper < 0) { snprintf(st->msg, sizeof st->msg, "That cartridge type isn't supported."); return 1; }
    int id = rom_identify(buf, (size_t)n);
    if (id != game) {
        if (id >= 0) snprintf(st->msg, sizeof st->msg, "That is %s. Drop it on its own tab.", games[id].name);
        else snprintf(st->msg, sizeof st->msg, "That isn't the %s ROM.", games[game].name);
        return 1;
    }
    st->exact = games[game].crc && ci.crc == games[game].crc;
    if (games[game].lifted && !st->exact) {
        snprintf(st->msg, sizeof st->msg, "That isn't the right %s dump (checksum %08X, expected %08X).", games[game].name, ci.crc, games[game].crc);
        return 1;
    }
    return 0;
}

int rom_probe(int game, const char *path, RomStatus *st)
{
    memset(st, 0, sizeof *st);
    uint8_t *buf = NULL;
    long n = read_file(path, &buf);
    if (n < 0) { snprintf(st->msg, sizeof st->msg, n == -2 ? "That file is too big for a Game Boy ROM." : "Can't open that file."); return 1; }
    int r = check(game, buf, n, st);
    free(buf);
    if (r) return 1;
    st->ok = 1;
    snprintf(st->msg, sizeof st->msg, "Verified.");
    return 0;
}

static void install_base(void)
{
    cart_install(base_img, base_len);
    interp_needed = !can_run_compiled_drmario();
    hack_active = 0;
}

int rom_load(int game, const char *path, RomStatus *st)
{
    memset(st, 0, sizeof *st);
    uint8_t *buf = NULL;
    long n = read_file(path, &buf);
    if (n < 0) { snprintf(st->msg, sizeof st->msg, "Can't open that file."); return 1; }
    if (check(game, buf, n, st)) { free(buf); return 1; }
    free(base_img);
    base_img = buf; base_len = (size_t)n; base_game = game;
    if (cart_install(base_img, base_len)) { snprintf(st->msg, sizeof st->msg, "Couldn't load that ROM."); return 1; }
    interp_needed = !(game == GAME_DRMARIO && recomp_rom_matches_build(base_img, base_len));
    hack_active = 0;
    st->ok = 1;
    snprintf(st->msg, sizeof st->msg, "Verified.");
    return 0;
}

void rom_clear_hack(RomStatus *st)
{
    if (base_game < 0) return;
    install_base();
    st->hack_loaded = 0; st->changed_bytes = 0; st->code_changed = 0;
}

int rom_apply_hack(const char *path, RomStatus *st)
{
    st->hack_loaded = 0;
    if (base_game < 0) { snprintf(st->msg, sizeof st->msg, "Pick the original ROM first."); return 1; }
    uint8_t *buf = NULL;
    long n = read_file(path, &buf);
    if (n < 0) { snprintf(st->msg, sizeof st->msg, "Can't open the hack file."); return 1; }
    uint8_t *out = (uint8_t *)malloc(MAX_ROM);
    if (!out) { free(buf); return 1; }
    size_t olen = 0;
    char err[200] = "";
    /* IPS has no embedded source checksum; BPS/UPS and full patched ROMs do
     * not need the DX-specific IPS source guard below. */
    int is_ips_patch = n >= 5 && !memcmp(buf, "PATCH", 5);
    if (patch_is_patch(buf, (size_t)n)) {
        if (patch_apply(buf, (size_t)n, base_img, base_len, out, MAX_ROM, &olen, err, sizeof err)) {
            free(buf); free(out);
            snprintf(st->msg, sizeof st->msg, "%s", err);
            return 1;
        }
    } else {
        if (n < 0x150) { free(buf); free(out); snprintf(st->msg, sizeof st->msg, "That doesn't look like a Game Boy ROM."); return 1; }
        memcpy(out, buf, (size_t)n);
        olen = (size_t)n;
    }
    free(buf);
    CartInfo ci;
    if (!cart_parse(out, olen, &ci) || ci.mapper < 0) {
        free(out); snprintf(st->msg, sizeof st->msg, "That hack uses a cartridge type that isn't supported."); return 1;
    }

    /* These two DX IPS patches assume a specific clean source ROM. Restrict
     * this guard to IPS: BPS/UPS verify their own source checksums, and an
     * already-patched .gb file no longer depends on the source ROM revision. */
    uint32_t source_crc = crc32_bytes(base_img, base_len);
    if (is_ips_patch && base_game == GAME_SML && ci.cgb_flag == 0xC0 && ci.mapper == 5 &&
        olen == 0x40000 && source_crc != 0x90776841u) {
        free(out);
        snprintf(st->msg, sizeof st->msg,
                 "Super Mario Land DX IPS needs clean World v1.0 (expected CRC32 90776841; this ROM is %08X).",
                 source_crc);
        return 1;
    }
    if (is_ips_patch && base_game == GAME_SML2 && ci.cgb_flag == 0xC0 && ci.mapper == 5 &&
        olen == 0x100000 && source_crc != 0xD5EC24E4u) {
        free(out);
        if (source_crc == 0xE6F886E5u) {
            snprintf(st->msg, sizeof st->msg,
                     "Recognized SML2 USA/Europe Rev A, but DX v1.8.1 IPS requires v1.0 (CRC32 D5EC24E4). Use a v1.0 ROM or an already-patched DX ROM.");
        } else {
            snprintf(st->msg, sizeof st->msg,
                     "SML2 DX v1.8.1 IPS requires clean USA/Europe v1.0 (CRC32 D5EC24E4); this ROM has CRC32 %08X.",
                     source_crc);
        }
        return 1;
    }

    int changed = 0, code = 0;
    size_t cmp = olen > base_len ? olen : base_len;
    for (size_t i = 0; i < cmp; i++) {
        uint8_t a = i < olen ? out[i] : 0xFF, b = i < base_len ? base_img[i] : 0xFF;
        if (a != b) {
            changed++;
            if (base_game == GAME_DRMARIO && i < 0x8000 && (recomp_code_mask[i >> 3] & (1 << (i & 7)))) code = 1;
        }
    }
    if (base_game == GAME_DRMARIO && olen > 0x8000) code = 1;
    if (cart_install(out, olen)) { free(out); snprintf(st->msg, sizeof st->msg, "Couldn't load that hack."); return 1; }
    free(out);
    hack_active = 1;
    st->hack_loaded = 1; st->changed_bytes = changed; st->code_changed = code;
    interp_needed = !can_run_compiled_drmario() || code;
    if (changed == 0)
        snprintf(st->msg, sizeof st->msg, "Applied, but it doesn't change anything.");
    else if (base_game == GAME_DRMARIO && code)
        snprintf(st->msg, sizeof st->msg, "%d bytes changed. Game code is modified, so compatibility mode is used.", changed);
    else if (base_game == GAME_DRMARIO)
        snprintf(st->msg, sizeof st->msg, "%d bytes changed. Data only, so it runs at full speed.", changed);
    else
        snprintf(st->msg, sizeof st->msg, "%d bytes changed. Hack applied.", changed);
    return 0;
}

int rom_needs_interpreter(void) { return interp_needed; }
int rom_loaded_game(void) { return base_game; }
int rom_hack_active(void) { return hack_active; }

static int has_ext(const char *n)
{
    size_t l = strlen(n);
    if (l < 4) return 0;
    const char *e = n + l - 3;
    if (!strcmp(e, ".gb") || !strcmp(e, ".GB") || !strcmp(e, ".Gb")) return 1;
    if (l >= 5) {
        e = n + l - 4;
        return !strcmp(e, ".gbc") || !strcmp(e, ".GBC");
    }
    return 0;
}

void rom_scan(char paths[N_GAMES][512])
{
    char dirs[4][1100];
    snprintf(dirs[0], sizeof dirs[0], "%s", settings_dir());
    snprintf(dirs[1], sizeof dirs[1], "%sroms", settings_dir());
    snprintf(dirs[2], sizeof dirs[2], ".");
    snprintf(dirs[3], sizeof dirs[3], "roms");
    for (int d = 0; d < 4; d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent *e;
        while ((e = readdir(dir))) {
            if (!has_ext(e->d_name)) continue;
            char path[1300];
            snprintf(path, sizeof path, "%s/%s", dirs[d], e->d_name);
            if (strlen(path) >= 512) continue;
            FILE *f = fopen(path, "rb");
            if (!f) continue;
            uint8_t head[0x150];
            size_t got = fread(head, 1, sizeof head, f);
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fclose(f);
            if (got < 0x150 || sz > (long)MAX_ROM) continue;
            int g = rom_identify(head, got);
            if (g < 0 || paths[g][0]) continue;
            RomStatus st;
            if (rom_probe(g, path, &st) == 0) snprintf(paths[g], 512, "%s", path);
        }
        closedir(dir);
    }
}

int rom_load_raw(const char *path)
{
    uint8_t *buf = NULL;
    long n = read_file(path, &buf);
    if (n < 0) { free(buf); return 1; }
    int r = cart_install(buf, (size_t)n);
    free(buf);
    base_game = -1;
    interp_needed = 1;
    return r;
}