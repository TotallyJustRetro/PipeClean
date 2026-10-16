#include "museum.h"
#include "settings.h"
#include <SDL.h>
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
#define MUSEUM_NOTICE_QUEUE_CAP 16
static char discovery_notices[MUSEUM_NOTICE_QUEUE_CAP][160];
static int discovery_notice_head;
static int discovery_notice_count;
static SDL_SpinLock discovery_notice_lock;

static void museum_queue_discovery_notice(const MuseumEntry *entry)
{
    if (!entry) return;
    SDL_AtomicLock(&discovery_notice_lock);
    if (!settings.museum_discovery_notifications) {
        SDL_AtomicUnlock(&discovery_notice_lock);
        return;
    }
    if (discovery_notice_count >= MUSEUM_NOTICE_QUEUE_CAP) {
        discovery_notice_head = (discovery_notice_head + 1) % MUSEUM_NOTICE_QUEUE_CAP;
        --discovery_notice_count;
    }
    int slot = (discovery_notice_head + discovery_notice_count) % MUSEUM_NOTICE_QUEUE_CAP;
    snprintf(discovery_notices[slot], sizeof discovery_notices[slot], "New discovery: %.125s", entry->name);
    ++discovery_notice_count;
    SDL_AtomicUnlock(&discovery_notice_lock);
}

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
    {"Blue Tablet","N2 The Peaceful Village","Gray"},
    {"Green Tablet","N3 The Vast Plain","Gray"},
    {"Top Half of Scroll","W1 Desert Ruins","Gray"},
    {"Bottom Half of Scroll","W1 Desert Ruins","Red"},
    {"Lead Overalls","W2 The Volcano's Base","Gray"},
    {"Jar","N1 Out of the Woods","Red"},
    {"First Gear for Machine","W3 The Pool of Rain","Gray"},
    {"Second Gear for Machine","W4 A Town in Chaos","Gray"},
    {"Yellow Music Box","S1 The Grasslands","Gray"},
    {"Magic Seeds","S2 The Big Bridge","Gray"},
    {"Skull Ring (red)","N3 The Vast Plain","Green"},
    {"Skull Ring (blue)","W3 The Pool of Rain","Red"},
    {"Swimming Flippers","S3 Tower of Revival","Gray"},
    {"Ornamental Fan","W3 The Pool of Rain","Green"},
    {"Flute","E1 The Stagnant Swamp","Gray"},
    {"Blue Book","N2 The Peaceful Village","Red"},
    {"Magic Wand","S1 The Grasslands","Green"},
    {"Head Smash Helmet","E2 The Frigid Sea","Gray"},
    {"Blue Music Box","W4 A Town in Chaos","Red"},
    {"Trident","N4 Bank of the Wild River","Gray"},
    {"Yellow Book","N5 The Tidal Coast","Gray"},
    {"Grab Glove","S4 The Steep Canyon","Gray"},
    {"Foot of Stone","N5 The Tidal Coast","Red"},
    {"Tusk (blue)","W2 The Volcano's Base","Blue"},
    {"Tusk (red)","S4 The Steep Canyon","Red"},
    {"Green Flower","E1 The Stagnant Swamp","Red"},
    {"Right Glass Eye","W6 The West Crater","Gray"},
    {"Left Glass Eye","E4 The Colossal Hole","Gray"},
    {"Super Smash","S3 Tower of Revival","Red"},
    {"Green Music Box","N6 Sea Turtle Rock","Gray"},
    {"Scepter","S5 Cave of Flames","Gray"},
    {"Lantern","S2 The Big Bridge","Blue"},
    {"Magical Flame","E2 The Frigid Sea","Green"},
    {"Super Jump Slam Overalls","E3 Castle of Illusions","Gray"},
    {"Top Half of Sun Medallion","N6 Sea Turtle Rock","Green"},
    {"Bottom Half of Sun Medallion","W1 Desert Ruins","Blue"},
    {"High Jump Boots","E4 The Colossal Hole","Green"},
    {"Red Music Box","E1 The Stagnant Swamp","Green"},
    {"Explosive Plunger Box","W5 Beneath the Waves","Gray"},
    {"Pouch","S5 Cave of Flames","Green"},
    {"Eye of the Storm","E1 The Stagnant Swamp","Blue"},
    {"Prince Frog's Glove","N1 Out of the Woods","Green"},
    {"Statue","N4 Bank of the Wild River","Green"},
    {"Treasure Map","S3 Tower of Revival","Green"},
    {"Super Grab Gloves","E6 The East Crater","Gray"},
    {"Gold Magic","E3 Castle of Illusions","Green"},
    {"Gold Music Box","N1 Out of the Woods","Blue"},
    {"Crayon (blue)","N3 The Vast Plain","Red"},
    {"Truck Wheel","S1 The Grasslands","Red"},
    {"Blue Gem","W2 The Volcano's Base","Red"},
    {"Crayon (cyan)","S2 The Big Bridge","Red"},
    {"Goblet","N2 The Peaceful Village","Green"},
    {"Crown","W2 The Volcano's Base","Green"},
    {"Crayon (pink)","W1 Desert Ruins","Green"},
    {"Teapot","E2 The Frigid Sea","Red"},
    {"Pocket Pet","S2 The Big Bridge","Green"},
    {"Magnifying Glass","N4 Bank of the Wild River","Red"},
    {"Air-Pump","E4 The Colossal Hole","Red"},
    {"Rocket","N4 Bank of the Wild River","Blue"},
    {"Crayon (yellow)","W3 The Pool of Rain","Blue"},
    {"Saber","W6 The West Crater","Red"},
    {"Day or Night Spell","N2 The Peaceful Village","Blue"},
    {"UFO","E2 The Frigid Sea","Blue"},
    {"Torch","E3 Castle of Illusions","Red"},
    {"Warp Compact","E7 Forest of Fear","Gray"},
    {"Mystery Handle","E5 The Warped Void","Gray"},
    {"Warp Removal Apparatus","E7 Forest of Fear","Green"},
    {"Crayon (brown)","E5 The Warped Void","Red"},
    {"Demon's Blood","E5 The Warped Void","Green"},
    {"Key Card (red)","E7 Forest of Fear","Red"},
    {"Key Card (blue)","E7 Forest of Fear","Blue"},
    {"Heart Crest","E5 The Warped Void","Blue"},
    {"Minicar","S5 Cave of Flames","Red"},
    {"Locomotive","E4 The Colossal Hole","Blue"},
    {"Telephone","S4 The Steep Canyon","Green"},
    {"Crayon (red)","W5 Beneath the Waves","Red"},
    {"Electric Fan Propeller","N5 The Tidal Coast","Green"},
    {"Sapling of Growth","W4 A Town in Chaos","Blue"},
    {"Green Gem","N5 The Tidal Coast","Blue"},
    {"Fighter Mannequin","S1 The Grasslands","Blue"},
    {"Golden Left Eye","W4 A Town in Chaos","Green"},
    {"Golden Right Eye","N6 Sea Turtle Rock","Red"},
    {"Sky Key","S3 Tower of Revival","Blue"},
    {"Crayon (green)","S6 Above the Clouds","Gray"},
    {"Fire Drencher","S6 Above the Clouds","Red"},
    {"Diamonds Crest","E6 The East Crater","Red"},
    {"Castle Brick","W6 The West Crater","Green"},
    {"Rust Spray","E3 Castle of Illusions","Blue"},
    {"Spades Crest","W6 The West Crater","Blue"},
    {"Red Chemical","W5 Beneath the Waves","Green"},
    {"Blue Chemical","S4 The Steep Canyon","Blue"},
    {"Red Gem","N3 The Vast Plain","Blue"},
    {"Clubs Crest","W5 Beneath the Waves","Blue"},
    {"Scissors","S5 Cave of Flames","Blue"},
    {"Jackhammer","S6 Above the Clouds","Green"},
    {"Night Vision Scope","E6 The East Crater","Green"},
    {"Full Moon Gong","N6 Sea Turtle Rock","Blue"},
    {"Pick-Axe","S6 Above the Clouds","Blue"},
    {"Earthen Figure","E6 The East Crater","Blue"}
};

static const char *wl3_treasure_purpose(const char *name)
{
    if (!name) return "A collectible from the four-chest treasure set.";
    if (!strcmp(name, "Axe")) return "Cuts down the large tree on the north side of the world map, opening the paths to The Peaceful Village and The Vast Plain.";
    if (!strcmp(name, "Blue Tablet")) return "Pairs with the Green Tablet to unlock the hill gate and open the route to Desert Ruins.";
    if (!strcmp(name, "Green Tablet")) return "Pairs with the Blue Tablet to unlock the hill gate and open the route to Desert Ruins.";
    if (!strcmp(name, "Top Half of Scroll")) return "Pairs with the Bottom Half of Scroll to calm the western storm and open The Volcano's Base.";
    if (!strcmp(name, "Bottom Half of Scroll")) return "Pairs with the Top Half of Scroll to calm the western storm and open The Volcano's Base.";
    if (!strcmp(name, "Lead Overalls")) return "Gives Wario the Smash Attack, which breaks cracked blocks and defeats enemies. It opens routes that require this stronger attack.";
    if (!strcmp(name, "Jar")) return "Summons a rain storm that opens the paths to The Pool of Rain and A Town in Chaos.";
    if (!strcmp(name, "First Gear for Machine")) return "One of two machine gears. Together they repair the elevator at The Pool of Rain and open the path to The Grasslands.";
    if (!strcmp(name, "Second Gear for Machine")) return "One of two machine gears. Together they repair the elevator at The Pool of Rain and open the path to The Grasslands.";
    if (!strcmp(name, "Yellow Music Box")) return "The first of five music boxes. It opens the route to The Big Bridge; all five together unlock the final battle in The Temple.";
    if (!strcmp(name, "Magic Seeds")) return "Makes stalks grow in The Vast Plain, The Pool of Rain, and The Grasslands, opening new paths.";
    if (!strcmp(name, "Skull Ring (red)")) return "Pairs with the blue Skull Ring to raise the Tower of Revival from underground.";
    if (!strcmp(name, "Skull Ring (blue)")) return "Pairs with the red Skull Ring to raise the Tower of Revival from underground.";
    if (!strcmp(name, "Swimming Flippers")) return "Lets Wario swim and dive underwater, opening routes in The Pool of Rain and The Big Bridge.";
    if (!strcmp(name, "Ornamental Fan")) return "Blows away the mist south of the world map and opens the route to The Stagnant Swamp.";
    if (!strcmp(name, "Flute")) return "Summons snakes in The Peaceful Village, The Grasslands, and The Volcano's Base, opening treasure routes.";
    if (!strcmp(name, "Blue Book")) return "Pairs with the Magic Wand to freeze the water on the east side of the map and open The Frigid Sea.";
    if (!strcmp(name, "Magic Wand")) return "Pairs with the Blue Book to freeze the water on the east side of the map and open The Frigid Sea.";
    if (!strcmp(name, "Head Smash Helmet")) return "Lets Wario break cracked and solid blocks by jumping into them from below.";
    if (!strcmp(name, "Blue Music Box")) return "The second of five music boxes. It opens the paths to Bank of the Wild River and The Tidal Coast; all five unlock the final battle.";
    if (!strcmp(name, "Trident")) return "A required treasure from Bank of the Wild River. The reference does not list a separate world-map effect, but it counts toward the story's treasure requirements.";
    if (!strcmp(name, "Yellow Book")) return "Pairs with the Lightning Wand to create a lightning storm, striking rocks and opening the route to The Steep Canyon.";
    if (!strcmp(name, "Grab Glove")) return "Lets Wario lift and throw small objects and enemies, opening routes blocked by movable objects.";
    if (!strcmp(name, "Foot of Stone")) return "Creates an earthquake that breaks boulder-blocked paths in The Volcano's Base and The Steep Canyon, and lowers water in The Stagnant Swamp.";
    if (!strcmp(name, "Tusk (blue)")) return "Pairs with the red Tusk to trigger volcanic eruptions, opening paths to The West Crater and The Colossal Hole.";
    if (!strcmp(name, "Tusk (red)")) return "Pairs with the blue Tusk to trigger volcanic eruptions, opening paths to The West Crater and The Colossal Hole.";
    if (!strcmp(name, "Green Flower")) return "A required treasure from The Stagnant Swamp. Its separate world-map effect is not specified in the reference, but it contributes to story completion.";
    if (!strcmp(name, "Right Glass Eye")) return "Pairs with the Left Glass Eye to open the blue snake door in the Tower of Revival.";
    if (!strcmp(name, "Left Glass Eye")) return "Pairs with the Right Glass Eye to open the blue snake door in the Tower of Revival.";
    if (!strcmp(name, "Super Smash")) return "Upgrades Wario's Dash Attack, letting him break solid blocks and access new paths. It also improves his ability to smash blocks with his head.";
    if (!strcmp(name, "Green Music Box")) return "The third of five music boxes. It opens the route to Cave of Flames; all five unlock the final battle.";
    if (!strcmp(name, "Scepter")) return "Purifies murky water in The Big Bridge, The Frigid Sea, and Sea Turtle Rocks, opening new paths.";
    if (!strcmp(name, "Lantern")) return "Pairs with the Magical Flame to reveal Castle of Illusions on the east side of the world map.";
    if (!strcmp(name, "Magical Flame")) return "Pairs with the Lantern to reveal Castle of Illusions on the east side of the world map.";
    if (!strcmp(name, "Super Jump Slam Overalls")) return "Strengthens Wario's Smash Attack so it can break solid blocks and stun enemies, opening new routes.";
    if (!strcmp(name, "Top Half of Sun Medallion")) return "Pairs with the Bottom Half of Sun Medallion to end the eternal night on the eastern side of the world, opening daytime routes.";
    if (!strcmp(name, "Bottom Half of Sun Medallion")) return "Pairs with the Top Half of Sun Medallion to end the eternal night on the eastern side of the world, opening daytime routes.";
    if (!strcmp(name, "High Jump Boots")) return "Lets Wario jump higher by holding Up while jumping, and bounce to greater heights from enemies.";
    if (!strcmp(name, "Red Music Box")) return "The fourth of five music boxes. It opens the route to Beneath the Waves; all five unlock the final battle.";
    if (!strcmp(name, "Explosive Plunger Box")) return "Detonates bombs attached to wooden logs in several levels, opening routes through Cave of Flames, The Stagnant Swamp, and The Colossal Hole.";
    if (!strcmp(name, "Pouch")) return "A required treasure from Cave of Flames. The reference doesn't list a separate world-map effect, but it contributes to the treasure set needed for completion.";
    if (!strcmp(name, "Eye of the Storm")) return "Causes leaves to fall in Out of the Woods, opening a hidden path inside the large tree.";
    if (!strcmp(name, "Prince Frog's Glove")) return "Lets Wario swim faster and push against strong currents, opening new routes in Bank of the Wild River, The Steep Canyon, and Beneath the Waves.";
    if (!strcmp(name, "Statue")) return "Creates climbable wire/chain sections in Tower of Revival and The Tidal Coast, opening new paths.";
    if (!strcmp(name, "Treasure Map")) return "Reveals a hidden route through the mountains and opens The East Crater.";
    if (!strcmp(name, "Super Grab Gloves")) return "Lets Wario lift heavy enemies, opening routes that require moving them.";
    if (!strcmp(name, "Gold Magic")) return "Opens the large tree in Out of the Woods, creating a new path inside.";
    if (!strcmp(name, "Gold Music Box")) return "The fifth of five music boxes. Together, the five boxes break the seal and unlock the final battle in The Temple.";
    if (!strcmp(name, "Crayon (blue)")) return "Colors the bushes on the hill between the north and west sides of the map. All seven crayons unlock the Golf mini-game.";
    if (!strcmp(name, "Truck Wheel")) return "Makes the carts in The Volcano's Base operational, opening a new path.";
    if (!strcmp(name, "Blue Gem")) return "A decorative collection treasure with no separate ability. It still counts toward the 100-treasure collection.";
    if (!strcmp(name, "Crayon (cyan)")) return "Colors the sky on the hill between the north and west sides of the map. All seven crayons unlock the Golf mini-game.";
    if (!strcmp(name, "Goblet")) return "A decorative collection treasure with no separate ability. It still counts toward the 100-treasure collection.";
    if (!strcmp(name, "Crown")) return "A decorative collection treasure with no separate ability. It still counts toward the 100-treasure collection.";
    if (!strcmp(name, "Crayon (pink)")) return "Colors the building on the hill between the north and west sides of the map. All seven crayons unlock the Golf mini-game.";
    if (!strcmp(name, "Teapot")) return "A decorative collection treasure with no separate ability. It still counts toward the 100-treasure collection.";
    if (!strcmp(name, "Pocket Pet")) return "A keepsake that resembles a pocket-sized electronic pet. It has no separate ability but counts toward the 100-treasure collection.";
    if (!strcmp(name, "Magnifying Glass")) return "Lets Wario check which treasures he has already collected on the current level tile without entering the level.";
    if (!strcmp(name, "Air-Pump")) return "Raises blocks of ice in Bank of the Wild River and The Pool of Rain, opening access to their blue chests.";
    if (!strcmp(name, "Rocket")) return "A decorative collection treasure with no separate ability. It still counts toward the 100-treasure collection.";
    if (!strcmp(name, "Crayon (yellow)")) return "Colors the sign on the hill between the north and west sides of the map. All seven crayons unlock the Golf mini-game.";
    if (!strcmp(name, "Saber")) return "A decorative collection treasure with no separate ability. It still counts toward the 100-treasure collection.";
    if (!strcmp(name, "Day or Night Spell")) return "Lets Wario switch between day and night on the world map.";
    if (!strcmp(name, "UFO")) return "A decorative collection treasure with no separate ability. It still counts toward the 100-treasure collection.";
    if (!strcmp(name, "Torch")) return "Opens the path to Forest of Fear and connects the eastern region with the northern region.";
    if (!strcmp(name, "Warp Compact")) return "Makes The Warped Void appear on the east side of the world map.";
    if (!strcmp(name, "Mystery Handle")) return "Raises a warp pipe from the ground in Forest of Fear, opening a new path.";
    if (!strcmp(name, "Warp Removal Apparatus")) return "Makes dimensional rifts in The Warped Void disappear, opening new paths.";
    if (!strcmp(name, "Crayon (brown)")) return "Colors the ground and tree trunks on the hill between the north and west sides of the map. All seven crayons unlock the Golf mini-game.";
    if (!strcmp(name, "Demon's Blood")) return "Causes zombies to emerge in an area of Forest of Fear, opening a new path.";
    if (!strcmp(name, "Key Card (red)")) return "Pairs with the blue Key Card to release the trapped Para-Goom in The Warped Void and open a new path.";
    if (!strcmp(name, "Key Card (blue)")) return "Pairs with the red Key Card to release the trapped Para-Goom in The Warped Void and open a new path.";
    if (!strcmp(name, "Heart Crest")) return "A decorative collection treasure with no separate ability. It still counts toward the 100-treasure collection.";
    if (!strcmp(name, "Minicar")) return "A decorative collection treasure with no separate ability. It still counts toward the 100-treasure collection.";
    if (!strcmp(name, "Locomotive")) return "A decorative collection treasure with no separate ability. It still counts toward the 100-treasure collection.";
    if (!strcmp(name, "Telephone")) return "A decorative collection treasure with no separate ability. It still counts toward the 100-treasure collection.";
    if (!strcmp(name, "Crayon (red)")) return "Colors the door on the hill between the north and west sides of the map. All seven crayons unlock the Golf mini-game.";
    if (!strcmp(name, "Electric Fan Propeller")) return "Creates air currents in A Town in Chaos when the wind switch is activated, opening access to its blue treasure.";
    if (!strcmp(name, "Sapling of Growth")) return "Makes the little octopi grow in The Tidal Coast and Beneath the Waves, opening routes to their chests.";
    if (!strcmp(name, "Green Gem")) return "A decorative collection treasure with no separate ability. It still counts toward the 100-treasure collection.";
    if (!strcmp(name, "Fighter Mannequin")) return "A decorative collection treasure with no separate ability. It still counts toward the 100-treasure collection.";
    if (!strcmp(name, "Golden Left Eye")) return "Pairs with the Golden Right Eye to open the golden snake door in the Tower of Revival.";
    if (!strcmp(name, "Golden Right Eye")) return "Pairs with the Golden Left Eye to open the golden snake door in the Tower of Revival.";
    if (!strcmp(name, "Sky Key")) return "Unlocks the door at the top of the Tower of Revival and opens Above the Clouds.";
    if (!strcmp(name, "Crayon (green)")) return "Colors the trees and grass on the hill between the north and west sides of the map. All seven crayons unlock the Golf mini-game.";
    if (!strcmp(name, "Fire Drencher")) return "Extinguishes flames in The West Crater and The East Crater, opening new paths.";
    if (!strcmp(name, "Diamonds Crest")) return "A decorative collection treasure with no separate ability. It still counts toward the 100-treasure collection.";
    if (!strcmp(name, "Castle Brick")) return "Fills gaps in Castle of Illusions and opens a new path.";
    if (!strcmp(name, "Rust Spray")) return "Rusts iron walls in The West Crater, The Steep Canyon, and Cave of Flames, making them breakable.";
    if (!strcmp(name, "Spades Crest")) return "A decorative collection treasure with no separate ability. It still counts toward the 100-treasure collection.";
    if (!strcmp(name, "Red Chemical")) return "Pairs with the Blue Chemical to move heavy blocks in The Vast Plain and Beneath the Waves.";
    if (!strcmp(name, "Blue Chemical")) return "Pairs with the Red Chemical to move heavy blocks in The Vast Plain and Beneath the Waves.";
    if (!strcmp(name, "Red Gem")) return "A decorative collection treasure with no separate ability. It still counts toward the 100-treasure collection.";
    if (!strcmp(name, "Clubs Crest")) return "A decorative collection treasure with no separate ability. It still counts toward the 100-treasure collection.";
    if (!strcmp(name, "Scissors")) return "Cuts the balloon in Above the Clouds loose, opening a new path.";
    if (!strcmp(name, "Jackhammer")) return "Drills through dirt blocking streams in The East Crater, opening access to its green treasure.";
    if (!strcmp(name, "Night Vision Scope")) return "Illuminates a dark room in Sea Turtle Rocks, opening access to its blue treasure.";
    if (!strcmp(name, "Full Moon Gong")) return "Summons the moon above the clouds, opening a new path in Above the Clouds.";
    if (!strcmp(name, "Pick-Axe")) return "Opens a new area in The East Crater.";
    if (!strcmp(name, "Earthen Figure")) return "A decorative collection treasure with no separate ability. It still counts toward the 100-treasure collection.";
    return "A collectible treasure from this level. It counts toward the complete set of 100 treasures.";
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

typedef struct { const char *letter; const char *name; const char *description; int course; int value; } WarioSml3TreasureDef;
static const WarioSml3TreasureDef wario_sml3_treasures[15] = {
    {"A", "Golden Horn", "A polished horn-shaped treasure from Course 03. Worth 9,000 coins toward the final reward.", 3, 9000},
    {"B", "Harp", "A harp recovered from Course 09. Worth 6,000 coins toward the final reward.", 9, 6000},
    {"C", "Crown", "A crown recovered from Course 11. Worth 2,000 coins toward the final reward.", 11, 2000},
    {"D", "Shield", "A shield recovered from Course 16. Worth 7,000 coins toward the final reward.", 16, 7000},
    {"E", "Bell", "A bell recovered from Course 17. Worth 5,000 coins toward the final reward.", 17, 5000},
    {"F", "Lamp", "A lamp recovered from Course 18. Worth 9,000 coins toward the final reward.", 18, 9000},
    {"G", "Crystal Ball", "A crystal ball recovered from Course 20. Worth 5,000 coins toward the final reward.", 20, 5000},
    {"H", "Gold Cup", "A gold cup recovered from Course 24. Worth 8,000 coins toward the final reward.", 24, 8000},
    {"I", "Dagger", "A dagger recovered from Course 26. Worth 7,000 coins toward the final reward.", 26, 7000},
    {"J", "Axe", "An axe recovered from Course 29. Worth 3,000 coins toward the final reward.", 29, 3000},
    {"K", "Conch Horn", "A conch-shaped horn recovered from Course 30. Worth 4,000 coins toward the final reward.", 30, 4000},
    {"L", "Idol", "An idol recovered from Course 31. Worth 8,000 coins toward the final reward.", 31, 8000},
    {"M", "Golden Glove", "A golden glove recovered from Course 34. Worth 4,000 coins toward the final reward.", 34, 4000},
    {"N", "Whale", "A whale-shaped treasure recovered from Course 37. Worth 7,000 coins toward the final reward.", 37, 7000},
    {"O", "Diamond Ring", "A diamond ring recovered from Course 39. Worth 6,000 coins toward the final reward.", 39, 6000}
};

static void add_wario_sml3_treasures(void)
{
    for (int i = 0; i < 15; ++i) {
        char id[40], description[200];
        snprintf(id, sizeof id, "wario1_treasure_%02d", i + 1);
        snprintf(description, sizeof description, "%s Course %02d; worth %d coins.",
                 wario_sml3_treasures[i].description,
                 wario_sml3_treasures[i].course, wario_sml3_treasures[i].value);
        add_entry(GAME_WARIO_SML3, id, wario_sml3_treasures[i].name,
                  MUSEUM_CATEGORY_TREASURE, description, "");
        MuseumEntry *e = &catalog[GAME_WARIO_SML3][catalog_count[GAME_WARIO_SML3] - 1];
        snprintf(e->source_url, sizeof e->source_url, "%s",
                 "https://www.mariowiki.com/Treasure_%28Wario_franchise%29");
    }
}


typedef struct { const char *id; const char *name; const char *description; MuseumCategory category; } SmlMuseumDef;
static const SmlMuseumDef sml_museum_defs[] = {
    {"sml_goombo", "Goombo (Chibibo)", "A small Sarasaland enemy that walks straight ahead and can wander off ledges. A stomp or Superball defeats it.", MUSEUM_CATEGORY_ENEMY},
    {"sml_piranha", "Piranha Plant", "Piranha Plants rise from pipes. Touching one from above hurts Mario; defeat them with Superballs. Standing next to or on a pipe keeps the plant from emerging.", MUSEUM_CATEGORY_ENEMY},
    {"sml_ganchan", "Ganchan Boulder", "A living boulder that bounces around and cannot be defeated. Mario can stand on one to cross gaps or avoid floor hazards; Hiyoihoi also throws them.", MUSEUM_CATEGORY_ENEMY},
    {"sml_bombshell_koopa", "Bombshell Koopa (Nokobon)", "A turtle with an explosive shell. Stomping it makes it retreat; the shell explodes shortly afterward. Superballs or a hit from below defeat it without triggering the blast.", MUSEUM_CATEGORY_ENEMY},
    {"sml_nokobon_bomb", "Nokobon Bomb", "The explosive shell left by a Bombshell Koopa. It detonates after a short delay, so move away before it explodes.", MUSEUM_CATEGORY_ENEMY},
    {"sml_pipe_fist", "Pipe Fist", "Fists punch out of pipes in pairs, alternating their strikes. Time a run or jump between punches; touching a fist is dangerous.", MUSEUM_CATEGORY_ENEMY},
    {"sml_king_totomesu", "King Totomesu", "The Birabuto Kingdom boss is a giant fire-breathing Gao. Defeat it with five Superballs or reach the switch behind it.", MUSEUM_CATEGORY_ENEMY},
    {"sml_pompon_flower", "Pompon Flower", "A walking flower that releases toxic pollen. Avoid the drifting pollen; the flower can be stomped or hit with Superballs.", MUSEUM_CATEGORY_ENEMY},
    {"sml_fly", "Fly", "A hopping insect enemy that bounces along the ground. Stomp it or hit it twice with Superballs.", MUSEUM_CATEGORY_ENEMY},
    {"sml_kumo", "Kumo", "A hairy spider found in Easton Kingdom. It hops toward Mario like a Fly; some Kumos drop down from above when they appear. Stomp one or hit it twice with Superballs.", MUSEUM_CATEGORY_ENEMY},
    {"sml_honen", "Honen", "An undead fish that leaps vertically out of the water. Unlike many enemies, Honen can be defeated with any attack.", MUSEUM_CATEGORY_ENEMY},
    {"sml_mekabon", "Mekabon", "A squat robot that throws its head at Mario. Stomping the head breaks it temporarily, but it can regrow; attack the headless body to finish the robot.", MUSEUM_CATEGORY_ENEMY},
    {"sml_dragonzamasu", "Dragonzamasu", "The Muda Kingdom boss is a giant fire-breathing Yurarin Boo. It swims up and down and shoots fireballs; defeat it with Marine Pop torpedoes or by hitting the switch behind it.", MUSEUM_CATEGORY_ENEMY},
    {"sml_tamao", "Tamao", "An egg-shaped creature that appears around Dragonzamasu. It bounces off walls and ceilings and hurts Mario on contact.", MUSEUM_CATEGORY_ENEMY},
    {"sml_yurarin", "Yurarin", "A seahorse enemy that swims horizontally or diagonally through underwater stages. The Marine Pop can defeat it with torpedoes.", MUSEUM_CATEGORY_ENEMY},
    {"sml_yurarin_boo", "Yurarin Boo", "A fire-breathing seahorse that moves vertically and shoots fireballs toward Mario. It can be stomped or defeated with Marine Pop torpedoes.", MUSEUM_CATEGORY_ENEMY},
    {"sml_suu", "Suu", "A striped spider that descends from the ceiling on a thread. Watch overhead spaces; a stomp or two Superballs can defeat it.", MUSEUM_CATEGORY_ENEMY},
    {"sml_gunion", "Gunion", "An octopus enemy in the underwater stages. Three Marine Pop torpedo hits defeat it, but it then splits into two fireballs that target Mario.", MUSEUM_CATEGORY_ENEMY},
    {"sml_torion", "Torion", "A fish that appears in schools of three and swims through Muda Kingdom's underwater stages. The Marine Pop can defeat Torion with torpedoes.", MUSEUM_CATEGORY_ENEMY},
    {"sml_tokotoko", "Tokotoko", "A stone-head enemy that shuffles quickly across the ground. Avoid its charge with a jump or defeat it with a stomp or Superball.", MUSEUM_CATEGORY_ENEMY},
    {"sml_hiyoihoi", "Hiyoihoi", "The Easton Kingdom boss hurls Ganchan boulders at Mario. Defeat it with Superballs or use a boulder to reach the switch behind it.", MUSEUM_CATEGORY_ENEMY},
    {"sml_batadon", "Batadon", "A winged stone head that bounces along the ground and chases Mario. A stomp or repeated Superballs can defeat it.", MUSEUM_CATEGORY_ENEMY},
    {"sml_gao", "Gao / Nyololin", "Gao are lion-like sphinxes in Birabuto; Nyololin are snake-like enemies in Chai. Both spit fireballs toward Mario and can be beaten with a stomp or Superballs.", MUSEUM_CATEGORY_ENEMY},
    {"sml_bunbun", "Bunbun", "A flying bee that carries arrows. It pauses while flying to drop arrows that pass through terrain; stomp it or hit it with a Superball.", MUSEUM_CATEGORY_ENEMY},
    {"sml_pipe_cannon", "Pipe Cannon", "A cannon that rises from a pipe and fires Bullet Biffs at intervals, regardless of how close Mario is. Mario can safely stand on top of the cannon.", MUSEUM_CATEGORY_ENEMY},
    {"sml_gira", "Gira / Bullet Biff", "A bullet-like projectile fired by Pipe Cannons and Roketon. It flies horizontally across the screen and can be defeated by stomping it.", MUSEUM_CATEGORY_ENEMY},
    {"sml_rocketon", "Roketon", "An enemy pilot working for Tatanga in the Sky Pop stage. Roketon fires Bullet Biffs backward as Mario flies past; shoot it down with a missile.", MUSEUM_CATEGORY_ENEMY},
    {"sml_chicken", "Chicken", "A helmeted bird released by Biokinton in the final airborne stage. Contact hurts Mario; the Sky Pop can shoot it down with missiles.", MUSEUM_CATEGORY_ENEMY},
    {"sml_roto_disc", "Roto Disc", "A ring of spinning flames that circles blocks in Chai Kingdom. Touching the flames costs Mario a life, so time your movement around them.", MUSEUM_CATEGORY_ENEMY},
    {"sml_reverse_piranha", "Headstand Piranha Plant", "An upside-down Piranha Plant that extends from overhead pipes. Unlike ordinary Piranha Plants, it cannot be kept inside its pipe by standing nearby.", MUSEUM_CATEGORY_ENEMY},
    {"sml_pionpi", "Pionpi", "An undead enemy that hops along the ground. A stomp only stuns it briefly; two Superball hits are needed to defeat it.", MUSEUM_CATEGORY_ENEMY},
    {"sml_chikako", "Chikako", "A hovering electrical enemy in the Sky Pop stage. It takes ten missile hits to destroy, unless Mario touches it while invincible.", MUSEUM_CATEGORY_ENEMY},
    {"sml_tatanga", "Tatanga", "The alien who kidnapped Princess Daisy and the final boss. Fight him aboard the Sky Pop, avoid shots that split into three smaller projectiles, and fire missiles at his ship.", MUSEUM_CATEGORY_ENEMY},
    {"sml_biokinton", "Biokinton", "A bouncing cloud boss that releases Chickens at Mario. The Sky Pop must hit it with twenty missiles to defeat it.", MUSEUM_CATEGORY_ENEMY},
    {"sml_mushroom", "Super Mushroom", "Turns Small Mario into Super Mario. Super Mario can break bricks and take one hit before shrinking back to Small Mario.", MUSEUM_CATEGORY_ITEM},
    {"sml_heart", "1-Up Heart", "Grants Mario one extra life. It can emerge from a Mystery Block or brick and falls through the floor, so collect it before it disappears.", MUSEUM_CATEGORY_ITEM},
    {"sml_star", "Star", "Makes Mario invincible for a short time. During the effect, touching most enemies defeats them.", MUSEUM_CATEGORY_ITEM},
    {"sml_superball_flower", "Superball Flower", "Upgrades Super Mario into Superball Mario, who throws bouncing Superballs to defeat enemies and collect coins. Small Mario first becomes Super Mario when he picks up a Flower.", MUSEUM_CATEGORY_ITEM}
};
static void add_sml_catalog(void)
{
    for (size_t i = 0; i < sizeof sml_museum_defs / sizeof sml_museum_defs[0]; ++i)
        add_entry(GAME_SML, sml_museum_defs[i].id, sml_museum_defs[i].name,
                  sml_museum_defs[i].category, sml_museum_defs[i].description, "");
    add_entry(GAME_SML, "coin", "Coin", MUSEUM_CATEGORY_ITEM,
              "Each coin adds 100 points; collecting 100 coins awards Mario an extra life.", "coin");
    add_entry(GAME_SML, "power_up", "Power-up", MUSEUM_CATEGORY_ITEM,
              "Mario gained a stronger form from a power-up. The named Mushroom, Superball Flower, and Star entries describe those items.", "power");
    add_entry(GAME_SML, "block", "Mystery Block or Brick", MUSEUM_CATEGORY_ITEM,
              "Blocks and bricks can hide coins or power-ups. Mario can hit them from below, and Super Mario can break bricks.", "bump");
    add_entry(GAME_SML, "life_lost", "Life lost", MUSEUM_CATEGORY_MILESTONE,
              "Small Mario loses a life when hit by an enemy or certain hazards. Powered-up Mario usually shrinks instead; after losing a life, Mario returns in Small form.", "life");
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

    /* Super Mario Land 2: the shared Museum catalogue uses Mario-style categories,
       never the Wario-only treasure shelf. Generic event records are wired to
       reliable counter signals; named specimens remain locked until their own
       game-specific detection is available. */
    add_entry(GAME_SML2, "coin", "Coin", MUSEUM_CATEGORY_ITEM,
              "A common coin found throughout Mario Land. Coins can be spent at the Game Boy's bonus-game house for chances to win extra lives and prizes.", "coin");
    add_entry(GAME_SML2, "enemy_defeated", "Enemy encounter", MUSEUM_CATEGORY_ENEMY,
              "Mario defeated an enemy. The current signal tracks the game's defeated-enemy counter, but does not yet identify the exact species.", "enemy");
    add_entry(GAME_SML2, "extra_life", "Extra life", MUSEUM_CATEGORY_ITEM,
              "An extra life was awarded. The game represents these rewards with hearts rather than the familiar green 1-Up Mushroom.", "oneup");
    add_entry(GAME_SML2, "life_lost", "Life lost", MUSEUM_CATEGORY_MILESTONE,
              "Mario lost a life during the adventure. Ordinary stage progress remains, but the six Golden Coins must be reclaimed if a Game Over returns them to their guardians.", "life");
    add_entry(GAME_SML2, "mushroom", "Super Mushroom", MUSEUM_CATEGORY_ITEM,
              "Transforms Small Mario into Super Mario, allowing him to take one hit before shrinking back down.", "");
    add_entry(GAME_SML2, "fire_flower", "Fire Flower", MUSEUM_CATEGORY_ITEM,
              "Transforms Mario into Fire Mario, who can throw bouncing fireballs. In this Game Boy game, Fire Mario's cap has a distinctive feather.", "");
    add_entry(GAME_SML2, "magic_carrot", "Magic Carrot", MUSEUM_CATEGORY_ITEM,
              "Transforms Mario into Bunny Mario. His ears let him flutter and slow his descent, making it possible to reach otherwise difficult platforms.", "");
    add_entry(GAME_SML2, "starman", "Super Star", MUSEUM_CATEGORY_ITEM,
              "Grants temporary invincibility, letting Mario defeat most enemies by touching them.", "");
    add_entry(GAME_SML2, "golden_coins", "Six Golden Coins", MUSEUM_CATEGORY_MILESTONE,
              "Six unique coins are held by the guardians of Tree, Turtle, Macro, Pumpkin, Mario, and Space Zones. Recovering all six opens the way to Mario's castle.", "");
    add_entry(GAME_SML2, "wario", "Wario", MUSEUM_CATEGORY_CHARACTER,
              "Mario's rival has taken over Mario's castle. The final confrontation unfolds in three rounds, each with a different power-up form.", "");
    add_entry(GAME_SML2, "zone_bosses", "Zone guardians", MUSEUM_CATEGORY_ENEMY,
              "Each of the six main zones has a guardian protecting its Golden Coin, including Big Bird, the octopus, the Three Little Pigheads, Tatanga, the Sewer Rat, and the Witch.", "");
    add_entry(GAME_SML2, "secret_exit", "Secret exit", MUSEUM_CATEGORY_STAGE,
              "Some courses contain an alternate exit that opens a hidden course on the world map. Look for the star-marked goal door.", "");
    add_entry(GAME_SML2, "castle_cleared", "Castle reclaimed", MUSEUM_CATEGORY_MILESTONE,
              "Mario has defeated Wario and reclaimed his castle. This marks completion of the main adventure.", "");

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
    SDL_AtomicLock(&discovery_notice_lock);
    discovery_notice_head = discovery_notice_count = 0;
    SDL_AtomicUnlock(&discovery_notice_lock);
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
    case 0x33: case 0x47: id = "sml_ganchan"; break; /* Normal and single boulders. */
    case 0x04: id = "sml_bombshell_koopa"; break;
    case 0x05: id = "sml_nokobon_bomb"; break;
    case 0x06: id = "sml_pipe_fist"; break;
    case 0x08: id = "sml_king_totomesu"; break;
    case 0x09: id = "sml_pompon_flower"; break;
    case 0x0E: case 0x0F: id = "sml_fly"; break; /* 0x0F is its flattened state. */
    case 0x10: id = "sml_honen"; break;
    case 0x16: case 0x17: case 0x18: id = "sml_mekabon"; break;
    case 0x1A: id = "sml_dragonzamasu"; break;
    case 0x1D: id = "sml_yurarin"; break;
    case 0x20: id = "sml_kumo"; break; /* Floating Kumo variant in the object table. */
    case 0x24: id = "sml_yurarin_boo"; break;
    case 0x25: id = "sml_suu"; break;
    case 0x29: cat = MUSEUM_CATEGORY_ITEM; id = "sml_mushroom"; break;
    case 0x2B: cat = MUSEUM_CATEGORY_ITEM; id = "sml_heart"; break;
    case 0x2C: case 0x34: cat = MUSEUM_CATEGORY_ITEM; id = "sml_star"; break;
    case 0x2D: case 0x2E: cat = MUSEUM_CATEGORY_ITEM; id = "sml_superball_flower"; break;
    case 0x2F: case 0x30: id = "sml_torion"; break;
    case 0x31: id = "sml_tokotoko"; break;
    case 0x32: id = "sml_hiyoihoi"; break;
    case 0x3C: id = "sml_batadon"; break;
    case 0x3F: case 0x40: id = "sml_gao"; break; /* 0x40 is Gao's defeated state. */
    case 0x42: case 0x43: id = "sml_bunbun"; break;
    case 0x48: id = "sml_tamao"; break;
    case 0x49: id = "sml_pipe_cannon"; break;
    case 0x4A: case 0x4B: case 0x5A: id = "sml_gira"; break; /* High/low Bullet Biffs and Roketon's shot. */
    case 0x52: id = "sml_rocketon"; break;
    case 0x53: id = "sml_chicken"; break;
    case 0x54: id = "sml_roto_disc"; break;
    case 0x55: id = "sml_reverse_piranha"; break;
    case 0x56: case 0x57: id = "sml_pionpi"; break;
    case 0x59: id = "sml_chikako"; break;
    case 0x5B: case 0x60: id = "sml_tatanga"; break;
    case 0x61: case 0x62: id = "sml_biokinton"; break;
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
    int n;
    if (game == GAME_SML)
        n = snprintf(out, out_size, "%s%spipeclean-museum-assets/%s/%s.v2.png",
                     base, sep, games[game].id, id);
    else
        n = snprintf(out, out_size, "%s%spipeclean-museum-assets/%s/%s.png",
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
    museum_queue_discovery_notice(&catalog[game][idx]);
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

int museum_clear_discoveries(void)
{
    museum_init();
    if (!storage_path[0]) {
        snprintf(storage_error, sizeof storage_error, "Museum storage path is unavailable");
        return 0;
    }
    FILE *f = fopen(storage_path, "w");
    if (!f) {
        snprintf(storage_error, sizeof storage_error,
                 "Couldn't reset Museum progress: %.100s", storage_path);
        return 0;
    }
    if (fclose(f) != 0) {
        snprintf(storage_error, sizeof storage_error, "Couldn't finish resetting Museum progress");
        return 0;
    }

    memset(discovered, 0, sizeof discovered);
    SDL_AtomicLock(&discovery_notice_lock);
    discovery_notice_head = discovery_notice_count = 0;
    SDL_AtomicUnlock(&discovery_notice_lock);
    storage_error[0] = 0;
    return 1;
}

const char *museum_last_storage_error(void) { return storage_error; }

int museum_take_notification(char *out, size_t out_size)
{
    if (!out || out_size == 0) return 0;
    out[0] = 0;
    SDL_AtomicLock(&discovery_notice_lock);
    if (!settings.museum_discovery_notifications) {
        discovery_notice_head = discovery_notice_count = 0;
        SDL_AtomicUnlock(&discovery_notice_lock);
        return 0;
    }
    if (discovery_notice_count <= 0) {
        SDL_AtomicUnlock(&discovery_notice_lock);
        return 0;
    }
    snprintf(out, out_size, "%s", discovery_notices[discovery_notice_head]);
    discovery_notices[discovery_notice_head][0] = 0;
    discovery_notice_head = (discovery_notice_head + 1) % MUSEUM_NOTICE_QUEUE_CAP;
    --discovery_notice_count;
    SDL_AtomicUnlock(&discovery_notice_lock);
    return 1;
}
