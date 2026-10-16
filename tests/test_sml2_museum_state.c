#include "sml2_museum_state.h"

#include <assert.h>
#include <string.h>

static Sml2MuseumSnapshot snapshot(int coins, int kills, int lives, int powerup,
                                   int star, int level, int ending, int secret)
{
    Sml2MuseumSnapshot s;
    memset(&s, 0, sizeof s);
    s.valid = 1;
    s.gameplay = 1;
    s.coins = coins;
    s.enemy_kills = kills;
    s.lives = lives;
    s.powerup = powerup;
    s.star_active = star;
    s.level = level;
    s.ending_flag = ending;
    s.secret_exit_seen = secret;
    return s;
}

int main(void)
{
    assert(!sml2_museum_is_secret_level(0x18));
    assert(sml2_museum_is_secret_level(0x19));
    assert(sml2_museum_is_secret_level(0x1E));
    assert(!sml2_museum_is_secret_level(0x1F));

    Sml2MuseumTracker tracker;
    memset(&tracker, 0, sizeof tracker);

    Sml2MuseumSnapshot s = snapshot(0, 0, 3, 0, 0, 2, 0, 0);
    assert(sml2_museum_tracker_update(&tracker, &s) == 0);

    s = snapshot(1, 1, 2, 1, 1, 2, 0, 1);
    uint32_t found = sml2_museum_tracker_update(&tracker, &s);
    assert(found & SML2_MUSEUM_COIN);
    assert(found & SML2_MUSEUM_ENEMY);
    assert(found & SML2_MUSEUM_LIFE_LOST);
    assert(found & SML2_MUSEUM_MUSHROOM);
    assert(found & SML2_MUSEUM_SUPER_STAR);
    assert(found & SML2_MUSEUM_SECRET_EXIT);

    /* Stable gameplay frames do not repeatedly emit collection events. */
    assert(sml2_museum_tracker_update(&tracker, &s) == 0);

    s = snapshot(1, 1, 4, 2, 0, 2, 0, 0);
    found = sml2_museum_tracker_update(&tracker, &s);
    assert(found & SML2_MUSEUM_EXTRA_LIFE);
    assert(found & SML2_MUSEUM_MAGIC_CARROT);

    s = snapshot(1, 1, 4, 3, 0, 2, 0, 0);
    found = sml2_museum_tracker_update(&tracker, &s);
    assert(found & SML2_MUSEUM_FIRE_FLOWER);

    s = snapshot(1, 1, 4, 0, 0, 0x1F, 0, 0);
    found = sml2_museum_tracker_update(&tracker, &s);
    assert(found & SML2_MUSEUM_GOLDEN_COINS);
    /* Entering the castle unlocks the route milestone, not a boss encounter. */
    assert(!(found & SML2_MUSEUM_WARIO));

    s = snapshot(1, 1, 4, 0, 0, 0x1F, 1, 0);
    found = sml2_museum_tracker_update(&tracker, &s);
    assert(found & SML2_MUSEUM_CASTLE_CLEARED);
    assert(sml2_museum_tracker_update(&tracker, &s) == 0);

    /* Invalid/loading frames do not erase the baseline or invent discoveries. */
    Sml2MuseumSnapshot invalid = s;
    invalid.valid = 0;
    invalid.coins = 99;
    assert(sml2_museum_tracker_update(&tracker, &invalid) == 0);
    s = snapshot(1, 1, 4, 0, 0, 0x1F, 1, 0);
    assert(sml2_museum_tracker_update(&tracker, &s) == 0);

    /* The 99 -> 00 enemy counter wrap is still a valid enemy encounter. */
    memset(&tracker, 0, sizeof tracker);
    s = snapshot(0, 99, 3, 0, 0, 4, 0, 0);
    assert(sml2_museum_tracker_update(&tracker, &s) & SML2_MUSEUM_ENEMY);
    s = snapshot(0, 0, 3, 0, 0, 4, 0, 0);
    assert(sml2_museum_tracker_update(&tracker, &s) & SML2_MUSEUM_ENEMY);

    /* Resuming after already collecting coins/defeating an enemy should not
       leave those ordinary catalogue rows locked until the next pickup. */
    memset(&tracker, 0, sizeof tracker);
    s = snapshot(12, 4, 3, 0, 0, 0, 0, 0);
    found = sml2_museum_tracker_update(&tracker, &s);
    assert(found & SML2_MUSEUM_COIN);
    assert(found & SML2_MUSEUM_ENEMY);

    /* Resuming with an active form discovers it even without a new transition. */
    memset(&tracker, 0, sizeof tracker);
    s = snapshot(0, 0, 3, 4, 1, 0, 0, 0);
    found = sml2_museum_tracker_update(&tracker, &s);
    assert(found & SML2_MUSEUM_MUSHROOM);
    assert(found & SML2_MUSEUM_SUPER_STAR);

    return 0;
}
