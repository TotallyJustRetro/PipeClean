#pragma once
#include <stdint.h>

enum {
    WARIO_EVENT_TREASURE = 0,
    WARIO_EVENT_POWER_UP,
    WARIO_EVENT_DAMAGE,
    WARIO_EVENT_COIN,
    WARIO_EVENT_HEART,
    WARIO_EVENT_EXTRA_LIFE,
    WARIO_EVENT_LIFE_LOST,
    WARIO_EVENT_COURSE_ENTERED,
    WARIO_EVENT_KEY_OBTAINED,
    WARIO_EVENT_COURSE_RESULTS,
    WARIO_EVENT_COUNT
};

#define WARIO_EVENT_MASK(event) (UINT32_C(1) << (event))

typedef struct {
    int coins;
    int hearts;
    int lives;
    int power;
    int treasure_mask;
    int course;
    int game_mode;
    int sub_mode;
    int held_key;
} WarioEventSnapshot;

typedef struct {
    WarioEventSnapshot previous;
    int have_previous;
} WarioEventTracker;

static inline int wario_event_snapshot_valid(const WarioEventSnapshot *s)
{
    return s && s->coins >= 0 && s->coins <= 99999 &&
           s->hearts >= 0 && s->hearts <= 99 &&
           s->lives >= 0 && s->lives <= 99 &&
           s->power >= 0 && s->power <= 4 &&
           s->treasure_mask >= 0 && s->treasure_mask <= 0xFFFF &&
           s->course >= 0 && s->course <= 0xFF &&
           s->game_mode >= 0 && s->game_mode <= 0x0E &&
           s->sub_mode >= 0 && s->sub_mode <= 0xFF &&
           s->held_key >= 0 && s->held_key <= 1;
}

/* Return an event bitmask so a same-frame course transition and counter
 * change can both produce effects. Treasure rollbacks indicate a load/rewind,
 * and are rebased without replaying collection-related effects. */
static inline uint32_t wario_event_tracker_update(WarioEventTracker *tracker,
                                                  const WarioEventSnapshot *current)
{
    if (!tracker || !wario_event_snapshot_valid(current)) {
        if (tracker) tracker->have_previous = 0;
        return 0;
    }

    if (!tracker->have_previous) {
        tracker->previous = *current;
        tracker->have_previous = 1;
        return 0;
    }

    const WarioEventSnapshot old = tracker->previous;
    const unsigned old_mask = (unsigned)old.treasure_mask & 0xFFFFu;
    const unsigned new_mask = (unsigned)current->treasure_mask & 0xFFFFu;
    if ((new_mask & old_mask) != old_mask) {
        tracker->previous = *current;
        return 0;
    }

    uint32_t events = 0;
    if (new_mask != old_mask) events |= WARIO_EVENT_MASK(WARIO_EVENT_TREASURE);
    if (current->lives < old.lives) events |= WARIO_EVENT_MASK(WARIO_EVENT_LIFE_LOST);
    if (current->lives > old.lives) events |= WARIO_EVENT_MASK(WARIO_EVENT_EXTRA_LIFE);
    if (current->power > old.power) events |= WARIO_EVENT_MASK(WARIO_EVENT_POWER_UP);
    if (current->power < old.power || current->hearts < old.hearts)
        events |= WARIO_EVENT_MASK(WARIO_EVENT_DAMAGE);
    else if (current->hearts > old.hearts)
        events |= WARIO_EVENT_MASK(WARIO_EVENT_HEART);
    if (current->coins > old.coins) events |= WARIO_EVENT_MASK(WARIO_EVENT_COIN);

    /* Wario Land: Super Mario Land 3 modes:
     * 02 = load/fade into a course; 03 = gameplay; 04 = course clear.
     * Submodes 03+ of mode 04 are the actual course-clear/results screen. */
    if (current->game_mode == 0x02 && old.game_mode != 0x02)
        events |= WARIO_EVENT_MASK(WARIO_EVENT_COURSE_ENTERED);
    if (current->held_key && !old.held_key && current->game_mode == 0x03)
        events |= WARIO_EVENT_MASK(WARIO_EVENT_KEY_OBTAINED);
    if (current->game_mode == 0x04 && current->sub_mode >= 0x03 &&
        !(old.game_mode == 0x04 && old.sub_mode >= 0x03))
        events |= WARIO_EVENT_MASK(WARIO_EVENT_COURSE_RESULTS);

    tracker->previous = *current;
    return events;
}
