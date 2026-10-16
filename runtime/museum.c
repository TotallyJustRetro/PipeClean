#include "museum.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MUSEUM_MAX_PER_GAME 128
#define MUSEUM_PATH_MAX 1200

static MuseumEntry catalog[N_GAMES][MUSEUM_MAX_PER_GAME];
static unsigned char discovered[N_GAMES][MUSEUM_MAX_PER_GAME];
static int catalog_count[N_GAMES];
static int initialized;
static char storage_path[MUSEUM_PATH_MAX];
static char storage_error[160];

static const char *game_source(int game)
{
    switch (game) {
    case GAME_DRMARIO: return "https://www.mariowiki.com/Dr._Mario";
    case GAME_SML: return "https://www.mariowiki.com/Super_Mario_Land";
    case GAME_SML2: return "https://www.mariowiki.com/Super_Mario_Land_2:_6_Golden_Coins";
    case GAME_WARIO_SML3: return "https://www.mariowiki.com/Wario_Land:_Super_Mario_Land_3";
    case GAME_WARIO_LAND2_GB:
    case GAME_WARIO_LAND2_GBC: return "https://www.mariowiki.com/Wario_Land_II";
    case GAME_WARIO_LAND3_GBC: return "https://www.mariowiki.com/Wario_Land_3";
    default: return "https://www.mariowiki.com/Main_Page";
    }
}

static void add_entry(int game, const char *id, const char *name,
                      MuseumCategory category, const char *description,
                      const char *trigger)
{
    if (game < 0 || game >= N_GAMES || catalog_count[game] >= MUSEUM_MAX_PER_GAME)
        return;
    MuseumEntry *e = &catalog[game][catalog_count[game]++];
    memset(e, 0, sizeof *e);
    e->game = game;
    snprintf(e->id, sizeof e->id, "%s", id ? id : "");
    snprintf(e->name, sizeof e->name, "%s", name ? name : "");
    e->category = category;
    snprintf(e->description, sizeof e->description, "%s", description ? description : "");
    snprintf(e->source_url, sizeof e->source_url, "%s", game_source(game));
    snprintf(e->trigger_event, sizeof e->trigger_event, "%s", trigger ? trigger : "");
}

static void add_numbered(int game, const char *prefix, const char *label,
                         MuseumCategory category, const char *description, int count)
{
    for (int i = 1; i <= count; ++i) {
        char id[40], name[80];
        snprintf(id, sizeof id, "%s_%02d", prefix, i);
        snprintf(name, sizeof name, "%s %02d", label, i);
        add_entry(game, id, name, category, description, "");
    }
}

typedef struct { const char *name; const char *level; const char *chest; } Wl3TreasureDef;
static const Wl3TreasureDef wl3_treasures[100] = {
    {"Axe","N1 Out of the Woods","Gray"},
    {"Blue Tablet Half","N2 The Peaceful Village","Gray"},
    {"Green Tablet Half","N3 The Vast Plain","Gray"},
    {"Top Half of Scroll","W1 Desert Ruins","Gray"},
    {"Bottom Half of Scroll","W1 Desert Ruins","Red"},
    {"Blue Overalls","W2 The Volcano's Base","Gray"},
    {"Jar","N1 Out of the Woods","Red"},
    {"Screw","W3 The Pool of Rain","Gray"},
    {"Cog","W4 A Town in Chaos","Gray"},
    {"First Music Box","S1 The Grasslands","Gray"},
    {"Seed","S2 The Big Bridge","Gray"},
    {"Red Skull","N3 The Vast Plain","Green"},
    {"Blue Skull","W3 The Pool of Rain","Red"},
    {"Flippers","S3 Tower of Revival","Gray"},
    {"Paper Fan","W3 The Pool of Rain","Green"},
    {"Flute","E1 The Stagnant Swamp","Gray"},
    {"Ice Spellbook","N2 The Peaceful Village","Red"},
    {"Lightning Wand","S1 The Grasslands","Green"},
    {"Spike Helmet","E2 The Frigid Sea","Gray"},
    {"Second Music Box","W4 A Town in Chaos","Red"},
    {"Lightning Wand","N4 Bank of the Wild River","Gray"},
    {"Lightning Spellbook","N5 The Tidal Coast","Gray"},
    {"Red Glove","S4 The Steep Canyon","Gray"},
    {"Foot Weight","N5 The Tidal Coast","Red"},
    {"Leaf","W2 The Volcano's Base","Blue"},
    {"Conch","S4 The Steep Canyon","Red"},
    {"Pinecone","E1 The Stagnant Swamp","Red"},
    {"Left Blue Snake Eye","W6 The West Crater","Gray"},
    {"Right Blue Snake Eye","E4 The Colossal Hole","Gray"},
    {"Garlic","S3 Tower of Revival","Red"},
    {"Third Music Box","N6 Sea Turtle Rock","Gray"},
    {"Water Wand","S5 Cave of Flames","Gray"},
    {"Lantern","S2 The Big Bridge","Blue"},
    {"Lantern Fire","E2 The Frigid Sea","Green"},
    {"Red Overalls","E3 Castle of Illusions","Gray"},
    {"Left Half of Sun Stone","N6 Sea Turtle Rock","Green"},
    {"Right Half of Sun Stone","W1 Desert Ruins","Blue"},
    {"Winged Boot","E4 The Colossal Hole","Green"},
    {"Fourth Music Box","E1 The Stagnant Swamp","Green"},
    {"Detonator","W5 Beneath the Waves","Gray"},
    {"Sack","S5 Cave of Flames","Green"},
    {"Whirlwind","E1 The Stagnant Swamp","Blue"},
    {"Super Flippers","N1 Out of the Woods","Green"},
    {"Little Knitter","N4 Bank of the Wild River","Green"},
    {"Map","S3 Tower of Revival","Green"},
    {"Brown Glove","E6 The East Crater","Gray"},
    {"Sand","E3 Castle of Illusions","Green"},
    {"Fifth Music Box","N1 Out of the Woods","Blue"},
    {"Blue Crayon","N3 The Vast Plain","Red"},
    {"Wheels","S1 The Grasslands","Red"},
    {"Blue Bracelet","W2 The Volcano's Base","Red"},
    {"Cyan Crayon","S2 The Big Bridge","Red"},
    {"Chalice","N2 The Peaceful Village","Green"},
    {"Crown","W2 The Volcano's Base","Green"},
    {"Pink Crayon","W1 Desert Ruins","Green"},
    {"Teapot","E2 The Frigid Sea","Red"},
    {"Pikachu Doll","S2 The Big Bridge","Green"},
    {"Magnifying Glass","N4 Bank of the Wild River","Red"},
    {"Pump","E4 The Colossal Hole","Red"},
    {"Toy Rocket","N4 Bank of the Wild River","Blue"},
    {"Yellow Crayon","W3 The Pool of Rain","Blue"},
    {"Saber","W6 The West Crater","Red"},
    {"Day/Night Tablet","N2 The Peaceful Village","Blue"},
    {"UFO","E2 The Frigid Sea","Blue"},
    {"Torch","E3 Castle of Illusions","Red"},
    {"Pocket Mirror","E7 Forest of Fear","Gray"},
    {"Valve","E5 The Warped Void","Gray"},
    {"Remote Control","E7 Forest of Fear","Green"},
    {"Brown Crayon","E5 The Warped Void","Red"},
    {"Blood","E5 The Warped Void","Green"},
    {"Red Keycard","E7 Forest of Fear","Red"},
    {"Blue Keycard","E7 Forest of Fear","Blue"},
    {"Heart","E5 The Warped Void","Blue"},
    {"Toy Car","S5 Cave of Flames","Red"},
    {"Toy Train","E4 The Colossal Hole","Blue"},
    {"Telephone","S4 The Steep Canyon","Green"},
    {"Red Crayon","W5 Beneath the Waves","Red"},
    {"Mechanical Fan","N5 The Tidal Coast","Green"},
    {"Fruit","W4 A Town in Chaos","Blue"},
    {"Green Bracelet","N5 The Tidal Coast","Blue"},
    {"Doll","S1 The Grasslands","Blue"},
    {"Right Golden Snake Eye","W4 A Town in Chaos","Green"},
    {"Left Golden Snake Eye","N6 Sea Turtle Rock","Red"},
    {"Key","S3 Tower of Revival","Blue"},
    {"Green Crayon","S6 Above the Clouds","Gray"},
    {"Fire Extinguisher","S6 Above the Clouds","Red"},
    {"Diamond","E6 The East Crater","Red"},
    {"Box","W6 The West Crater","Green"},
    {"Dust Spray","E3 Castle of Illusions","Blue"},
    {"Spade","W6 The West Crater","Blue"},
    {"Red Chemicals","W5 Beneath the Waves","Green"},
    {"Blue Chemicals","S4 The Steep Canyon","Blue"},
    {"Red Bracelet","N3 The Vast Plain","Blue"},
    {"Club","W5 Beneath the Waves","Blue"},
    {"Scissors","S5 Cave of Flames","Blue"},
    {"Drill","S6 Above the Clouds","Green"},
    {"Infrared Goggles","E6 The East Crater","Green"},
    {"Gong","N6 Sea Turtle Rock","Blue"},
    {"Pickaxe","S6 Above the Clouds","Blue"},
    {"Gloom","E6 The East Crater","Blue"}
};

static void add_wl3_treasures(void)
{
    for (int i = 0; i < 100; ++i) {
        char id[40], description[200];
        snprintf(id, sizeof id, "wl3_treasure_%03d", i + 1);
        snprintf(description, sizeof description, "Found in %s, from the %s chest.",
                 wl3_treasures[i].level, wl3_treasures[i].chest);
        add_entry(GAME_WARIO_LAND3_GBC, id, wl3_treasures[i].name,
                  MUSEUM_CATEGORY_TREASURE, description, "");
        MuseumEntry *e = &catalog[GAME_WARIO_LAND3_GBC][catalog_count[GAME_WARIO_LAND3_GBC] - 1];
        snprintf(e->source_url, sizeof e->source_url,
                 "%s", "https://www.mariowiki.com/Wario_Land_III#Treasures");
    }
}

typedef struct { const char *letter; int course; int value; } WarioSml3TreasureDef;
static const WarioSml3TreasureDef wario_sml3_treasures[15] = {
    {"A", 3, 9000}, {"B", 9, 6000}, {"C", 11, 2000},
    {"D", 16, 7000}, {"E", 17, 5000}, {"F", 18, 9000},
    {"G", 20, 5000}, {"H", 24, 8000}, {"I", 26, 7000},
    {"J", 29, 3000}, {"K", 30, 4000}, {"L", 31, 8000},
    {"M", 34, 4000}, {"N", 37, 7000}, {"O", 39, 6000}
};

static void add_wario_sml3_treasures(void)
{
    for (int i = 0; i < 15; ++i) {
        char id[40], name[80], description[200];
        snprintf(id, sizeof id, "wario1_treasure_%02d", i + 1);
        snprintf(name, sizeof name, "Treasure %s", wario_sml3_treasures[i].letter);
        snprintf(description, sizeof description,
                 "Found in Course %02d. Worth %d coins toward Wario's ending.",
                 wario_sml3_treasures[i].course, wario_sml3_treasures[i].value);
        add_entry(GAME_WARIO_SML3, id, name, MUSEUM_CATEGORY_TREASURE,
                  description, "");
        MuseumEntry *e = &catalog[GAME_WARIO_SML3][catalog_count[GAME_WARIO_SML3] - 1];
        snprintf(e->source_url, sizeof e->source_url, "%s",
                 "https://www.mariowiki.com/Treasure_%28Wario_franchise%29");
    }
}

static void build_catalog(void)
{
    memset(catalog, 0, sizeof catalog);
    memset(catalog_count, 0, sizeof catalog_count);

    /* Dr. Mario */
    add_entry(GAME_DRMARIO, "capsule", "Capsule", MUSEUM_CATEGORY_ITEM,
              "A two-colour medical capsule used to clear matching lines in the bottle.", "land");
    add_entry(GAME_DRMARIO, "virus", "Virus", MUSEUM_CATEGORY_ENEMY,
              "A virus was registered by the game's event signal; exact colour identification is not mapped yet.", "virus");
    add_entry(GAME_DRMARIO, "virus_clear", "Virus cleared", MUSEUM_CATEGORY_MILESTONE,
              "A clearing event was observed during play.", "clear");

    /* Super Mario Land */
    add_entry(GAME_SML, "coin", "Coin", MUSEUM_CATEGORY_ITEM,
              "A coin collected during the adventure.", "coin");
    add_entry(GAME_SML, "enemy_defeated", "Enemy defeated", MUSEUM_CATEGORY_ENEMY,
              "An enemy-stomp event was observed. Species-specific recognition is not yet mapped.", "stomp");
    add_entry(GAME_SML, "power_up", "Power-up", MUSEUM_CATEGORY_ITEM,
              "A power-up event was observed; its exact item type needs an item-specific detector.", "power");
    add_entry(GAME_SML, "block", "Bumpable block", MUSEUM_CATEGORY_ITEM,
              "A block interaction was detected during play.", "bump");
    add_entry(GAME_SML, "life_lost", "Life lost", MUSEUM_CATEGORY_MILESTONE,
              "A life-loss event was observed.", "life");

    /* Super Mario Land 2 */
    add_entry(GAME_SML2, "coin", "Coin", MUSEUM_CATEGORY_ITEM,
              "A coin-counter increase was observed.", "coin");
    add_entry(GAME_SML2, "enemy_defeated", "Enemy defeated", MUSEUM_CATEGORY_ENEMY,
              "The defeated-enemy counter increased. This does not identify the species yet.", "enemy");
    add_entry(GAME_SML2, "extra_life", "Extra life", MUSEUM_CATEGORY_ITEM,
              "The life counter increased.", "oneup");
    add_entry(GAME_SML2, "life_lost", "Life lost", MUSEUM_CATEGORY_MILESTONE,
              "The life counter decreased.", "life");

    /* Wario Land: Super Mario Land 3. Individual treasure flags are available. */
    add_entry(GAME_WARIO_SML3, "coin", "Coin", MUSEUM_CATEGORY_ITEM,
              "A coin-count increase was observed.", "coin");
    add_entry(GAME_WARIO_SML3, "power_up", "Power-up", MUSEUM_CATEGORY_ITEM,
              "A Wario power-state change was observed; exact forms need their own mapping.", "power");
    add_wario_sml3_treasures();
    add_entry(GAME_WARIO_SML3, "course", "Course", MUSEUM_CATEGORY_STAGE,
              "A course was entered.", "course");
    add_entry(GAME_WARIO_SML3, "course_results", "Course results", MUSEUM_CATEGORY_MILESTONE,
              "The course-results screen was reached.", "results");

    /* Wario Land II: the GB and GBC editions have separate progress files. */
    for (int g = GAME_WARIO_LAND2_GB; g <= GAME_WARIO_LAND2_GBC; ++g) {
        if (g != GAME_WARIO_LAND2_GB && g != GAME_WARIO_LAND2_GBC) continue;
        add_numbered(g, "wl2_treasure", "Treasure", MUSEUM_CATEGORY_TREASURE,
                     "A persistent treasure flag. A named description can be added from the matching game guide.", 50);
        add_numbered(g, "wl2_picture", "Picture panel", MUSEUM_CATEGORY_TREASURE,
                     "A persistent picture-panel flag from this specific game version.", 50);
        add_entry(g, "coin", "Coin", MUSEUM_CATEGORY_ITEM,
                  "A coin-bank increase was observed.", "coin");
        add_entry(g, "course", "Stage", MUSEUM_CATEGORY_STAGE,
                  "A stage-selection change was observed.", "course");
        add_entry(g, "course_results", "Course results", MUSEUM_CATEGORY_MILESTONE,
                  "The game's course-results handling was reached.", "results");
    }

    /* Wario Land 3: individual key colours are unlocked through the key bitmask. */
    add_entry(GAME_WARIO_LAND3_GBC, "coin", "Coin", MUSEUM_CATEGORY_ITEM,
              "A coin-count increase during a course.", "coin");
    add_entry(GAME_WARIO_LAND3_GBC, "power_up", "Permanent ability", MUSEUM_CATEGORY_ITEM,
              "A permanent power-up state was observed.", "power");
    add_entry(GAME_WARIO_LAND3_GBC, "course_treasure", "Course treasure", MUSEUM_CATEGORY_TREASURE,
              "A course treasure was added to the 100-item collection.", "treasure");
    add_wl3_treasures();
    add_entry(GAME_WARIO_LAND3_GBC, "key", "Course key", MUSEUM_CATEGORY_TREASURE,
              "A course-key acquisition scene was observed.", "key");
    add_entry(GAME_WARIO_LAND3_GBC, "key_gray", "Gray Key", MUSEUM_CATEGORY_TREASURE,
              "The gray key was present in a course's four-key inventory.", "");
    add_entry(GAME_WARIO_LAND3_GBC, "key_red", "Red Key", MUSEUM_CATEGORY_TREASURE,
              "The red key was present in a course's four-key inventory.", "");
    add_entry(GAME_WARIO_LAND3_GBC, "key_green", "Green Key", MUSEUM_CATEGORY_TREASURE,
              "The green key was present in a course's four-key inventory.", "");
    add_entry(GAME_WARIO_LAND3_GBC, "key_blue", "Blue Key", MUSEUM_CATEGORY_TREASURE,
              "The blue key was present in a course's four-key inventory.", "");
    add_entry(GAME_WARIO_LAND3_GBC, "course", "Course", MUSEUM_CATEGORY_STAGE,
              "A course was entered from the overworld.", "course");
    add_entry(GAME_WARIO_LAND3_GBC, "course_results", "Course results", MUSEUM_CATEGORY_MILESTONE,
              "The course-clear results screen was reached.", "results");
}

static int entry_index(int game, const char *id)
{
    if (game < 0 || game >= N_GAMES || !id) return -1;
    for (int i = 0; i < catalog_count[game]; ++i)
        if (!strcmp(catalog[game][i].id, id)) return i;
    return -1;
}

void museum_set_storage_path(const char *path)
{
    memset(discovered, 0, sizeof discovered);
    storage_error[0] = 0;
    if (path && path[0]) snprintf(storage_path, sizeof storage_path, "%s", path);
    else storage_path[0] = 0;
    initialized = 0;
    museum_init();
}

void museum_init(void)
{
    if (initialized) return;
    build_catalog();
    if (!storage_path[0])
        snprintf(storage_path, sizeof storage_path, "%spipeclean-museum.txt", settings_dir());
    initialized = 1;
    FILE *f = fopen(storage_path, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof line, f)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        char *sep = strchr(line, '|');
        if (!sep) continue;
        *sep++ = 0;
        for (int g = 0; g < N_GAMES; ++g) {
            if (strcmp(line, games[g].id)) continue;
            int idx = entry_index(g, sep);
            if (idx >= 0) discovered[g][idx] = 1;
            break;
        }
    }
    fclose(f);
}

int museum_catalog_count(int game)
{
    museum_init();
    return game >= 0 && game < N_GAMES ? catalog_count[game] : 0;
}

const MuseumEntry *museum_catalog_entry(int game, int index)
{
    museum_init();
    if (game < 0 || game >= N_GAMES || index < 0 || index >= catalog_count[game])
        return NULL;
    return &catalog[game][index];
}

int museum_is_discovered(int game, const char *id)
{
    museum_init();
    int idx = entry_index(game, id);
    return idx >= 0 ? discovered[game][idx] != 0 : 0;
}

int museum_discover_entry(int game, const char *id)
{
    museum_init();
    int idx = entry_index(game, id);
    if (idx < 0 || discovered[game][idx]) return 0;
    if (!storage_path[0]) {
        snprintf(storage_error, sizeof storage_error, "Museum storage path is unavailable");
        return 0;
    }
    FILE *f = fopen(storage_path, "a");
    if (!f) {
        snprintf(storage_error, sizeof storage_error, "Couldn't save museum progress: %.100s", storage_path);
        return 0;
    }
    int ok = fprintf(f, "%s|%s\n", games[game].id, catalog[game][idx].id) > 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        snprintf(storage_error, sizeof storage_error, "Couldn't write museum progress");
        return 0;
    }
    storage_error[0] = 0;
    discovered[game][idx] = 1;
    return 1;
}

void museum_observe_event(int game, const char *event_id)
{
    museum_init();
    if (game < 0 || game >= N_GAMES || !event_id || !event_id[0]) return;
    for (int i = 0; i < catalog_count[game]; ++i) {
        MuseumEntry *e = &catalog[game][i];
        if (e->trigger_event[0] && !strcmp(e->trigger_event, event_id))
            (void)museum_discover_entry(game, e->id);
    }
}

void museum_sync_flags(int game, const char *id_prefix, uint64_t flags, int count)
{
    museum_init();
    if (game < 0 || game >= N_GAMES || !id_prefix || count < 0) return;
    if (count > 64) count = 64;
    for (int bit = 0; bit < count; ++bit) {
        if (!(flags & (UINT64_C(1) << bit))) continue;
        char id[40];
        snprintf(id, sizeof id, "%s_%02d", id_prefix, bit + 1);
        (void)museum_discover_entry(game, id);
    }
}

int museum_discovered_count(int game)
{
    museum_init();
    if (game < 0 || game >= N_GAMES) return 0;
    int total = 0;
    for (int i = 0; i < catalog_count[game]; ++i)
        if (discovered[game][i]) total++;
    return total;
}

const char *museum_category_name(MuseumCategory category)
{
    switch (category) {
    case MUSEUM_CATEGORY_ENEMY: return "Enemies";
    case MUSEUM_CATEGORY_ITEM: return "Items";
    case MUSEUM_CATEGORY_TREASURE: return "Treasures";
    case MUSEUM_CATEGORY_STAGE: return "Stages";
    case MUSEUM_CATEGORY_MILESTONE: return "Milestones";
    case MUSEUM_CATEGORY_CHARACTER: return "Characters";
    default: return "All";
    }
}

const char *museum_last_storage_error(void) { return storage_error; }
