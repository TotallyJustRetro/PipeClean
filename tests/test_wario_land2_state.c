#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "wario_land2_state.h"

static uint8_t memory_image[0x2000];
static uint8_t test_peek(size_t offset)
{
    return offset < sizeof memory_image ? memory_image[offset] : 0xFF;
}

int main(void)
{
    memset(memory_image, 0, sizeof memory_image);
    assert(wario_land2_supported_game(GAME_WARIO_LAND2_GB));
    assert(wario_land2_supported_game(GAME_WARIO_LAND2_GBC));
    assert(!wario_land2_supported_game(GAME_WARIO_SML3));
    assert(wario_land2_wram_base(GAME_WARIO_LAND2_GB) == 0x0500u);
    assert(wario_land2_wram_base(GAME_WARIO_LAND2_GBC) == 0x1500u);

    memory_image[0x0510] = 0x12;
    memory_image[0x050B] = 0x01;
    memory_image[0x050C] = 0x23;
    memory_image[0x050D] = 0x45;
    memory_image[0x050E] = 0x00;
    memory_image[0x050F] = 0x67;
    memory_image[0x0504] = 0x01;
    memory_image[0x050A] = 0x02; /* the 50th treasure bit */
    memory_image[0x0513] = 0x03;
    WarioLand2Snapshot gb = wario_land2_read_snapshot(GAME_WARIO_LAND2_GB, test_peek);
    assert(gb.stage == 0x12);
    assert(gb.bank_coins == 12345);
    assert(gb.overworld_coins == 67);
    assert(wario_land2_popcount(gb.treasures) == 2);
    assert((gb.treasures & (UINT64_C(1) << 49)) != 0);
    assert(wario_land2_popcount(gb.picture_panels) == 2);

    memory_image[0x1510] = 0x2A;
    memory_image[0x1504] = 0x04;
    memory_image[0x1519] = 0x02; /* the 50th picture-panel bit */
    WarioLand2Snapshot gbc = wario_land2_read_snapshot(GAME_WARIO_LAND2_GBC, test_peek);
    assert(gbc.stage == 0x2A);
    assert(wario_land2_popcount(gbc.treasures) == 1);
    assert(wario_land2_popcount(gbc.picture_panels) == 1);
    assert((gbc.picture_panels & (UINT64_C(1) << 49)) != 0);

    memory_image[0x050B] = 0xFA;
    assert(wario_land2_read_bcd(GAME_WARIO_LAND2_GB, test_peek, 0x0B, 3) == -1);

    WarioLand2EventTracker events = {0};
    WarioLand2Snapshot w = {3, 100, 10, 0, 0};
    assert(wario_land2_tracker_update(&events, &w) == 0); /* baseline */

    w.bank_coins = 101;
    w.overworld_coins = 11;
    assert(wario_land2_tracker_update(&events, &w) ==
           WARIO_LAND2_EVENT_MASK(WARIO_LAND2_EVENT_COIN));

    w.bank_coins = 102;
    w.treasures = UINT64_C(1);
    w.picture_panels = UINT64_C(1);
    assert(wario_land2_tracker_update(&events, &w) ==
           (WARIO_LAND2_EVENT_MASK(WARIO_LAND2_EVENT_COIN) |
            WARIO_LAND2_EVENT_MASK(WARIO_LAND2_EVENT_TREASURE) |
            WARIO_LAND2_EVENT_MASK(WARIO_LAND2_EVENT_PICTURE) |
            WARIO_LAND2_EVENT_MASK(WARIO_LAND2_EVENT_COURSE_RESULTS)));

    w.stage = 4;
    assert(wario_land2_tracker_update(&events, &w) ==
           WARIO_LAND2_EVENT_MASK(WARIO_LAND2_EVENT_COURSE_ENTERED));

    /* A counter decrease on the same stage gets its own red/strong effect;
       it may be damage or coins spent in a minigame, so the UI calls it loss. */
    w.bank_coins = 96;
    w.overworld_coins = 5;
    assert(wario_land2_tracker_update(&events, &w) ==
           WARIO_LAND2_EVENT_MASK(WARIO_LAND2_EVENT_COIN_LOSS));

    w.treasures = 0; /* load/rewind rollback should not make a new sound */
    assert(wario_land2_tracker_update(&events, &w) == 0);

    w.overworld_coins = -1;
    assert(wario_land2_tracker_update(&events, &w) == 0);
    assert(!events.have_previous);

    puts("Wario Land II state regression passed.");
    return 0;
}
