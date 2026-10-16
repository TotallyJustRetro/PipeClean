#pragma once

#include <stdint.h>

/* Live Super Mario Land 2 cartridge-RAM observations, already decoded into
 * ordinary counter values by events.c. Invalid/non-gameplay frames are ignored
 * so title screens and short fades cannot create Museum discoveries. */
typedef struct {
    int valid;
    int gameplay;
    int coins;
    int enemy_kills;
    int lives;
    int powerup;
    int star_active;
    int level;
    int ending_flag;
    int secret_exit_seen;
} Sml2MuseumSnapshot;

enum {
    SML2_MUSEUM_COIN          = 1u << 0,
    SML2_MUSEUM_ENEMY         = 1u << 1,
    SML2_MUSEUM_LIFE_LOST     = 1u << 2,
    SML2_MUSEUM_EXTRA_LIFE    = 1u << 3,
    SML2_MUSEUM_MUSHROOM      = 1u << 4,
    SML2_MUSEUM_MAGIC_CARROT  = 1u << 5,
    SML2_MUSEUM_FIRE_FLOWER   = 1u << 6,
    SML2_MUSEUM_SUPER_STAR    = 1u << 7,
    SML2_MUSEUM_GOLDEN_COINS  = 1u << 8,
    SML2_MUSEUM_WARIO         = 1u << 10,
    SML2_MUSEUM_SECRET_EXIT   = 1u << 11,
    SML2_MUSEUM_CASTLE_CLEARED= 1u << 12
};

typedef struct {
    Sml2MuseumSnapshot previous;
    int have_previous;
} Sml2MuseumTracker;

static inline uint32_t sml2_museum_powerup_discovery(int powerup)
{
    /* SML2 RAM: 00 Small, 01 Super, 02 Bunny, 03 Fire, 04+ Super. */
    if (powerup == 1 || (powerup >= 4 && powerup <= 0x0F))
        return SML2_MUSEUM_MUSHROOM;
    if (powerup == 2) return SML2_MUSEUM_MAGIC_CARROT;
    if (powerup == 3) return SML2_MUSEUM_FIRE_FLOWER;
    return 0;
}

static inline int sml2_museum_is_secret_level(int level)
{
    /* The six secret courses occupy level IDs 19..1E; 1F is the final castle. */
    return level >= 0x19 && level <= 0x1E;
}

static inline uint32_t sml2_museum_final_area_discoveries(int level)
{
    /* Reaching the castle proves the six-coin route is open, not that Mario has
     * met Wario. The boss record must wait for the live species detector. */
    return level == 0x1F ? SML2_MUSEUM_GOLDEN_COINS : 0;
}

static inline uint32_t sml2_museum_tracker_update(Sml2MuseumTracker *tracker,
                                                  const Sml2MuseumSnapshot *current)
{
    if (!tracker || !current || !current->valid || !current->gameplay ||
        current->coins < 0 || current->coins > 999 ||
        current->enemy_kills < 0 || current->enemy_kills > 99 ||
        current->lives < 0 || current->lives > 99 ||
        current->powerup < 0 || current->powerup > 0x0F ||
        current->level < 0 || current->level > 0x1F)
        return 0;

    uint32_t discoveries = 0;
    if (!tracker->have_previous) {
        /* The player may resume directly in a powered-up state or an already
         * unlocked endgame. Those states are valid evidence for the Museum. */
        /* A resumed save may already have non-zero counters before PipeClean
           starts observing it; those values are evidence of real encounters. */
        if (current->coins > 0) discoveries |= SML2_MUSEUM_COIN;
        if (current->enemy_kills > 0) discoveries |= SML2_MUSEUM_ENEMY;
        discoveries |= sml2_museum_powerup_discovery(current->powerup);
        if (current->star_active) discoveries |= SML2_MUSEUM_SUPER_STAR;
        discoveries |= sml2_museum_final_area_discoveries(current->level);
        if (current->ending_flag) discoveries |= SML2_MUSEUM_CASTLE_CLEARED;
        if (current->secret_exit_seen) discoveries |= SML2_MUSEUM_SECRET_EXIT;
        tracker->previous = *current;
        tracker->have_previous = 1;
        return discoveries;
    }

    const Sml2MuseumSnapshot old = tracker->previous;
    const int same_level = old.level == current->level;

    if (current->coins > old.coins)
        discoveries |= SML2_MUSEUM_COIN;
    /* The kill counter wraps after 99; any in-level change therefore counts. */
    if (same_level && current->enemy_kills != old.enemy_kills)
        discoveries |= SML2_MUSEUM_ENEMY;
    if (same_level && current->lives < old.lives)
        discoveries |= SML2_MUSEUM_LIFE_LOST;
    if (same_level && current->lives > old.lives)
        discoveries |= SML2_MUSEUM_EXTRA_LIFE;

    if (current->powerup != old.powerup)
        discoveries |= sml2_museum_powerup_discovery(current->powerup);
    if (current->star_active && !old.star_active)
        discoveries |= SML2_MUSEUM_SUPER_STAR;

    discoveries |= sml2_museum_final_area_discoveries(current->level) &
                   ~sml2_museum_final_area_discoveries(old.level);
    if (current->ending_flag && !old.ending_flag)
        discoveries |= SML2_MUSEUM_CASTLE_CLEARED;
    if (current->secret_exit_seen && !old.secret_exit_seen)
        discoveries |= SML2_MUSEUM_SECRET_EXIT;

    tracker->previous = *current;
    return discoveries;
}
