#include <assert.h>
#include <stdio.h>
#include "wario_event_state.h"
#include "wario_land3_state.h"

static WarioEventSnapshot snapshot(int coins, int hearts, int lives, int power, int treasures)
{
    WarioEventSnapshot s = {coins, hearts, lives, power, treasures};
    return s;
}

int main(void)
{
    WarioEventTracker tracker = {0};
    WarioEventSnapshot s = snapshot(120, 4, 5, 0, 0);

    assert(wario_event_tracker_update(&tracker, &s) == -1); /* baseline */
    s.coins++;
    assert(wario_event_tracker_update(&tracker, &s) == WARIO_EVENT_COIN);

    s.power = 2;
    assert(wario_event_tracker_update(&tracker, &s) == WARIO_EVENT_POWER_UP);
    s.power = 1;
    assert(wario_event_tracker_update(&tracker, &s) == WARIO_EVENT_DAMAGE);

    s.hearts++;
    assert(wario_event_tracker_update(&tracker, &s) == WARIO_EVENT_HEART);
    s.hearts--;
    assert(wario_event_tracker_update(&tracker, &s) == WARIO_EVENT_DAMAGE);

    s.lives++;
    assert(wario_event_tracker_update(&tracker, &s) == WARIO_EVENT_EXTRA_LIFE);
    s.lives--;
    assert(wario_event_tracker_update(&tracker, &s) == WARIO_EVENT_LIFE_LOST);

    s.treasure_mask = 1;
    assert(wario_event_tracker_update(&tracker, &s) == WARIO_EVENT_TREASURE);
    s.treasure_mask = 3;
    assert(wario_event_tracker_update(&tracker, &s) == WARIO_EVENT_TREASURE);
    s.treasure_mask = 1; /* load/rewind rollback */
    assert(wario_event_tracker_update(&tracker, &s) == -1);
    assert(tracker.previous.treasure_mask == 1);

    s.coins++;
    assert(wario_event_tracker_update(&tracker, &s) == WARIO_EVENT_COIN);
    s.coins = -1;
    assert(wario_event_tracker_update(&tracker, &s) == -1);
    assert(!tracker.have_previous);
    s.coins = 10;
    assert(wario_event_tracker_update(&tracker, &s) == -1); /* re-baseline */

    WarioLand3Tracker wl3 = {0};
    WarioLand3Snapshot wl3s = {1, 0}; /* overworld baseline */
    assert(wario_land3_tracker_update(&wl3, &wl3s) == -1);
    wl3s.scene = 2;
    assert(wario_land3_tracker_update(&wl3, &wl3s) == WARIO_LAND3_EVENT_LEVEL_ENTERED);
    wl3s.operation = 3; /* normal gameplay begins; no duplicate entry */
    assert(wario_land3_tracker_update(&wl3, &wl3s) == -1);
    wl3s.scene = 8; /* key-acquisition animation */
    assert(wario_land3_tracker_update(&wl3, &wl3s) == WARIO_LAND3_EVENT_KEY_OBTAINED);
    wl3s.scene = 2; /* return from key animation is not another level entry */
    assert(wario_land3_tracker_update(&wl3, &wl3s) == -1);
    wl3s.scene = 3; /* end-of-course results screen */
    assert(wario_land3_tracker_update(&wl3, &wl3s) == WARIO_LAND3_EVENT_COURSE_RESULTS);
    assert(wario_land3_tracker_update(&wl3, &wl3s) == -1); /* no repeated results rumble */
    wl3s.scene = 1;
    assert(wario_land3_tracker_update(&wl3, &wl3s) == -1);
    wl3s.scene = 2;
    assert(wario_land3_tracker_update(&wl3, &wl3s) == WARIO_LAND3_EVENT_LEVEL_ENTERED);
    wl3s.scene = 0xFF;
    assert(wario_land3_tracker_update(&wl3, &wl3s) == -1);
    assert(!wl3.have_previous);

    puts("Wario event state regression passed.");
    return 0;
}
