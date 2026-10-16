#pragma once
#include <stdint.h>

/* Wario Land 3 has no normal heart meter or lives counter. We expose the
 * closest game-native events to the shared controller-effects system:
 * power upgrades, temporary status changes/recovery, treasure/coin gains,
 * the story-completion milestone, and its rare Game Over scene.
 *
 * WRAM offsets correspond to the C000-based backing store:
 *   C09B/C09C: scene/subscene
 *   CA04/CA05: coin total (three decimal digits)
 *   CA39/CA3A: treasure total (three decimal digits)
 *   CA3C: power-up state; values 0A-0C are Action Help screens
 *   CA3D: completion flags (bit 0 = Rudy defeated)
 *   CA8E: temporary Wario status (0 = normal)
 */
enum {
    WARIO_LAND3_EVENT_POWER_UP = 0,
    WARIO_LAND3_EVENT_DAMAGE,
    WARIO_LAND3_EVENT_TREASURE,
    WARIO_LAND3_EVENT_COIN,
    WARIO_LAND3_EVENT_HEART,
    WARIO_LAND3_EVENT_EXTRA_LIFE,
    WARIO_LAND3_EVENT_LIFE_LOST,
    WARIO_LAND3_EVENT_COURSE_ENTERED,
    WARIO_LAND3_EVENT_KEY_OBTAINED,
    WARIO_LAND3_EVENT_COURSE_RESULTS,
    WARIO_LAND3_EVENT_COUNT
};

#define WARIO_LAND3_EVENT_MASK(event) (UINT32_C(1) << (event))

typedef struct {
    int scene;
    int operation;
    int coins;
    int treasures;
    int powerups;
    int status;
    int completion;
} WarioLand3Snapshot;

typedef struct {
    WarioLand3Snapshot previous;
    int have_previous;
} WarioLand3Tracker;

/* CA04/CA39 contain the leading decimal digit; CA05/CA3A contain the last
 * two digits as a plain decimal byte (e.g. 99 coins is stored as 0x63).
 * Return -1 for transient or uninitialized RAM. */
static inline int wario_land3_read_counter3(uint8_t leading, uint8_t trailing)
{
    if (leading > 9 || trailing > 99) return -1;
    return (int)leading * 100 + (int)trailing;
}

static inline int wario_land3_snapshot_valid(const WarioLand3Snapshot *s)
{
    return s && s->scene >= 0 && s->scene <= 0x0E &&
           s->operation >= 0 && s->operation <= 0xFF &&
           s->coins >= 0 && s->coins <= 999 &&
           s->treasures >= 0 && s->treasures <= 999 &&
           s->powerups >= 0 && s->powerups <= 0xFF &&
           s->status >= 0 && s->status <= 0xFF &&
           s->completion >= 0 && s->completion <= 0xFF;
}

/* Return an event bitmask, not a single event: if the game updates coins and
 * a treasure in the same frame, both effects must be delivered. */
static inline uint32_t wario_land3_tracker_update(WarioLand3Tracker *tracker,
                                                  const WarioLand3Snapshot *current)
{
    if (!tracker || !wario_land3_snapshot_valid(current)) {
        if (tracker) tracker->have_previous = 0;
        return 0;
    }

    if (!tracker->have_previous) {
        tracker->previous = *current;
        tracker->have_previous = 1;
        return 0;
    }

    const WarioLand3Snapshot old = tracker->previous;
    uint32_t events = 0;

    /* Wario Land 3 exposes these three transitions explicitly:
     * scene 2 = course, scene 8 = key acquisition, scene 3 = course results.
     * Returning from scene 8 to scene 2 is not another course entry. */
    if (current->scene == 2 && old.scene != 2 && old.scene != 8)
        events |= WARIO_LAND3_EVENT_MASK(WARIO_LAND3_EVENT_COURSE_ENTERED);
    if (current->scene == 8 && old.scene != 8)
        events |= WARIO_LAND3_EVENT_MASK(WARIO_LAND3_EVENT_KEY_OBTAINED);
    if (current->scene == 3 && old.scene != 3)
        events |= WARIO_LAND3_EVENT_MASK(WARIO_LAND3_EVENT_COURSE_RESULTS);

    /* Scene C: Game Over, which is only possible in the Rudy encounter. */
    if (current->scene == 12 && old.scene != 12)
        events |= WARIO_LAND3_EVENT_MASK(WARIO_LAND3_EVENT_LIFE_LOST);

    /* The game has no extra-life mechanic; use the Rudy-defeated flag as the
     * nearest one-time major milestone rather than inventing a life counter. */
    if ((current->completion & 0x01) && !(old.completion & 0x01))
        events |= WARIO_LAND3_EVENT_MASK(WARIO_LAND3_EVENT_EXTRA_LIFE);

    if (current->treasures > old.treasures)
        events |= WARIO_LAND3_EVENT_MASK(WARIO_LAND3_EVENT_TREASURE);

    /* CA3C values 0A-0C are Action Help screens, not a new ability. Ignore
     * those transitions to prevent false power-up pings in help/tutorial UI. */
    if (old.scene == current->scene &&
        current->powerups > old.powerups &&
        old.powerups < 0x0A && current->powerups > 0 && current->powerups < 0x0A)
        events |= WARIO_LAND3_EVENT_MASK(WARIO_LAND3_EVENT_POWER_UP);

    /* Wario is effectively immortal: temporary forms are the game's closest
     * equivalent to damage/recovery. Only count transitions during gameplay. */
    if (old.scene == 2 && current->scene == 2) {
        if (old.status == 0 && current->status != 0)
            events |= WARIO_LAND3_EVENT_MASK(WARIO_LAND3_EVENT_DAMAGE);
        else if (old.status != 0 && current->status == 0)
            events |= WARIO_LAND3_EVENT_MASK(WARIO_LAND3_EVENT_HEART);
        if (current->coins > old.coins)
            events |= WARIO_LAND3_EVENT_MASK(WARIO_LAND3_EVENT_COIN);
    }

    tracker->previous = *current;
    return events;
}
