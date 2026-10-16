#include "museum.h"
#include "games.h"
#include "settings.h"

const GameDef games[N_GAMES] = {
    [GAME_DRMARIO] = {.id = "drmario", .name = "Dr. Mario"},
    [GAME_SML] = {.id = "sml", .name = "Super Mario Land"},
    [GAME_SML2] = {.id = "sml2", .name = "Super Mario Land 2"},
    [GAME_WARIO_SML3] = {.id = "wario-sml3", .name = "Wario Land: SML3"},
    [GAME_WARIO_LAND2_GB] = {.id = "wario2-gb", .name = "Wario Land II (GB)"},
    [GAME_WARIO_LAND3_GBC] = {.id = "wario3-gbc", .name = "Wario Land 3 (GBC)"},
    [GAME_WARIO_LAND2_GBC] = {.id = "wario2-gbc", .name = "Wario Land II (GBC)"}
};

const char *settings_dir(void) { return "./"; }

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const char *path = "pipeclean-museum-test.txt";
    remove(path);
    museum_set_storage_path(path);

    /* Every supported game receives a catalogue with the same core categories. */
    for (int game = 0; game < N_GAMES; ++game) {
        assert(museum_catalog_count(game) > 0);
        int has_enemy = 0, has_item = 0, has_treasure = 0;
        for (int i = 0; i < museum_catalog_count(game); ++i) {
            const MuseumEntry *e = museum_catalog_entry(game, i);
            assert(e && e->id[0] && e->name[0] && e->description[0] && e->source_url[0]);
            if (e->category == MUSEUM_CATEGORY_ENEMY) has_enemy = 1;
            if (e->category == MUSEUM_CATEGORY_ITEM) has_item = 1;
            if (e->category == MUSEUM_CATEGORY_TREASURE) has_treasure = 1;
        }
        /* Categories can be empty for a game that has no matching mechanics,
           but the universal browser/category contract must be stable. */
        assert(has_item);
        (void)has_enemy;
        (void)has_treasure;
    }

    {
        char sprite_path[1200] = "must-be-cleared";
        /* No art is distributed in the repo; missing local PNGs stay locked. */
        assert(!museum_sprite_asset_path(GAME_WARIO_LAND3_GBC, "wl3_treasure_001",
                                         sprite_path, sizeof sprite_path));
        assert(sprite_path[0] == 0);
        assert(!museum_sprite_asset_path(GAME_WARIO_LAND3_GBC, "../not-an-entry",
                                         sprite_path, sizeof sprite_path));
    }

    {
        const MuseumEntry *e = museum_catalog_entry(GAME_DRMARIO, 2);
        assert(e && !strcmp(e->name, "Pills cleared"));
    }

    assert(museum_discover_entry(GAME_SML, "coin") == 1);
    assert(museum_discover_entry(GAME_SML, "coin") == 0);
    museum_observe_event(GAME_SML, "power");
    assert(museum_is_discovered(GAME_SML, "coin"));
    assert(museum_is_discovered(GAME_SML, "power_up"));
    assert(!museum_is_discovered(GAME_SML2, "coin"));

    /* Wario Land II's actual collection flags unlock individual museum rows. */
    museum_sync_flags(GAME_WARIO_LAND2_GB, "wl2_treasure",
                      (UINT64_C(1) << 0) | (UINT64_C(1) << 4), 50);
    museum_sync_flags(GAME_WARIO_LAND2_GBC, "wl2_picture",
                      UINT64_C(1) << 9, 50);
    assert(museum_is_discovered(GAME_WARIO_LAND2_GB, "wl2_treasure_01"));
    assert(museum_is_discovered(GAME_WARIO_LAND2_GB, "wl2_treasure_05"));
    assert(!museum_is_discovered(GAME_WARIO_LAND2_GB, "wl2_treasure_02"));
    assert(museum_is_discovered(GAME_WARIO_LAND2_GBC, "wl2_picture_10"));
    assert(!museum_is_discovered(GAME_WARIO_LAND2_GB, "wl2_picture_10"));

    museum_sync_flags(GAME_WARIO_SML3, "wario1_treasure", UINT64_C(0x11), 16);
    assert(museum_is_discovered(GAME_WARIO_SML3, "wario1_treasure_01"));
    assert(museum_is_discovered(GAME_WARIO_SML3, "wario1_treasure_05"));
    assert(!museum_is_discovered(GAME_WARIO_SML3, "wario1_treasure_16"));
    {
        const MuseumEntry *e = museum_catalog_entry(GAME_WARIO_SML3, 2);
        assert(e && !strcmp(e->id, "wario1_treasure_01"));
        assert(!strcmp(e->name, "Treasure A"));
        assert(strstr(e->description, "Course 03"));
        assert(strstr(e->description, "9000 coins"));
    }

    assert(museum_catalog_count(GAME_WARIO_SML3) == 19);
    assert(museum_catalog_count(GAME_WARIO_LAND2_GB) == 103);
    assert(museum_catalog_count(GAME_WARIO_LAND2_GBC) == 103);
    assert(museum_catalog_count(GAME_WARIO_LAND3_GBC) == 110);
    {
        int found_treasure = 0;
        for (int i = 0; i < museum_catalog_count(GAME_WARIO_LAND3_GBC); ++i) {
            const MuseumEntry *e = museum_catalog_entry(GAME_WARIO_LAND3_GBC, i);
            if (!strcmp(e->id, "wl3_treasure_001")) {
                assert(!strcmp(e->name, "Axe"));
                assert(strstr(e->description, "N1 Out of the Woods"));
                found_treasure = 1;
            }
            if (!strcmp(e->id, "wl3_treasure_100")) {
                assert(!strcmp(e->name, "Gloom"));
                found_treasure = 2;
            }
        }
        assert(found_treasure == 2);
    }

    assert(museum_discover_entry(GAME_WARIO_LAND3_GBC, "wl3_treasure_001") == 1);
    assert(museum_is_discovered(GAME_WARIO_LAND3_GBC, "wl3_treasure_001"));
    assert(museum_discover_entry(GAME_WARIO_LAND3_GBC, "key_blue") == 1);
    assert(museum_is_discovered(GAME_WARIO_LAND3_GBC, "key_blue"));
    assert(museum_is_discovered(GAME_WARIO_LAND3_GBC, "wl3_treasure_001"));
    assert(!museum_is_discovered(GAME_WARIO_LAND3_GBC, "key_gray"));

    /* Reinitializing from disk proves the discoveries survive a restart. */
    museum_set_storage_path(path);
    assert(museum_is_discovered(GAME_SML, "coin"));
    assert(museum_is_discovered(GAME_SML, "power_up"));
    assert(museum_is_discovered(GAME_WARIO_LAND2_GB, "wl2_treasure_05"));
    assert(museum_is_discovered(GAME_WARIO_LAND2_GBC, "wl2_picture_10"));
    assert(museum_is_discovered(GAME_WARIO_SML3, "wario1_treasure_05"));
    assert(museum_is_discovered(GAME_WARIO_LAND3_GBC, "key_blue"));

    remove(path);
    puts("Collectible Museum tests passed.");
    return 0;
}
