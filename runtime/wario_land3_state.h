#pragma once

/* Wario Land 3 keeps its current scene and scene operation in fixed WRAM.
 * Scene IDs used here are documented by the game's RAM map:
 *   1 = overworld, 2 = level, 3 = level results (treasures/music coins),
 *   8 = key obtained during a level.
 *
 * The tracker only emits events on scene transitions, so it doesn't repeat
 * rumble every frame while one of the result/animation scenes is active.
 */
enum {
    WARIO_LAND3_EVENT_LEVEL_ENTERED = 0,
    WARIO_LAND3_EVENT_KEY_OBTAINED,
    WARIO_LAND3_EVENT_COURSE_RESULTS,
    WARIO_LAND3_EVENT_COUNT
};

typedef struct {
    int scene;
    int operation;
} WarioLand3Snapshot;

typedef struct {
    WarioLand3Snapshot previous;
    int have_previous;
} WarioLand3Tracker;

static inline int wario_land3_snapshot_valid(const WarioLand3Snapshot *s)
{
    return s && s->scene >= 0 && s->scene <= 0x0E &&
           s->operation >= 0 && s->operation <= 0xFF;
}

/* Returns an event index or -1. Scene 8 returns to scene 2 after its key
 * animation; do not misreport that return as a second level entry. */
static inline int wario_land3_tracker_update(WarioLand3Tracker *tracker,
                                             const WarioLand3Snapshot *current)
{
    if (!tracker || !wario_land3_snapshot_valid(current)) {
        if (tracker) tracker->have_previous = 0;
        return -1;
    }

    if (!tracker->have_previous) {
        tracker->previous = *current;
        tracker->have_previous = 1;
        return -1;
    }

    const WarioLand3Snapshot old = tracker->previous;
    int event = -1;
    if (current->scene == 8 && old.scene != 8)
        event = WARIO_LAND3_EVENT_KEY_OBTAINED;
    else if (current->scene == 3 && old.scene != 3)
        event = WARIO_LAND3_EVENT_COURSE_RESULTS;
    else if (current->scene == 2 && old.scene != 2 && old.scene != 8)
        event = WARIO_LAND3_EVENT_LEVEL_ENTERED;

    tracker->previous = *current;
    return event;
}
