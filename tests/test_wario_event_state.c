#include <assert.h>
#include <stdio.h>
#include "wario_event_state.h"
#include "wario_land3_state.h"

static WarioEventSnapshot snapshot(int coins, int hearts, int lives, int power,
                                   int treasures, int course, int mode, int sub,
                                   int held_key)
{
    WarioEventSnapshot s = {
        coins, hearts, lives, power, treasures, course, mode, sub, held_key
    };
    return s;
}

static WarioLand3Snapshot wl3_snapshot(int scene, int operation, int coins,
                                       int treasures, int powerups, int status,
                                       int completion)
{
    WarioLand3Snapshot s = {
        scene, operation, coins, treasures, powerups, status, completion
    };
    return s;
}

int main(void)
{
    WarioEventTracker tracker = {0};
    WarioEventSnapshot s = snapshot(120, 4, 5, 0, 0, 1, 0, 0, 0);

    assert(wario_event_tracker_update(&tracker, &s) == 0); /* baseline */
    s.coins++;
    assert(wario_event_tracker_update(&tracker, &s) ==
           WARIO_EVENT_MASK(WARIO_EVENT_COIN));

    s.power = 2;
    assert(wario_event_tracker_update(&tracker, &s) ==
           WARIO_EVENT_MASK(WARIO_EVENT_POWER_UP));
    s.power = 1;
    assert(wario_event_tracker_update(&tracker, &s) ==
           WARIO_EVENT_MASK(WARIO_EVENT_DAMAGE));

    s.hearts++;
    assert(wario_event_tracker_update(&tracker, &s) ==
           WARIO_EVENT_MASK(WARIO_EVENT_HEART));
    s.hearts--;
    assert(wario_event_tracker_update(&tracker, &s) ==
           WARIO_EVENT_MASK(WARIO_EVENT_DAMAGE));

    s.lives++;
    assert(wario_event_tracker_update(&tracker, &s) ==
           WARIO_EVENT_MASK(WARIO_EVENT_EXTRA_LIFE));
    s.lives--;
    assert(wario_event_tracker_update(&tracker, &s) ==
           WARIO_EVENT_MASK(WARIO_EVENT_LIFE_LOST));

    s.treasure_mask = 1;
    assert(wario_event_tracker_update(&tracker, &s) ==
           WARIO_EVENT_MASK(WARIO_EVENT_TREASURE));
    s.treasure_mask = 3;
    assert(wario_event_tracker_update(&tracker, &s) ==
           WARIO_EVENT_MASK(WARIO_EVENT_TREASURE));
    s.treasure_mask = 1; /* load/rewind rollback: don't replay collection */
    assert(wario_event_tracker_update(&tracker, &s) == 0);
    assert(tracker.previous.treasure_mask == 1);

    s.coins++;
    assert(wario_event_tracker_update(&tracker, &s) ==
           WARIO_EVENT_MASK(WARIO_EVENT_COIN));

    /* Wario Land: Super Mario Land 3: RAM game modes track course entry,
       key pickup, and the actual course-clear/results submodes. */
    s.game_mode = 2; /* load/fade into a course */
    assert(wario_event_tracker_update(&tracker, &s) ==
           WARIO_EVENT_MASK(WARIO_EVENT_COURSE_ENTERED));
    s.game_mode = 3; /* normal level gameplay */
    s.held_key = 1;
    assert(wario_event_tracker_update(&tracker, &s) ==
           WARIO_EVENT_MASK(WARIO_EVENT_KEY_OBTAINED));
    s.game_mode = 4;
    s.sub_mode = 3; /* course clear/results screen starts */
    assert(wario_event_tracker_update(&tracker, &s) ==
           WARIO_EVENT_MASK(WARIO_EVENT_COURSE_RESULTS));
    s.sub_mode = 4; /* still on the same screen; no duplicate event */
    assert(wario_event_tracker_update(&tracker, &s) == 0);

    s.coins = -1;
    assert(wario_event_tracker_update(&tracker, &s) == 0);
    assert(!tracker.have_previous);
    s.coins = 10;
    assert(wario_event_tracker_update(&tracker, &s) == 0); /* re-baseline */

    /* WL3 counter bytes: the high digit is 0-9; the trailing byte is
       a decimal number 0-99, not a packed-BCD pair. */
    assert(wario_land3_read_counter3(0, 0) == 0);
    assert(wario_land3_read_counter3(1, 23) == 123);
    assert(wario_land3_read_counter3(9, 99) == 999);
    assert(wario_land3_read_counter3(10, 0) == -1);
    assert(wario_land3_read_counter3(1, 100) == -1);
    assert(wario_land3_read_counter3(1, 0xFA) == -1);

    WarioLand3Tracker wl3 = {0};
    WarioLand3Snapshot w = wl3_snapshot(1, 0, 10, 0, 0, 0, 0);
    assert(wario_land3_tracker_update(&wl3, &w) == 0); /* overworld baseline */

    w.scene = 2;
    assert(wario_land3_tracker_update(&wl3, &w) ==
           WARIO_LAND3_EVENT_MASK(WARIO_LAND3_EVENT_COURSE_ENTERED));

    w.coins = 11;
    assert(wario_land3_tracker_update(&wl3, &w) ==
           WARIO_LAND3_EVENT_MASK(WARIO_LAND3_EVENT_COIN));

    /* Coin and treasure count can change in the same frame. */
    w.coins = 12;
    w.treasures = 1;
    assert(wario_land3_tracker_update(&wl3, &w) ==
           (WARIO_LAND3_EVENT_MASK(WARIO_LAND3_EVENT_COIN) |
            WARIO_LAND3_EVENT_MASK(WARIO_LAND3_EVENT_TREASURE)));

    w.powerups = 1;
    assert(wario_land3_tracker_update(&wl3, &w) ==
           WARIO_LAND3_EVENT_MASK(WARIO_LAND3_EVENT_POWER_UP));

    w.status = 9; /* enter a temporary transformation */
    assert(wario_land3_tracker_update(&wl3, &w) ==
           WARIO_LAND3_EVENT_MASK(WARIO_LAND3_EVENT_DAMAGE));
    w.status = 0; /* recover to normal */
    assert(wario_land3_tracker_update(&wl3, &w) ==
           WARIO_LAND3_EVENT_MASK(WARIO_LAND3_EVENT_HEART));

    w.powerups = 0x0A; /* Action Help selection isn't a power-up */
    assert(wario_land3_tracker_update(&wl3, &w) == 0);
    w.powerups = 2; /* leaving help state must not create a false alert */
    assert(wario_land3_tracker_update(&wl3, &w) == 0);

    w.scene = 4; /* pause menu */
    assert(wario_land3_tracker_update(&wl3, &w) == 0);
    w.scene = 2; /* resuming from pause isn't a new course entry */
    assert(wario_land3_tracker_update(&wl3, &w) == 0);

    w.scene = 8; /* key acquisition */
    assert(wario_land3_tracker_update(&wl3, &w) ==
           WARIO_LAND3_EVENT_MASK(WARIO_LAND3_EVENT_KEY_OBTAINED));
    w.scene = 2; /* return after key animation must not retrigger course entry */
    assert(wario_land3_tracker_update(&wl3, &w) == 0);

    w.scene = 3; /* end-of-course treasure/music-coin results */
    assert(wario_land3_tracker_update(&wl3, &w) ==
           WARIO_LAND3_EVENT_MASK(WARIO_LAND3_EVENT_COURSE_RESULTS));
    assert(wario_land3_tracker_update(&wl3, &w) == 0); /* no repeated results */

    w.completion = 1; /* Rudy defeated / story milestone */
    assert(wario_land3_tracker_update(&wl3, &w) ==
           WARIO_LAND3_EVENT_MASK(WARIO_LAND3_EVENT_EXTRA_LIFE));

    w.scene = 12; /* rare Game Over scene */
    assert(wario_land3_tracker_update(&wl3, &w) ==
           WARIO_LAND3_EVENT_MASK(WARIO_LAND3_EVENT_LIFE_LOST));

    w.treasures = 0; /* rollback from a load/rewind isn't a new treasure */
    assert(wario_land3_tracker_update(&wl3, &w) == 0);

    w.coins = -1;
    assert(wario_land3_tracker_update(&wl3, &w) == 0);
    assert(!wl3.have_previous);

    puts("Wario event state regression passed.");
    return 0;
}
