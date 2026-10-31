#include "sml2_native_actor.h"

#include <string.h>

typedef struct {
    Sml2NativeActorWorld *world;
    int failed;
} SyncContext;

static Sml2NativeActor *find_mutable(Sml2NativeActorWorld *world, uint16_t id)
{
    uint16_t i;
    for (i = 0; i < world->count; ++i)
        if (world->actors[i].id == id) return &world->actors[i];
    return NULL;
}

const Sml2NativeActor *sml2_native_actor_find(
    const Sml2NativeActorWorld *world, uint16_t actor_id)
{
    uint16_t i;
    if (!world) return NULL;
    for (i = 0; i < world->count; ++i)
        if (world->actors[i].id == actor_id) return &world->actors[i];
    return NULL;
}

void sml2_native_actors_init(Sml2NativeActorWorld *world)
{
    if (world) memset(world, 0, sizeof(*world));
}

static int materialize_object(void *userdata,
                              const Sml2NativeStageObject *object)
{
    SyncContext *ctx = (SyncContext *)userdata;
    Sml2NativeActor *actor = find_mutable(ctx->world, object->id);
    if (actor) {
        if (actor->state != SML2_NATIVE_ACTOR_DEFEATED ||
            !(actor->flags & SML2_NATIVE_ACTOR_UNIQUE))
            actor->state = SML2_NATIVE_ACTOR_AWAKE;
        return 0;
    }
    if (ctx->world->count >= SML2_NATIVE_MAX_ACTORS) {
        ctx->failed = 1;
        return -1;
    }
    actor = &ctx->world->actors[ctx->world->count++];
    memset(actor, 0, sizeof(*actor));
    actor->id = object->id;
    actor->kind = object->kind;
    actor->subtype = object->subtype;
    actor->x = object->x * SML2_NATIVE_FP_ONE;
    actor->y = object->y * SML2_NATIVE_FP_ONE;
    actor->width = object->width;
    actor->height = object->height;
    actor->health = 1;
    actor->flags = 0;
    if (object->flags & SML2_NATIVE_STAGE_OBJECT_UNIQUE)
        actor->flags |= SML2_NATIVE_ACTOR_UNIQUE;
    if (object->flags & SML2_NATIVE_STAGE_OBJECT_PERSISTENT)
        actor->flags |= SML2_NATIVE_ACTOR_PERSISTENT;
    if (object->flags & SML2_NATIVE_STAGE_OBJECT_BOSS)
        actor->flags |= SML2_NATIVE_ACTOR_BOSS;
    if (object->flags & SML2_NATIVE_STAGE_OBJECT_PROJECTILE)
        actor->flags |= SML2_NATIVE_ACTOR_PROJECTILE;
    actor->state = SML2_NATIVE_ACTOR_AWAKE;
    return 0;
}

static int actor_intersects(const Sml2NativeActor *actor,
                            const Sml2NativeRect *bounds)
{
    int64_t left = actor->x;
    int64_t top = actor->y;
    int64_t right = left + (int64_t)actor->width * SML2_NATIVE_FP_ONE;
    int64_t bottom = top + (int64_t)actor->height * SML2_NATIVE_FP_ONE;
    return right > bounds->left && left < bounds->right &&
           bottom > bounds->top && top < bounds->bottom;
}

int sml2_native_actors_sync(Sml2NativeActorWorld *world,
                            const Sml2NativeStage *stage,
                            const Sml2NativeRect *activation_bounds)
{
    SyncContext ctx;
    uint16_t i;
    if (!world || !stage || !activation_bounds ||
        activation_bounds->right <= activation_bounds->left ||
        activation_bounds->bottom <= activation_bounds->top)
        return -1;
    ctx.world = world;
    ctx.failed = 0;
    if (sml2_native_stage_visit_objects(stage, activation_bounds,
                                         materialize_object, &ctx) != 0)
        return -1;

    for (i = 0; i < world->count; ++i) {
        Sml2NativeActor *actor = &world->actors[i];
        if (actor->state == SML2_NATIVE_ACTOR_DEFEATED) continue;
        if (actor_intersects(actor, activation_bounds) ||
            (actor->flags & SML2_NATIVE_ACTOR_PERSISTENT))
            actor->state = SML2_NATIVE_ACTOR_AWAKE;
        else
            actor->state = SML2_NATIVE_ACTOR_ASLEEP;
    }
    return ctx.failed ? -1 : 0;
}

int sml2_native_actors_step(Sml2NativeActorWorld *world,
                            Sml2NativeActorUpdate update, void *userdata)
{
    uint16_t i;
    if (!world || !update) return -1;
    for (i = 0; i < world->count; ++i) {
        Sml2NativeActor *actor = &world->actors[i];
        int result;
        if (actor->state != SML2_NATIVE_ACTOR_AWAKE) continue;
        result = update(userdata, actor, world->tick);
        if (result) return result;
    }
    world->tick++;
    return 0;
}

int sml2_native_actor_damage(Sml2NativeActorWorld *world, uint16_t actor_id,
                             uint16_t damage)
{
    Sml2NativeActor *actor;
    if (!world || !damage) return -1;
    actor = find_mutable(world, actor_id);
    if (!actor || actor->state == SML2_NATIVE_ACTOR_DEFEATED) return -1;
    if (damage >= actor->health) {
        actor->health = 0;
        actor->state = SML2_NATIVE_ACTOR_DEFEATED;
        return 1;
    }
    actor->health = (uint16_t)(actor->health - damage);
    return 0;
}
