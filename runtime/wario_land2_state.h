#pragma once
#include <stddef.h>
#include <stdint.h>
#include "games.h"

/* Wario Land II uses matching 50-bit collections in each cartridge build.
 * The GB build stores this working data in C5xx; the GBC build uses D5xx.
 * gb_wram_peek() exposes the emulator's linear WRAM backing store so the
 * result is independent of whichever CGB WRAM bank the game last selected. */
#define WARIO_LAND2_COLLECTION_COUNT 50
#define WARIO_LAND2_COLLECTION_BYTES 7
#define WARIO_LAND2_COLLECTION_MASK ((UINT64_C(1) << WARIO_LAND2_COLLECTION_COUNT) - UINT64_C(1))

typedef uint8_t (*WarioLand2Peek)(size_t offset);

typedef struct {
    int stage;
    int bank_coins;
    int overworld_coins;
    uint64_t treasures;
    uint64_t picture_panels;
} WarioLand2Snapshot;

static inline int wario_land2_supported_game(int game)
{
    return game == GAME_WARIO_LAND2_GB || game == GAME_WARIO_LAND2_GBC;
}

static inline size_t wario_land2_wram_base(int game)
{
    return game == GAME_WARIO_LAND2_GBC ? 0x1500u : 0x0500u;
}

static inline uint8_t wario_land2_read(int game, WarioLand2Peek peek, size_t offset)
{
    return peek && wario_land2_supported_game(game)
        ? peek(wario_land2_wram_base(game) + offset) : 0xFF;
}

static inline uint64_t wario_land2_read_flags(int game, WarioLand2Peek peek,
                                               size_t first_offset)
{
    uint64_t flags = 0;
    for (size_t i = 0; i < WARIO_LAND2_COLLECTION_BYTES; ++i)
        flags |= (uint64_t)wario_land2_read(game, peek, first_offset + i) << (i * 8u);
    return flags & WARIO_LAND2_COLLECTION_MASK;
}

/* The counter uses two packed-decimal digits per byte. */
static inline int wario_land2_read_bcd(int game, WarioLand2Peek peek,
                                        size_t first_offset, size_t byte_count)
{
    int value = 0;
    for (size_t i = 0; i < byte_count; ++i) {
        uint8_t b = wario_land2_read(game, peek, first_offset + i);
        int high = (b >> 4) & 0x0F, low = b & 0x0F;
        if (high > 9 || low > 9) return -1;
        value = value * 100 + high * 10 + low;
    }
    return value;
}

static inline WarioLand2Snapshot wario_land2_read_snapshot(int game, WarioLand2Peek peek)
{
    WarioLand2Snapshot s;
    uint8_t stage = wario_land2_read(game, peek, 0x10);
    s.stage = stage == 0xFF ? -1 : (int)stage;
    s.bank_coins = wario_land2_read_bcd(game, peek, 0x0B, 3);
    s.overworld_coins = wario_land2_read_bcd(game, peek, 0x0E, 2);
    s.treasures = wario_land2_read_flags(game, peek, 0x04);
    s.picture_panels = wario_land2_read_flags(game, peek, 0x13);
    return s;
}

enum {
    WARIO_LAND2_EVENT_TREASURE = 0,
    WARIO_LAND2_EVENT_PICTURE,
    WARIO_LAND2_EVENT_COIN,
    WARIO_LAND2_EVENT_COURSE_ENTERED,
    WARIO_LAND2_EVENT_COIN_LOSS,
    WARIO_LAND2_EVENT_COURSE_RESULTS,
    WARIO_LAND2_EVENT_COUNT
};

#define WARIO_LAND2_EVENT_MASK(event) (UINT32_C(1) << (event))

typedef struct {
    WarioLand2Snapshot previous;
    int have_previous;
} WarioLand2EventTracker;

/* Return a bitmask so one snapshot can deliver several real events. Coin
 * decreases can be caused by Wario being hit or by paying for a minigame,
 * so the event deliberately describes the observable coin loss rather than
 * claiming every decrease is damage. Collection rollbacks are treated as
 * save-state load/rewind, not new collection events. */
static inline uint32_t wario_land2_tracker_update(WarioLand2EventTracker *tracker,
                                                  const WarioLand2Snapshot *current)
{
    if (!tracker || !current || current->stage < 0 ||
        current->bank_coins < 0 || current->overworld_coins < 0) {
        if (tracker) tracker->have_previous = 0;
        return 0;
    }

    if (!tracker->have_previous) {
        tracker->previous = *current;
        tracker->have_previous = 1;
        return 0;
    }

    const WarioLand2Snapshot old = tracker->previous;
    uint32_t events = 0;
    const uint64_t new_treasures = current->treasures & WARIO_LAND2_COLLECTION_MASK;
    const uint64_t old_treasures = old.treasures & WARIO_LAND2_COLLECTION_MASK;
    const uint64_t new_pictures = current->picture_panels & WARIO_LAND2_COLLECTION_MASK;
    const uint64_t old_pictures = old.picture_panels & WARIO_LAND2_COLLECTION_MASK;

    const int rollback = ((new_treasures & old_treasures) != old_treasures) ||
                         ((new_pictures & old_pictures) != old_pictures);
    if (rollback) {
        tracker->previous = *current;
        return 0;
    }

    const int new_treasure_reward = (new_treasures & ~old_treasures) != 0;
    const int new_picture_reward = (new_pictures & ~old_pictures) != 0;
    if (new_treasure_reward)
        events |= WARIO_LAND2_EVENT_MASK(WARIO_LAND2_EVENT_TREASURE);
    if (new_picture_reward)
        events |= WARIO_LAND2_EVENT_MASK(WARIO_LAND2_EVENT_PICTURE);
    /* WL2 exposes these two reward flags reliably, but not a separate
       course-results scene byte in our current runtime map. Treat the
       end-of-course reward reveal as the observable results event. */
    if (new_treasure_reward || new_picture_reward)
        events |= WARIO_LAND2_EVENT_MASK(WARIO_LAND2_EVENT_COURSE_RESULTS);

    if (current->bank_coins > old.bank_coins ||
        current->overworld_coins > old.overworld_coins)
        events |= WARIO_LAND2_EVENT_MASK(WARIO_LAND2_EVENT_COIN);

    if (current->stage != old.stage)
        events |= WARIO_LAND2_EVENT_MASK(WARIO_LAND2_EVENT_COURSE_ENTERED);
    else if (current->bank_coins < old.bank_coins ||
             current->overworld_coins < old.overworld_coins)
        events |= WARIO_LAND2_EVENT_MASK(WARIO_LAND2_EVENT_COIN_LOSS);

    tracker->previous = *current;
    return events;
}

static inline int wario_land2_popcount(uint64_t value)
{
    int count = 0;
    value &= WARIO_LAND2_COLLECTION_MASK;
    while (value) {
        count += (int)(value & UINT64_C(1));
        value >>= 1;
    }
    return count;
}
