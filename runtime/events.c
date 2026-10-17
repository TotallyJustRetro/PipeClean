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
#include "cart.h"
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

/* SML2 actor IDs are contextual: the first ten courses and later courses
 * use different object-type-to-sprite tables. Keep discoveries conservative
 * until an object's type and course have been cross-checked against ROM data.
 * This first pass contains only mappings supported by the decoded course
 * spawn lists, composed sprites, and course references. */
typedef struct {
    uint8_t level;
    uint8_t object_type;
    const char *museum_id;
} Sml2MuseumEnemyMap;

static const Sml2MuseumEnemyMap sml2_museum_enemy_map[] = {
    /* Intro / Classic enemy set. These IDs also recur in its bonus courses. */
    {0x00, 0x09, "sml2_goomba"},
    {0x00, 0x01, "sml2_koopa_troopa"},
    {0x00, 0x08, "sml2_paragoomba"},
    {0x00, 0x0C, "sml2_piranha_plant"},
    {0x19, 0x09, "sml2_goomba"},
    {0x19, 0x01, "sml2_koopa_troopa"},
    {0x19, 0x08, "sml2_paragoomba"},
    {0x19, 0x0C, "sml2_piranha_plant"},
    {0x1A, 0x09, "sml2_goomba"},
    {0x1A, 0x01, "sml2_koopa_troopa"},
    {0x1A, 0x08, "sml2_paragoomba"},
    {0x1A, 0x0C, "sml2_piranha_plant"},
    {0x1B, 0x09, "sml2_goomba"},
    {0x1B, 0x01, "sml2_koopa_troopa"},
    {0x1B, 0x08, "sml2_paragoomba"},
    {0x1B, 0x0C, "sml2_piranha_plant"},
    {0x1D, 0x09, "sml2_goomba"},
    {0x1D, 0x01, "sml2_koopa_troopa"},
    {0x1D, 0x08, "sml2_paragoomba"},
    {0x1D, 0x0C, "sml2_piranha_plant"},
    {0x1E, 0x09, "sml2_goomba"},
    {0x1E, 0x01, "sml2_koopa_troopa"},
    {0x1E, 0x08, "sml2_paragoomba"},
    {0x1E, 0x0C, "sml2_piranha_plant"},
    {0x1F, 0x09, "sml2_goomba"},
    {0x1F, 0x01, "sml2_koopa_troopa"},
    {0x1F, 0x08, "sml2_paragoomba"},
    {0x1F, 0x0C, "sml2_piranha_plant"},

    /* The Cheep Cheep Course enemy roster is count-checkable against its
     * ROM spawn list: 7 Blurps, 5 sharks, 8 Cheep-Cheeps, 3 Spiny Cheep-Cheeps,
     * 3 Goombas, and 1 Para-Goomba. Types 07 and 3D share the fish mapping. */
    {0x0E, 0x05, "sml2_blurp"},
    {0x0E, 0x06, "sml2_shark"},
    {0x0E, 0x07, "sml2_cheep_cheep"},
    {0x0E, 0x0B, "sml2_spiny_cheep_cheep"},
    {0x0E, 0x3D, "sml2_cheep_cheep"},
    {0x0E, 0x08, "sml2_paragoomba"},
    {0x0E, 0x09, "sml2_goomba"},

    /* Sunken Ship Course: type 01 is Koopa, 06 is Shark, and the spiky
     * round sprite on type 56 matches Unibo (also listed for Turtle Zone). */
    {0x0F, 0x01, "sml2_koopa_troopa"},
    {0x0F, 0x06, "sml2_shark"},
    {0x0F, 0x56, "sml2_unibo"},

    /* Whale Course: the helmeted aquatic Goomba occupies type 20; types 08
     * and 3A both use the same winged Goomba sprite mapping. */
    {0x10, 0x01, "sml2_koopa_troopa"},
    {0x10, 0x08, "sml2_paragoomba"},
    {0x10, 0x20, "sml2_aqua_goomba"},
    {0x10, 0x3A, "sml2_paragoomba"},

    /* Piranha Plant and Fire Piranha Plant use separate type IDs. */
    {0x04, 0x0C, "sml2_piranha_plant"},
    {0x04, 0x0D, "sml2_fire_piranha_plant"},
    {0x06, 0x0C, "sml2_piranha_plant"},
    {0x06, 0x0D, "sml2_fire_piranha_plant"},
    {0x08, 0x0D, "sml2_fire_piranha_plant"},
    {0x09, 0x0C, "sml2_piranha_plant"},

    /* Roots Course: the composed sprite sheet shows the spiked hedgehog
     * on type 21 and the flying insect on type 22. */
    {0x01, 0x1F, "sml2_ragumo"},
    {0x01, 0x21, "sml2_spikey"},
    {0x01, 0x22, "sml2_kyotonbo"},

    /* Tree Trunk Course: the atlas and named course roster agree on these. */
    {0x02, 0x44, "sml2_noko_bombette"},
    {0x02, 0x58, "sml2_moofish"},

    /* Leaf Course: the broad-winged sprite is Bigbee, the segmented crawler
     * is Ant, the spiky beetle sprite is Battle Beetle, and type 43 is Tamara. */
    {0x03, 0x40, "sml2_bigbee"},
    {0x03, 0x41, "sml2_ant"},
    {0x03, 0x42, "sml2_battle_beetle"},
    {0x03, 0x43, "sml2_tamara"},

    /* Tree Zone 4's type 39 composes into the swooping bird sprite. */
    {0x05, 0x39, "sml2_big_bird"},

    /* Manhole Course ant variant. */
    {0x04, 0x5D, "sml2_ant"},

    /* The Ant Monsters (Macro Zone 1) uses course-specific ant and mole maps. */
    {0x14, 0x22, "sml2_ant"},
    {0x14, 0x23, "sml2_goronto"},

    /* The second Macro Zone course reuses the ant object's alternate map. */
    {0x15, 0x5D, "sml2_ant"},

    /* Pumpkin Zone 2/3 use separate mappings for the ghost enemies:
     * the side-on ghost-Goomba sprite is type 4D; type 54 is the masked ghoul. */
    {0x07, 0x4D, "sml2_ghost_goomba"},
    {0x07, 0x54, "sml2_masked_ghoul"},
    {0x08, 0x4D, "sml2_ghost_goomba"},
    {0x08, 0x57, "sml2_boo"},

    /* Pumpkin Zone's fourth course uses the same chain-ball mapping and
     * its type 53 sprite is the witch on a broom. */
    {0x09, 0x4F, "sml2_spiked_ball_pumpkin"},
    {0x09, 0x53, "sml2_witch"},

    /* Mario Zone 1 has a classic Koopa sprite in its type-01 slot. */
    {0x0A, 0x01, "sml2_koopa_troopa"},

    /* Mario Zone 2's course-specific type 4D composes into the bear sprite. */
    {0x0B, 0x4D, "sml2_bear"},

    /* Mario Zone 3 type 48 uses the toy-soldier sprite. */
    {0x0C, 0x48, "sml2_kiddokatto"},

    /* Mario Zone 4's course-specific type 4D is the pig-like boss sprite. */
    {0x0D, 0x4D, "sml2_three_little_pigs"},

    /* Space Zone 1: the face-like actor is the Floating Face, the small
     * UFO-shaped sprite is Poro, and the satellite-with-orbiting-dot sprite
     * is Satellite. */
    {0x12, 0x4D, "sml2_floating_face"},
    {0x12, 0x58, "sml2_poro"},
    {0x12, 0x5A, "sml2_satellite"},

    /* Space Zone 2 contains the star enemy under two object IDs which share
     * the same composed sprite map. */
    {0x13, 0x5E, "sml2_star_enemy"},
    {0x13, 0x5F, "sml2_star_enemy"},

    /* Macro Zone courses use the same classic enemy mappings in their own
     * course tables. Sewer Rat's drill-nosed sprite is type 42 in course 16. */
    {0x16, 0x01, "sml2_koopa_troopa"},
    {0x16, 0x08, "sml2_paragoomba"},
    {0x16, 0x09, "sml2_goomba"},
    {0x16, 0x0C, "sml2_piranha_plant"},
    {0x16, 0x42, "sml2_sewer_rat"},
    {0x17, 0x01, "sml2_koopa_troopa"},
    {0x17, 0x08, "sml2_mechanical_fist"},
    {0x17, 0x09, "sml2_goomba"},
    {0x17, 0x0C, "sml2_piranha_plant"},
    {0x17, 0x0D, "sml2_fire_piranha_plant"},

    /* Space Zone boss course: type 67 maps to the Tatanga/ship sprite. */
    {0x18, 0x67, "sml2_tatanga"},
};

static const char *sml2_museum_enemy_id(uint8_t level, uint8_t object_type)
{
    for (size_t i = 0; i < sizeof sml2_museum_enemy_map / sizeof sml2_museum_enemy_map[0]; ++i) {
        if (sml2_museum_enemy_map[i].level == level &&
            sml2_museum_enemy_map[i].object_type == object_type)
            return sml2_museum_enemy_map[i].museum_id;
    }
    return NULL;
}

/* The active marker is set to 2 once an object has been initialized and has
 * entered the live actor list. The captured v7 trace confirms byte +10 is Y
 * and byte +11 is X; +9 is not a coordinate. Require a visible vertical
 * position, but allow X values beyond 159 because PipeClean's SML2 widescreen
 * camera can expose actors to the right of the original Game Boy viewport. */
static void sml2_museum_observe_visible_enemies(uint8_t level)
{
    if (!cart_ram || level > 0x1Fu ||
        !sml2_museum_mode_is_active(rd8(0xFF9Bu)))
        return;

    for (int slot = 0; slot < 16; ++slot) {
        const uint16_t base = (uint16_t)(0xAD00u + (unsigned)slot * 0x20u);
        if (sml2_ram_read((uint16_t)(base + 8u)) != 0x02u) continue;

        const uint8_t y = sml2_ram_read((uint16_t)(base + 10u));
        if (y < 0x08u || y > 0xA0u) continue;

        /* The live actor's +5 field changes during transitions (for example,
         * a collected item can change its runtime type). Its source pointer
         * addresses descriptor byte +2, so +3 from that pointer is the
         * original object ID used by the course spawn list. */
        const uint16_t source_ref = (uint16_t)(
            ((uint16_t)sml2_ram_read((uint16_t)(base + 6u)) << 8) |
            sml2_ram_read((uint16_t)(base + 7u)));
        /* Valid source refs point to descriptor byte +2 in the expanded AB06
         * spawn list. Requiring the exact six-byte stride rejects stale actor
         * records and arbitrary pointers before reading the source object ID. */
        if (source_ref < 0xAB08u || source_ref > 0xACFAu ||
            ((source_ref - 0xAB08u) % 6u) != 0)
            continue;
        const uint8_t x = sml2_ram_read((uint16_t)(base + 11u));
        if (x < 0x08u || x > 0xF0u) continue;
        const uint8_t object_type = sml2_ram_read((uint16_t)(source_ref + 3u));
        const char *museum_id = sml2_museum_enemy_id(level, object_type);
        if (!museum_id) continue;

        if (museum_discover_entry(GAME_SML2, museum_id) && events_log) {
            fprintf(stderr,
                    "[museum] SML2 enemy encounter level=%02X slot=%02d type=%02X -> %s\n",
                    level, slot, object_type, museum_id);
        }
    }
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

    /* Encounter discoveries are independent from kill-count milestones, so
     * walking past or seeing an enemy is sufficient to add its specimen. */
    const uint8_t level = sml2_ram_read(0xA269);
    sml2_museum_observe_visible_enemies(level);

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
/* Avoid repeating static ROM sprite lookup evidence for the same type/profile. */
static uint8_t sml2_actor_trace_rom_type_seen[0x20][0x100];
static void sml2_actor_trace_sprite_mapping(const char *kind, int mode, int level,
                                            int slot, uint16_t record_addr,
                                            uint16_t source_ref, uint8_t object_type,
                                            int observed_db);
static void sml2_actor_trace_dump_rom_level_lists(void);

/* Render the 128 object-tile IDs used by SML2's sprite maps.
 * IDs 80-AF use the shared bank-1B tile set, B0-E7 use the selected course's
 * SPRITES_TILES entry, and E8-FF use the leading 24 tiles from bank 7:6A00.
 * The separate HEADER_TILES table is for map graphics, not enemy sprites. */
static void sml2_actor_trace_hex_digit(SDL_Surface *surface, int x, int y,
                                       unsigned digit, Uint32 color)
{
    static const uint16_t glyphs[16] = {
        0x7B6F, 0x2492, 0x73E7, 0x73CF,
        0x5BC9, 0x79CF, 0x79EF, 0x7249,
        0x7BEF, 0x7BCF, 0x7BED, 0x6BAE,
        0x7927, 0x6B6E, 0x79E7, 0x79E4
    };
    const uint16_t bits = glyphs[digit & 15u];
    for (int gy = 0; gy < 5; ++gy) {
        for (int gx = 0; gx < 3; ++gx) {
            if ((bits & (uint16_t)(1u << (14 - (gy * 3 + gx)))) == 0)
                continue;
            for (int sy = 0; sy < 2; ++sy) {
                Uint32 *row = (Uint32 *)((Uint8 *)surface->pixels +
                    (size_t)(y + gy * 2 + sy) * (size_t)surface->pitch);
                row[x + gx * 2] = color;
                row[x + gx * 2 + 1] = color;
            }
        }
    }
}

static int sml2_actor_trace_sprite_tile_source_table(int base,
                                                      uint8_t banks[32],
                                                      uint16_t addresses[32])
{
    const CartInfo *info = cart_info();
    /* LevelEnemySets is in fixed ROM bank 0 at $362A, not bank $1B. */
    if (!rom || !info || info->rom_banks <= 0 ||
        0x1B >= info->rom_banks || base < 0 || base + 32 * 4 > 0x4000)
        return -1;

    int valid = 0;
    for (int level = 0; level < 32; ++level) {
        const size_t off = (size_t)base + (size_t)level * 4u;
        const uint8_t bank = rom[off];
        const uint16_t address = (uint16_t)(
            (uint16_t)rom[off + 2u] | ((uint16_t)rom[off + 3u] << 8));
        banks[level] = bank;
        addresses[level] = address;
        /* 56 tiles occupy 0x380 bytes and must fit in the bank's 0x4000-0x7FFF
         * CPU window. */
        if (bank < info->rom_banks && address >= 0x4000u &&
            address <= 0x7C80u)
            ++valid;
    }
    return valid;
}

static void sml2_actor_trace_dump_sprite_atlas(void)
{
    if (!sml2_actor_trace_file) return;
    if (!rom) {
        fprintf(sml2_actor_trace_file,
                "sprite_atlas skipped reason=rom_pointer_unavailable\\n");
        fflush(sml2_actor_trace_file);
        return;
    }

    /* The original SML2 disassembly places LevelEnemySets at ROM0:$362A.
     * Each course entry is bank, unused byte, and a banked graphics address. */
    uint8_t banks_a[32] = {0}, banks_b[32] = {0};
    uint16_t addrs_a[32] = {0}, addrs_b[32] = {0};
    const int valid_a = sml2_actor_trace_sprite_tile_source_table(
        0x362A, banks_a, addrs_a);
    const int valid_b = sml2_actor_trace_sprite_tile_source_table(
        0x362D, banks_b, addrs_b);
    const int table_base = valid_b > valid_a ? 0x362D : 0x362A;
    const int valid = valid_b > valid_a ? valid_b : valid_a;
    uint8_t *banks = table_base == 0x362D ? banks_b : banks_a;
    uint16_t *addresses = table_base == 0x362D ? addrs_b : addrs_a;

    /* Keep the diagnostic image even when the source table does not pass
     * the confidence threshold. Invalid courses are drawn as grey panels,
     * while usable course tables still render their real tile graphics. */
    if (valid < 24) {
        fprintf(sml2_actor_trace_file,
                "sprite_atlas warning=source_table_below_threshold valid_362A=%d valid_362D=%d selected=%04X; rendering_valid_courses_only\\n",
                valid_a, valid_b, table_base);
        fflush(sml2_actor_trace_file);
    }

    enum {
        TILE_SCALE = 2,
        TILES_WIDE = 16,
        TILES_HIGH = 8,
        PANEL_W = TILES_WIDE * 8 * TILE_SCALE,
        PANEL_H = TILES_HIGH * 8 * TILE_SCALE + 12,
        PANEL_GAP = 8,
        PANELS_WIDE = 8,
        ATLAS_W = PANELS_WIDE * PANEL_W + (PANELS_WIDE - 1) * PANEL_GAP,
        ATLAS_H = 4 * PANEL_H + 3 * PANEL_GAP
    };

    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
        0, ATLAS_W, ATLAS_H, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) {
        fprintf(sml2_actor_trace_file,
                "sprite_atlas skipped reason=surface_create_failed error=%s\n",
                SDL_GetError());
        fflush(sml2_actor_trace_file);
        return;
    }

    const Uint32 bg = SDL_MapRGBA(surface->format, 34, 34, 34, 255);
    const Uint32 grid = SDL_MapRGBA(surface->format, 72, 72, 72, 255);
    const Uint32 label = SDL_MapRGBA(surface->format, 245, 245, 245, 255);
    const Uint32 shade[4] = {
        SDL_MapRGBA(surface->format, 238, 238, 238, 255),
        SDL_MapRGBA(surface->format, 166, 166, 166, 255),
        SDL_MapRGBA(surface->format, 92, 92, 92, 255),
        SDL_MapRGBA(surface->format, 18, 18, 18, 255)
    };
    SDL_FillRect(surface, NULL, bg);

    for (int level = 0; level < 32; ++level) {
        const int panel_x = (level % PANELS_WIDE) * (PANEL_W + PANEL_GAP);
        const int panel_y = (level / PANELS_WIDE) * (PANEL_H + PANEL_GAP);
        sml2_actor_trace_hex_digit(surface, panel_x + 4, panel_y + 1,
                                   (unsigned)level >> 4, label);
        sml2_actor_trace_hex_digit(surface, panel_x + 12, panel_y + 1,
                                   (unsigned)level & 0x0Fu, label);

        const uint8_t course_bank = banks[level];
        const uint16_t course_address = addresses[level];
        if (course_bank >= cart_info()->rom_banks ||
            course_address < 0x4000u || course_address > 0x7C80u) {
            SDL_Rect rect = {panel_x, panel_y + 12, PANEL_W, PANEL_H - 12};
            SDL_FillRect(surface, &rect, grid);
            continue;
        }

        /* The displayed raw sprite tile IDs 80-FF resolve to cache indices
         * 0-127. Assemble those tiles from the same three source ranges used
         * by MooJelly's renderer, so the map's piece indices match this atlas. */
        for (int tile_y = 0; tile_y < TILES_HIGH; ++tile_y) {
            for (int tile_x = 0; tile_x < TILES_WIDE; ++tile_x) {
                const unsigned cache_tile =
                    (unsigned)(tile_y * TILES_WIDE + tile_x);
                int source_bank;
                uint16_t source_address;
                if (cache_tile < 48u) {
                    source_bank = 0x1B;
                    source_address = (uint16_t)(0x7000u + cache_tile * 16u);
                } else if (cache_tile < 104u) {
                    source_bank = course_bank;
                    source_address = (uint16_t)(
                        course_address + (cache_tile - 48u) * 16u);
                } else {
                    source_bank = 7;
                    source_address = (uint16_t)(
                        0x6A00u + (cache_tile - 104u) * 16u);
                }
                if (source_bank >= cart_info()->rom_banks ||
                    source_address < 0x4000u || source_address >= 0x8000u)
                    continue;

                for (int py = 0; py < 8; ++py) {
                    const uint8_t lo = cart_rom_read_bank(
                        source_bank, (uint16_t)(source_address + (unsigned)py * 2u));
                    const uint8_t hi = cart_rom_read_bank(
                        source_bank, (uint16_t)(source_address + (unsigned)py * 2u + 1u));
                    for (int px = 0; px < 8; ++px) {
                        const unsigned bit = (unsigned)(7 - px);
                        const unsigned value = ((lo >> bit) & 1u) |
                            (((hi >> bit) & 1u) << 1);
                        const int out_x = panel_x +
                            (tile_x * 8 + px) * TILE_SCALE;
                        const int out_y = panel_y + 12 +
                            (tile_y * 8 + py) * TILE_SCALE;
                        for (int sy = 0; sy < TILE_SCALE; ++sy) {
                            Uint32 *row = (Uint32 *)((Uint8 *)surface->pixels +
                                (size_t)(out_y + sy) * (size_t)surface->pitch);
                            for (int sx = 0; sx < TILE_SCALE; ++sx)
                                row[out_x + sx] = shade[value];
                        }
                    }
                }
            }
        }

        for (int gx = 0; gx <= TILES_WIDE; ++gx) {
            SDL_Rect line = {
                panel_x + gx * 8 * TILE_SCALE,
                panel_y + 12,
                1,
                TILES_HIGH * 8 * TILE_SCALE
            };
            SDL_FillRect(surface, &line, grid);
        }
        for (int gy = 0; gy <= TILES_HIGH; ++gy) {
            SDL_Rect line = {
                panel_x,
                panel_y + 12 + gy * 8 * TILE_SCALE,
                PANEL_W,
                1
            };
            SDL_FillRect(surface, &line, grid);
        }
    }

    const char *base = settings_dir();
    const size_t base_len = base ? strlen(base) : 0;
    const char *sep = (base_len &&
        (base[base_len - 1] == '/' || base[base_len - 1] == '\\')) ? "" : "/";
    char path[1200];
    const int n = snprintf(path, sizeof path,
        "%s%spipeclean-sml2-sprite-atlas.bmp", base ? base : ".", sep);
    if (n < 0 || (size_t)n >= sizeof path) {
        fprintf(sml2_actor_trace_file,
                "sprite_atlas skipped reason=path_too_long\n");
    } else if (SDL_SaveBMP(surface, path) != 0) {
        fprintf(sml2_actor_trace_file,
                "sprite_atlas skipped reason=save_failed error=%s\n",
                SDL_GetError());
    } else {
        fprintf(sml2_actor_trace_file,
                "sprite_atlas path=%s table=ROM0:%04X valid_levels=%d panels=32 "
                "layout=levels_00_to_1F_row_major tiles=80_to_FF grid=16x8 "
                "scale=2 tile_bytes=2bpp_grayscale\n",
                path, table_base, valid);
    }
    SDL_FreeSurface(surface);
    fflush(sml2_actor_trace_file);
}

/* Render actual composed sprite mappings, not just the source tile sheet.
 * Each cell is labeled with level, raw object type, and lookup index (hex).
 * Keep every course/type pair because a course selects its own enemy graphics
 * set; the same raw type or mapping index may not look alike across courses. */
typedef struct {
    uint8_t level, object_type, lookup_id, course_bank;
    uint16_t map_ptr, course_address;
} Sml2MappedSpriteEntry;

static int sml2_actor_trace_signed_offset(uint8_t value)
{
    return value < 0x80u ? (int)value : (int)value - 256;
}

static unsigned sml2_actor_trace_read_sprite_tile_pixel(
    int course_bank, uint16_t course_address, uint8_t tile,
    int px, int py, uint8_t attributes)
{
    if (tile < 0x80u) return 0;
    const unsigned cache_tile = (unsigned)(tile - 0x80u);
    int source_bank;
    uint16_t source_address;
    if (cache_tile < 48u) {
        source_bank = 0x1B;
        source_address = (uint16_t)(0x7000u + cache_tile * 16u);
    } else if (cache_tile < 104u) {
        source_bank = course_bank;
        source_address = (uint16_t)(course_address +
                                    (cache_tile - 48u) * 16u);
    } else {
        source_bank = 7;
        source_address = (uint16_t)(0x6A00u +
                                    (cache_tile - 104u) * 16u);
    }
    const CartInfo *info = cart_info();
    if (!info || source_bank < 0 || source_bank >= info->rom_banks ||
        source_address < 0x4000u || source_address >= 0x8000u)
        return 0;

    if (attributes & 0x20u) px = 7 - px;
    if (attributes & 0x40u) py = 7 - py;
    const uint16_t row_address = (uint16_t)(source_address + (unsigned)py * 2u);
    const uint8_t lo = cart_rom_read_bank(source_bank, row_address);
    const uint8_t hi = cart_rom_read_bank(source_bank, (uint16_t)(row_address + 1u));
    const unsigned bit = (unsigned)(7 - px);
    return ((lo >> bit) & 1u) | (((hi >> bit) & 1u) << 1);
}

static void sml2_actor_trace_draw_mapped_sprite(
    SDL_Surface *surface, int cell_x, int cell_y,
    const Sml2MappedSpriteEntry *entry, Uint32 shades[4],
    Uint32 background, unsigned *unsupported_tiles)
{
    uint8_t pieces[40][4];
    unsigned piece_count = 0;
    int terminated = 0;
    if (!entry || entry->map_ptr < 0x4000u || entry->map_ptr >= 0x8000u)
        return;

    for (unsigned i = 0; i < 40; ++i) {
        const unsigned address = (unsigned)entry->map_ptr + i * 4u;
        if (address + 3u >= 0x8000u) break;
        for (unsigned j = 0; j < 4; ++j)
            pieces[i][j] = cart_rom_read_bank(3, (uint16_t)(address + j));
        if (pieces[i][0] == 0x80u) {
            terminated = 1;
            break;
        }
        ++piece_count;
    }
    if (!terminated || piece_count == 0) return;

    int min_x = 127, min_y = 127, max_x = -128, max_y = -128;
    for (unsigned i = 0; i < piece_count; ++i) {
        const int y = sml2_actor_trace_signed_offset(pieces[i][0]);
        const int x = sml2_actor_trace_signed_offset(pieces[i][1]);
        if (x < min_x) min_x = x;
        if (y < min_y) min_y = y;
        if (x + 8 > max_x) max_x = x + 8;
        if (y + 8 > max_y) max_y = y + 8;
        if (pieces[i][2] < 0x80u && unsupported_tiles)
            ++*unsupported_tiles;
    }
    const int native_w = max_x - min_x;
    const int native_h = max_y - min_y;
    if (native_w <= 0 || native_h <= 0) return;

    enum { CELL_W = 128, CELL_H = 100, ART_TOP = 20, ART_H = 76, ART_W = 120 };
    int scale = 2;
    if (native_w * scale > ART_W || native_h * scale > ART_H) scale = 1;
    const int draw_w = native_w * scale;
    const int draw_h = native_h * scale;
    const int origin_x = cell_x + (CELL_W - draw_w) / 2;
    const int origin_y = cell_y + ART_TOP + (ART_H - draw_h) / 2;

    for (unsigned i = 0; i < piece_count; ++i) {
        const uint8_t tile = pieces[i][2];
        if (tile < 0x80u) continue;
        const int piece_y = sml2_actor_trace_signed_offset(pieces[i][0]);
        const int piece_x = sml2_actor_trace_signed_offset(pieces[i][1]);
        const int tile_x = origin_x + (piece_x - min_x) * scale;
        const int tile_y = origin_y + (piece_y - min_y) * scale;
        for (int py = 0; py < 8; ++py) {
            for (int px = 0; px < 8; ++px) {
                const unsigned color = sml2_actor_trace_read_sprite_tile_pixel(
                    entry->course_bank, entry->course_address, tile,
                    px, py, pieces[i][3]);
                if (color == 0) continue; /* leave transparent pixels as the panel background */
                for (int sy = 0; sy < scale; ++sy) {
                    const int out_y = tile_y + py * scale + sy;
                    if (out_y < cell_y + ART_TOP || out_y >= cell_y + ART_TOP + ART_H)
                        continue;
                    Uint32 *row = (Uint32 *)((Uint8 *)surface->pixels +
                        (size_t)out_y * (size_t)surface->pitch);
                    for (int sx = 0; sx < scale; ++sx) {
                        const int out_x = tile_x + px * scale + sx;
                        if (out_x < cell_x + 4 || out_x >= cell_x + CELL_W - 4)
                            continue;
                        row[out_x] = shades[color];
                    }
                }
            }
        }
    }
    (void)background;
}

static void sml2_actor_trace_dump_mapped_sprite_atlas(void)
{
    if (!sml2_actor_trace_file || !rom) return;
    const CartInfo *info = cart_info();
    if (!info || info->rom_banks <= 0 || info->rom_banks <= 0x1B) {
        fprintf(sml2_actor_trace_file,
                "mapped_sprite_atlas skipped reason=required_rom_bank_unavailable\n");
        fflush(sml2_actor_trace_file);
        return;
    }

    uint8_t banks_a[32] = {0}, banks_b[32] = {0};
    uint16_t addrs_a[32] = {0}, addrs_b[32] = {0};
    const int valid_a = sml2_actor_trace_sprite_tile_source_table(0x362A,
                                                                  banks_a, addrs_a);
    const int valid_b = sml2_actor_trace_sprite_tile_source_table(0x362D,
                                                                  banks_b, addrs_b);
    const int table_base = valid_b > valid_a ? 0x362D : 0x362A;
    uint8_t *course_banks = table_base == 0x362D ? banks_b : banks_a;
    uint16_t *course_addrs = table_base == 0x362D ? addrs_b : addrs_a;

    Sml2MappedSpriteEntry entries[512];
    size_t entry_count = 0;
    uint8_t seen[0x20][256] = {{0}};
    unsigned invalid_lookups = 0;
    for (int level = 0; level < 0x20; ++level) {
        const uint16_t list_table = (uint16_t)(0x6037u + (unsigned)level * 2u);
        const uint16_t list_ptr = (uint16_t)(
            (uint16_t)cart_rom_read_bank(3, list_table) |
            ((uint16_t)cart_rom_read_bank(3, (uint16_t)(list_table + 1u)) << 8));
        if (list_ptr < 0x4000u || list_ptr >= 0x8000u) continue;

        for (unsigned i = 0; i < 128u; ++i) {
            const unsigned raw_address = (unsigned)list_ptr + i * 3u;
            if (raw_address + 2u >= 0x8000u) break;
            const uint8_t a = cart_rom_read_bank(3, (uint16_t)raw_address);
            if (a == 0xFFu) break;
            const uint8_t b = cart_rom_read_bank(3, (uint16_t)(raw_address + 1u));
            const uint8_t object_type = (uint8_t)(
                ((a & 0x10u) << 2) | ((a & 0xE0u) >> 2) | ((b & 0xE0u) >> 5));
            const unsigned profile = level < 0x0A ? 0u : 1u;
            if (seen[level][object_type]) continue;
            seen[level][object_type] = 1;

            const uint16_t type_table = (uint16_t)(profile == 0 ? 0x4B61u : 0x4BC1u);
            const uint8_t lookup_id = cart_rom_read_bank(2,
                (uint16_t)(type_table + object_type));
            const unsigned pointer_count = profile == 0 ? 0xA8u : 0xE4u;
            if (lookup_id >= pointer_count) {
                ++invalid_lookups;
                continue;
            }
            const uint16_t pointer_table = (uint16_t)(profile == 0 ? 0x40B1u : 0x4F11u);
            const uint16_t pointer_entry = (uint16_t)(pointer_table + lookup_id * 2u);
            const uint16_t map_ptr = (uint16_t)(
                (uint16_t)cart_rom_read_bank(3, pointer_entry) |
                ((uint16_t)cart_rom_read_bank(3, (uint16_t)(pointer_entry + 1u)) << 8));
            if (map_ptr < 0x4000u || map_ptr >= 0x8000u) continue;
            if (entry_count >= sizeof entries / sizeof entries[0]) {
                fprintf(sml2_actor_trace_file,
                        "mapped_sprite_atlas warning=entry_capacity_reached\n");
                break;
            }
            entries[entry_count++] = (Sml2MappedSpriteEntry){
                (uint8_t)level, object_type, lookup_id,
                course_banks[level], map_ptr, course_addrs[level]
            };
        }
    }

    if (entry_count == 0) {
        fprintf(sml2_actor_trace_file,
                "mapped_sprite_atlas skipped reason=no_static_object_mappings valid_362A=%d valid_362D=%d\n",
                valid_a, valid_b);
        fflush(sml2_actor_trace_file);
        return;
    }

    enum { CELL_W = 128, CELL_H = 100, COLUMNS = 8 };
    const size_t rows = (entry_count + COLUMNS - 1u) / COLUMNS;
    const int width = COLUMNS * CELL_W;
    if (rows > (size_t)(32768 / CELL_H)) {
        fprintf(sml2_actor_trace_file,
                "mapped_sprite_atlas skipped reason=dimensions_too_large entries=%u\n",
                (unsigned)entry_count);
        fflush(sml2_actor_trace_file);
        return;
    }
    const int height = (int)rows * CELL_H;
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
        0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) {
        fprintf(sml2_actor_trace_file,
                "mapped_sprite_atlas skipped reason=surface_create_failed error=%s\n",
                SDL_GetError());
        fflush(sml2_actor_trace_file);
        return;
    }

    const Uint32 background = SDL_MapRGBA(surface->format, 34, 34, 34, 255);
    const Uint32 grid = SDL_MapRGBA(surface->format, 72, 72, 72, 255);
    const Uint32 label = SDL_MapRGBA(surface->format, 245, 245, 245, 255);
    Uint32 shades[4] = {
        background,
        SDL_MapRGBA(surface->format, 238, 238, 238, 255),
        SDL_MapRGBA(surface->format, 166, 166, 166, 255),
        SDL_MapRGBA(surface->format, 92, 92, 92, 255)
    };
    SDL_FillRect(surface, NULL, background);
    unsigned unsupported_tiles = 0;

    for (size_t i = 0; i < entry_count; ++i) {
        const int cell_x = (int)(i % COLUMNS) * CELL_W;
        const int cell_y = (int)(i / COLUMNS) * CELL_H;
        const Sml2MappedSpriteEntry *entry = &entries[i];
        /* Header digits are level, raw type, and lookup ID, in that order. */
        sml2_actor_trace_hex_digit(surface, cell_x + 4, cell_y + 1,
                                   entry->level >> 4, label);
        sml2_actor_trace_hex_digit(surface, cell_x + 12, cell_y + 1,
                                   entry->level & 0x0Fu, label);
        sml2_actor_trace_hex_digit(surface, cell_x + 34, cell_y + 1,
                                   entry->object_type >> 4, label);
        sml2_actor_trace_hex_digit(surface, cell_x + 42, cell_y + 1,
                                   entry->object_type & 0x0Fu, label);
        sml2_actor_trace_hex_digit(surface, cell_x + 64, cell_y + 1,
                                   entry->lookup_id >> 4, label);
        sml2_actor_trace_hex_digit(surface, cell_x + 72, cell_y + 1,
                                   entry->lookup_id & 0x0Fu, label);
        SDL_Rect border = {cell_x, cell_y, CELL_W, CELL_H};
        SDL_FillRect(surface, &border, grid);
        SDL_Rect inner = {cell_x + 1, cell_y + 1, CELL_W - 2, CELL_H - 2};
        SDL_FillRect(surface, &inner, background);
        /* Redraw the numbers over the interior fill. */
        sml2_actor_trace_hex_digit(surface, cell_x + 4, cell_y + 1,
                                   entry->level >> 4, label);
        sml2_actor_trace_hex_digit(surface, cell_x + 12, cell_y + 1,
                                   entry->level & 0x0Fu, label);
        sml2_actor_trace_hex_digit(surface, cell_x + 34, cell_y + 1,
                                   entry->object_type >> 4, label);
        sml2_actor_trace_hex_digit(surface, cell_x + 42, cell_y + 1,
                                   entry->object_type & 0x0Fu, label);
        sml2_actor_trace_hex_digit(surface, cell_x + 64, cell_y + 1,
                                   entry->lookup_id >> 4, label);
        sml2_actor_trace_hex_digit(surface, cell_x + 72, cell_y + 1,
                                   entry->lookup_id & 0x0Fu, label);
        SDL_Rect sep1 = {cell_x + 26, cell_y + 1, 1, 10};
        SDL_Rect sep2 = {cell_x + 56, cell_y + 1, 1, 10};
        SDL_FillRect(surface, &sep1, grid);
        SDL_FillRect(surface, &sep2, grid);
        sml2_actor_trace_draw_mapped_sprite(surface, cell_x, cell_y,
                                            entry, shades, background,
                                            &unsupported_tiles);
    }

    const char *base = settings_dir();
    const size_t base_len = base ? strlen(base) : 0;
    const char *sep = (base_len &&
        (base[base_len - 1] == '/' || base[base_len - 1] == '\\')) ? "" : "/";
    char path[1200];
    const int n = snprintf(path, sizeof path,
        "%s%spipeclean-sml2-mapped-sprite-atlas.bmp", base ? base : ".", sep);
    if (n < 0 || (size_t)n >= sizeof path) {
        fprintf(sml2_actor_trace_file,
                "mapped_sprite_atlas skipped reason=path_too_long\n");
    } else if (SDL_SaveBMP(surface, path) != 0) {
        fprintf(sml2_actor_trace_file,
                "mapped_sprite_atlas skipped reason=save_failed error=%s\n",
                SDL_GetError());
    } else {
        fprintf(sml2_actor_trace_file,
                "mapped_sprite_atlas path=%s entries=%u invalid_lookups=%u "
                "unsupported_low_tile_refs=%u columns=8 cell=128x100 "
                "labels=level,object_type,lookup_id\n",
                path, (unsigned)entry_count, invalid_lookups, unsupported_tiles);
    }
    SDL_FreeSurface(surface);
    fflush(sml2_actor_trace_file);
}

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
    memset(sml2_actor_trace_rom_type_seen, 0, sizeof sml2_actor_trace_rom_type_seen);
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
            "PipeClean SML2 live sprite actor trace v7\n"
            "Each actor record is 0x20 bytes; the active marker is at record offset +8.\n"
            "Record bases begin at AD00; active markers begin at AD08.\n"
            "The source descriptor pointer is stored high-byte first at record offsets +6/+7.\n"
            "It points at descriptor byte +2; descriptor bytes are read from pointer - 2.\n"
            "All 32 course spawn lists are decoded from the ROM when the trace opens.\n"
            "The live AB06 list is also logged in gameplay/pause/death modes.\n"
            "sprite_map rows decode the game ROM type-to-sprite and sprite-pointer tables.\n"
            "rom_spawn rows store the ROM source address in record_addr; live actors use RAM.\n"
            "Raw type values and sprite indices are evidence, not species names.\n"
            "A companion BMP atlas renders the actual sprite tiles for all 32 courses.\n"
            "spawn frame level slot record_addr source_ref source_addr descriptor[00..05]\n"
            "actor frame level slot record_addr active source_ref field05 field0B record[00..1F]\n"
            "sprite_map kind frame mode level slot record_addr source_ref object_type lookup_id observed_db map_ptr pieces[y,x,tile,attr;...]\n");
    fflush(sml2_actor_trace_file);
    sml2_actor_trace_dump_rom_level_lists();
    sml2_actor_trace_dump_sprite_atlas();
    sml2_actor_trace_dump_mapped_sprite_atlas();
}

static void sml2_actor_trace_spawn_table(int mode, int level)
{
    /* $AB06 can contain stale or zeroed RAM during title/transition scenes. */
    if (!sml2_actor_trace_file || level < 0 || level >= 0x20 ||
        !sml2_museum_mode_is_active(mode)) return;

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

/* Decode the ROM's object-type -> mapping-index -> sprite-piece chain.
 * Fixed-bank reads avoid depending on the currently mapped CPU ROM bank. */
static void sml2_actor_trace_sprite_mapping(const char *kind, int mode, int level,
                                            int slot, uint16_t record_addr,
                                            uint16_t source_ref, uint8_t object_type,
                                            int observed_db)
{
    if (!sml2_actor_trace_file || level < 0 || level >= 0x20 ||
        object_type == 0xFFu)
        return;

    const uint16_t type_table = (uint16_t)(level < 0x0A ? 0x4B61u : 0x4BC1u);
    const uint8_t lookup_id = cart_rom_read_bank(2,
        (uint16_t)(type_table + object_type));
    const uint16_t pointer_table = (uint16_t)(level < 0x0A ? 0x40B1u : 0x4F11u);
    const unsigned pointer_count = level < 0x0A ? 0xA8u : 0xE4u;
    if (lookup_id >= pointer_count) {
        fprintf(sml2_actor_trace_file,
                "sprite_map kind=%s frame=%d mode=%02X level=%02X slot=%02d record_addr=%04X source_ref=%04X object_type=%02X lookup_id=%02X observed_db=out_of_range map_ptr=---- pieces=unavailable\n",
                kind ? kind : "unknown", frame_count, mode, level, slot,
                record_addr, source_ref, object_type, lookup_id);
        fflush(sml2_actor_trace_file);
        return;
    }
    const uint16_t pointer_entry = (uint16_t)(pointer_table + (uint16_t)lookup_id * 2u);
    const uint8_t lo = cart_rom_read_bank(3, pointer_entry);
    const uint8_t hi = cart_rom_read_bank(3, (uint16_t)(pointer_entry + 1u));
    const uint16_t map_ptr = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));

    fprintf(sml2_actor_trace_file,
            "sprite_map kind=%s frame=%d mode=%02X level=%02X slot=%02d record_addr=%04X source_ref=%04X object_type=%02X lookup_id=%02X observed_db=",
            kind ? kind : "unknown", frame_count, mode, level, slot,
            record_addr, source_ref, object_type, lookup_id);
    if (observed_db < 0) fputs("--", sml2_actor_trace_file);
    else fprintf(sml2_actor_trace_file, "%02X", (unsigned)observed_db & 0xFFu);
    fprintf(sml2_actor_trace_file, " map_ptr=%04X pieces=", map_ptr);

    if (map_ptr < 0x4000u || map_ptr >= 0x8000u) {
        fputs("unavailable\n", sml2_actor_trace_file);
        fflush(sml2_actor_trace_file);
        return;
    }

    int piece_count = 0;
    int terminated = 0;
    for (unsigned i = 0; i < 40; ++i) {
        const unsigned raw_addr = (unsigned)map_ptr + i * 4u;
        if (raw_addr + 3u >= 0x8000u) break;
        uint8_t piece[4];
        for (unsigned j = 0; j < 4; ++j)
            piece[j] = cart_rom_read_bank(3, (uint16_t)(raw_addr + j));
        if (piece[0] == 0x80u) {
            terminated = 1;
            break;
        }
        fprintf(sml2_actor_trace_file, "%s%02X,%02X,%02X,%02X",
                piece_count ? ";" : "", piece[0], piece[1], piece[2], piece[3]);
        ++piece_count;
    }
    if (!terminated) fprintf(sml2_actor_trace_file, "%struncated", piece_count ? ";" : "");
    if (piece_count == 0 && terminated) fputs("empty", sml2_actor_trace_file);
    fputc('\n', sml2_actor_trace_file);
    fflush(sml2_actor_trace_file);
}

/* The complete source placement lists are contiguous in bank 3. The table
 * at CPU $6037 holds one bank-3 pointer per course; each entry is three bytes
 * and 0xFF in byte zero terminates that course. Decode every list up front so
 * species mapping does not require visiting each course manually. */
static void sml2_actor_trace_dump_rom_level_lists(void)
{
    if (!sml2_actor_trace_file) return;

    const uint16_t pointer_table = 0x6037u;
    const unsigned max_entries = 128u;
    for (int level = 0; level < 0x20; ++level) {
        const uint16_t pointer_addr = (uint16_t)(pointer_table + (unsigned)level * 2u);
        const uint8_t lo = cart_rom_read_bank(3, pointer_addr);
        const uint8_t hi = cart_rom_read_bank(3, (uint16_t)(pointer_addr + 1u));
        const uint16_t list_addr = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
        if (list_addr < 0x4000u || list_addr >= 0x8000u) {
            fprintf(sml2_actor_trace_file,
                    "rom_spawn_list level=%02X pointer=%04X entries=0 status=invalid_pointer\n",
                    level, list_addr);
            continue;
        }

        unsigned count = 0;
        int terminated = 0;
        for (unsigned i = 0; i < max_entries; ++i) {
            const unsigned raw_addr = (unsigned)list_addr + i * 3u;
            if (raw_addr + 2u >= 0x8000u) break;
            const uint8_t a = cart_rom_read_bank(3, (uint16_t)raw_addr);
            if (a == 0xFFu) {
                terminated = 1;
                break;
            }
            ++count;
        }

        fprintf(sml2_actor_trace_file,
                "rom_spawn_list level=%02X pointer=%04X entries=%u terminated=%d\n",
                level, list_addr, count, terminated);
        for (unsigned i = 0; i < count; ++i) {
            const uint16_t raw_addr = (uint16_t)(list_addr + i * 3u);
            const uint8_t a = cart_rom_read_bank(3, raw_addr);
            const uint8_t b = cart_rom_read_bank(3, (uint16_t)(raw_addr + 1u));
            const uint8_t d = cart_rom_read_bank(3, (uint16_t)(raw_addr + 2u));
            const uint8_t object_type = (uint8_t)(
                ((a & 0x10u) << 2) | ((a & 0xE0u) >> 2) | ((b & 0xE0u) >> 5));
            fprintf(sml2_actor_trace_file,
                    "rom_spawn level=%02X index=%03u record_addr=%04X raw=%02X %02X %02X object_type=%02X\n",
                    level, i, raw_addr, a, b, d, object_type);

            const unsigned profile = level < 0x0A ? 0u : 1u;
            if (!sml2_actor_trace_rom_type_seen[level][object_type]) {
                sml2_actor_trace_rom_type_seen[level][object_type] = 1;
                sml2_actor_trace_sprite_mapping("rom_spawn", 0xFF, level, -1,
                                                raw_addr, 0, object_type, -1);
            }
        }
        if (!terminated)
            fprintf(sml2_actor_trace_file,
                    "rom_spawn_list_warning level=%02X pointer=%04X status=missing_terminator_or_limit\n",
                    level, list_addr);
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

    if (level > 0x1F) {
        if (diagnostic_tick)
            fprintf(sml2_actor_trace_file,
                    "diag frame=%d reason=invalid_level mode=%02X level=%02X\n",
                    frame_count, mode, level);
        return;
    }

    /* Avoid dumping stale/zeroed tables and actor RAM during title/transition
     * modes. Gameplay captures showed reliable actor records in mode 04. */
    if (!sml2_museum_mode_is_active(mode)) {
        if (diagnostic_tick)
            fprintf(sml2_actor_trace_file,
                    "diag frame=%d reason=nonstandard_mode_ignored mode=%02X level=%02X\n",
                    frame_count, mode, level);
        return;
    }

    sml2_actor_trace_spawn_table(mode, level);

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
            /* SML2 rebuilds its six-byte source-descriptor table at AB06.
             * The stored pointer targets entry byte +2, so it must resolve to
             * AB06 + n*6 + 2. The earlier 1002 values came from reading eight
             * bytes into an actor record and were not reliable pointer evidence. */
            const uint16_t source_addr = (uint16_t)(source_ref - 2u);
            const int source_is_entry =
                source_addr >= 0xAB06u && source_addr <= 0xACF8u &&
                ((source_addr - 0xAB06u) % 6u) == 0;
            if (source_ref >= 2u && source_is_entry) {
                uint8_t descriptor[6];
                for (unsigned i = 0; i < sizeof descriptor; ++i)
                    descriptor[i] = rd8((uint16_t)(source_addr + i));
                fprintf(sml2_actor_trace_file,
                        "spawn frame=%d mode=%02X level=%02X slot=%02d record_addr=%04X source_ref=%04X source_addr=%04X descriptor=",
                        frame_count, mode, level, slot, base, source_ref, source_addr);
                for (unsigned i = 0; i < sizeof descriptor; ++i)
                    fprintf(sml2_actor_trace_file, "%s%02X", i ? " " : "", descriptor[i]);
                fputc('\n', sml2_actor_trace_file);
            } else {
                fprintf(sml2_actor_trace_file,
                        "spawn frame=%d mode=%02X level=%02X slot=%02d record_addr=%04X source_ref=%04X descriptor=unavailable\n",
                        frame_count, mode, level, slot, base, source_ref);
            }
            if (source_ref >= 2u && source_is_entry) {
                sml2_actor_trace_sprite_mapping("spawn", mode, level, slot, base,
                                                source_ref, rd8((uint16_t)(source_addr + 5u)), -1);
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
        sml2_actor_trace_sprite_mapping("actor", mode, level, slot, base, source_ref,
                                        field05, field0B);
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