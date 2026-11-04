#include "internal.h"

static atomic_uint_fast64_t ced_world_sequence = 1;

static ced_World *ced_init_storage(bool indexed) {
	ced_Allocation *allocation = ced_xmalloc(sizeof(*allocation));
    // Unused entity slots are never read: fresh creation initializes a full
    // record before publication. Keep metadata zeroed without clearing the
    // entire inline record reserve on every short-lived world construction.
    memset(&allocation->world, 0, sizeof(allocation->world));
    ced_Runtime *runtime=&allocation->runtime;
    size_t records_begin=offsetof(ced_Runtime,inline_records);
    size_t records_end=records_begin+sizeof(runtime->inline_records);
    memset(runtime,0,records_begin);
    memset((unsigned char *)runtime+records_end,0,sizeof(*runtime)-records_end);
	ced_World *w = &allocation->world;
	w->indexed = indexed;
	w->identity =
		atomic_fetch_add_explicit(&ced_world_sequence, 1, memory_order_relaxed);
	if (!w->identity)
		abort();
	w->_runtime = &allocation->runtime;
	w->_runtime->empty_owner = UINT64_MAX;
	w->components = w->_runtime->inline_component_ptrs;
	w->component_capacity = ced_max_components < 8 ? ced_max_components : 8;
	w->archetypes = w->_runtime->inline_archetypes;
	w->archetype_capacity = 8;
	ced_mutex_init(&w->_runtime->mutex);
	atomic_init(&w->_runtime->parallel, false);
	atomic_init(&w->_runtime->has_buffers, false);
	atomic_init(&w->_runtime->indexed_entities_ready, false);
	atomic_init(&w->ecs_quit, false);
	ced_reserve_entities(w, 256);
	ced_archetype_new(w, NULL, 0);
	w->start = ced_start;
	w->update = ced_default_update;
#ifdef USE_FPS_DELAY
	w->targetFPS = 60.0f;
#endif
	return w;
}

ced_World *ced_init(void) { return ced_init_storage(false); }
ced_World *ced_init_indexed(void) { return ced_init_storage(true); }

void ced_free(ced_World *w) {
	if (!ced_mutable(w))
		return;
	// The caller must have joined external producers before dropping a world.
	ced_workers_free(w);
	while (w->_views)
		ced_view_free(w->_views);
	ced_buffers_free(w);
	for (size_t i = 0; i < w->archetype_count; i++)
		ced_archetype_free(w->archetypes[i]);
	ced_NameMap_free(&w->_component_names);
	for (size_t i = 0; i < w->component_count; i++) {
        ced_indexed_access_invalidate(&w->components[i]->indexed);
		if (w->components[i]->name && w->components[i]->name != w->components[i]->_short_name)
			free(w->components[i]->name);
		if (w->components[i]->indexed.data)
			ced_storage_free(w->components[i]->indexed.data, ced_bytes(w->components[i]->indexed.capacity, w->components[i]->size));
		if (w->components[i]->indexed.owners &&
		    w->components[i]->indexed.owners != &w->_runtime->empty_owner)
			ced_storage_free(w->components[i]->indexed.owners, ced_indexed_owner_bytes(w->components[i], w->components[i]->indexed.capacity));
		if (i >= 4) free(w->components[i]);
	}
	if (w->components != w->_runtime->inline_component_ptrs) free(w->components);
	ced_storage_free(w->_indexed_entities, ced_bytes(w->entity_capacity, sizeof(*w->_indexed_entities)));
	if (w->entities != w->_runtime->inline_records) {
        if (ced_records_use_native(w->entity_capacity)) ced_aligned_free(w->entities);
        else ced_storage_free(w->entities, ced_bytes(w->entity_capacity, sizeof(*w->entities)));
    }
	free(w->free_list);
	if (w->archetypes != w->_runtime->inline_archetypes)
		free(w->archetypes);
	ced_ArchetypeMap_free(&w->_archetype_index);
	free(w->start_systems);
	free(w->update_systems);
	free(w->fixed_systems);
	ced_mutex_free(&w->_runtime->mutex);
	free(w);
}

ced_ReadAccess ced_read_access(const ced_World *w) {
	return w ? (ced_ReadAccess){&w->entities, &w->entity_count, &w->component_count} :
		(ced_ReadAccess){0};
}

ced_Access ced_access(ced_World *w) { return (ced_Access){ced_read_access(w)}; }

bool ced_is_alive(const ced_World *w, ced_Entity entity) {
	return ced_record_const(w, entity) != NULL;
}

bool ced_reserve_entities(ced_World *w, size_t capacity) {
	if (!ced_mutable(w) || capacity > UINT32_MAX)
		return false;
	if (capacity <= w->entity_capacity)
		return true;
	size_t cap = ced_storage_capacity(w->entity_capacity, capacity);
	if (cap > UINT32_MAX)
		cap = capacity;
	size_t old_capacity = w->entity_capacity;
	if (!old_capacity && cap <= 256)
		w->entities = w->_runtime->inline_records;
	else {
        size_t bytes=ced_bytes(cap,sizeof(*w->entities));
        size_t live=ced_bytes(w->entity_count,sizeof(*w->entities));
        bool embedded=w->entities==w->_runtime->inline_records;
        bool native=ced_records_use_native(cap);
        bool old_native=ced_records_use_native(old_capacity);
        if(embedded || (old_native && !native)) {
            ced_Record *old=w->entities;
            w->entities=native ? ced_aligned_resize(NULL,0,bytes,_Alignof(ced_Record)) :
                ced_storage_resize(NULL,0,0,bytes,_Alignof(ced_Record));
            if(live)memcpy(w->entities,old,live);
            if(!embedded)ced_aligned_free(old);
        } else if(native) {
            w->entities=ced_aligned_resize(w->entities,live,bytes,_Alignof(ced_Record));
        } else {
            w->entities=ced_storage_resize(w->entities,ced_bytes(old_capacity,sizeof(*w->entities)),
                live,bytes,_Alignof(ced_Record));
        }
    }
	w->entity_capacity = cap;
	if (w->indexed)
		ced_indexed_grow(w, old_capacity);
	return true;
}

static ced_Entity ced_create_in(ced_World *w, ced_Archetype *arch,
                                const void *const *values) {
	uint32_t index;
	bool fresh = w->free_count == 0;
	if (!fresh)
		index = (uint32_t)w->free_list[--w->free_count];
	else {
		if (w->entity_count == UINT32_MAX)
			return ced_entity_none;
		if (w->entity_count == w->entity_capacity &&
			!ced_reserve_entities(w, w->entity_count + 1))
			return ced_entity_none;
		index = (uint32_t)w->entity_count++;
	}
	ced_Record *rec = &w->entities[index];
	uint32_t generation = fresh ? 0 : rec->generation;
	ced_Entity entity = ((uint64_t)generation << 32) | index;
	size_t row;
	if (w->indexed) {
		row = index;
		arch->count++;
		if (w->_indexed_entities) w->_indexed_entities[index] = entity;
		if (index >= arch->_initialized_rows) {
            for (size_t c=0;c<arch->type_count;c++) ced_indexed_reserve_slot(w, arch->type[c], index);
        }
		for (size_t c = 0; c < arch->type_count; c++) {
			size_t size = arch->columns[c].component_size;
            ced_indexed_cached_owner_add(w, arch->type[c], arch->_column_allocations[c], size, entity);
			if (!size) continue;
			void *out = arch->columns[c].data + row * size;
			if (values && values[c]) ced_copy_value(out, values[c], size);
			else ced_zero_value(out, size);
		}
	} else {
        bool dirty = !arch->_zero_tail || arch->count < arch->_initialized_rows;
		row = ced_append(w, arch, entity);
        if (!values) { if (dirty) ced_zero_row(arch, row); }
        else for (size_t c = 0; c < arch->type_count; c++) {
			size_t size = arch->columns[c].component_size;
			if (!size) continue;
			void *out = arch->columns[c].data + row * size;
			if (values[c]) ced_copy_value(out, values[c], size);
			else if (dirty) ced_zero_value(out, size);
		}
	}
	*rec = (ced_Record){arch, (uint32_t)row, generation};
	w->alive_count++;
	ced_changed(w);
	return entity;
}

ced_SpawnPlan ced_spawn_plan_init(ced_World *w, size_t count,
								  const ced_Component *ids) {
	if (!ced_mutable(w))
		return (ced_SpawnPlan){0};
	ced_Archetype *arch = ced_archetype_for(w, ids, count);
	return (ced_SpawnPlan){arch, w->identity};
}
ced_Entity ced_entity_from_plan(ced_World *w, const ced_SpawnPlan *plan) {
	if (!ced_mutable(w) || !plan || plan->world_identity != w->identity ||
		!plan->archetype)
		return ced_entity_none;
	return ced_create_in(w, plan->archetype, NULL);
}
ced_Entity ced_entity_from_plan_values(ced_World *w, const ced_SpawnPlan *plan,
                                      const void *const *values) {
	if (!ced_mutable(w) || !plan || plan->world_identity != w->identity ||
		!plan->archetype)
		return ced_entity_none;
	ced_Archetype *arch = plan->archetype;
	bool growing = w->indexed ? (!w->free_count && w->entity_count == w->entity_capacity)
	                         : arch->count == arch->capacity;
	if (w->indexed) {
        uint32_t next_index = w->free_count ? w->free_list[w->free_count - 1] : (uint32_t)w->entity_count;
		growing |= arch->type_count && next_index >= arch->_initialized_rows;
    }
	if (!values || !growing)
		return ced_create_in(w, arch, values);
	// A growth can relocate the source values. Capture before reallocating any
	// columns, including the pointer array if the caller stored it in a column.
	const void *local_ptrs[32];
	const void **ptrs = arch->type_count <= 32 ? local_ptrs :
		ced_xmalloc(ced_bytes(arch->type_count, sizeof(*ptrs)));
	size_t bytes = 0;
	for (size_t i = 0; i < arch->type_count; i++) {
		ptrs[i] = values[i];
		size_t size = ptrs[i] ? arch->columns[i].component_size : 0;
		if (size > SIZE_MAX - bytes)
			abort();
		bytes += size;
	}
	unsigned char local_data[256];
	unsigned char *data = bytes <= sizeof(local_data) ? local_data : ced_xmalloc(bytes);
	size_t offset = 0;
	for (size_t i = 0; i < arch->type_count; i++) {
		if (!ptrs[i])
			continue;
		size_t size = arch->columns[i].component_size;
		ced_copy_value(data + offset, ptrs[i], size);
		ptrs[i] = data + offset;
		offset += size;
	}
	ced_Entity entity = ced_create_in(w, arch, ptrs);
	if (data != local_data)
		free(data);
	if (ptrs != local_ptrs)
		free(ptrs);
	return entity;
}
bool ced_reserve_plan(ced_World *w, const ced_SpawnPlan *plan, size_t capacity) {
	if (!ced_mutable(w) || !plan || plan->world_identity != w->identity ||
		!plan->archetype || capacity > UINT32_MAX)
		return false;
	ced_archetype_reserve(w, plan->archetype, capacity);
	return true;
}
ced_Entity ced_entity_with_ids(ced_World *w, size_t count, const ced_Component *ids) {
	if (!ced_mutable(w))
		return ced_entity_none;
	ced_Archetype *arch = ced_archetype_for(w, ids, count);
	return arch ? ced_create_in(w, arch, NULL) : ced_entity_none;
}
ced_Entity ced_entity_init(ced_World *w) {
	if (!ced_mutable(w)) return ced_entity_none;
    uint32_t index;
    bool fresh=w->free_count==0;
    if (!fresh) index=w->free_list[--w->free_count];
    else {
        if(w->entity_count==UINT32_MAX) return ced_entity_none;
        if(w->entity_count==w->entity_capacity && !ced_reserve_entities(w,w->entity_count+1)) return ced_entity_none;
        index=(uint32_t)w->entity_count++;
    }
    ced_Record *rec=&w->entities[index];
    uint32_t generation=fresh?0:rec->generation;
    ced_Entity e=((uint64_t)generation<<32)|index;
    ced_Archetype *arch=w->archetypes[0];size_t row;
    if(w->indexed) {
        row=index;arch->count++;
        if(w->_indexed_entities)w->_indexed_entities[index]=e;
    } else row=ced_append(w,arch,e);
    *rec=(ced_Record){arch,(uint32_t)row,generation};
    w->alive_count++;ced_changed(w);return e;
}

void ced_entity_destroy(ced_World *w, ced_Entity entity) {
	if (!ced_mutable(w))
		return;
	ced_Record *rec = ced_record(w, entity);
	if (!rec)
		return;
	if (w->indexed) {
        for (size_t i = 0; i < rec->archetype->type_count; i++)
            ((ced_Entity *)rec->archetype->_column_allocations[i])[rec->row] = UINT64_MAX;
		rec->archetype->count--;
		if (w->_indexed_entities) w->_indexed_entities[rec->row] = ced_entity_none;
	} else {
		if ((size_t)rec->row + 1 == rec->archetype->count) rec->archetype->count--;
        else {
            ced_Entity moved = ced_archetype_swap_remove(rec->archetype, rec->row);
            if (moved != ced_entity_none) w->entities[ced_entity_index(moved)].row = rec->row;
        }
	}
	// ced_record already checked every generation bit against this handle.
	// Reuse that value across owner clearing instead of reloading a record that
	// the compiler must conservatively treat as aliased by the owner arrays.
	uint32_t generation = (uint32_t)(entity >> 32);
	uint32_t next_generation = generation == UINT32_MAX ? generation : generation + 1;
	*rec = (ced_Record){NULL, 0, next_generation};
	w->alive_count--;
	ced_changed(w);
	// Retire the slot instead of wrapping a generation and resurrecting handles.
	if (generation == UINT32_MAX)
		return;
	if (w->free_count == w->free_capacity) {
		w->free_capacity = ced_capacity(w->free_capacity, w->free_count + 1);
		w->free_list =
			ced_xrealloc(w->free_list, w->free_capacity, sizeof(*w->free_list));
	}
	w->free_list[w->free_count++] = ced_entity_index(entity);
}

bool ced_reserve_archetype(ced_World *w, size_t count, const ced_Component *ids,
						   size_t capacity) {
	if (!ced_mutable(w) || capacity > UINT32_MAX)
		return false;
	ced_Archetype *arch = ced_archetype_for(w, ids, count);
	if (!arch)
		return false;
	ced_archetype_reserve(w, arch, capacity);
	return true;
}

void ced_print_archs(ced_World *w) {
	if (!w)
		return;
	printf("=== Archetype Graph (%zu archetypes, %zu live entities) ===\n",
		   w->archetype_count, w->alive_count);
	for (size_t i = 0; i < w->archetype_count; i++) {
		ced_Archetype *a = w->archetypes[i];
		printf("Archetype %u: [", a->id);
		for (size_t j = 0; j < a->type_count; j++) {
			const char *name = ced_component_name(w, a->type[j]);
			if (j)
				printf(", ");
			if (name)
				printf("%s", name);
			else
				printf("%u", a->type[j]);
		}
		printf("] - %zu entities (cap %zu)\n", a->count, a->capacity);
		kama_foreach (&a->edges, j) {
			ced_ArchetypeEdge *e = ced_EdgeMap_val(&a->edges, j);
			if (e->add)
				printf("  +%u -> arch %u\n", e->component, e->add->id);
			if (e->remove)
				printf("  -%u -> arch %u\n", e->component, e->remove->id);
		}
	}
}
