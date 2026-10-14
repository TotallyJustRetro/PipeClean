#include <assert.h>
#include <stdio.h>
#include "wario_event_state.h"

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

    puts("Wario event state regression passed.");
    return 0;
}
