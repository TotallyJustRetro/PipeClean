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
    add_numbered(GAME_WARIO_SML3, "wario1_treasure", "Treasure",
                 MUSEUM_CATEGORY_TREASURE, "A treasure slot recorded in the cartridge's collection flags.", 16);
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
              "The game's treasure total increased. Exact treasure identities require course-specific flags.", "treasure");
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
