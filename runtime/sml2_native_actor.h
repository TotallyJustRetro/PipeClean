#pragma once

#include "sml2_native_stage.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SML2_NATIVE_MAX_ACTORS 256

enum {
    SML2_NATIVE_ACTOR_ASLEEP = 0,
    SML2_NATIVE_ACTOR_AWAKE = 1,
    SML2_NATIVE_ACTOR_DEFEATED = 2
};

enum {
    SML2_NATIVE_ACTOR_PERSISTENT = 1u << 0,
    SML2_NATIVE_ACTOR_UNIQUE = 1u << 1,
    SML2_NATIVE_ACTOR_BOSS = 1u << 2,
    SML2_NATIVE_ACTOR_PROJECTILE = 1u << 3
};

typedef struct {
    uint16_t id;
    uint16_t kind;
    uint16_t subtype;
    int32_t x;
    int32_t y;
    int32_t vx;
    int32_t vy;
    uint16_t width;
    uint16_t height;
    uint16_t health;
    uint8_t state;
    uint8_t flags;
} Sml2NativeActor;

typedef struct {
    Sml2NativeActor actors[SML2_NATIVE_MAX_ACTORS];
    uint16_t count;
    uint64_t tick;
} Sml2NativeActorWorld;

typedef int (*Sml2NativeActorUpdate)(void *userdata,
                                     Sml2NativeActor *actor,
                                     uint64_t tick);

/* One awake actor projected into the current camera's logical view. */
typedef struct {
    const Sml2NativeActor *actor;
    Sml2NativeRect view_bounds;
} Sml2NativeActorDraw;
typedef int (*Sml2NativeActorDrawVisitor)(
    void *userdata, const Sml2NativeActorDraw *draw);

/* Clear all actor state. Call once when entering a new stage. */
void sml2_native_actors_init(Sml2NativeActorWorld *world);

/* Materialize stage spawn records intersecting activation_bounds. Existing
 * IDs are reused; unique defeated actors stay defeated. */
int sml2_native_actors_sync(Sml2NativeActorWorld *world,
                            const Sml2NativeStage *stage,
                            const Sml2NativeRect *activation_bounds);

/* Derive activation bounds from the live logical camera view plus a world-
 * pixel wake margin, then synchronize stage objects into the actor world.
 * This is the camera-to-gameplay bridge for newly exposed widescreen edges. */
int sml2_native_actors_sync_camera(Sml2NativeActorWorld *world,
                                   const Sml2NativeStage *stage,
                                   const Sml2NativeWorld *camera,
                                   int32_t wake_margin_pixels);

/* Advance awake actors in stable slot order. Update callbacks must be
 * deterministic and may mutate only the actor passed to them. */
int sml2_native_actors_step(Sml2NativeActorWorld *world,
                            Sml2NativeActorUpdate update, void *userdata);

/* Apply damage; returns 1 when this hit defeats the actor, 0 otherwise,
 * and -1 for an invalid actor ID or zero damage. */
int sml2_native_actor_damage(Sml2NativeActorWorld *world, uint16_t actor_id,
                             uint16_t damage);

const Sml2NativeActor *sml2_native_actor_find(
    const Sml2NativeActorWorld *world, uint16_t actor_id);

/* Visit visible awake actors in stable slot order with projected view bounds.
 * A nonzero visitor result stops traversal and is returned unchanged. */
int sml2_native_actors_visit_visible(
    const Sml2NativeActorWorld *actors, const Sml2NativeWorld *camera,
    Sml2NativeActorDrawVisitor visitor, void *userdata);

#ifdef __cplusplus
}
#endif
