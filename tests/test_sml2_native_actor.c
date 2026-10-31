#include "sml2_native_actor.h"

#include <assert.h>
#include <string.h>

static int update_actor(void *userdata, Sml2NativeActor *actor, uint64_t tick)
{
    unsigned *updates = (unsigned *)userdata;
    ++*updates;
    actor->x += SML2_NATIVE_FP_ONE;
    assert(tick == *updates - 1);
    return 0;
}

int main(void)
{
    uint8_t cells[8 * 4] = {0};
    Sml2NativeStageTile tiles[1] = {{{0, 0, 0, 0}, 0, 0}};
    Sml2NativeStageObject objects[2] = {
        {1, 10, 2, 16, 16, 8, 8,
         SML2_NATIVE_STAGE_OBJECT_ENABLED | SML2_NATIVE_STAGE_OBJECT_UNIQUE},
        {2, 11, 0, 120, 16, 8, 8, SML2_NATIVE_STAGE_OBJECT_ENABLED | SML2_NATIVE_STAGE_OBJECT_PERSISTENT}
    };
    Sml2NativeStage stage;
    Sml2NativeActorWorld world;
    Sml2NativeRect near = {0, 0, 64 * SML2_NATIVE_FP_ONE, 48 * SML2_NATIVE_FP_ONE};
    Sml2NativeRect far = {96 * SML2_NATIVE_FP_ONE, 0,
                          160 * SML2_NATIVE_FP_ONE, 48 * SML2_NATIVE_FP_ONE};
    unsigned updates = 0;

    assert(sml2_native_stage_init(&stage, 8, 4, cells, tiles, 1,
                                  objects, 2, NULL, 0, 1) == 0);
    sml2_native_actors_init(&world);
    assert(sml2_native_actors_sync(&world, &stage, &near) == 0);
    assert(world.count == 1);
    assert(sml2_native_actor_find(&world, 1)->state == SML2_NATIVE_ACTOR_AWAKE);
    assert(sml2_native_actors_step(&world, update_actor, &updates) == 0);
    assert(updates == 1 && world.tick == 1);
    assert(sml2_native_actor_damage(&world, 1, 1) == 1);
    assert(sml2_native_actor_find(&world, 1)->state == SML2_NATIVE_ACTOR_DEFEATED);

    assert(sml2_native_actors_sync(&world, &stage, &far) == 0);
    assert(world.count == 2);
    assert(sml2_native_actor_find(&world, 1)->state == SML2_NATIVE_ACTOR_DEFEATED);
    assert(sml2_native_actor_find(&world, 2)->state == SML2_NATIVE_ACTOR_AWAKE);
    assert(sml2_native_actor_find(&world, 2)->flags & SML2_NATIVE_ACTOR_PERSISTENT);
    assert(sml2_native_actors_step(&world, update_actor, &updates) == 0);
    assert(updates == 2 && world.tick == 2);
    assert(sml2_native_actor_damage(&world, 99, 1) == -1);
    return 0;
}
