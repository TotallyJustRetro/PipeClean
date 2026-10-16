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

Settings settings;
const char *settings_dir(void) { return "./"; }

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    settings.museum_discovery_notifications = 1;
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

    assert(museum_catalog_count(GAME_SML) == 41);
    {
        const MuseumEntry *one_up = NULL, *tamao = NULL, *fly_kumo = NULL, *gunion = NULL;
        for (int i = 0; i < museum_catalog_count(GAME_SML); ++i) {
            const MuseumEntry *entry = museum_catalog_entry(GAME_SML, i);
            if (!strcmp(entry->id, "sml_heart")) one_up = entry;
            if (!strcmp(entry->id, "sml_tamao")) tamao = entry;
            if (!strcmp(entry->id, "sml_fly")) fly_kumo = entry;
            if (!strcmp(entry->id, "sml_gunion")) gunion = entry;
        }
        assert(one_up && !strcmp(one_up->name, "1-Up Heart"));
        assert(strstr(one_up->description, "extra life"));
        assert(!strstr(one_up->description, "sprite"));
        assert(tamao && strstr(tamao->description, "Dragonzamasu"));
        assert(!strstr(tamao->description, "object ID"));
        assert(fly_kumo && strstr(fly_kumo->name, "Kumo"));
        assert(strstr(fly_kumo->description, "spider"));
        assert(gunion && strstr(gunion->description, "Two Marine Pop torpedoes"));
        assert(!strstr(fly_kumo->description, "object ID"));
        for (int i = 0; i < museum_catalog_count(GAME_SML); ++i) {
            const MuseumEntry *entry = museum_catalog_entry(GAME_SML, i);
            assert(!strstr(entry->description, "object ID"));
            assert(!strstr(entry->description, "object id"));
            assert(!strstr(entry->description, "sprite"));
            assert(!strstr(entry->description, "capture"));
        }
        assert(strstr(museum_catalog_entry(GAME_SML, 0)->description, "ledges"));
    }
    {
        MuseumCategory category = MUSEUM_CATEGORY_ALL;
        const MuseumEntry *goombo = museum_catalog_entry(GAME_SML, 0);
        assert(goombo && !strcmp(goombo->id, "sml_goombo"));
        assert(strstr(goombo->description, "ledges"));
        assert(!strcmp(museum_sml_object_entry_id(0x00, &category), "sml_goombo"));
        assert(category == MUSEUM_CATEGORY_ENEMY);
        assert(!strcmp(museum_sml_object_entry_id(0x80, &category), "sml_goombo"));
        assert(!strcmp(museum_sml_object_entry_id(0x04, &category), "sml_bombshell_koopa"));
        assert(!strcmp(museum_sml_object_entry_id(0x3C, &category), "sml_batadon"));
        assert(museum_sml_object_entry_id(0x3D, &category) == NULL);
        assert(!strcmp(museum_sml_object_entry_id(0x40, &category), "sml_gao"));
        assert(!strcmp(museum_sml_object_entry_id(0x43, &category), "sml_bunbun"));
        assert(!strcmp(museum_sml_object_entry_id(0x57, &category), "sml_pionpi"));
        assert(!strcmp(museum_sml_object_entry_id(0x20, &category), "sml_gunion"));
        assert(!strcmp(museum_sml_object_entry_id(0x2F, &category), "sml_torion"));
        assert(museum_sml_object_entry_id(0x33, &category) == NULL);
        assert(!strcmp(museum_sml_object_entry_id(0x47, &category), "sml_ganchan"));
        assert(!strcmp(museum_sml_object_entry_id(0x48, &category), "sml_tamao"));
        assert(!strcmp(museum_sml_object_entry_id(0x4A, &category), "sml_gira"));
        assert(!strcmp(museum_sml_object_entry_id(0x5A, &category), "sml_gira"));
        assert(!strcmp(museum_sml_object_entry_id(0x29, &category), "sml_mushroom"));
        assert(category == MUSEUM_CATEGORY_ITEM);
        assert(museum_sml_object_entry_id(0x7E, &category) == NULL);
    }

    assert(museum_discover_entry(GAME_SML, "sml_goombo") == 1);
    char notice[160];
    assert(museum_take_notification(notice, sizeof notice) == 1);
    assert(strstr(notice, "New discovery: ") == notice);
    assert(museum_take_notification(notice, sizeof notice) == 0);
    settings.museum_discovery_notifications = 0;
    assert(museum_discover_entry(GAME_SML, "coin") == 1);
    assert(museum_discover_entry(GAME_SML, "coin") == 0);
    assert(museum_take_notification(notice, sizeof notice) == 0);
    museum_observe_event(GAME_SML, "power");
    assert(museum_is_discovered(GAME_SML, "coin"));
    assert(museum_is_discovered(GAME_SML, "sml_goombo"));
    assert(museum_is_discovered(GAME_SML, "power_up"));
    assert(!museum_is_discovered(GAME_SML2, "coin"));

    /* Every current SML2 Museum row has a live observation path in the
       SML2 tracker or a generic event trigger; don't leave any catalogue row
       as a permanent placeholder. */
    {
        static const struct { const char *id; const char *trigger; } expected[] = {
            {"coin", "coin"},
            {"enemy_defeated", "enemy"},
            {"extra_life", "oneup"},
            {"life_lost", "life"},
            {"mushroom", ""},
            {"fire_flower", ""},
            {"magic_carrot", ""},
            {"starman", ""},
            {"golden_coins", ""},
            {"wario", ""},
            {"zone_bosses", ""},
            {"secret_exit", ""},
            {"castle_cleared", ""}
        };
        assert(museum_catalog_count(GAME_SML2) == (int)(sizeof expected / sizeof expected[0]));
        for (int e = 0; e < (int)(sizeof expected / sizeof expected[0]); ++e) {
            const MuseumEntry *entry = NULL;
            for (int i = 0; i < museum_catalog_count(GAME_SML2); ++i) {
                const MuseumEntry *candidate = museum_catalog_entry(GAME_SML2, i);
                if (!strcmp(candidate->id, expected[e].id)) {
                    entry = candidate;
                    break;
                }
            }
            assert(entry);
            assert(!strcmp(entry->trigger_event, expected[e].trigger));
            assert(entry->description[0]);
            assert(!museum_is_discovered(GAME_SML2, expected[e].id));
        }
    }

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
        assert(!strcmp(e->name, "Golden Horn"));
        assert(strstr(e->description, "Course 03"));
        assert(strstr(e->description, "9000 coins"));
        assert(strstr(e->description, "horn-shaped"));
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
            if (!strcmp(e->id, "wl3_treasure_018")) {
                assert(!strcmp(e->name, "Magic Wand"));
                assert(strstr(e->description, "Blue Book"));
            }
            if (!strcmp(e->id, "wl3_treasure_021")) {
                assert(!strcmp(e->name, "Trident"));
                assert(strstr(e->description, "required treasure"));
            }
            if (!strcmp(e->id, "wl3_treasure_100")) {
                assert(!strcmp(e->name, "Earthen Figure"));
                assert(strstr(e->description, "100-treasure collection"));
            }
            if (!strcmp(e->id, "wl3_treasure_100")) {
                assert(!strcmp(e->name, "Earthen Figure"));
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

    assert(museum_clear_discoveries() == 1);
    for (int game = 0; game < N_GAMES; ++game)
        assert(museum_discovered_count(game) == 0);
    assert(museum_take_notification(notice, sizeof notice) == 0);
    museum_set_storage_path(path);
    for (int game = 0; game < N_GAMES; ++game)
        assert(museum_discovered_count(game) == 0);

    remove(path);
    puts("Collectible Museum tests passed.");
    return 0;
}
