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
