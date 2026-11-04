#include "internal.h"

ced_IndexedReader ced_indexed_reader(const ced_World *w, ced_Component c) {
    return ced_valid_component(w, c) && w->indexed ?
        (ced_IndexedReader){&w->components[c]->indexed, w->components[c]->size} :
        (ced_IndexedReader){0};
}


static void ced_indexed_reserve_to(ced_World *w, ced_Component c, size_t needed) {
    ced_ComponentInfo *info = w->components[c];
    ced_IndexedColumn *column = &info->indexed;
    if (column->capacity >= needed)
        return;
    size_t cap = ced_storage_capacity(column->capacity, needed);
    if (cap > w->entity_capacity) cap = w->entity_capacity;
    if (cap < needed) abort();
    ced_lock(&w->_runtime->mutex);
    column->data = ced_storage_resize(column->data, ced_bytes(column->capacity, info->size),
        ced_bytes(column->capacity, info->size), ced_bytes(cap, info->size), info->alignment);
    void *old_owners = column->owners == &w->_runtime->empty_owner ? NULL : column->owners;
    size_t old_bytes = old_owners ? ced_indexed_owner_bytes(info, column->capacity) : 0;
    size_t new_bytes = ced_indexed_owner_bytes(info, cap);
    column->owners = ced_storage_resize(old_owners, old_bytes, old_bytes, new_bytes, _Alignof(ced_Entity));
    memset(column->owners + column->capacity, 0xff,
           ced_bytes(cap - column->capacity, sizeof(ced_Entity)));
    column->capacity = cap;
    for (ced_IndexedAccess *a = column->_accesses; a; a = a->_next) {
        a->data = column->data; a->owners = column->owners; a->capacity = cap;
    }
    ced_unlock(&w->_runtime->mutex);
    // Archetype metadata borrows the per-component indexed allocation.
    for (size_t i = 0; i < w->archetype_count; i++) {
        ced_Archetype *arch = w->archetypes[i];
        ced_Component col = arch->column_for[c];
        if (col != ced_component_none) {
            arch->columns[col].data = column->data;
            arch->_column_allocations[col] = column->owners;
            arch->_initialized_rows = ced_indexed_row_capacity(w, arch);
        }
    }
    ced_changed(w);
}
void ced_indexed_reserve(ced_World *w, ced_Component c) {
    ced_indexed_reserve_to(w, c, w->entity_capacity);
}
void ced_indexed_reserve_slot(ced_World *w, ced_Component c, uint32_t index) {
    // Keep the small-component path and explicit reserve behavior unchanged.
    // Large columns need only cover their highest published slot.
    size_t needed = w->components[c]->size < ced_indexed_partial_column_min_size ?
        w->entity_capacity : (size_t)index + 1;
    ced_indexed_reserve_to(w, c, needed);
}

void ced_indexed_grow(ced_World *w, size_t old_capacity) {
    if (w->_indexed_entities) {
        w->_indexed_entities = ced_storage_resize(w->_indexed_entities,
            ced_bytes(old_capacity, sizeof(*w->_indexed_entities)), ced_bytes(old_capacity, sizeof(*w->_indexed_entities)),
            ced_bytes(w->entity_capacity, sizeof(*w->_indexed_entities)), _Alignof(ced_Entity));
        memset(w->_indexed_entities + old_capacity, 0xff,
               ced_bytes(w->entity_capacity - old_capacity, sizeof(*w->_indexed_entities)));
    }
    for (size_t c = 0; c < w->component_count; c++)
        if (w->components[c]->indexed.capacity &&
            w->components[c]->size < ced_indexed_partial_column_min_size)
            ced_indexed_reserve(w, (ced_Component)c);
    for (size_t i = 0; i < w->archetype_count; i++) {
        w->archetypes[i]->capacity = w->entity_capacity;
        w->archetypes[i]->_initialized_rows = ced_indexed_row_capacity(w, w->archetypes[i]);
    }
    // Queries also borrow the materialized entity list. Its relocation must
    // invalidate cached batches even when no component column needed growth.
    ced_changed(w);
}
void ced_indexed_materialize_entities(ced_World *w) {
    if (atomic_load_explicit(&w->_runtime->indexed_entities_ready, memory_order_acquire)) return;
    ced_lock(&w->_runtime->mutex);
    if (atomic_load_explicit(&w->_runtime->indexed_entities_ready, memory_order_relaxed)) {
        ced_unlock(&w->_runtime->mutex);
        return;
    }
    w->_indexed_entities = ced_storage_resize(NULL, 0, 0,
        ced_bytes(w->entity_capacity, sizeof(*w->_indexed_entities)), _Alignof(ced_Entity));
    for (size_t i=0;i<w->entity_count;i++) {
        const ced_Record *r=&w->entities[i];
        w->_indexed_entities[i]=r->archetype ? ((uint64_t)r->generation << 32) | i : ced_entity_none;
    }
    memset(w->_indexed_entities+w->entity_count,0xff,
        ced_bytes(w->entity_capacity-w->entity_count,sizeof(*w->_indexed_entities)));
    atomic_store_explicit(&w->_runtime->indexed_entities_ready, true, memory_order_release);
    ced_unlock(&w->_runtime->mutex);
}

void ced_indexed_move(ced_World *w, ced_Entity entity, ced_Record *record,
                      ced_Archetype *dst, ced_Component component) {
    ced_Archetype *src = record->archetype;
    size_t index = record->row;
    ced_Component from = src->column_for[component];
    if (from != ced_component_none) {
        // Single-component removal: all shared data stays in its existing slot.
        ced_indexed_cached_owner_remove(w, component, src->_column_allocations[from], src->columns[from].component_size, entity);
    } else {
        // Every other destination column already belongs to the live source
        // signature. Prepare only this newly added component, once per edge.
        if (index >= w->components[component]->indexed.capacity)
            ced_indexed_reserve_slot(w, component, (uint32_t)index);
        ced_Component to = dst->column_for[component];
        size_t size = dst->columns[to].component_size;
        ced_indexed_cached_owner_add(w, component, dst->_column_allocations[to], size, entity);
        if (size) ced_zero_value(dst->columns[to].data + index * size, size);
    }
    src->count--; dst->count++;
    record->archetype = dst;
    ced_changed(w);
}


ced_IndexedWriter ced_indexed_write_begin(ced_World *w) {
    if (!ced_mutable(w) || !w->indexed)
        return (ced_IndexedWriter){0};
    if (w->_runtime->indexed_writers == SIZE_MAX)
        abort();
    w->_runtime->indexed_writers++;
    return (ced_IndexedWriter){w, ced_read_access(w), &w->storage_revision, true};
}
bool ced_indexed_write_end(ced_IndexedWriter *writer) {
    if (!writer || !writer->active)
        return false;
    writer->active = false;
    writer->world->_runtime->indexed_writers--;
    return true;
}
ced_IndexedBinding ced_indexed_bind(ced_IndexedWriter *writer, ced_Component c) {
    if (!writer || !writer->active || !ced_valid_component(writer->world, c))
        return (ced_IndexedBinding){0};
    return (ced_IndexedBinding){.writer = writer, .reader = ced_indexed_reader(writer->world, c),
                                .component = c};
}
static void ced_indexed_bind_pair(ced_IndexedBinding *b, ced_Archetype *with,
                                  ced_Archetype *without) {
    if (!b->reader.column->capacity) {
        b->with_component = b->without_component = NULL;
        b->with_count = b->without_count = NULL;
        return;
    }
    b->with_component = with; b->without_component = without;
    b->with_count = &with->count; b->without_count = &without->count;
}
bool ced_indexed_prepare(ced_IndexedBinding *b) {
    if (!b || !b->writer || !b->writer->active || !b->reader.column) return false;
    ced_World *w = b->writer->world;
    if (!ced_mutable(w) || !w->indexed || !ced_valid_component(w, b->component)) return false;
    ced_Archetype *empty = w->archetypes[0];
    ced_Archetype *with = ced_archetype_with_added(w, empty, b->component);
    if (!with) return false;
    ced_indexed_reserve(w, b->component);
    ced_indexed_bind_pair(b, with, empty);
    return true;
}
void *ced_indexed_add_cold(ced_IndexedBinding *b, ced_Entity entity) {
    ced_World *w = b->writer->world;
    ced_Record *rec = ced_record(w, entity);
    ced_Archetype *with = rec->archetype, *without;
    if (with->column_for[b->component] == ced_component_none) {
        without = with;
        ced_add_component(w, entity, b->component);
        with = rec->archetype;
    } else
        without = ced_archetype_with_removed(w, with, b->component);
    ced_indexed_bind_pair(b, with, without);
    return b->reader.size ? w->components[b->component]->indexed.data +
        (size_t)(uint32_t)entity * b->reader.size : NULL;
}
void ced_indexed_remove_cold(ced_IndexedBinding *b, ced_Entity entity) {
    ced_World *w = b->writer->world;
    ced_Record *rec = ced_record(w, entity);
    ced_Archetype *without = rec->archetype, *with;
    if (without->column_for[b->component] != ced_component_none) {
        with = without;
        ced_remove_component(w, entity, b->component);
        without = rec->archetype;
    } else
        with = ced_archetype_with_added(w, without, b->component);
    ced_indexed_bind_pair(b, with, without);
}



static const ced_Entity ced_empty_owner = UINT64_MAX;
static void ced_indexed_access_empty(ced_IndexedAccess *a) {
    *a = (ced_IndexedAccess){.owners = &ced_empty_owner};
}
void ced_indexed_access_init(ced_IndexedAccess *a, ced_IndexedReader reader) {
    if (!a) return;
    ced_indexed_access_reset(a);
    if (!reader.column) return;
    // Metadata from indexed_reader is world-owned mutable storage.
    ced_IndexedColumn *c = (ced_IndexedColumn *)reader.column;
    if (c->_world) ced_lock(&c->_world->_runtime->mutex);
    a->data = c->data; a->owners = c->owners; a->capacity = c->capacity;
    if (!a->owners) a->owners = &ced_empty_owner;
    if (c->_world) {
        a->_source = c; a->_prev = NULL; a->_next = c->_accesses;
        if (a->_next) a->_next->_prev = a;
        c->_accesses = a;
        ced_unlock(&c->_world->_runtime->mutex);
    }
}
void ced_indexed_access_reset(ced_IndexedAccess *a) {
    if (!a) return;
    if (a->_source) {
        ced_IndexedColumn *c = (ced_IndexedColumn *)a->_source;
        ced_lock(&c->_world->_runtime->mutex);
        if (a->_prev) a->_prev->_next = a->_next;
        else c->_accesses = a->_next;
        if (a->_next) a->_next->_prev = a->_prev;
        ced_unlock(&c->_world->_runtime->mutex);
    }
    ced_indexed_access_empty(a);
}
void ced_indexed_access_copy(ced_IndexedAccess *a, const ced_IndexedAccess *source) {
    if (!a || a == source) return;
    if (source && source->_source) ced_indexed_access_init(a, (ced_IndexedReader){source->_source, 0});
    else {
        ced_indexed_access_reset(a);
        if (source) { a->data = source->data; a->owners = source->owners ? source->owners : &ced_empty_owner; a->capacity = source->capacity; }
    }
}
void ced_indexed_access_invalidate(ced_IndexedColumn *c) {
    while (c->_accesses) {
        ced_IndexedAccess *a = c->_accesses;
        c->_accesses = a->_next;
        ced_indexed_access_empty(a);
    }
}
