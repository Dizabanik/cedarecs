#include <cedarecs/cedar.h>
#include <stdio.h>

typedef struct Position { float x, y; } Position;
typedef struct Velocity { float x, y; } Velocity;
ced_slots_def(Position, Velocity, Player)
ced_define_registration(register_game,
    ced_components(Position, Velocity), ced_tags(Player))

// This defines a C value type and a world-bound plan type. Components remain
// separate in the ECS. Field order never depends on registration order.
ced_define_bundle(MovingPlayer,
    (Position, position),
    (Velocity, velocity),
    ced_bundle_tag(Player))

int main(void) {
    ced_World *world = ced_world();
    if (!register_game(world)) { ced_drop(world); return 1; }

    ced_Entity first = ced_spawn_with(world,
        ced_value(Position, {.x = 10, .y = 20}),
        ced_value(Velocity, {.x = 2, .y = 0}),
        ced_tag(Player));

    MovingPlayer_Plan players = MovingPlayer_prepare(world);
    if (!MovingPlayer_valid(world, &players)) { ced_drop(world); return 1; }
    MovingPlayer initial = {.position = {30, 40}, .velocity = {3, 0}};
    ced_Entity second = MovingPlayer_spawn(world, &players, &initial);
    if (first == ced_entity_none || second == ced_entity_none) { ced_drop(world); return 1; }

    printf("Players: %.0f, %.0f\n", ced_get_const(world, first, Position)->x,
           ced_get_const(world, second, Position)->x);
    ced_drop(world);
}
