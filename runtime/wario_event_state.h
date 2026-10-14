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
    WARIO_EVENT_COUNT
};

typedef struct {
    int coins;
    int hearts;
    int lives;
    int power;
    int treasure_mask;
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
           s->treasure_mask >= 0 && s->treasure_mask <= 0xFFFF;
}

/* Returns one prioritized event per observed change. Treasure-bit rollbacks
   are treated as load/rewind, not a fresh treasure discovery. */
static inline int wario_event_tracker_update(WarioEventTracker *tracker,
                                             const WarioEventSnapshot *current)
{
    if (!tracker || !wario_event_snapshot_valid(current)) {
        if (tracker) tracker->have_previous = 0;
        return -1;
    }

    if (!tracker->have_previous) {
        tracker->previous = *current;
        tracker->have_previous = 1;
        return -1;
    }

    const WarioEventSnapshot old = tracker->previous;
    const unsigned old_mask = (unsigned)old.treasure_mask & 0xFFFFu;
    const unsigned new_mask = (unsigned)current->treasure_mask & 0xFFFFu;
    if ((new_mask & old_mask) != old_mask) {
        tracker->previous = *current;
        return -1;
    }

    int event = -1;
    if (new_mask != old_mask) event = WARIO_EVENT_TREASURE;
    else if (current->lives < old.lives) event = WARIO_EVENT_LIFE_LOST;
    else if (current->lives > old.lives) event = WARIO_EVENT_EXTRA_LIFE;
    else if (current->power > old.power) event = WARIO_EVENT_POWER_UP;
    else if (current->power < old.power || current->hearts < old.hearts)
        event = WARIO_EVENT_DAMAGE;
    else if (current->hearts > old.hearts) event = WARIO_EVENT_HEART;
    else if (current->coins > old.coins) event = WARIO_EVENT_COIN;

    tracker->previous = *current;
    return event;
}
