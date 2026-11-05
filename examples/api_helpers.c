#include <cedarecs/cedar.h>
#include <stdio.h>

typedef struct Position { float x, y; } Position;
typedef struct Velocity { float x, y; } Velocity;
// Use ced_slots(...) in shared headers, and define the slots in one .c file.
ced_slots_def(Position, Velocity, Sleeping)

ced_define_registration(register_game,
    ced_components(Position, Velocity),
    ced_tags(Sleeping))
ced_define_view(make_active_movers,
    ced_all(Position, Velocity),
    ced_without(Sleeping))
ced_define_spawn_plan(make_mover_plan, Position, Velocity)

int main(void) {
    ced_World *world = ced_world();
    if (!register_game(world)) { ced_drop(world); return 1; }
    ced_SpawnPlan movers = make_mover_plan(world);
    ced_Entity entity = ced_entity_from_plan(world, &movers);
    const Position initial = {10, 20};
    if (!ced_set_ptr(world, entity, Position, &initial) ||
        !ced_set(world, entity, Velocity, {.x = 2, .y = 0})) {
        ced_drop(world);
        return 1;
    }
    ced_View *active = make_active_movers(world);
    if (!active) { ced_drop(world); return 1; }
    ced_view_each(active, batch) {
        Position *p = ced_col(batch, Position);
        const Velocity *v = ced_col_const(batch, Velocity);
        ced_rows(batch, row) {
            p[row].x += v[row].x;
            p[row].y += v[row].y;
        }
    }
    const Position *p = ced_get_const(world, entity, Position);
    printf("Position: %.0f, %.0f\n", p->x, p->y);
    ced_view_free(active);
    ced_drop(world);
}
