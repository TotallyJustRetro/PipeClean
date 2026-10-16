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
    {"Pokémon Pikachu","S2 The Big Bridge","Green"},
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

static const char *wl3_treasure_purpose(const char *name)
{
    if (!name) return "A collectible from the four-chest treasure set.";
    if (!strcmp(name,"Axe")) return "Cuts down the large tree and opens routes to The Peaceful Village and The Vast Plain.";
    if (!strcmp(name,"Jar")) return "Summons a rain cloud that opens routes to The Pool of Rain and A Town in Chaos.";
    if (!strcmp(name,"Blue Tablet Half") || !strcmp(name,"Green Tablet Half")) return "Pairs with the other tablet half to unlock the gate to Desert Ruins.";
    if (!strcmp(name,"Top Half of Scroll") || !strcmp(name,"Bottom Half of Scroll")) return "Pairs with the other scroll half to calm the western storm and open The Volcano's Base.";
    if (!strcmp(name,"Blue Overalls")) return "A power-granting outfit that improves Wario's dash and lets him break cracked blocks.";
    if (!strcmp(name,"Screw")) return "A machine component whose discovery opens the route to The Grasslands.";
    if (!strcmp(name,"Cog")) return "A machine component from A Town in Chaos, counted toward the 100 treasures.";
    if (strstr(name,"Music Box")) return "One of five magical music boxes. Together they break the seal and unlock the final battle.";
    if (!strcmp(name,"Seed")) return "Causes stalks to grow in other levels, revealing new routes.";
    if (!strcmp(name,"Red Skull") || !strcmp(name,"Blue Skull")) return "The two skull rings together raise the Tower of Revival.";
    if (!strcmp(name,"Flippers")) return "Lets Wario swim and dive, opening routes in The Pool of Rain and The Big Bridge.";
    if (!strcmp(name,"Paper Fan")) return "Blows away mist and opens the route to The Stagnant Swamp.";
    if (!strcmp(name,"Flute")) return "Summons snakes in The Peaceful Village, The Grasslands and The Volcano's Base.";
    if (!strcmp(name,"Ice Spellbook") || !strcmp(name,"Lightning Spellbook") || !strcmp(name,"Lightning Wand")) return "A magic relic that combines with its partner to alter the world map and reveal new routes.";
    if (!strcmp(name,"Spike Helmet")) return "Lets Wario break blocks by jumping into them from below.";
    if (!strcmp(name,"Foot Weight")) return "Creates an earthquake, opens routes and drains water from The Stagnant Swamp.";
    if (!strcmp(name,"Lantern")) return "Opens the route to Castle of Illusions.";
    if (!strcmp(name,"Left Half of Sun Stone") || !strcmp(name,"Right Half of Sun Stone")) return "The two medallion halves end the eternal night on the eastern side and reveal new routes.";
    if (!strcmp(name,"Detonator")) return "Sets off explosive blocks to clear blocked routes.";
    if (!strcmp(name,"Map")) return "Reveals the hidden route to The East Crater.";
    if (strstr(name,"Crayon")) return "One of seven crayons that recolour world-map scenery. Finding all seven unlocks the Golf mini-game.";
    if (!strcmp(name,"Day/Night Tablet")) return "Lets Wario switch between day and night on the world map.";
    if (!strcmp(name,"Magnifying Glass")) return "Shows which treasures Wario has already collected without entering a stage.";
    if (!strcmp(name,"Pocket Mirror")) return "Reveals the route to The Warped Void.";
    if (!strcmp(name,"Blood")) return "Causes zombies to appear in Forest of Fear and reveals a new route.";
    if (!strcmp(name,"Red Keycard") || !strcmp(name,"Blue Keycard")) return "Part of a pair of keycards needed to open a sealed area in The Warped Void.";
    if (!strcmp(name,"Key")) return "Unlocks the door at the top of the Tower of Revival and opens Above the Clouds.";
    if (!strcmp(name,"Torch")) return "Lights the route to Forest of Fear and opens a passage between regions.";
    if (!strcmp(name,"Infrared Goggles")) return "Lets Wario see in dark areas and discover hidden paths.";
    if (!strcmp(name,"Drill")) return "A digging tool that opens a path through rock.";
    if (!strcmp(name,"Pickaxe")) return "Opens a new passage in The East Crater.";
    if (!strcmp(name,"Gong")) return "Summons the moon above the clouds and reveals a route through the level.";
    if (!strcmp(name,"Scissors")) return "Cuts a balloon obstacle to open a new route.";
    if (!strcmp(name,"Fire Extinguisher")) return "Puts out flames that block routes through the crater levels.";
    if (!strcmp(name,"Magic Seeds")) return "Grows stalks in The Vast Plain, The Pool of Rain and The Grasslands.";
    if (!strcmp(name,"Trident")) return "A collectible relic from Bank of the Wild River, counted toward complete collection.";
    if (!strcmp(name,"Blue Chemicals") || !strcmp(name,"Red Chemicals")) return "A chemical treasure used to move heavy blocks when its matching chemical is found.";
    if (!strcmp(name,"Mechanical Fan")) return "A fan component that creates air currents and opens new paths.";
    if (!strcmp(name,"Left Blue Snake Eye") || !strcmp(name,"Right Blue Snake Eye")) return "Pairs with the other blue eye to open the blue snake door in the Tower of Revival.";
    if (!strcmp(name,"Left Golden Snake Eye") || !strcmp(name,"Right Golden Snake Eye") || !strcmp(name,"Golden Left Eye"))
        return "Pairs with the other golden eye to open the golden snake door in the Tower of Revival.";
    if (!strcmp(name,"Valve")) return "A mechanism-related treasure from The Warped Void, counted toward complete collection.";
    if (!strcmp(name,"Remote Control")) return "A gadget recovered from Forest of Fear, counted toward the full treasure collection.";
    if (!strcmp(name,"Green Flower")) return "A special flower treasure from The Stagnant Swamp, part of the 100-item set.";
    if (!strcmp(name,"Pouch")) return "A treasure pouch recovered from Cave of Flames, counted toward complete collection.";
    if (!strcmp(name,"Chalice") || !strcmp(name,"Crown") || !strcmp(name,"Teapot") || !strcmp(name,"Pokémon Pikachu") ||
        !strcmp(name,"Toy Car") || !strcmp(name,"Toy Train") || !strcmp(name,"Telephone") ||
        !strcmp(name,"Saber") || !strcmp(name,"Club") || !strcmp(name,"Spade"))
        return "A keepsake treasure with no separate ability, but it counts toward the full 100-item collection.";
    return "A collectible treasure recovered from this chest. It counts toward the complete set of 100 treasures.";
}

static void add_wl3_treasures(void)
{
    for (int i = 0; i < 100; ++i) {
        char id[40], description[200];
        snprintf(id, sizeof id, "wl3_treasure_%03d", i + 1);
        snprintf(description, sizeof description, "%s Found in %s's %s Chest.",
                 wl3_treasure_purpose(wl3_treasures[i].name),
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

typedef struct { const char *id; const char *name; const char *description; MuseumCategory category; } SmlMuseumDef;
static const SmlMuseumDef sml_museum_defs[] = {
    {"sml_goombo", "Goombo (Chibibo)", "A mushroom-like Sarasaland enemy. It walks forward without turning at ledges, so it can wander off platforms. Stomping or a Superball defeats it.", MUSEUM_CATEGORY_ENEMY},
    {"sml_piranha", "Piranha Plant", "A plant that rises from a pipe into Mario's path. Watch its timing before crossing a pipe.", MUSEUM_CATEGORY_ENEMY},
    {"sml_ganchan", "Ganchan Boulder", "A rolling stone hazard. Some Ganchan can also act as moving platforms.", MUSEUM_CATEGORY_ENEMY},
    {"sml_bombshell_koopa", "Bombshell Koopa (Nokobon)", "A turtle whose shell is explosive. Stomping it triggers a blast, so keep clear after an attack.", MUSEUM_CATEGORY_ENEMY},
    {"sml_nokobon_bomb", "Nokobon Bomb", "The explosive shell released by a Bombshell Koopa. Its blast can hurt Mario if he stays nearby.", MUSEUM_CATEGORY_ENEMY},
    {"sml_pipe_fist", "Pipe Fist", "A punch thrown from a pipe that blocks Mario's route. Dash past between strikes.", MUSEUM_CATEGORY_ENEMY},
    {"sml_king_totomesu", "King Totomesu", "The lion-like boss of the Birabuto Kingdom. Watch its fireball timing to find an opening.", MUSEUM_CATEGORY_ENEMY},
    {"sml_pompon_flower", "Pompon Flower", "A flower enemy that releases drifting pollen. Keep moving rather than waiting in its path.", MUSEUM_CATEGORY_ENEMY},
    {"sml_fly", "Fly / Kumo", "A small flying enemy. Its stomped form uses another object ID, but belongs to the same specimen record.", MUSEUM_CATEGORY_ENEMY},
    {"sml_honen", "Honen", "A skeletal fish found in water. It patrols underwater routes and can obstruct narrow passages.", MUSEUM_CATEGORY_ENEMY},
    {"sml_mekabon", "Mekabon", "A robot enemy that can detach and throw its head. Watch both the body and its flying head.", MUSEUM_CATEGORY_ENEMY},
    {"sml_dragonzamasu", "Dragonzamasu", "A seahorse-like boss of the Muda Kingdom. Its attacks make the surrounding water dangerous to cross.", MUSEUM_CATEGORY_ENEMY},
    {"sml_yurarin", "Yurarin", "A seahorse enemy that swims through underwater stages. Avoid its path in narrow passages.", MUSEUM_CATEGORY_ENEMY},
    {"sml_yurarin_boo", "Yurarin Boo", "A jumping seahorse variant that attacks from the water and can surprise Mario near the surface.", MUSEUM_CATEGORY_ENEMY},
    {"sml_suu", "Suu", "A spider that descends into Mario's path. Watch overhead areas as you move through the stage.", MUSEUM_CATEGORY_ENEMY},
    {"sml_gunion", "Gunion", "A squid-like sea enemy encountered underwater. Nearby projectiles can make a direct swim unsafe.", MUSEUM_CATEGORY_ENEMY},
    {"sml_torion", "Torion", "A fish enemy encountered in water. Stay alert for it approaching from the side.", MUSEUM_CATEGORY_ENEMY},
    {"sml_tokotoko", "Tokotoko", "A running stone-head enemy found in Easton Kingdom. A well-timed jump can avoid its charge.", MUSEUM_CATEGORY_ENEMY},
    {"sml_hiyoihoi", "Hiyoihoi", "The stone-head boss of Easton Kingdom. It attacks by sending Ganchan boulders across the arena.", MUSEUM_CATEGORY_ENEMY},
    {"sml_batadon", "Batadon", "A flying stone-head enemy that approaches from the air. Watch its movement before jumping.", MUSEUM_CATEGORY_ENEMY},
    {"sml_gao", "Gao / Nyololin", "A lion-like fire-spitting enemy. Its defeated animation has its own object ID, but belongs to this specimen.", MUSEUM_CATEGORY_ENEMY},
    {"sml_bunbun", "Bunbun", "A bee enemy that drops arrows while hovering. Avoid standing below it for too long.", MUSEUM_CATEGORY_ENEMY},
    {"sml_pipe_cannon", "Pipe Cannon", "A cannon hazard that launches Gira projectiles across the stage. Check the firing lane before crossing.", MUSEUM_CATEGORY_ENEMY},
    {"sml_gira", "Gira / Bullet Biff", "A fast bullet-like projectile fired by a Pipe Cannon. Keep moving to avoid its path.", MUSEUM_CATEGORY_ENEMY},
    {"sml_rocketon", "Roketon", "An aircraft enemy from Chai Kingdom. Its attacks make it dangerous to linger in one spot.", MUSEUM_CATEGORY_ENEMY},
    {"sml_chicken", "Chicken", "A distinct live object in the cartridge's stage data. The Museum records the in-game specimen rather than a generic stomp.", MUSEUM_CATEGORY_ENEMY},
    {"sml_roto_disc", "Roto Disc", "A rotating disc hazard. Avoid its path rather than treating it as a normal enemy.", MUSEUM_CATEGORY_ENEMY},
    {"sml_reverse_piranha", "Headstand Piranha Plant", "An upside-down Piranha Plant variant. Its unusual placement changes how it threatens Mario.", MUSEUM_CATEGORY_ENEMY},
    {"sml_pionpi", "Pionpi", "An undead hopping enemy from Chai Kingdom. Its knocked-out state uses a different object ID.", MUSEUM_CATEGORY_ENEMY},
    {"sml_chikako", "Chikako", "A distinct enemy in Chai Kingdom. Its entry is tied to the cartridge's live object ID.", MUSEUM_CATEGORY_ENEMY},
    {"sml_tatanga", "Tatanga", "The alien antagonist and final boss of Super Mario Land. Mario fights Tatanga while piloting the Sky Pop.", MUSEUM_CATEGORY_ENEMY},
    {"sml_biokinton", "Biokinton", "A cloud-like boss associated with the final Chai Kingdom sequence. It has its own object type.", MUSEUM_CATEGORY_ENEMY},
    {"sml_mushroom", "Super Mushroom", "Grows Small Mario into Super Mario. Super Mario can take one hit before returning to small form.", MUSEUM_CATEGORY_ITEM},
    {"sml_heart", "Heart", "A collectible heart from Sarasaland. Its sprite is captured from the active game when it appears.", MUSEUM_CATEGORY_ITEM},
    {"sml_star", "Star", "Grants a short burst of invincibility, allowing Mario to defeat most enemies by touching them.", MUSEUM_CATEGORY_ITEM},
    {"sml_superball_flower", "Superball Flower", "Turns Mario into Superball Mario, who throws bouncing Superballs that defeat enemies and collect coins.", MUSEUM_CATEGORY_ITEM}
};
static void add_sml_catalog(void)
{
    for (size_t i = 0; i < sizeof sml_museum_defs / sizeof sml_museum_defs[0]; ++i)
        add_entry(GAME_SML, sml_museum_defs[i].id, sml_museum_defs[i].name,
                  sml_museum_defs[i].category, sml_museum_defs[i].description, "");
    add_entry(GAME_SML, "coin", "Coin", MUSEUM_CATEGORY_ITEM, "A coin collected in a stage. Coins increase Mario's counter.", "coin");
    add_entry(GAME_SML, "enemy_defeated", "Unidentified enemy", MUSEUM_CATEGORY_ENEMY, "Fallback record for an enemy signal whose exact object ID could not be resolved.", "");
    add_entry(GAME_SML, "power_up", "Unidentified power-up", MUSEUM_CATEGORY_ITEM, "Fallback record for a power-up signal when the live object type is not recognized.", "");
    add_entry(GAME_SML, "block", "Mystery block", MUSEUM_CATEGORY_ITEM, "A block interaction was recorded. Blocks may contain coins or a power-up.", "bump");
    add_entry(GAME_SML, "life_lost", "Life lost", MUSEUM_CATEGORY_MILESTONE, "Mario lost a life during the adventure.", "life");
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
    add_entry(GAME_DRMARIO, "virus_clear", "Pills cleared", MUSEUM_CATEGORY_MILESTONE,
              "Four or more matching capsule segments cleared from the bottle.", "clear");

    /* Super Mario Land */
    add_sml_catalog();

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
                     "A hidden stage treasure recovered after finding its matching key and opening the same-colour chest. This is one of 50 treasure collection flags.", 50);
        add_numbered(g, "wl2_picture", "Picture panel", MUSEUM_CATEGORY_TREASURE,
                     "A picture-panel tile earned through the number-matching mini-game. Collecting all 50 panels completes this game's picture collection.", 50);
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

/* Super Mario Land object IDs; bit 7 flags hard-mode variants. */
const char *museum_sml_object_entry_id(uint8_t raw_type, MuseumCategory *category)
{
    const uint8_t type = raw_type & 0x7Fu;
    const char *id = NULL;
    MuseumCategory cat = MUSEUM_CATEGORY_ENEMY;
    switch (type) {
    case 0x00: case 0x01: id = "sml_goombo"; break;
    case 0x02: id = "sml_piranha"; break;
    case 0x03: case 0x33: case 0x47: id = "sml_ganchan"; break;
    case 0x04: id = "sml_bombshell_koopa"; break;
    case 0x05: id = "sml_nokobon_bomb"; break;
    case 0x06: id = "sml_pipe_fist"; break;
    case 0x08: id = "sml_king_totomesu"; break;
    case 0x09: id = "sml_pompon_flower"; break;
    case 0x0E: case 0x0F: id = "sml_fly"; break;
    case 0x10: id = "sml_honen"; break;
    case 0x16: case 0x17: case 0x18: id = "sml_mekabon"; break;
    case 0x1A: id = "sml_dragonzamasu"; break;
    case 0x1D: id = "sml_yurarin"; break;
    case 0x20: id = "sml_gunion"; break;
    case 0x24: id = "sml_yurarin_boo"; break;
    case 0x25: id = "sml_suu"; break;
    case 0x29: cat = MUSEUM_CATEGORY_ITEM; id = "sml_mushroom"; break;
    case 0x2B: cat = MUSEUM_CATEGORY_ITEM; id = "sml_heart"; break;
    case 0x2D: case 0x2E: cat = MUSEUM_CATEGORY_ITEM; id = "sml_superball_flower"; break;
    case 0x30: id = "sml_torion"; break;
    case 0x31: id = "sml_tokotoko"; break;
    case 0x32: id = "sml_hiyoihoi"; break;
    case 0x34: cat = MUSEUM_CATEGORY_ITEM; id = "sml_star"; break;
    case 0x3C: id = "sml_batadon"; break;
    case 0x3F: case 0x40: id = "sml_gao"; break;
    case 0x42: case 0x43: id = "sml_bunbun"; break;
    case 0x49: id = "sml_pipe_cannon"; break;
    case 0x4A: case 0x4B: case 0x5A: id = "sml_gira"; break;
    case 0x52: id = "sml_rocketon"; break;
    case 0x53: id = "sml_chicken"; break;
    case 0x54: id = "sml_roto_disc"; break;
    case 0x55: id = "sml_reverse_piranha"; break;
    case 0x56: case 0x57: id = "sml_pionpi"; break;
    case 0x59: id = "sml_chikako"; break;
    case 0x5B: case 0x60: id = "sml_tatanga"; break;
    case 0x61: id = "sml_biokinton"; break;
    default: break;
    }
    if (category) *category = id ? cat : MUSEUM_CATEGORY_ALL;
    return id;
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

int museum_sprite_asset_path(int game, const char *id, char *out, size_t out_size)
{
    museum_init();
    if (!out || out_size == 0) return 0;
    out[0] = 0;
    if (game < 0 || game >= N_GAMES || entry_index(game, id) < 0) return 0;

    const char *base = settings_dir();
    if (!base || !base[0]) return 0;
    size_t len = strlen(base);
    const char *sep = (len && (base[len - 1] == '/' || base[len - 1] == '\\')) ? "" : "/";
    int n = snprintf(out, out_size, "%s%spipeclean-museum-assets/%s/%s.png",
                     base, sep, games[game].id, id);
    if (n < 0 || (size_t)n >= out_size) { out[0] = 0; return 0; }

    FILE *f = fopen(out, "rb");
    if (!f) { out[0] = 0; return 0; }
    fclose(f);
    return 1;
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
