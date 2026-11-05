#ifndef CEDARECS_CEDAR_H
#define CEDARECS_CEDAR_H

#include <cedarecs/wexds.h>
#include <cedarecs/access.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Cedar ECS
//
// C11 helpers include ced_slots, ced_define_registration, ced_define_view,
// ced_define_spawn_plan, ced_rows and pointer setters. See API_GUIDE.md for
// examples and constructor lifetimes.
//
// Common usage:
//     ced_slot(Position)              // in a header
//     ced_slot_def(Position)          // in one .c file
//     ced_slot_init(world, Position)  // during startup
//
//     ced_Entity e = ced_entity(world);
//     ced_set(world, e, Position, {.x = 10, .y = 20});
//
//     ced_each(world, batch, Position, Velocity) {
//         Position *pos = ced_col(batch, Position);
//         Velocity *vel = ced_col(batch, Velocity);
//         for (size_t i = 0; i < ced_len(batch); i++) {
//             pos[i].x += vel[i].x * dt;
//         }
//     }
//
// ced_each reuses a world-owned, per-thread view. Use deferred commands
// for structural changes while iterating. See README.md for lifetimes
// and the threading contract. C11 and POSIX threads (or Win32) are required.

// Configuration
#ifndef ced_block_size
#define ced_block_size 16384
#endif

#ifndef ced_max_components
#define ced_max_components 256
#endif
#if ced_max_components < 1 || ced_max_components > 65535
#error "ced_max_components must be between 1 and 65535"
#endif
#define ced_mask_words ((ced_max_components + 63) / 64)
#ifndef ced_max_fixed_steps
#define ced_max_fixed_steps 8
#endif

#ifndef ced_align

#if defined(__AVX512F__)
#define ced_align 64
#elif defined(__AVX2__) || defined(__AVX__)
#define ced_align 32
#else
#define ced_align 16 // NEON / SSE2
#endif
#endif

_Static_assert(ced_align >= _Alignof(void *) && !(ced_align & (ced_align - 1)),
			   "ced_align must be a power of two supporting pointer alignment");
_Static_assert(ced_block_size > 0 && ced_max_fixed_steps > 0,
			   "ECS capacities must be positive");

// Public IDs

// Aliases used by the implementation and low-level callers.
typedef ced_Component ComponentId;
typedef ced_Entity EntityId;
typedef ced_ArchetypeId ArchetypeId;

#define ced_entity_none UINT64_MAX
#define ced_component_none UINT16_MAX

// Preprocessor helpers
#define ced__cat(a, b) ced__cat_(a, b)
#define ced__cat_(a, b) a##b

#define ced__argc(...)                                                                 \
	ced__argc_(__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define ced__argc_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15,   \
				   _16, N, ...)                                                        \
	N

#define ced__pick(prefix, n) ced__cat(prefix, n)
#define ced_unpack(...) __VA_ARGS__

#define ced_id(Type) ced_id_##Type
#define ced__ids(...) ced__pick(ced__ids_, ced__argc(__VA_ARGS__))(__VA_ARGS__)
#define ced__ids_1(T1) ced_id(T1)
#define ced__ids_2(T1, T2) ced_id(T1), ced_id(T2)
#define ced__ids_3(T1, T2, T3) ced_id(T1), ced_id(T2), ced_id(T3)
#define ced__ids_4(T1, T2, T3, T4) ced_id(T1), ced_id(T2), ced_id(T3), ced_id(T4)
#define ced__ids_5(T1, T2, T3, T4, T5)                                                 \
	ced_id(T1), ced_id(T2), ced_id(T3), ced_id(T4), ced_id(T5)
#define ced__ids_6(T1, T2, T3, T4, T5, T6)                                             \
	ced_id(T1), ced_id(T2), ced_id(T3), ced_id(T4), ced_id(T5), ced_id(T6)
#define ced__ids_7(T1, T2, T3, T4, T5, T6, T7)                                         \
	ced_id(T1), ced_id(T2), ced_id(T3), ced_id(T4), ced_id(T5), ced_id(T6), ced_id(T7)
#define ced__ids_8(T1, T2, T3, T4, T5, T6, T7, T8)                                     \
	ced_id(T1), ced_id(T2), ced_id(T3), ced_id(T4), ced_id(T5), ced_id(T6),            \
		ced_id(T7), ced_id(T8)
#define ced__ids_9(T1, T2, T3, T4, T5, T6, T7, T8, T9)                                 \
	ced__ids_8(T1, T2, T3, T4, T5, T6, T7, T8), ced_id(T9)
#define ced__ids_10(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10)                           \
	ced__ids_9(T1, T2, T3, T4, T5, T6, T7, T8, T9), ced_id(T10)
#define ced__ids_11(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11)                      \
	ced__ids_10(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10), ced_id(T11)
#define ced__ids_12(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12)                 \
	ced__ids_11(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11), ced_id(T12)
#define ced__ids_13(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13)            \
	ced__ids_12(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12), ced_id(T13)
#define ced__ids_14(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13, T14)       \
	ced__ids_13(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13), ced_id(T14)
#define ced__ids_15(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13, T14, T15)  \
	ced__ids_14(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13, T14),          \
		ced_id(T15)
#define ced__ids_16(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13, T14, T15,  \
					T16)                                                               \
	ced__ids_15(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13, T14, T15),     \
		ced_id(T16)

// Component slots
#define ced_slot(Type) extern _Atomic(ced_Component) ced_id(Type);
#define ced_slot_def(Type) _Atomic(ced_Component) ced_id(Type) = ced_component_none;
#define ced_slot_init(world, Type)                                                     \
	(ced_id(Type) =                                                                    \
		 ced_component_register((world), #Type, sizeof(Type), _Alignof(Type)))
#define ced_tag_init(world, Type)                                                      \
	(ced_id(Type) = ced_component_register((world), #Type, 0, 1))
// IDs belong to a world. The global slot is only a fast lookup hint.
#define ced_type_id(world, Type) ced_component_lookup((world), ced_id(Type), #Type)

// Forward declarations
typedef struct ced_World ced_World;
typedef struct ced_Archetype ced_Archetype;
typedef struct ced_Query ced_Query;
typedef ced_Query ced_View;

// Systems
typedef uint8_t (*ced_SystemFn)(ced_World *world, int system_id, double dt);

#define ced_system(name) uint8_t name(ced_World *world, int system_id, double dt)

typedef enum ced_Stage {
	ced_stage_plugin,
	ced_stage_start,
	ced_stage_update,
	ced_stage_fixed_update
} ced_Stage;

// Component presence bitmask
typedef struct ced_Mask {
	uint64_t bits[ced_mask_words];
} ced_Mask;

static inline void ced_mask_clear(ced_Mask *m) {
	for (int i = 0; i < ced_mask_words; i++)
		m->bits[i] = 0;
}
static inline void ced_mask_set(ced_Mask *m, ced_Component c) {
	if (c >= ced_max_components)
		return;
	m->bits[c >> 6] |= (1ULL << (c & 63));
}
static inline bool ced_mask_has(const ced_Mask *m, ced_Component c) {
	return c < ced_max_components && (m->bits[c >> 6] & (1ULL << (c & 63))) != 0;
}
static inline bool ced_mask_contains_all(const ced_Mask *a, const ced_Mask *b) {
	for (int i = 0; i < ced_mask_words; i++)
		if ((a->bits[i] & b->bits[i]) != b->bits[i])
			return false;
	return true;
}

// Storage (public, for advanced use and tooling)
typedef struct ced_ColumnMapping {
	ced_Component src_col;
	ced_Component dst_col;
} ced_ColumnMapping;

typedef struct ced_ArchetypeEdge {
	ced_Component component;
	ced_Archetype *add;
	ced_Archetype *remove;
	// Packed maps list shared columns. Indexed maps store the changed
	// component in one internal descriptor (src_col); the column-map count is zero.
	ced_ColumnMapping *add_map;
	size_t add_map_count;
	ced_ColumnMapping *remove_map;
	size_t remove_map_count;
} ced_ArchetypeEdge;

kama_u32(ced_EdgeMap, ced_ArchetypeEdge *)
kama(ced_NameMap, ced_Component)
kama_fixed(ced_ArchetypeMap, sizeof(ced_Mask), ced_Archetype *)

struct ced_Archetype {
	// The world owns this allocation, including the metadata subarrays.
	// Borrow the subarray pointers; do not free them separately.
	ced_ArchetypeAccess access;
	bool indexed;
	bool _embedded:1, _zero_tail:1, _indexed_ready:1, _inline_rows:1, _bounded_rows:1;
	ced_ArchetypeId id;
	ced_Component *type;
	size_t type_count;
	size_t _uniform_size; // SIZE_MAX for mixed widths; immutable per signature
	size_t _initialized_rows; // packed dirty boundary; indexed common slot capacity

	ced_Column *columns;
    void **_column_allocations; // Packed buffer bases; indexed owner-array cache.
	ced_Entity *entities;
	size_t count;
	size_t capacity;

	ced_EdgeMap_t edges;
	ced_ArchetypeEdge *_edge_cache[8];
	ced_Mask mask;
	ced_Component column_for[ced_max_components];
};

_Static_assert(offsetof(ced_Archetype, access) == 0,
               "Read metadata must be the first archetype subobject");
_Static_assert(sizeof(ced_Record) == sizeof(void *) + 8,
			   "Entity records must stay compact");

typedef struct ced_SpawnPlan {
	ced_Archetype *archetype;
	uint64_t world_identity;
} ced_SpawnPlan;

// Initialized construction accepts components in any caller order. A name
// resolves the hint against this world; NULL names select a raw component ID.
enum { ced_bundle_max_members = 16 };
typedef struct ced_ComponentValue {
    ced_Component component;
    const char *name;
    size_t size, alignment;
    const void *value;
} ced_ComponentValue;
typedef struct ced_BundleMember {
    ced_Component component;
    const char *name;
    size_t size, alignment, offset;
} ced_BundleMember;
// Prepared plans are immutable, world-bound values. Do not edit private fields.
typedef struct ced_BundlePlan {
    ced_SpawnPlan _spawn;
    size_t _size;
    size_t _offsets[ced_bundle_max_members];
    ced_Component _columns[ced_bundle_max_members];
    const atomic_bool *_parallel;
} ced_BundlePlan;

typedef struct ced_ComponentInfo {
	size_t size;
	size_t alignment;
	char *name;
	char _short_name[24];
	ced_IndexedColumn indexed;
} ced_ComponentInfo;

typedef enum ced_CmdType {
	ced_cmd_add,
	ced_cmd_remove,
	ced_cmd_set,
	ced_cmd_put,
	ced_cmd_kill
} ced_CmdType;

typedef struct ced_DeferredCmd {
	ced_Entity entity;
	size_t data_offset;
	ced_CmdType type;
	ced_Component component;
} ced_DeferredCmd;

_Static_assert(sizeof(ced_DeferredCmd) <= 3 * sizeof(uint64_t),
               "Deferred command records must stay compact");

typedef struct ced_CmdBuffer {
	ced_DeferredCmd *cmds;
	size_t count;
	size_t capacity;
	unsigned char *data;
	size_t data_size, data_capacity;
} ced_CmdBuffer;

// A contiguous slice of one archetype that matches a view.
typedef struct ced_Batch {
	struct ced_Query *query;
	ced_Archetype *archetype;
	void **column_ptrs;
	ced_Entity *entities;
	uint32_t count, _field_count; // world index limit; validated query arity
	size_t offset;
} ced_Batch;

typedef ced_Batch ced_QueryMatch;

// Required components define column_ptrs order. Excluded and any-of filters
// affect matching; ced_col can also access any optional component in a batch.
typedef struct ced_ViewDesc {
	size_t all_count;
	const ced_Component *all;
	size_t none_count;
	const ced_Component *none;
	size_t any_count;
	const ced_Component *any;
} ced_ViewDesc;

typedef struct ced_Iter {
	ced_View *view;
	size_t index;
    ced_Batch *_matches;
    size_t _match_count;
} ced_Iter;

struct ced_Query {
	ced_Batch *matches;
	size_t match_count;
	size_t _match_capacity;
	ced_Component *components;
	size_t *_field_sizes;
	size_t component_count;
	ced_Mask mask;
	size_t _last_archetype_count;
	ced_Mask exclude_mask, any_mask;
	bool has_any;
	uint64_t _revision, _identity;
	struct ced_Job *_dispatch_jobs;
	size_t _dispatch_count, _dispatch_capacity, _dispatch_grain;
	uint64_t _dispatch_revision;
    unsigned char *_serial_jobs;
    size_t _serial_capacity, _serial_stride, _serial_grain;
    uint64_t _serial_revision, _serial_observed_revision;
    size_t _serial_observed_grain;
	ced_World *world;
	ced_Query *_prev, *_next, *_cache_next;
	ced_Query **_cache_head;
	void **_column_storage;
	size_t _column_slots;
};

struct ced_World {
	bool indexed;
	ced_Entity *_indexed_entities;
	ced_Record *entities;
	size_t entity_count;
	size_t entity_capacity;
	size_t alive_count;
	uint32_t *free_list;
	size_t free_count;
	size_t free_capacity;

	ced_ComponentInfo **components; // Stable metadata objects; growable pointer table.
	size_t component_count;
	size_t component_capacity;

	ced_Archetype **archetypes;
	size_t archetype_count;
	size_t archetype_capacity;

	void (*start)(ced_World *);
	void (*update)(ced_World *);

	ced_SystemFn *start_systems;
	size_t start_systems_size;
	struct ced_SystemRecord *update_systems;
	size_t update_systems_size;

	// Private runtime owns command buffers, query caches and the worker pool.
	struct ced_Runtime *_runtime;
	ced_View *_views;
	ced_ArchetypeMap_t _archetype_index;
	ced_NameMap_t _component_names;
	size_t _start_capacity, _update_capacity, _fixed_capacity;
	struct ced_SystemRecord *fixed_systems;
	size_t fixed_systems_size;
	uint64_t identity, storage_revision;

#ifdef USE_FPS_DELAY
	float targetFPS;
#endif
	atomic_bool ecs_quit;
};

typedef struct ced_SystemRecord {
	ced_SystemFn func;
	double interval; // 0.0 = every frame
	double accumulator;
	double elapsed;
} ced_SystemRecord;

// Core API
CEDARECS_ALLOCATOR ced_World *ced_init(void);
// Select before creating the world. Indexed storage avoids retained-component
// copies on transitions, at the cost of per-type slot storage and fragmented
// query runs. ced_init uses compact archetype storage by default.
CEDARECS_ALLOCATOR ced_World *ced_init_indexed(void);
ced_IndexedReader ced_indexed_reader(const ced_World *world, ced_Component component);
void ced_free(ced_World *world);
void ced_run(ced_World *world);
void ced_run_with(ced_World *world, void (*before_start)(void),
				  void (*before_update)(void));
void ced_start(ced_World *world);
void ced_update_frame(ced_World *world, double dt);

static inline ced_World *ced_world(void) { return ced_init(); }
static inline void ced_drop(ced_World *world) { ced_free(world); }

// Entity API
// A lightweight, non-owning interface also usable from C++ via access.h.
ced_ReadAccess ced_read_access(const ced_World *world);
ced_Entity ced_entity_init(ced_World *world);
void ced_entity_destroy(ced_World *world, ced_Entity entity);
bool ced_is_alive(const ced_World *world, ced_Entity entity);
static inline uint32_t ced_entity_index(ced_Entity entity) { return (uint32_t)entity; }
static inline uint32_t ced_entity_generation(ced_Entity entity) {
	return (uint32_t)(entity >> 32);
}
static inline size_t ced_alive_count(const ced_World *world) {
	return world ? world->alive_count : 0;
}
bool ced_reserve_entities(ced_World *world, size_t capacity);
// Create with the final component set in one storage insertion (all values zero).
ced_Entity ced_entity_with_ids(ced_World *world, size_t count,
							   const ced_Component *ids);
// Prepared signatures validate once; plans remain valid until their world dies.
ced_SpawnPlan ced_spawn_plan_init(ced_World *world, size_t count,
								  const ced_Component *ids);
ced_Entity ced_entity_from_plan(ced_World *world, const ced_SpawnPlan *plan);
// Initialize directly on insertion. values has archetype->type_count entries,
// ordered by ascending component ID (the plan's canonical type order). NULL
// values/entries zero-initialize; tag entries are ignored. Values may alias
// existing component storage, including when insertion grows that storage.
ced_Entity ced_entity_from_plan_values(ced_World *world, const ced_SpawnPlan *plan,
                                      const void *const *values);
bool ced_reserve_plan(ced_World *world, const ced_SpawnPlan *plan, size_t capacity);
// Validate the complete initializer before creating an entity. Duplicate,
// unregistered or layout-mismatched components and NULL data values fail.
ced_Entity ced_entity_with_values(ced_World *world, size_t count,
                                  const ced_ComponentValue *values);
ced_BundlePlan ced_bundle_plan_init(ced_World *world, size_t object_size,
                                    size_t count, const ced_BundleMember *members);
ced_Entity ced_entity_from_bundle(ced_World *world, const ced_BundlePlan *plan,
                                   const void *value);
bool ced_reserve_bundle(ced_World *world, const ced_BundlePlan *plan, size_t capacity);
static inline bool ced_bundle_plan_valid(const ced_World *world, const ced_BundlePlan *plan) {
    return world && plan && plan->_spawn.world_identity == world->identity && plan->_spawn.archetype;
}
bool ced_reserve_archetype(ced_World *world, size_t count, const ced_Component *ids,
						   size_t capacity);

static inline ced_Entity ced_entity(ced_World *world) { return ced_entity_init(world); }
static inline void ced_kill(ced_World *world, ced_Entity entity) {
	ced_entity_destroy(world, entity);
}

// Component API
ced_Component ced_component_init(ced_World *world, size_t component_size);
ced_Component ced_component_register(ced_World *world, const char *name, size_t size,
									 size_t alignment);
ced_Component ced_component_find(const ced_World *world, const char *name);
ced_Component ced_component_lookup(const ced_World *world, ced_Component hint,
								   const char *name);
const char *ced_component_name(const ced_World *world, ced_Component component);
#if defined(__GNUC__) || defined(__clang__)
#define CEDARECS_INLINE inline __attribute__((always_inline))
#else
#define CEDARECS_INLINE inline
#endif
// C11 inline definitions expose validation and address arithmetic to callers.
// Out-of-line ABI symbols are still provided by component.c.
#ifdef CEDARECS_NO_INLINE_ACCESSORS
const void *ced_get_component_const(const ced_World *world, ced_Entity entity,
									ced_Component component);
#else
CEDARECS_INLINE const void *ced_get_component_const(const ced_World *world, ced_Entity entity,
										   ced_Component component) {
	uint32_t index = (uint32_t)entity;
	if (!world || component >= world->component_count || index >= world->entity_count)
		return NULL;
	const ced_Record *rec = &world->entities[index];
	if (!rec->archetype || rec->generation != (uint32_t)(entity >> 32))
		return NULL;
	ced_Component col = rec->archetype->column_for[component];
	if (col == ced_component_none)
		return NULL;
	const ced_Column *column = &rec->archetype->columns[col];
	return column->component_size
			   ? column->data + (size_t)rec->row * column->component_size
			   : NULL;
}
#endif
void ced_add_component(ced_World *world, ced_Entity entity, ced_Component component);
void ced_remove_component(ced_World *world, ced_Entity entity, ced_Component component);
bool ced_set_component(ced_World *world, ced_Entity entity, ced_Component component,
					   const void *value);
bool ced_add_set_component(ced_World *world, ced_Entity entity, ced_Component component,
						   const void *value);
#ifdef CEDARECS_NO_INLINE_ACCESSORS
void *ced_get_component(ced_World *world, ced_Entity entity, ced_Component component);
#else
CEDARECS_INLINE void *ced_get_component(ced_World *world, ced_Entity entity,
							   ced_Component component) {
	return (void *)ced_get_component_const(world, entity, component);
}
#endif
#ifdef CEDARECS_NO_INLINE_ACCESSORS
bool ced_has_component(ced_World *world, ced_Entity entity, ced_Component component);
#else
CEDARECS_INLINE bool ced_has_component(ced_World *world, ced_Entity entity,
                              ced_Component component) {
	uint32_t index = (uint32_t)entity;
	if (!world || component >= world->component_count || index >= world->entity_count)
		return false;
	const ced_Record *rec = &world->entities[index];
	if (!rec->archetype || rec->generation != (uint32_t)(entity >> 32))
		return false;
	return rec->archetype->column_for[component] != ced_component_none;
}
#endif

// Typed wrappers evaluate world/entity/value expressions exactly once.
static inline void ced__add(ced_World *w, ced_Entity e, ced_Component hint,
							const char *name) {
	ced_add_component(w, e, ced_component_lookup(w, hint, name));
}
static inline void ced__remove(ced_World *w, ced_Entity e, ced_Component hint,
							   const char *name) {
	ced_remove_component(w, e, ced_component_lookup(w, hint, name));
}
static inline bool ced__set(ced_World *w, ced_Entity e, ced_Component hint,
							const char *name, const void *value) {
	return ced_add_set_component(w, e, ced_component_lookup(w, hint, name), value);
}
static inline bool ced__write(ced_World *w, ced_Entity e, ced_Component hint,
							  const char *name, const void *value) {
	return ced_set_component(w, e, ced_component_lookup(w, hint, name), value);
}
static inline void *ced__get(ced_World *w, ced_Entity e, ced_Component hint,
							 const char *name) {
	return ced_get_component(w, e, ced_component_lookup(w, hint, name));
}
static inline const void *ced__get_const(const ced_World *w, ced_Entity e,
										 ced_Component hint, const char *name) {
	return ced_get_component_const(w, e, ced_component_lookup(w, hint, name));
}
static inline bool ced__has(ced_World *w, ced_Entity e, ced_Component hint,
							const char *name) {
	return ced_has_component(w, e, ced_component_lookup(w, hint, name));
}
#define ced_add(world, entity, Type) ced__add((world), (entity), ced_id(Type), #Type)
#define ced_remove(world, entity, Type)                                                \
	ced__remove((world), (entity), ced_id(Type), #Type)
#define ced_set(world, entity, Type, ...)                                              \
	ced__set((world), (entity), ced_id(Type), #Type, &(Type)__VA_ARGS__)
#define ced_write(world, entity, Type, value_ptr)                                      \
	ced__write((world), (entity), ced_id(Type), #Type, (value_ptr))
#define ced_get(world, entity, Type)                                                   \
	((Type *)ced__get((world), (entity), ced_id(Type), #Type))
#define ced_get_const(world, entity, Type)                                             \
	((const Type *)ced__get_const((world), (entity), ced_id(Type), #Type))
#define ced_has(world, entity, Type) ced__has((world), (entity), ced_id(Type), #Type)

// Deferred API (multi-producer queues; flush on the world owner thread)
void ced_defer_add_component(ced_World *world, ced_Entity entity,
							 ced_Component component);
void ced_defer_remove_component(ced_World *world, ced_Entity entity,
								ced_Component component);
void ced_defer_set_component(ced_World *world, ced_Entity entity,
							 ced_Component component, const void *value);
void ced_defer_add_set_component(ced_World *world, ced_Entity entity,
								 ced_Component component, const void *value);
void ced_defer_destroy_entity(ced_World *world, ced_Entity entity);
// Optional, nestable producer scope: acquire the queue mutex once for a group
// of commands. End on the same thread before waiting for another thread to
// flush. The owner may flush its own scope. Ending an unopened scope is false.
// Scopes do not automatically flush; unscoped commands remain supported.
bool ced_commands_begin(ced_World *world);
bool ced_commands_end(ced_World *world);
void ced_flush_commands(ced_World *world);
void ced_discard_commands(ced_World *world);
static inline void ced__defer_add(ced_World *w, ced_Entity e, ced_Component hint,
								  const char *name) {
	ced_defer_add_component(w, e, ced_component_lookup(w, hint, name));
}
static inline void ced__defer_remove(ced_World *w, ced_Entity e, ced_Component hint,
									 const char *name) {
	ced_defer_remove_component(w, e, ced_component_lookup(w, hint, name));
}
static inline void ced__defer_set(ced_World *w, ced_Entity e, ced_Component hint,
								  const char *name, const void *value) {
	ced_defer_add_set_component(w, e, ced_component_lookup(w, hint, name), value);
}
static inline void ced__defer_write(ced_World *w, ced_Entity e, ced_Component hint,
									const char *name, const void *value) {
	ced_defer_set_component(w, e, ced_component_lookup(w, hint, name), value);
}
#define ced_defer_add(world, entity, Type)                                             \
	ced__defer_add((world), (entity), ced_id(Type), #Type)
#define ced_defer_remove(world, entity, Type)                                          \
	ced__defer_remove((world), (entity), ced_id(Type), #Type)
#define ced_defer_set(world, entity, Type, ...)                                        \
	ced__defer_set((world), (entity), ced_id(Type), #Type, &(Type)__VA_ARGS__)
#define ced_defer_write(world, entity, Type, value_ptr)                                \
	ced__defer_write((world), (entity), ced_id(Type), #Type, (value_ptr))
static inline void ced_defer_kill(ced_World *world, ced_Entity entity) {
	ced_defer_destroy_entity(world, entity);
}
static inline void ced_flush(ced_World *world) { ced_flush_commands(world); }

// Views and batches
ced_View *ced_view_init(ced_World *world, const ced_ViewDesc *desc);
ced_View *ced_view_from_ids(ced_World *world, size_t component_count,
							const ced_Component *components);
ced_Query *ced_query_init(ced_World *world, size_t component_count, ...);
void ced_view_refresh(ced_View *view, ced_World *world);
void ced_query_refresh(ced_Query *query, ced_World *world);
void ced_view_free(ced_View *view);
void ced_query_free(ced_Query *query);

// Batch pointers and rows remain valid until the next structural change.
static inline void *ced_batch_col(const ced_Batch *batch, ced_Component component) {
	if (!batch || component >= ced_max_components)
		return NULL;
	ced_Component col = batch->archetype->column_for[component];
	if (col == ced_component_none || !batch->archetype->columns[col].component_size)
		return NULL;
	ced_Column *c = &batch->archetype->columns[col];
	return c->data ? c->data + batch->offset * c->component_size : NULL;
}
// Required fields are already bound in query order (also for parallel slices).
// Use ced_batch_col / ced_col for optional fields selected by component ID.
static inline void *ced_batch_field(const ced_Batch *batch, size_t index) {
	return batch && batch->query && batch->column_ptrs &&
				   index < batch->_field_count
			   ? batch->column_ptrs[index]
			   : NULL;
}
#define ced_field(batch, Type, index) ((Type *)ced_batch_field((batch), (index)))
#define ced_field_const(batch, Type, index)                                            \
	((const Type *)ced_batch_field((batch), (index)))

static inline void *ced__batch_col_named(const ced_Batch *batch, ced_Component hint,
										 const char *name) {
	return batch ? ced_batch_col(batch,
								 ced_component_lookup(batch->query->world, hint, name))
				 : NULL;
}
static inline size_t ced_len(const ced_Batch *batch) { return batch->count; }
static inline ced_Entity *ced_entities(const ced_Batch *batch) {
	return batch->entities;
}
static inline ced_Entity ced_entity_at(const ced_Batch *batch, size_t index) {
	return batch && index < batch->count ? batch->entities[index] : ced_entity_none;
}
size_t ced_view_count(ced_View *view);
static inline ced_Iter ced_iter_begin(ced_View *view) {
	if (view && view->_revision != view->world->storage_revision)
		ced_view_refresh(view, view->world);
	return (ced_Iter){view, 0, view ? view->matches : NULL, view ? view->match_count : 0};
}
static inline ced_Batch *ced_iter_next(ced_Iter *iter) {
	if (!iter || !iter->view)
		return NULL;
	while (iter->index < iter->_match_count) {
		ced_Batch *batch = &iter->_matches[iter->index++];
		if (batch->count)
			return batch;
	}
	return NULL;
}
ced_View *ced_cached_view(ced_World *world, size_t count, const char *const *names);
ced_View *ced_view_from_names(ced_World *world, size_t count, const char *const *names);

#define ced__names_1(T1) #T1
#define ced__names_2(T1, T2) #T1, #T2
#define ced__names_3(T1, T2, T3) #T1, #T2, #T3
#define ced__names_4(T1, T2, T3, T4) #T1, #T2, #T3, #T4
#define ced__names_5(T1, T2, T3, T4, T5) #T1, #T2, #T3, #T4, #T5
#define ced__names_6(T1, T2, T3, T4, T5, T6) #T1, #T2, #T3, #T4, #T5, #T6
#define ced__names_7(T1, T2, T3, T4, T5, T6, T7) #T1, #T2, #T3, #T4, #T5, #T6, #T7
#define ced__names_8(T1, T2, T3, T4, T5, T6, T7, T8)                                   \
	#T1, #T2, #T3, #T4, #T5, #T6, #T7, #T8
#define ced__names_9(T1, T2, T3, T4, T5, T6, T7, T8, T9)                               \
	#T1, #T2, #T3, #T4, #T5, #T6, #T7, #T8, #T9
#define ced__names_10(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10)                         \
	#T1, #T2, #T3, #T4, #T5, #T6, #T7, #T8, #T9, #T10
#define ced__names_11(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11)                    \
	#T1, #T2, #T3, #T4, #T5, #T6, #T7, #T8, #T9, #T10, #T11
#define ced__names_12(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12)               \
	#T1, #T2, #T3, #T4, #T5, #T6, #T7, #T8, #T9, #T10, #T11, #T12
#define ced__names_13(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13)          \
	#T1, #T2, #T3, #T4, #T5, #T6, #T7, #T8, #T9, #T10, #T11, #T12, #T13
#define ced__names_14(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13, T14)     \
	#T1, #T2, #T3, #T4, #T5, #T6, #T7, #T8, #T9, #T10, #T11, #T12, #T13, #T14
#define ced__names_15(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13, T14,     \
					  T15)                                                             \
	#T1, #T2, #T3, #T4, #T5, #T6, #T7, #T8, #T9, #T10, #T11, #T12, #T13, #T14, #T15
#define ced__names_16(T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13, T14,     \
					  T15, T16)                                                        \
	#T1, #T2, #T3, #T4, #T5, #T6, #T7, #T8, #T9, #T10, #T11, #T12, #T13, #T14, #T15,   \
		#T16
#define ced__names(...) ced__pick(ced__names_, ced__argc(__VA_ARGS__))(__VA_ARGS__)
#define ced_view(world, ...)                                                           \
	ced_view_from_names((world), ced__argc(__VA_ARGS__),                               \
						(const char *const[]){ced__names(__VA_ARGS__)})
#define ced_spawn(world, ...)                                                          \
	ced_entity_with_names((world), ced__argc(__VA_ARGS__),                             \
						  (const char *const[]){ced__names(__VA_ARGS__)})
ced_Entity ced_entity_with_names(ced_World *world, size_t count,
								 const char *const *names);
#define ced_col(batch, Type)                                                           \
	((Type *)ced__batch_col_named((batch), ced_id(Type), #Type))
#define ced_col_const(batch, Type) ((const Type *)ced_col((batch), Type))
#define ced_view_each(view, batch) ced__iterate(__LINE__, (view), batch)
#define ced__iterate(line, view, batch) ced__iterate_(line, view, batch)
#define ced__iterate_(line, q, batch)                                                  \
	for (ced_Iter ced__cat(ced__iter_, line) = ced_iter_begin(q);                      \
		 ced__cat(ced__iter_, line).view; ced__cat(ced__iter_, line).view = NULL)      \
		for (ced_Batch *batch = ced_iter_next(&ced__cat(ced__iter_, line)); batch;     \
			 batch = ced_iter_next(&ced__cat(ced__iter_, line)))
#define ced_each(world, batch, ...)                                                    \
	ced_view_each (ced_cached_view((world), ced__argc(__VA_ARGS__),                    \
								   (const char *const[]){ced__names(__VA_ARGS__)}),    \
				   batch)

// Synchronous parallel iteration. Total thread count includes the caller.
// Each callback owns its rows; use ced_defer_* for structural changes.
// Commands are flushed after all workers finish. Nested dispatch returns false.
typedef void (*ced_BatchFn)(ced_World *world, const ced_Batch *batch, void *context);
bool ced_set_threads(ced_World *world, size_t total_threads);
size_t ced_thread_count(const ced_World *world);
bool ced_parallel_each(ced_View *view, size_t grain, ced_BatchFn fn, void *context);
// Automatic batching: one full match with a single thread; balanced chunks
// across workers otherwise. Use parallel_each for a strict maximum grain.
bool ced_parallel_each_auto(ced_View *view, ced_BatchFn fn, void *context);

// System API
void ced_add_start_system(ced_World *world, ced_SystemFn system);
void ced_add_update_system(ced_World *world, ced_SystemFn system);
void ced_add_update_system_capped(ced_World *world, ced_SystemFn system,
								  double target_fps);
void ced_add_fixed_system(ced_World *world, ced_SystemFn system, double step);
void ced_custom_update(ced_World *world, void (*func)(ced_World *));
void ced_custom_start(ced_World *world, void (*func)(ced_World *));
ced_World *ced_add_systems_impl(ced_World *world, int stage, ...);

static inline void ced_on_start_one(ced_World *world, ced_SystemFn system) {
	ced_add_start_system(world, system);
}
static inline void ced_on_update_one(ced_World *world, ced_SystemFn system) {
	ced_add_update_system(world, system);
}
static inline void ced_on_update_capped(ced_World *world, ced_SystemFn system,
										double target_fps) {
	ced_add_update_system_capped(world, system, target_fps);
}

#define ced_on_start(world, systems)                                                   \
	ced_add_systems_impl((world), ced_stage_start, ced_unpack systems,                 \
						 (ced_SystemFn)NULL)
#define ced_on_update(world, systems)                                                  \
	ced_add_systems_impl((world), ced_stage_update, ced_unpack systems,                \
						 (ced_SystemFn)NULL)
#define ced_systems(world, stage, systems)                                             \
	ced_add_systems_impl((world), (stage), ced_unpack systems, (ced_SystemFn)NULL)
#define ced_on_fixed(world, systems)                                                   \
	ced_add_systems_impl((world), ced_stage_fixed_update, ced_unpack systems,          \
						 (ced_SystemFn)NULL)
#define ced_use(world, plugins)                                                        \
	ced_add_systems_impl((world), ced_stage_plugin, ced_unpack plugins,                \
						 (ced_SystemFn)NULL)

// Debug
void ced_print_archs(ced_World *world);

// C11 convenience declarations and iteration
// See API_GUIDE.md for factories, ownership and constructor costs.
// These macros expand to the existing checked API; no runtime state is added.

// All type lists accept 1-16 typedef identifiers. ced__api_* names are private.
#if defined(__GNUC__) || defined(__clang__)
#define ced__api_generated CEDARECS_INLINE __attribute__((unused))
#else
#define ced__api_generated CEDARECS_INLINE
#endif
#define ced__api_each(M, context, ...) \
    ced__pick(ced__api_each_, ced__argc(__VA_ARGS__))(M, context, __VA_ARGS__)
#define ced__api_each_1(M, context, T) M(context, T)
#define ced__api_each_2(M, context, T, ...) \
    M(context, T) ced__api_each_1(M, context, __VA_ARGS__)
#define ced__api_each_3(M, context, T, ...) \
    M(context, T) ced__api_each_2(M, context, __VA_ARGS__)
#define ced__api_each_4(M, context, T, ...) \
    M(context, T) ced__api_each_3(M, context, __VA_ARGS__)
#define ced__api_each_5(M, context, T, ...) \
    M(context, T) ced__api_each_4(M, context, __VA_ARGS__)
#define ced__api_each_6(M, context, T, ...) \
    M(context, T) ced__api_each_5(M, context, __VA_ARGS__)
#define ced__api_each_7(M, context, T, ...) \
    M(context, T) ced__api_each_6(M, context, __VA_ARGS__)
#define ced__api_each_8(M, context, T, ...) \
    M(context, T) ced__api_each_7(M, context, __VA_ARGS__)
#define ced__api_each_9(M, context, T, ...) \
    M(context, T) ced__api_each_8(M, context, __VA_ARGS__)
#define ced__api_each_10(M, context, T, ...) \
    M(context, T) ced__api_each_9(M, context, __VA_ARGS__)
#define ced__api_each_11(M, context, T, ...) \
    M(context, T) ced__api_each_10(M, context, __VA_ARGS__)
#define ced__api_each_12(M, context, T, ...) \
    M(context, T) ced__api_each_11(M, context, __VA_ARGS__)
#define ced__api_each_13(M, context, T, ...) \
    M(context, T) ced__api_each_12(M, context, __VA_ARGS__)
#define ced__api_each_14(M, context, T, ...) \
    M(context, T) ced__api_each_13(M, context, __VA_ARGS__)
#define ced__api_each_15(M, context, T, ...) \
    M(context, T) ced__api_each_14(M, context, __VA_ARGS__)
#define ced__api_each_16(M, context, T, ...) \
    M(context, T) ced__api_each_15(M, context, __VA_ARGS__)

#define ced__api_slot(context, Type) \
    _Static_assert(sizeof(#Type) > 1, "ced_slots requires a type name"); ced_slot(Type)
#define ced__api_slot_def(context, Type) \
    _Static_assert(sizeof(#Type) > 1, "ced_slots_def requires a type name"); ced_slot_def(Type)
#define ced_slots(...) ced__api_each(ced__api_slot, unused, __VA_ARGS__)
#define ced_slots_def(...) ced__api_each(ced__api_slot_def, unused, __VA_ARGS__)

// Registration clauses are only for ced_define_registration. Every type is
// attempted, including after failure. Failure is reported, not rolled back.
#define ced__api_register_component(world, Type) \
    if (ced_slot_init(world, Type) == ced_component_none) ced__api_ok = false;
#define ced__api_register_tag(world, Type) \
    if (ced_tag_init(world, Type) == ced_component_none) ced__api_ok = false;
#define ced_components(...) \
    ced__api_each(ced__api_register_component, ced__api_world, __VA_ARGS__)
#define ced_tags(...) \
    ced__api_each(ced__api_register_tag, ced__api_world, __VA_ARGS__)
#define ced__api_clause(context, clause) clause
#define ced_define_registration(name, ...) \
    static ced__api_generated bool name(ced_World *ced__api_world) { \
        bool ced__api_ok = true; \
        ced__api_each(ced__api_clause, unused, __VA_ARGS__) \
        return ced__api_ok; \
    }

// Query clauses are only for ced_define_view. Omit unused clauses rather than
// supplying an empty list. Required fields retain their declared order.
#define ced__api_id(world, Type) ced_type_id(world, Type),
#define ced_all(...) \
    .all_count = ced__argc(__VA_ARGS__), \
    .all = (const ced_Component[]){ced__api_each(ced__api_id, ced__api_world, __VA_ARGS__)}
#define ced_without(...) \
    .none_count = ced__argc(__VA_ARGS__), \
    .none = (const ced_Component[]){ced__api_each(ced__api_id, ced__api_world, __VA_ARGS__)}
#define ced_any(...) \
    .any_count = ced__argc(__VA_ARGS__), \
    .any = (const ced_Component[]){ced__api_each(ced__api_id, ced__api_world, __VA_ARGS__)}
#define ced_define_view(name, ...) \
    static ced__api_generated ced_View *name(ced_World *ced__api_world) { \
        return ced_view_init(ced__api_world, &(ced_ViewDesc){__VA_ARGS__}); \
    }

// Prepare a signature for repeated spawning. Keep the returned plan in user
// state and call ced_entity_from_plan in spawning loops. This creates no entity
// or global cache.
#define ced_define_spawn_plan(name, ...) \
    static ced__api_generated ced_SpawnPlan name(ced_World *ced__api_world) { \
        return ced_spawn_plan_init(ced__api_world, ced__argc(__VA_ARGS__), \
            (const ced_Component[]){ced__api_each(ced__api_id, ced__api_world, __VA_ARGS__)}); \
    }

// Evaluates batch once. break and continue work as in any for loop.
// Structural changes during iteration remain invalid; use deferred commands.
#define ced_rows(batch, row) \
    for (size_t ced__cat(ced__api_count_, row) = ced_len((batch)), row = 0; \
         row < ced__cat(ced__api_count_, row); ++row)

// The conditional checks pointer compatibility without evaluating its unused
// branch. These are for data components; use ced_add/defer_add for tags.
#define ced__api_value_ptr(Type, pointer) (1 ? (pointer) : (const Type *)0)
#define ced_set_ptr(world, entity, Type, pointer) \
    ced__set((world), (entity), ced_id(Type), #Type, ced__api_value_ptr(Type, pointer))
#define ced_defer_set_ptr(world, entity, Type, pointer) \
    ced__defer_set((world), (entity), ced_id(Type), #Type, ced__api_value_ptr(Type, pointer))

// Fully initialized one-shot construction. Parenthesized descriptors keep
// initializer commas together; expressions are evaluated once.
#define ced_value(Type, ...) \
    ((ced_ComponentValue){ced_id(Type), #Type, sizeof(Type), _Alignof(Type), &(Type)__VA_ARGS__})
#define ced_value_ptr(Type, pointer) \
    ((ced_ComponentValue){ced_id(Type), #Type, sizeof(Type), _Alignof(Type), ced__api_value_ptr(Type, pointer)})
#define ced_tag(Type) ((ced_ComponentValue){ced_id(Type), #Type, 0, 1, NULL})
#define ced_spawn_with(world, ...) \
    ced_entity_with_values((world), ced__argc(__VA_ARGS__), \
        (const ced_ComponentValue[]){__VA_ARGS__})

// Named, typed initialized bundles: (Type, field) entries and optional tags.
// Preparing resolves field order once. Spawning takes one typed struct value.
#define ced_bundle_tag(Type) (Type, ced__unused_tag, tag)
#define ced__bundle_apply(M, Name, tuple) ced__bundle_apply_(M, Name, ced_unpack tuple)
#define ced__bundle_apply_(M, Name, ...) \
    ced__pick(M, ced__argc(__VA_ARGS__))(Name, __VA_ARGS__)
#define ced__bundle_decl(Name, tuple) ced__bundle_apply(ced__bundle_decl_, Name, tuple)
#define ced__bundle_decl_2(Name, Type, field) Type field;
#define ced__bundle_decl_3(Name, Type, unused, tag)
#define ced__bundle_count(Name, tuple) ced__bundle_apply(ced__bundle_count_, Name, tuple)
#define ced__bundle_count_2(Name, Type, field) + 1
#define ced__bundle_count_3(Name, Type, unused, tag) + 0
#define ced__bundle_member(Name, tuple) ced__bundle_apply(ced__bundle_member_, Name, tuple)
#define ced__bundle_member_2(Name, Type, field) \
    {ced_id(Type), #Type, sizeof(Type), _Alignof(Type), offsetof(Name, field)},
#define ced__bundle_member_3(Name, Type, unused, tag) \
    {ced_id(Type), #Type, 0, 1, 0},

// Prepared insertion and publication. A zero status leaves the world untouched
// and selects the allocating path, which handles aliased input. Named bundles
// always have data and require a non-NULL value. Check world identity before
// reading the cached flag: it belongs to the plan's original world.
typedef struct ced__BundleRow {
    ced_World *world;
    ced_Archetype *arch;
    ced_Record *record;
    ced_Column *columns;
    void *const *owners;
    ced_Entity entity;
    size_t row;
    bool indexed;
} ced__BundleRow;
static ced__api_generated int ced__bundle_begin(ced_World *world, const ced_BundlePlan *plan,
                                                const void *value, ced__BundleRow *out) {
    if (!world || !plan || plan->_spawn.world_identity != world->identity ||
        !plan->_spawn.archetype || !value ||
        atomic_load_explicit(plan->_parallel, memory_order_acquire)) return -1;
    bool fresh = world->free_count == 0;
    if (fresh && world->entity_count == UINT32_MAX) return -1;
    uint32_t index = fresh ? (uint32_t)world->entity_count : world->free_list[world->free_count - 1];
    ced_Archetype *arch = plan->_spawn.archetype;
    bool indexed = world->indexed;
    if ((fresh && world->entity_count == world->entity_capacity) ||
        (indexed ? index >= arch->_initialized_rows : arch->count == arch->capacity)) return 0;
    ced_Record *record = &world->entities[index];
    ced_Entity entity = ((uint64_t)(fresh ? 0 : record->generation) << 32) | index;
    *out = (ced__BundleRow){.world = world, .arch = arch, .record = record,
        .columns = arch->columns, .owners = arch->_column_allocations,
        .entity = entity, .indexed = indexed};
    if (fresh) world->entity_count++; else world->free_count--;
    if (indexed) {
        out->row = index;
        arch->count++;
        if (world->_indexed_entities) world->_indexed_entities[index] = entity;
    } else {
        out->row = arch->count++;
        if (arch->_initialized_rows < arch->count) arch->_initialized_rows = arch->count;
        arch->entities[out->row] = entity;
    }
    return 1;
}
static ced__api_generated ced_Entity ced__bundle_finish(ced__BundleRow *row) {
    *row->record = (ced_Record){row->arch, (uint32_t)row->row, (uint32_t)(row->entity >> 32)};
    row->world->alive_count++;
    row->world->storage_revision++;
    return row->entity;
}
#define ced__bundle_index(Name, Type) ced__cat(ced__cat(ced__cat(ced__bundle_, Name), _), Type)
#define ced__bundle_enum(Name, tuple) ced__bundle_apply(ced__bundle_enum_, Name, tuple)
#define ced__bundle_enum_2(Name, Type, field) ced__bundle_index(Name, Type),
#define ced__bundle_enum_3(Name, Type, unused, tag) ced__bundle_index(Name, Type),
#define ced__bundle_values(Name, tuple) ced__bundle_apply(ced__bundle_values_, Name, tuple)
#define ced__bundle_values_2(Name, Type, field) \
    ced__values[ced__plan->_impl._columns[ced__bundle_index(Name, Type)]] = &ced__value->field;
#define ced__bundle_values_3(Name, Type, unused, tag)
#define ced__bundle_packed(Name, tuple) ced__bundle_apply(ced__bundle_packed_, Name, tuple)
#define ced__bundle_packed_2(Name, Type, field) \
    { ced_Component ced__c = ced__plan->_impl._columns[ced__bundle_index(Name, Type)]; \
      memmove(ced__row.columns[ced__c].data + ced__row.row * sizeof(Type), &ced__value->field, sizeof(Type)); }
#define ced__bundle_packed_3(Name, Type, unused, tag)
#define ced__bundle_indexed(Name, tuple) ced__bundle_apply(ced__bundle_indexed_, Name, tuple)
#define ced__bundle_indexed_2(Name, Type, field) \
    { ced_Component ced__c = ced__plan->_impl._columns[ced__bundle_index(Name, Type)]; \
      ((ced_Entity *)ced__row.owners[ced__c])[(uint32_t)ced__row.entity] = ced__row.entity; \
      memmove(ced__row.columns[ced__c].data + ced__row.row * sizeof(Type), &ced__value->field, sizeof(Type)); }
#define ced__bundle_indexed_3(Name, Type, unused, tag) \
    { ced_Component ced__c = ced__plan->_impl._columns[ced__bundle_index(Name, Type)]; \
      ((ced_Entity *)ced__row.owners[ced__c])[(uint32_t)ced__row.entity] = ced__row.entity; }
#define ced_define_bundle(Name, ...) \
    _Static_assert((0 ced__api_each(ced__bundle_count, Name, __VA_ARGS__)) > 0, \
                   "A named bundle needs a data field; use spawn plans for tags only"); \
    typedef struct Name { ced__api_each(ced__bundle_decl, Name, __VA_ARGS__) } Name; \
    typedef struct ced__cat(Name, _Plan) { ced_BundlePlan _impl; } ced__cat(Name, _Plan); \
    static ced__api_generated ced__cat(Name, _Plan) ced__cat(Name, _prepare)(ced_World *ced__api_world) { \
        return (ced__cat(Name, _Plan)){ced_bundle_plan_init(ced__api_world, sizeof(Name), \
            ced__argc(__VA_ARGS__), (const ced_BundleMember[]){ \
                ced__api_each(ced__bundle_member, Name, __VA_ARGS__)})}; \
    } \
    static ced__api_generated ced_Entity ced__cat(Name, _spawn)(ced_World *ced__api_world, \
        const ced__cat(Name, _Plan) *ced__plan, const Name *ced__value) { \
        enum { ced__api_each(ced__bundle_enum, Name, __VA_ARGS__) }; \
        ced__BundleRow ced__row; \
        int ced__status = ced__bundle_begin(ced__api_world, ced__plan ? &ced__plan->_impl : NULL, ced__value, &ced__row); \
        if (ced__status < 0) return ced_entity_none; \
        if (!ced__status) { \
            const void *ced__values[ced__argc(__VA_ARGS__)] = {0}; \
            ced__api_each(ced__bundle_values, Name, __VA_ARGS__) \
            return ced_entity_from_plan_values(ced__api_world, &ced__plan->_impl._spawn, ced__values); \
        } \
        if (ced__row.indexed) { ced__api_each(ced__bundle_indexed, Name, __VA_ARGS__) } \
        else { ced__api_each(ced__bundle_packed, Name, __VA_ARGS__) } \
        return ced__bundle_finish(&ced__row); \
    } \
    static ced__api_generated bool ced__cat(Name, _valid)(const ced_World *ced__api_world, \
        const ced__cat(Name, _Plan) *ced__plan) { \
        return ced_bundle_plan_valid(ced__api_world, ced__plan ? &ced__plan->_impl : NULL); \
    } \
    static ced__api_generated bool ced__cat(Name, _reserve)(ced_World *ced__api_world, \
        const ced__cat(Name, _Plan) *ced__plan, size_t ced__capacity) { \
        return ced_reserve_bundle(ced__api_world, ced__plan ? &ced__plan->_impl : NULL, ced__capacity); \
    }

#endif
