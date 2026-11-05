#include "internal.h"

static bool ced_construct_component(ced_World *w, ced_Component hint, const char *name,
                                    size_t size, size_t alignment, ced_Component *out) {
    ced_Component c = name ? ced_component_lookup(w, hint, name) : hint;
    if (!ced_valid_component(w, c) || w->components[c]->size != size ||
        w->components[c]->alignment != alignment) return false;
    *out = c;
    return true;
}

ced_Entity ced_entity_with_values(ced_World *w, size_t count,
                                  const ced_ComponentValue *values) {
    if (!ced_mutable(w) || count > ced_bundle_max_members || (count && !values))
        return ced_entity_none;
    ced_Component ids[ced_bundle_max_members];
    const void *ordered[ced_bundle_max_members];
    for (size_t i = 0; i < count; ++i) {
        ced_Component c;
        if ((values[i].size && !values[i].value) ||
            !ced_construct_component(w, values[i].component, values[i].name,
                values[i].size, values[i].alignment, &c)) return ced_entity_none;
        // Sort paired IDs/values before constructing the final signature.
        size_t j = i;
        while (j && ids[j - 1] > c) {
            ids[j] = ids[j - 1]; ordered[j] = ordered[j - 1]; --j;
        }
        if (j && ids[j - 1] == c) return ced_entity_none;
        ids[j] = c; ordered[j] = values[i].value;
    }
    // Creating archetype metadata does not relocate existing component data.
    // Insertion saves input values before any storage growth.
    ced_SpawnPlan plan = ced_spawn_plan_init(w, count, ids);
    return ced_entity_from_plan_values(w, &plan, ordered);
}

ced_BundlePlan ced_bundle_plan_init(ced_World *w, size_t size, size_t count,
                                    const ced_BundleMember *members) {
    ced_BundlePlan result = {0};
    if (!ced_mutable(w) || count > ced_bundle_max_members || (count && !members))
        return result;
    ced_Component ids[ced_bundle_max_members];
    ced_Component original[ced_bundle_max_members];
    for (size_t i = 0; i < count; ++i) {
        ced_Component c;
        if (members[i].offset > size || members[i].size > size - members[i].offset ||
            !ced_construct_component(w, members[i].component, members[i].name,
                members[i].size, members[i].alignment, &c)) return (ced_BundlePlan){0};
        original[i] = c;
        size_t j = i;
        while (j && ids[j - 1] > c) {
            ids[j] = ids[j - 1]; result._offsets[j] = result._offsets[j - 1]; --j;
        }
        if (j && ids[j - 1] == c) return (ced_BundlePlan){0};
        ids[j] = c; result._offsets[j] = members[i].offset;
    }
    result._spawn = ced_spawn_plan_init(w, count, ids);
    result._size = size;
    if (result._spawn.archetype) {
        result._parallel = &w->_runtime->parallel;
        for (size_t i = 0; i < count; ++i)
            result._columns[i] = result._spawn.archetype->column_for[original[i]];
    }
    return result;
}

bool ced_reserve_bundle(ced_World *w, const ced_BundlePlan *plan, size_t capacity) {
    return plan && ced_reserve_plan(w, &plan->_spawn, capacity);
}

ced_Entity ced_entity_from_bundle(ced_World *w, const ced_BundlePlan *plan,
                                   const void *value) {
    if (!ced_mutable(w) || !plan || plan->_spawn.world_identity != w->identity ||
        !plan->_spawn.archetype || (!value && plan->_size))
        return ced_entity_none;
    bool fresh = w->free_count == 0;
    if (fresh && w->entity_count == UINT32_MAX) return ced_entity_none;
    uint32_t index = fresh ? (uint32_t)w->entity_count : w->free_list[w->free_count - 1];
    ced_Archetype *arch = plan->_spawn.archetype;
    const bool indexed = w->indexed;
    bool growing = (fresh && w->entity_count == w->entity_capacity) ||
        (indexed ? (arch->type_count && index >= arch->_initialized_rows) :
                      arch->count == arch->capacity);
    if (growing) {
        const void *values[ced_bundle_max_members];
        for (size_t c = 0; c < arch->type_count; ++c)
            values[c] = arch->columns[c].component_size ?
                (const unsigned char *)value + plan->_offsets[c] : NULL;
        return ced_entity_from_plan_values(w, &plan->_spawn, values);
    }
    // Prepared storage accepts a direct copy from the typed object. Supplied
    // values need no per-entity pointer array, setters or zeroing.
    ced_Record *record = &w->entities[index];
    uint32_t generation = fresh ? 0 : record->generation;
    ced_Entity entity = ((uint64_t)generation << 32) | index;
    // These metadata arrays cannot relocate on the prepared path. Capture
    // their bases before stores through other world/column pointers.
    ced_Column *columns = arch->columns;
    const unsigned char *source = value;
    const size_t *offsets = plan->_offsets;
    if (fresh) w->entity_count++; else w->free_count--;
    size_t row;
    if (indexed) {
        void *const *owners = arch->_column_allocations;
        row = index;
        arch->count++;
        if (w->_indexed_entities) w->_indexed_entities[index] = entity;
        for (size_t c = 0; c < arch->type_count; ++c) {
            size_t bytes = columns[c].component_size;
            ced_indexed_cached_owner_add(w, arch->type[c], owners[c], bytes, entity);
            if (bytes) ced_copy_value(columns[c].data + row * bytes, source + offsets[c], bytes);
        }
    } else {
        row = ced_append(w, arch, entity);
        for (size_t c = 0; c < arch->type_count; ++c) {
            size_t bytes = columns[c].component_size;
            if (bytes) ced_copy_value(columns[c].data + row * bytes, source + offsets[c], bytes);
        }
    }
    *record = (ced_Record){arch, (uint32_t)row, generation};
    w->alive_count++;
    ced_changed(w);
    return entity;
}
