#pragma once

#include "games.h"
#include <stdint.h>
#include <stddef.h>

typedef enum {
    MUSEUM_CATEGORY_ALL = -1,
    MUSEUM_CATEGORY_ENEMY = 0,
    MUSEUM_CATEGORY_ITEM,
    MUSEUM_CATEGORY_TREASURE,
    MUSEUM_CATEGORY_STAGE,
    MUSEUM_CATEGORY_MILESTONE,
    MUSEUM_CATEGORY_CHARACTER,
    MUSEUM_CATEGORY_COUNT
} MuseumCategory;

typedef struct {
    int game;
    char id[40];
    char name[80];
    MuseumCategory category;
    char description[200];
    char source_url[192];
    char trigger_event[24];
} MuseumEntry;

/* Catalogues contain original, concise summaries and source links; copyrighted
 * sprite assets are not bundled. Discovery records are stored locally. */
void museum_init(void);
void museum_set_storage_path(const char *path); /* Also used by isolated tests. */
int museum_catalog_count(int game);
const char *museum_sml_object_entry_id(uint8_t raw_type, MuseumCategory *category);
const MuseumEntry *museum_catalog_entry(int game, int index);
int museum_is_discovered(int game, const char *entry_id);
int museum_discover_entry(int game, const char *entry_id);
/* Resolves a local per-game sprite PNG only when the file exists. Assets are
 * never bundled; callers must also require a valid matching ROM before draw. */
int museum_sprite_asset_path(int game, const char *entry_id, char *out, size_t out_size);
void museum_observe_event(int game, const char *event_id);
void museum_sync_flags(int game, const char *id_prefix, uint64_t flags, int count);
int museum_discovered_count(int game);
const char *museum_category_name(MuseumCategory category);
const char *museum_last_storage_error(void);
/* Returns one queued new-discovery notification; disabled notifications are discarded. */
int museum_take_notification(char *out, size_t out_size);
