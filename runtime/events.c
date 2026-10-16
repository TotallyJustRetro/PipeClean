#include <SDL.h>
#include <SDL_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif
#include "events.h"
#include "museum.h"
#include "wario_event_state.h"
#include "sml2_museum_state.h"
#include "wario_land2_state.h"
#include "wario_land3_state.h"
#include "gb.h"
#include "rom.h"
#include "audio.h"
#include "settings.h"

int events_log;

/* ---- what can happen in each game --------------------------------------------------------- */
static const EventDef dr_defs[] = {
    {"clear",  "Pills cleared",    "Four in a row disappear",         1, 800, 0, 0xFFD23C, 220, 80},
    {"virus",  "Virus eliminated", "A virus is wiped out",            1, 700, 0, 0xFF4A4A, 320, 100},
    {"land",   "Pill lands",       "The pill settles in the bottle",  8, 250, 0, 0x4C8DFF, 90, 40},
    {"rotate", "Rotate",           "Flip the pill",                   1, 200, 0, 0x8CE0A0, 40, 20},
    {"move",   "Move",             "Slide the pill sideways",         1, 200, 0, 0x8CE0A0, 30, 15},
    {"over",   "Game over",        "The bottle fills up",             15, 2500, 0, 0xFF3030, 700, 100},
};
static const EventDef sml_defs[] = {
    {"coin",   "Coin",             "Pick up a coin",                  1, 350, 0, 0xFFD23C, 90, 40},
    {"stomp",  "Stomp an enemy",   "Jump on an enemy",                1, 300, 0, 0x8CE0A0, 140, 70},
    {"power",  "Power-up",         "Grab a mushroom or flower",       1, 1000, 0, 0xFF8CFF, 350, 80},
    {"bump",   "Bump a block",     "Hit a block from below",          1, 250, 0, 0xB0B0B0, 110, 60},
    {"jump",   "Jump",             "Mario jumps",                     1, 300, 0, 0x4C8DFF, 35, 15},
    {"life",   "Lose a life",      "Mario is hurt",                   15, 2500, 0, 0xFF3030, 600, 100},
    {"over",   "Game over",        "No lives left",                   15, 3000, 0, 0xFF3030, 800, 100},
};
static const EventDef sml2_defs[] = {
    {"coin",   "Coin",             "The coin counter goes up",        0, 0, 1, 0xFFD23C, 90, 40},
    {"enemy",  "Enemy defeated",   "The defeated counter goes up",    0, 0, 2, 0x8CE0A0, 140, 70},
    {"life",   "Lose a life",      "Lives go down",                   0, 0, 3, 0xFF3030, 600, 100},
    {"oneup",  "Extra life",       "Lives go up",                     0, 0, 4, 0x6CFF8C, 400, 60},
};
/* Wario Land SML3 shares these state values with its Companion Deck tracker. */
static const EventDef wario_sml3_defs[] = {
    {"treasure", "Treasure found",      "Recover a hidden treasure",       0, 0, 4, 0xFFD34A, 500, 100},
    {"power",    "Power-up",            "Gain a Wario power form",         0, 0, 1, 0xB58AFF, 300, 75},
    {"damage",   "Damage / power loss", "Take damage or lose a form",      0, 0, 2, 0xFF3D4F, 340, 85},
    {"coin",     "Coin collected",      "Collect coins",                   0, 0, 0, 0xFFD23C, 70, 35},
    {"heart",    "Heart restored",      "Recover health",                  0, 0, 5, 0x6CFF8C, 160, 55},
    {"extra",    "Extra life",          "Gain an extra life",              0, 0, 6, 0x6CFF8C, 450, 90},
    {"lost",     "Life lost",           "Lose a life",                     0, 0, 3, 0xFF3030, 650, 100},
    {"course",   "Course entered",      "Start a course from the map",     0, 0, 8, 0x4C8DFF, 150, 40},
    {"key",      "Key obtained",        "Pick up the course key",          0, 0, 9, 0xB58AFF, 320, 70},
    {"results",  "Course results",      "Reach the course-clear results screen", 0, 0, 10, 0xFFD34A, 500, 95},
};
static const EventDef wario2_defs[] = {
    {"treasure", "Treasure found", "Recover one of the 50 treasures", 0, 0, 4, 0xFFD34A, 500, 100},
    {"picture",  "Picture piece", "Find a piece for the picture panel", 0, 0, 7, 0xB58AFF, 380, 85},
    {"coin",     "Coins collected", "Increase the coin bank", 0, 0, 0, 0xFFD23C, 90, 40},
    {"course",   "Course entered", "Enter a stage from the map", 0, 0, 5, 0x4C8DFF, 150, 40},
    {"loss",     "Coins lost / wounded", "Coins dropped when Wario is hit or spent in a minigame", 0, 0, 2, 0xFF3D4F, 380, 90},
    {"results",  "Course results", "Reach the end-of-course results screen", 0, 0, 10, 0xFFD34A, 500, 95},
};
/* Wario Land 3 shares the seven general Wario effects and adds the three
 * course-flow events. It has no ordinary lives or heart meter, so recovery
 * means returning from a temporary status effect, the extra-life slot marks
 * the Rudy-defeated milestone, and life-lost marks its rare Game Over scene. */
static const EventDef wario3_defs[] = {
    {"power",    "Power-up",                "Gain a permanent Wario ability",                         0, 0, 1, 0xB58AFF, 300, 75},
    {"damage",   "Damage",                 "Wario enters a temporary status effect",                 0, 0, 2, 0xFF3D4F, 340, 85},
    {"treasure", "Treasure found",         "A course treasure is added to the collection",            0, 0, 4, 0xFFD34A, 500, 100},
    {"coin",     "Coin collected",         "The coin total increases during a course",                0, 0, 0, 0xFFD23C, 90, 40},
    {"heart",    "Heart restored",       "Wario returns to normal after a status effect (WL3 has no heart meter)", 0, 0, 5, 0x6CFF8C, 160, 55},
    {"extra",    "Extra life",           "Rudy defeated (WL3 has no extra-life system)",            0, 0, 6, 0x6CFF8C, 450, 90},
    {"lost",     "Life lost",            "Enter WL3's rare Game Over scene",                        0, 0, 3, 0xFF3030, 650, 100},
    {"course",   "Course entered",       "Enter a course from the overworld",                        0, 0, 8, 0x4C8DFF, 150, 40},
    {"key",      "Key obtained",         "Collect a course key",                                    0, 0, 9, 0xB58AFF, 320, 70},
    {"results",  "Course results",       "Reach the treasure and music-coin results screen",          0, 0, 10, 0xFFD34A, 500, 95},
};
typedef struct { const EventDef *defs; int n; } GameEvents;
#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const GameEvents ge[N_GAMES] = {
    {dr_defs, N(dr_defs)}, {sml_defs, N(sml_defs)}, {sml2_defs, N(sml2_defs)},
    {wario_sml3_defs, N(wario_sml3_defs)}, {wario2_defs, N(wario2_defs)},
    {wario3_defs, N(wario3_defs)}, {wario2_defs, N(wario2_defs)}
};

int events_count(int g) { return g >= 0 && g < N_GAMES ? ge[g].n : 0; }
const EventDef *events_def(int g, int i) { return (g >= 0 && g < N_GAMES && i >= 0 && i < ge[g].n) ? &ge[g].defs[i] : NULL; }

/* sound-request rules: the game writes an effect number into a mailbox byte */
typedef struct { uint16_t addr; uint8_t val; int ev; } Rule;
static const Rule dr_rules[] = {
    {0xDFE0, 0x06, 0}, {0xDFE0, 0x0C, 1}, {0xDFF8, 0x01, 2}, {0xDFE0, 0x02, 3}, {0xDFE0, 0x03, 4}, {0xDFE8, 0x08, 5},
};
static const Rule sml_rules[] = {
    {0xDFE0, 0x05, 0}, {0xDFE0, 0x03, 1}, {0xDFE0, 0x04, 2}, {0xDFE0, 0x07, 3}, {0xDFE0, 0x01, 4}, {0xDFE8, 0x02, 5}, {0xDFE8, 0x10, 6},
};
static const Rule *rules;
static int n_rules;

/* ---- queue (emulation thread -> main thread) ----------------------------------------------- */
#define QN 64
static volatile int q[QN];
static volatile unsigned qr, qw;
static int cur_game = -1, tap_frames;

static void push(int ev)
{
    if (qw - qr >= QN) return;
    q[qw % QN] = ev;
    qw++;
}

int events_pop(void)
{
    if (qr == qw) return -1;
    int v = q[qr % QN];
    qr++;
    return v;
}

static void fire(int ev)
{
    const EventDef *d = events_def(cur_game, ev);
    if (!d) return;
    if (events_log) fprintf(stderr, "[event] frame %d %s\n", frame_count, d->name);
    push(ev);
    museum_observe_event(cur_game, d->id);
    const GameCfg *c = &settings.g[cur_game];
    if ((c->ds_ev_speaker >> ev) & 1) {
        if (d->sfx_mask && audio_pad_available()) { apu_set_tap_mask(d->sfx_mask); tap_frames = d->sfx_ms * 60 / 1000 + 1; }
        else audio_pad_play_beep(d->beep);
    }
}

static void on_write(uint16_t a, uint8_t old_v, uint8_t v)
{
    (void)old_v;
    if (!v) return;
    for (int i = 0; i < n_rules; i++)
        if (rules[i].addr == a && rules[i].val == v) { fire(rules[i].ev); return; }
}

static void on_trigger(int ch, const uint8_t *r)
{
    if (events_log) fprintf(stderr, "[apu] frame %d ch%d  %02X %02X %02X %02X %02X\n", frame_count, ch + 1, r[0], r[1], r[2], r[3], r[4]);
}

/* ---- Super Mario Land 2 Museum observations ----------------------------------------------
 * SML2 keeps these counters/state flags in cartridge RAM. Use those values
 * rather than parsing the status bar at one hard-coded screen row: the bar's
 * vertical position can vary with the game's background/window layer. */
static Sml2MuseumTracker sml2_museum_tracker;

static uint8_t sml2_ram_read(uint16_t address)
{
    /* Read through the emulated bus so the active MBC RAM bank and RAM-enable
       state are respected. Indexing cart_ram directly always reads bank zero. */
    return cart_ram ? rd8(address) : 0xFF;
}

static int sml2_bcd_pair(uint8_t value)
{
    const int tens = (value >> 4) & 0x0F;
    const int ones = value & 0x0F;
    return tens <= 9 && ones <= 9 ? tens * 10 + ones : -1;
}

static int sml2_museum_snapshot_read(Sml2MuseumSnapshot *snapshot)
{
    if (!snapshot || !cart_ram) return 0;

    /* Match the modes recognized by the SML2 live compositor. The external
       RAM can retain plausible old counters on title/loading screens. */
    const uint8_t mode = rd8(0xFF9Bu);
    if (!sml2_museum_mode_is_active(mode)) return 0;

    const uint8_t coins_low = sml2_ram_read(0xA262);
    const uint8_t coins_high = sml2_ram_read(0xA263);
    const int coin_pair = sml2_bcd_pair(coins_low);
    const int coin_hundreds = coins_high & 0x0F;
    const int lives = sml2_bcd_pair(sml2_ram_read(0xA22C));
    const int kills = sml2_bcd_pair(sml2_ram_read(0xA28D));
    const uint8_t powerup = sml2_ram_read(0xA216);
    const uint8_t level = sml2_ram_read(0xA269);
    const uint8_t star_low = sml2_ram_read(0xA271);
    const uint8_t star_high = sml2_ram_read(0xA272);
    const uint8_t ending_raw = sml2_ram_read(0xA2D5);

    if (coin_pair < 0 || coin_hundreds > 9 || lives < 0 || kills < 0 ||
        powerup > 0x0F || level > 0x1F)
        return 0;

    memset(snapshot, 0, sizeof *snapshot);
    snapshot->valid = 1;
    snapshot->gameplay = 1;
    snapshot->coins = coin_hundreds * 100 + coin_pair;
    snapshot->enemy_kills = kills;
    snapshot->lives = lives;
    snapshot->powerup = powerup;
    snapshot->star_active = star_low != 0 && (star_high & 0x04) != 0;
    snapshot->level = level;
    snapshot->ending_flag = ending_raw == 1;
    /* The six secret courses use level IDs 19..1E; reaching one is a reliable
       secret-course discovery. Level 1F is the final castle course. */
    snapshot->secret_exit_seen = sml2_museum_is_secret_level(level);
    return 1;
}

static void sml2_museum_discover(uint32_t discoveries)
{
    if (discoveries & SML2_MUSEUM_COIN) fire(0);
    if (discoveries & SML2_MUSEUM_ENEMY) fire(1);
    if (discoveries & SML2_MUSEUM_LIFE_LOST) fire(2);
    if (discoveries & SML2_MUSEUM_EXTRA_LIFE) fire(3);
    if (discoveries & SML2_MUSEUM_MUSHROOM)
        (void)museum_discover_entry(GAME_SML2, "mushroom");
    if (discoveries & SML2_MUSEUM_MAGIC_CARROT)
        (void)museum_discover_entry(GAME_SML2, "magic_carrot");
    if (discoveries & SML2_MUSEUM_FIRE_FLOWER)
        (void)museum_discover_entry(GAME_SML2, "fire_flower");
    if (discoveries & SML2_MUSEUM_SUPER_STAR)
        (void)museum_discover_entry(GAME_SML2, "starman");
    if (discoveries & SML2_MUSEUM_GOLDEN_COINS)
        (void)museum_discover_entry(GAME_SML2, "golden_coins");
    if (discoveries & SML2_MUSEUM_WARIO)
        (void)museum_discover_entry(GAME_SML2, "wario");
    if (discoveries & SML2_MUSEUM_SECRET_EXIT)
        (void)museum_discover_entry(GAME_SML2, "secret_exit");
    if (discoveries & SML2_MUSEUM_CASTLE_CLEARED)
        (void)museum_discover_entry(GAME_SML2, "castle_cleared");
}

static void sml2_museum_live_frame(void)
{
    if (cur_game != GAME_SML2 || rom_loaded_game() != GAME_SML2) {
        sml2_museum_tracker.have_previous = 0;
        return;
    }

    /* The ending trigger is checked as the game loads its credits scene, where
       the ordinary status bar may no longer be drawn. Probe this flag separately
       so the final Museum entry doesn't depend on the HUD visibility check. */
    if (cart_ram && sml2_ram_read(0xA2D5) == 1)
        (void)museum_discover_entry(GAME_SML2, "castle_cleared");

    Sml2MuseumSnapshot snapshot;
    if (!sml2_museum_snapshot_read(&snapshot)) return;
    const uint32_t found = sml2_museum_tracker_update(&sml2_museum_tracker, &snapshot);
    if (found) sml2_museum_discover(found);
}

/* Wario Land SML3 stores its expedition counters in cartridge RAM. */
static WarioEventTracker wario_event_tracker;
static WarioLand2EventTracker wario2_event_tracker;
static WarioLand3Tracker wario3_event_tracker;

static void wario2_event_frame(void)
{
    WarioLand2Snapshot snapshot = wario_land2_read_snapshot(cur_game, gb_wram_peek);
    if (snapshot.stage >= 0 && snapshot.bank_coins >= 0 &&
        snapshot.overworld_coins >= 0 && snapshot.render_mode >= 0 &&
        snapshot.render_mode <= 0x13) {
        museum_sync_flags(cur_game, "wl2_treasure", snapshot.treasures, 50);
        museum_sync_flags(cur_game, "wl2_picture", snapshot.picture_panels, 50);
    }
    uint32_t events = wario_land2_tracker_update(&wario2_event_tracker, &snapshot);
    for (int ev = 0; ev < WARIO_LAND2_EVENT_COUNT; ++ev)
        if (events & WARIO_LAND2_EVENT_MASK(ev)) fire(ev);
}

static void wario3_event_frame(void)
{
    /* C000-based fixed WRAM is exposed as offset 0. */
    WarioLand3Snapshot snapshot = {
        (int)gb_wram_peek(0x009B), /* C09B: current scene */
        (int)gb_wram_peek(0x009C), /* C09C: current operation */
        wario_land3_read_counter3(gb_wram_peek(0x0A04), gb_wram_peek(0x0A05)),
        wario_land3_read_counter3(gb_wram_peek(0x0A39), gb_wram_peek(0x0A3A)),
        (int)gb_wram_peek(0x0A3C), /* CA3C: permanent power-up state */
        (int)gb_wram_peek(0x0A8E), /* CA8E: temporary Wario status */
        (int)gb_wram_peek(0x0A3D)  /* CA3D: completion flags */
    };
    /* CA5B is the current course's Gray/Red/Green/Blue key bitmask. Keep
       museum progress monotonic so rewind/load never removes a discovery. */
    const uint8_t key_mask_raw = gb_wram_peek(0x0A5B); /* CA5B */
    if (wario_land3_snapshot_valid(&snapshot) &&
        (snapshot.scene == 2 || snapshot.scene == 8) && key_mask_raw <= 0x0F) {
        static const char *key_ids[WARIO_LAND3_KEY_COUNT] = {
            "key_gray", "key_red", "key_green", "key_blue"
        };
        for (int key = 0; key < WARIO_LAND3_KEY_COUNT; ++key)
            if (wario_land3_key_collected(key_mask_raw, key))
                (void)museum_discover_entry(cur_game, key_ids[key]);
    }
    /* CEE3 stores the last collected treasure slot. The Museum uses the
       reference list's 001..100 sequence; only trust it alongside a validated
       increase in the game treasure total, to avoid false unlocks from stale RAM. */
    if (wario_land3_snapshot_valid(&snapshot) && snapshot.treasures <= 100 &&
        wario3_event_tracker.have_previous &&
        wario3_event_tracker.previous.treasures >= 0 &&
        wario3_event_tracker.previous.treasures <= 100 &&
        snapshot.treasures > wario3_event_tracker.previous.treasures) {
        const uint8_t last_slot = gb_wram_peek(0x0EE3); /* CEE3 */
        if (last_slot < 100) {
            char museum_id[40];
            snprintf(museum_id, sizeof museum_id, "wl3_treasure_%03u",
                     (unsigned)last_slot + 1u);
            (void)museum_discover_entry(cur_game, museum_id);
            if (events_log)
                fprintf(stderr, "[museum] WL3 treasure RAM id %02X -> catalogue %03u (total %d)\n",
                        last_slot, (unsigned)last_slot + 1u, snapshot.treasures);
        } else if (events_log) {
            fprintf(stderr, "[museum] ignored invalid WL3 treasure slot %02X\n", last_slot);
        }
    }
    uint32_t events = wario_land3_tracker_update(&wario3_event_tracker, &snapshot);
    for (int ev = 0; ev < WARIO_LAND3_EVENT_COUNT; ++ev)
        if (events & WARIO_LAND3_EVENT_MASK(ev)) fire(ev);
}

static uint8_t wario_ram_read(uint16_t address)
{
    return cart_ram ? cart_ram[address & 0x1FFFu] : 0xFF;
}

static int wario_bcd_pair(uint8_t value)
{
    int tens = (value >> 4) & 0x0F, ones = value & 0x0F;
    return tens <= 9 && ones <= 9 ? tens * 10 + ones : -1;
}

static int wario_bcd_coins(void)
{
    uint8_t high = wario_ram_read(0xA805);
    uint8_t mid = wario_ram_read(0xA806);
    uint8_t low = wario_ram_read(0xA807);
    int d0 = high & 0x0F, d1 = (mid >> 4) & 0x0F;
    int d2 = mid & 0x0F, d3 = (low >> 4) & 0x0F, d4 = low & 0x0F;
    if (d0 > 9 || d1 > 9 || d2 > 9 || d3 > 9 || d4 > 9) return -1;
    return d0 * 10000 + d1 * 1000 + d2 * 100 + d3 * 10 + d4;
}

static int wario_event_snapshot_read(WarioEventSnapshot *snapshot)
{
    if (!snapshot || !cart_ram) return 0;

    uint8_t hearts_raw = wario_ram_read(0xA808);
    uint8_t lives_raw = wario_ram_read(0xA809);
    uint8_t power_raw = wario_ram_read(0xA80A);
    uint8_t treasure_low = wario_ram_read(0xA80E);
    uint8_t treasure_high = wario_ram_read(0xA80F);
    if (treasure_low == 0xFF || treasure_high == 0xFF || power_raw > 4) return 0;

    snapshot->coins = wario_bcd_coins();
    snapshot->hearts = wario_bcd_pair(hearts_raw);
    snapshot->lives = wario_bcd_pair(lives_raw);
    snapshot->power = power_raw;
    snapshot->treasure_mask = (int)treasure_low | ((int)treasure_high << 8);
    snapshot->course = wario_ram_read(0xA9E4);
    snapshot->game_mode = wario_ram_read(0xA8C3);
    snapshot->sub_mode = wario_ram_read(0xA8C4);
    snapshot->held_key = wario_ram_read(0xA392);
    return snapshot->coins >= 0 && snapshot->hearts >= 0 && snapshot->lives >= 0;
}

static void wario_event_frame(void)
{
    WarioEventSnapshot snapshot;
    if (!wario_event_snapshot_read(&snapshot)) {
        wario_event_tracker.have_previous = 0;
        return;
    }

    museum_sync_flags(cur_game, "wario1_treasure", (uint64_t)(unsigned)snapshot.treasure_mask, 15);
    uint32_t events = wario_event_tracker_update(&wario_event_tracker, &snapshot);
    for (int ev = 0; ev < WARIO_EVENT_COUNT; ++ev)
        if (events & WARIO_EVENT_MASK(ev)) fire(ev);
}

typedef struct {
    int active;
    uint8_t type, x, y;
    MuseumCategory category;
    char id[40];
} SmlMuseumObject;
static SmlMuseumObject sml_museum_previous[10];

/* SML2 developer trace: actor records begin at AD00 and advance by 0x20 bytes.
 * Each record's active marker is at record +8, so the active
 * markers the game scans begin at AD08. Source descriptor pointers are stored
 * at record +6/+7 in high-byte-first order and point to descriptor byte +2.
 * Keep the raw descriptor and full actor record as diagnostic evidence; neither
 * a descriptor byte nor an animation/mapping field is assumed to be a species ID. */
#if defined(PIPECLEAN_DEV_BUILD)
static FILE *sml2_actor_trace_file;
/* Track unique animation-key combinations independently per actor slot.
 * Slots can be reused for different enemies; clear a slot's set when it empties. */
static uint8_t sml2_actor_trace_seen[0x20][16][0x100][0x20];
static uint8_t sml2_actor_trace_slot_active[16];
/* The live actor retains a pointer to its source spawn descriptor at +6/+7.
 * The stored pointer addresses descriptor byte +2, which the game marks as
 * spawned; descriptor byte +2 in the copy at actor +2 preserves the old value.
 * Log each source pointer independently from mutable animation keys. */
static uint16_t sml2_actor_trace_source_ref[0x20][16];
static uint8_t sml2_actor_trace_source_logged[0x20][16];
/* The level loader expands all six-byte spawn descriptors at AB06. This lets
 * diagnostics inspect the level's source list even when no live actor exists.
 * Byte +2 changes after spawning, so it is excluded from the list fingerprint. */
static uint32_t sml2_actor_trace_spawn_hash[0x20];
static uint8_t sml2_actor_trace_spawn_hash_valid[0x20];

static void sml2_actor_trace_close(void)
{
    if (sml2_actor_trace_file) {
        fflush(sml2_actor_trace_file);
        fclose(sml2_actor_trace_file);
        sml2_actor_trace_file = NULL;
    }
}

static void sml2_actor_trace_begin(void)
{
    sml2_actor_trace_close();
    memset(sml2_actor_trace_seen, 0, sizeof sml2_actor_trace_seen);
    memset(sml2_actor_trace_slot_active, 0, sizeof sml2_actor_trace_slot_active);
    memset(sml2_actor_trace_source_ref, 0, sizeof sml2_actor_trace_source_ref);
    memset(sml2_actor_trace_source_logged, 0, sizeof sml2_actor_trace_source_logged);
    memset(sml2_actor_trace_spawn_hash, 0, sizeof sml2_actor_trace_spawn_hash);
    memset(sml2_actor_trace_spawn_hash_valid, 0, sizeof sml2_actor_trace_spawn_hash_valid);
    if (cur_game != GAME_SML2) return;

    const char *base = settings_dir();
    if (!base || !base[0]) return;
    size_t len = strlen(base);
    const char *sep = (len && (base[len - 1] == '/' || base[len - 1] == '\\')) ? "" : "/";
    char path[1200];
    int n = snprintf(path, sizeof path, "%s%spipeclean-sml2-actor-trace.txt", base, sep);
    if (n < 0 || (size_t)n >= sizeof path) return;
    sml2_actor_trace_file = fopen(path, "w");
    if (!sml2_actor_trace_file) return;
    fprintf(sml2_actor_trace_file,
            "PipeClean SML2 live sprite actor trace v3\n"
            "Each actor record is 0x20 bytes; the active marker is at record offset +8.\n"
            "Record bases begin at AD00; active markers begin at AD08.\n"
            "The source descriptor pointer is stored high-byte first at record offsets +6/+7.\n"
            "It points at descriptor byte +2; descriptor bytes are read from pointer - 2.\n"
            "Descriptor bytes and actor fields are diagnostic evidence, not assumed species IDs.\n"
            "spawn frame level slot record_addr source_ref source_addr descriptor[00..05]\n"
            "actor frame level slot record_addr active source_ref field05 field0B record[00..1F]\n");
    fflush(sml2_actor_trace_file);
}

static void sml2_actor_trace_spawn_table(int mode, int level)
{
    if (!sml2_actor_trace_file || level < 0 || level >= 0x20) return;

    const uint16_t start = 0xAB06u;
    const unsigned max_entries = (0xACFFu - start + 1u) / 6u;
    uint32_t hash = 2166136261u;
    unsigned count = 0;

    for (unsigned i = 0; i < max_entries; ++i) {
        uint8_t d[6];
        const uint16_t addr = (uint16_t)(start + i * 6u);
        for (unsigned j = 0; j < 6; ++j)
            d[j] = rd8((uint16_t)(addr + j));
        if (d[0] == 0xFFu) break;
        ++count;
        for (unsigned j = 0; j < 6; ++j) {
            if (j == 2) continue;
            hash ^= d[j];
            hash *= 16777619u;
        }
    }

    if (sml2_actor_trace_spawn_hash_valid[level] &&
        sml2_actor_trace_spawn_hash[level] == hash) return;
    sml2_actor_trace_spawn_hash[level] = hash;
    sml2_actor_trace_spawn_hash_valid[level] = 1;

    fprintf(sml2_actor_trace_file,
            "spawn_table frame=%d mode=%02X level=%02X base=AB06 entries=%u hash=%08X\n",
            frame_count, mode, level, count, hash);
    for (unsigned i = 0; i < count; ++i) {
        const uint16_t addr = (uint16_t)(start + i * 6u);
        fprintf(sml2_actor_trace_file,
                "spawn_entry frame=%d mode=%02X level=%02X index=%02u addr=%04X descriptor=",
                frame_count, mode, level, i, addr);
        for (unsigned j = 0; j < 6; ++j)
            fprintf(sml2_actor_trace_file, "%s%02X", j ? " " : "", rd8((uint16_t)(addr + j)));
        fputc('\n', sml2_actor_trace_file);
    }
    fflush(sml2_actor_trace_file);
}

static void sml2_actor_trace_frame(void)
{
    if (!sml2_actor_trace_file) return;

    /* Keep a heartbeat even when a guard prevents actor scanning. A header-only
     * trace is otherwise indistinguishable from a trace hook that never ran. */
    const int loaded_game = rom_loaded_game();
    const int has_cart_ram = cart_ram != NULL;
    const uint8_t mode = rd8(0xFF9Bu);
    const uint8_t level = rd8(0xA269);
    const int diagnostic_tick = (frame_count % 60) == 0;

    if (cur_game != GAME_SML2 || loaded_game != GAME_SML2 || !has_cart_ram) {
        if (diagnostic_tick)
            fprintf(sml2_actor_trace_file,
                    "diag frame=%d reason=game_or_ram_guard cur_game=%d loaded_game=%d cart_ram=%d mode=%02X level=%02X\n",
                    frame_count, cur_game, loaded_game, has_cart_ram, mode, level);
        return;
    }

    /* The previous trace showed mode 0C, which production Museum logic does
     * not treat as gameplay. In this developer-only trace, still inspect the
     * actor table so we can determine whether records exist during that mode.
     * Actor/spawn rows include the observed mode for later filtering. */
    if (!sml2_museum_mode_is_active(mode) && diagnostic_tick)
        fprintf(sml2_actor_trace_file,
                "diag frame=%d reason=nonstandard_mode_scanning mode=%02X level=%02X\n",
                frame_count, mode, level);

    /* Capture the level's source descriptors even if its actor table is empty. */
    sml2_actor_trace_spawn_table(mode, level);

    if (level > 0x1F) {
        if (diagnostic_tick)
            fprintf(sml2_actor_trace_file,
                    "diag frame=%d reason=invalid_level mode=%02X level=%02X\n",
                    frame_count, mode, level);
        return;
    }

    if (diagnostic_tick) {
        unsigned nonzero_slots = 0;
        for (int slot = 0; slot < 16; ++slot) {
            const uint16_t base = (uint16_t)(0xAD08u + (unsigned)slot * 0x20u);
            if (rd8(base) != 0) ++nonzero_slots;
        }
        fprintf(sml2_actor_trace_file,
                "diag frame=%d reason=scanning mode=%02X level=%02X nonzero_slots=%u\n",
                frame_count, mode, level, nonzero_slots);
        fflush(sml2_actor_trace_file);
    }

    for (int slot = 0; slot < 16; ++slot) {
        /* The allocator rounds the selected active-marker address down to the
         * 16-byte boundary before copying the descriptor. The actor starts at
         * AD00 + slot*0x20; its active marker is record +8 (AD08, AD28, ...). */
        const uint16_t base = (uint16_t)(0xAD00u + (unsigned)slot * 0x20u);
        const uint16_t active_addr = (uint16_t)(base + 8u);
        const uint8_t active = rd8(active_addr);
        if (!active) {
            if (sml2_actor_trace_slot_active[slot]) {
                memset(sml2_actor_trace_seen[level][slot], 0,
                       sizeof sml2_actor_trace_seen[level][slot]);
                sml2_actor_trace_source_ref[level][slot] = 0;
                sml2_actor_trace_source_logged[level][slot] = 0;
                sml2_actor_trace_slot_active[slot] = 0;
            }
            continue;
        }

        /* Spawn routine copies the six-byte descriptor to record[0..5], then
         * stores source high byte at +6 and source low byte +2 at +7. */
        const uint16_t source_ref = (uint16_t)(
            ((uint16_t)rd8((uint16_t)(base + 6u)) << 8) |
            rd8((uint16_t)(base + 7u)));
        const int source_changed = !sml2_actor_trace_source_logged[level][slot] ||
            sml2_actor_trace_source_ref[level][slot] != source_ref;
        if (source_changed) {
            /* A reused slot can have the same animation fields as its previous
             * actor, so reset the seen-field combinations when its source changes. */
            memset(sml2_actor_trace_seen[level][slot], 0,
                   sizeof sml2_actor_trace_seen[level][slot]);
            /* Source descriptors normally live in ROM, but allow the visible
             * external-RAM window too. The previous check accepted only RAM,
             * incorrectly rejecting values such as source_ref=1002 (descriptor
             * starts at 1000 in fixed ROM). Range checks ensure all six bytes
             * stay within one mapped address window. */
            const int source_in_rom = source_ref >= 0x0002u && source_ref <= 0x7FFCu;
            const int source_in_ram = source_ref >= 0xA002u && source_ref <= 0xBFFCu;
            if (source_in_rom || source_in_ram) {
                const uint16_t source_addr = (uint16_t)(source_ref - 2u);
                uint8_t descriptor[6];
                for (unsigned i = 0; i < sizeof descriptor; ++i)
                    descriptor[i] = rd8((uint16_t)(source_addr + i));
                fprintf(sml2_actor_trace_file,
                        "spawn frame=%d mode=%02X level=%02X slot=%02d record_addr=%04X source_ref=%04X source_addr=%04X source_space=%s descriptor=",
                        frame_count, mode, level, slot, base, source_ref, source_addr,
                        source_in_rom ? "rom" : "ram");
                for (unsigned i = 0; i < sizeof descriptor; ++i)
                    fprintf(sml2_actor_trace_file, "%s%02X", i ? " " : "", descriptor[i]);
                fputc('\n', sml2_actor_trace_file);
            } else {
                fprintf(sml2_actor_trace_file,
                        "spawn frame=%d mode=%02X level=%02X slot=%02d record_addr=%04X source_ref=%04X descriptor=unavailable\n",
                        frame_count, mode, level, slot, base, source_ref);
            }
            sml2_actor_trace_source_ref[level][slot] = source_ref;
            sml2_actor_trace_source_logged[level][slot] = 1;
            fflush(sml2_actor_trace_file);
        }

        const uint8_t field05 = rd8((uint16_t)(base + 5u));
        const uint8_t field0B = rd8((uint16_t)(base + 11u));
        /* Disabled external RAM reads as FF; do not treat that bus value as an actor. */
        if (active == 0xFFu && field05 == 0xFFu && field0B == 0xFFu) continue;
        sml2_actor_trace_slot_active[slot] = 1;
        uint8_t *seen = &sml2_actor_trace_seen[level][slot][field05][field0B >> 3];
        const uint8_t field_bit = (uint8_t)(1u << (field0B & 7u));
        if (*seen & field_bit) continue;
        *seen |= field_bit;

        fprintf(sml2_actor_trace_file,
                "actor frame=%d mode=%02X level=%02X slot=%02d record_addr=%04X active=%02X source_ref=%04X field05=%02X field0B=%02X record=",
                frame_count, mode, level, slot, base, active, source_ref, field05, field0B);
        for (unsigned i = 0; i < 32; ++i)
            fprintf(sml2_actor_trace_file, "%s%02X", i ? " " : "", rd8((uint16_t)(base + i)));
        fputc('\n', sml2_actor_trace_file);
        fflush(sml2_actor_trace_file);
    }
}
#else
static void sml2_actor_trace_begin(void) { }
static void sml2_actor_trace_close(void) { }
static void sml2_actor_trace_frame(void) { }
#endif

void events_begin(void)
{
    cur_game = rom_loaded_game();
    sml2_actor_trace_begin();
    qr = qw = 0; tap_frames = 0; memset(&sml2_museum_tracker, 0, sizeof sml2_museum_tracker);
    wario_event_tracker.have_previous = 0;
    wario2_event_tracker.have_previous = 0;
    wario3_event_tracker.have_previous = 0;
    memset(sml_museum_previous, 0, sizeof sml_museum_previous);
    apu_set_tap_mask(0);
    apu_set_trigger_hook(on_trigger);
    rules = NULL; n_rules = 0;
    if (cur_game == GAME_DRMARIO) { rules = dr_rules; n_rules = N(dr_rules); }
    else if (cur_game == GAME_SML) { rules = sml_rules; n_rules = N(sml_rules); }
    static const uint16_t mailbox[] = {0xDFE0, 0xDFE8, 0xDFF8};
    if (n_rules) gb_watch_set(mailbox, 3, on_write);
}

void events_end(void)
{
    sml2_actor_trace_close();
    apu_set_trigger_hook(NULL);
    apu_set_tap_mask(0);
    gb_watch_set(NULL, 0, NULL);
    wario_event_tracker.have_previous = 0;
    wario2_event_tracker.have_previous = 0;
    wario3_event_tracker.have_previous = 0;
    memset(&sml2_museum_tracker, 0, sizeof sml2_museum_tracker);
}

void events_state_reset(void)
{
    /* Event observations are frontend history, not saved game state. */
    qr = qw = 0;
    memset(&sml2_museum_tracker, 0, sizeof sml2_museum_tracker);
    tap_frames = 0;
    wario_event_tracker.have_previous = 0;
    wario2_event_tracker.have_previous = 0;
    wario3_event_tracker.have_previous = 0;
    apu_set_tap_mask(0);
}

static int museum_make_dir(const char *path)
{
#if defined(_WIN32)
    if (_mkdir(path) == 0 || errno == EEXIST) return 1;
#else
    if (mkdir(path, 0700) == 0 || errno == EEXIST) return 1;
#endif
    return 0;
}

static int museum_sml_asset_output(const char *entry_id, char *out, size_t cap)
{
    char root[1200], game_dir[1200];
    const char *base = settings_dir();
    if (!base || !base[0] || !entry_id || !entry_id[0]) return 0;
    size_t len = strlen(base);
    const char *sep = (len && (base[len - 1] == '/' || base[len - 1] == '\\')) ? "" : "/";
    int n = snprintf(root, sizeof root, "%s%spipeclean-museum-assets", base, sep);
    if (n < 0 || (size_t)n >= sizeof root || !museum_make_dir(root)) return 0;
    n = snprintf(game_dir, sizeof game_dir, "%s/%s", root, games[GAME_SML].id);
    if (n < 0 || (size_t)n >= sizeof game_dir || !museum_make_dir(game_dir)) return 0;
    n = snprintf(out, cap, "%s/%s.v2.png", game_dir, entry_id);
    return n >= 0 && (size_t)n < cap;
}

/* Save the sprite pixels PipeClean just rendered. No image data is bundled;
 * the PNG is generated locally from the active cartridge's visible graphics. */
static int museum_sml_capture_sprite(const char *entry_id, uint8_t object_x, uint8_t object_y)
{
    if (!entry_id || rom_loaded_game() != GAME_SML) return 0;
    char existing[1200], path[1200];
    /* The versioned path avoids reusing old partial crops from screen pixels. */
    if (museum_sprite_asset_path(GAME_SML, entry_id, existing, sizeof existing)) return 1;
    if (!museum_sml_asset_output(entry_id, path, sizeof path)) return 0;

    uint32_t rgba[64 * 64];
    int width = 0, height = 0;
    int palette_idx = settings.g[GAME_SML].palette;
    if (palette_idx < 0 || palette_idx >= n_palettes) palette_idx = 0;
    const Palette *palette = &palettes[palette_idx];
    if (!ppu_museum_capture_object(object_y, object_x, rgba, 64u * 64u, 64,
                                   &width, &height, palette->ob0, palette->ob1))
        return 0;

    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
        0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return 0;
    for (int y = 0; y < height; ++y) {
        Uint32 *row = (Uint32 *)((Uint8 *)surface->pixels + y * surface->pitch);
        for (int x = 0; x < width; ++x) {
            const uint32_t pixel = rgba[(size_t)y * 64u + (size_t)x];
            row[x] = SDL_MapRGBA(surface->format,
                                 (Uint8)(pixel >> 24),
                                 (Uint8)(pixel >> 16),
                                 (Uint8)(pixel >> 8),
                                 (Uint8)pixel);
        }
    }
    int ok = IMG_SavePNG(surface, path) == 0;
    SDL_FreeSurface(surface);
    return ok;
}

/* Only treat a RAM object as encountered if its object-owned OAM pieces
 * produced pixels in the current frame. The shadow OAM range 20..39 belongs
 * to Super Mario Land's ten live object slots. */
static int museum_sml_object_visible(uint8_t object_x, uint8_t object_y)
{
    const int object_left = (int)object_x - 8;
    const int object_top = (int)object_y - 16;
    const int sprite_h = (rd8(0xFF40) & 0x04) ? 16 : 8;

    for (int i = 20; i < 40; ++i) {
        uint8_t oy = oam[i * 4], ox = oam[i * 4 + 1];
        if (!oy || !ox || oy == 0xFF || ox == 0xFF) continue;
        const int sx = (int)ox - 8, sy = (int)oy - 16;
        if (abs(sx - object_left) > 16 || abs(sy - object_top) > 16) continue;
        int y0 = sy < 0 ? 0 : sy;
        int y1 = sy + sprite_h > GB_H ? GB_H : sy + sprite_h;
        int x0 = sx < 0 ? 0 : sx;
        int x1 = sx + 8 > GB_W ? GB_W : sx + 8;
        for (int py = y0; py < y1; ++py) {
            for (int px = x0; px < x1; ++px) {
                const int render_x = px + ppu_xoff;
                if (render_x < 0 || render_x >= ppu_w || render_x >= GB_WMAX) continue;
                if (ppu_layer[py][render_x] != 0 && ppu_sprtile[py][render_x] != 0xFFFF)
                    return 1;
            }
        }
    }
    return 0;
}

/* A stable object type prevents capturing a one-frame spawn/death animation.
 * Item sprites are cached only after they settle into their collected form. */
static int museum_sml_capture_ready(uint8_t raw_type, MuseumCategory category)
{
    const uint8_t type = raw_type & 0x7Fu;
    if (category == MUSEUM_CATEGORY_ITEM)
        return type == 0x29 || type == 0x2B || type == 0x2C ||
               type == 0x2E || type == 0x34;
    switch (type) {
    case 0x00: case 0x02: case 0x04: case 0x06:
    case 0x08: case 0x09: case 0x0E: case 0x10:
    case 0x16: case 0x1A: case 0x1D: case 0x20:
    case 0x24: case 0x25: case 0x2F: case 0x30:
    case 0x31: case 0x32: case 0x3C:
    case 0x3F: case 0x42: case 0x47: case 0x48:
    case 0x49: case 0x4A: case 0x4B: case 0x52:
    case 0x53: case 0x54: case 0x55: case 0x56:
    case 0x59: case 0x5A: case 0x60: case 0x61:
        return 1;
    default:
        return 0;
    }
}

static void museum_sml_live_frame(void)
{
    if (cur_game != GAME_SML || rom_loaded_game() != GAME_SML) {
        memset(sml_museum_previous, 0, sizeof sml_museum_previous);
        return;
    }
    SmlMuseumObject current[10];
    memset(current, 0, sizeof current);
    const int player_x = rd8(0xC202), player_y = rd8(0xC201);
    for (int slot = 0; slot < 10; ++slot) {
        const size_t base = 0x1100u + (size_t)slot * 0x10u; /* D100-D190 */
        const uint8_t raw_type = gb_wram_peek(base);
        if (raw_type == 0xFF) continue;
        MuseumCategory category;
        const char *id = museum_sml_object_entry_id(raw_type, &category);
        if (!id) continue;
        const uint8_t y = gb_wram_peek(base + 2u), x = gb_wram_peek(base + 3u);
        if (x >= 0xE0 || y >= 0xC0 || x == 0 || y == 0) continue;

        const SmlMuseumObject *old = &sml_museum_previous[slot];
        SmlMuseumObject *obj = &current[slot];
        obj->active = 1;
        obj->type = raw_type;
        obj->x = x;
        obj->y = y;
        obj->category = category;
        snprintf(obj->id, sizeof obj->id, "%s", id);

        const int visible = museum_sml_object_visible(x, y);
        const int stable = old->active && old->type == raw_type && !strcmp(old->id, id);
        if (visible && category == MUSEUM_CATEGORY_ENEMY)
            (void)museum_discover_entry(GAME_SML, id);
        if (visible && stable && museum_sml_capture_ready(raw_type, category))
            (void)museum_sml_capture_sprite(id, x, y);
    }

    for (int slot = 0; slot < 10; ++slot) {
        const SmlMuseumObject *old = &sml_museum_previous[slot];
        const SmlMuseumObject *now = &current[slot];
        if (!old->active || old->category != MUSEUM_CATEGORY_ITEM) continue;
        if (now->active && !strcmp(old->id, now->id)) continue;
        if (abs(player_x - (int)old->x) <= 24 && abs(player_y - (int)old->y) <= 28)
            (void)museum_discover_entry(GAME_SML, old->id);
    }
    memcpy(sml_museum_previous, current, sizeof current);
}

void events_frame(void)
{
    static int16_t tb[2048 * 2];
    if (cur_game == GAME_SML) museum_sml_live_frame();
    if (cur_game == GAME_SML2) {
        sml2_actor_trace_frame();
        sml2_museum_live_frame();
    }
    else if (cur_game == GAME_WARIO_SML3) wario_event_frame();
    else if (cur_game == GAME_WARIO_LAND3_GBC) wario3_event_frame();
    else if (wario_land2_supported_game(cur_game)) wario2_event_frame();
    if (tap_frames > 0) {
        int n = apu_drain_tap(tb, 2048);
        if (n > 0) audio_pad_push(tb, n);
        if (--tap_frames == 0) apu_set_tap_mask(0);
    }
}