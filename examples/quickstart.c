#include <cedarecs/cedar.h>
#include <stdio.h>

typedef struct Position { float x, y; } Position;
typedef struct Velocity { float x, y; } Velocity;
ced_slots_def(Position, Velocity)

int main(void) {
    ced_World *world = ced_world();
    ced_slot_init(world, Position);
    ced_slot_init(world, Velocity);

    ced_Entity entity = ced_spawn_with(world,
        ced_value(Position, {.x = 10, .y = 20}),
        ced_value(Velocity, {.x = 2, .y = 0}));

    ced_each(world, batch, Position, Velocity) {
        Position *p = ced_col(batch, Position);
        const Velocity *v = ced_col_const(batch, Velocity);
        ced_rows(batch, row) {
            p[row].x += v[row].x;
            p[row].y += v[row].y;
        }
    }

    const Position *p = ced_get_const(world, entity, Position);
    printf("Position: %.0f, %.0f\n", p->x, p->y);
    ced_drop(world);
}
