#include "internal.h"

static int ced_component_compare(const void *a, const void *b) {
	return (int)*(const ced_Component *)a - (int)*(const ced_Component *)b;
}

ced_Archetype *ced_archetype_new(ced_World *w, const ced_Component *type,
								 size_t count) {
	bool embedded = w->archetype_count == 0 && count == 0;
	// A small signature can stage its first row in the metadata allocation.
	// Scalar add/remove chains often leave these signatures empty thereafter.
	// A second row promotes to ordinary colored SoA buffers once.
	size_t inline_bytes=0;
	bool small=!w->indexed && count && ced_align<=256;
	for(size_t i=0;small && i<count;i++) {
		size_t size=w->components[type[i]]->size;
		if(w->components[type[i]]->alignment>ced_align || size>256) {small=false;break;}
		size_t padded=(size+ced_align-1)&~(size_t)(ced_align-1);
		if(padded>256-inline_bytes) {small=false;break;}
		inline_bytes+=padded;
	}
	size_t type_bytes=ced_bytes(count,sizeof(*type));
	size_t column_bytes=ced_bytes(count,sizeof(ced_Column)+sizeof(void *));
	size_t alignment=_Alignof(max_align_t);
	if(type_bytes>SIZE_MAX-(alignment-1) || column_bytes>SIZE_MAX-type_bytes-(alignment-1))abort();
	size_t extra=count ? type_bytes+alignment-1+column_bytes : 0;
	size_t row_extra=small ? sizeof(ced_Entity)+_Alignof(ced_Entity)-1+ced_align-1+inline_bytes : 0;
	if(row_extra>SIZE_MAX-extra || sizeof(ced_Archetype)>SIZE_MAX-extra-row_extra)abort();
	extra+=row_extra;
	ced_Archetype *arch = embedded ? &w->_runtime->inline_empty : ced_xcalloc(1, sizeof(*arch)+extra);
	arch->_inline_rows=small;
	arch->_embedded = embedded;
	if (w->archetype_count > UINT32_MAX)
		abort();
	arch->id = (ced_ArchetypeId)w->archetype_count;
	arch->indexed = w->indexed;
	if (w->indexed)
		arch->capacity = w->entity_capacity;
	arch->type_count = count;
	// Immutable metadata shares one allocation, with stable, properly aligned
	// subarrays. Mutable component/owner storage remains independently owned.
	arch->type = count ? (ced_Component *)(void *)(arch+1) : NULL;
	if (count)
		memcpy(arch->type, type, count * sizeof(*type));
	if (count > 1)
		qsort(arch->type, count, sizeof(*type), ced_component_compare);
	if(count) {
		uintptr_t next=((uintptr_t)arch->type+type_bytes+alignment-1)&~(uintptr_t)(alignment-1);
		arch->columns=(ced_Column *)next;
		arch->_column_allocations=(void **)(void *)(arch->columns+count);
	}
	arch->access = (ced_ArchetypeAccess){arch->columns, arch->column_for};
	memset(arch->column_for, 0xff, sizeof(arch->column_for));
	size_t data_columns = 0;
	for (size_t i = 0; i < count; i++) {
		ced_Component c = arch->type[i];
        if(CEDARECS_VM_STORAGE && !w->indexed && w->components[c]->size>=128)arch->_bounded_rows=true;
        data_columns += w->components[c]->size != 0;
		arch->columns[i].component_size = w->components[c]->size;
		if (!i) arch->_uniform_size = w->components[c]->size;
		else if (arch->_uniform_size != w->components[c]->size) arch->_uniform_size = SIZE_MAX;
		if (w->indexed) {
			arch->columns[i].data = w->components[c]->indexed.data;
            arch->_column_allocations[i] = w->components[c]->indexed.owners;
        }
		arch->column_for[c] = (ced_Component)i;
		ced_mask_set(&arch->mask, c);
	}
    arch->_zero_tail = data_columns >= 8;
    if (w->indexed) arch->_initialized_rows = ced_indexed_row_capacity(w, arch);
	if (w->archetype_count == w->archetype_capacity) {
		w->archetype_capacity =
			ced_capacity(w->archetype_capacity, w->archetype_count + 1);
		if (w->archetypes == w->_runtime->inline_archetypes) {
			w->archetypes = ced_xmalloc(ced_bytes(w->archetype_capacity, sizeof(*w->archetypes)));
			memcpy(w->archetypes, w->_runtime->inline_archetypes,
			       ced_bytes(w->archetype_count, sizeof(*w->archetypes)));
		} else
			w->archetypes = ced_xrealloc(w->archetypes, w->archetype_capacity, sizeof(*w->archetypes));
	}
	w->archetypes[w->archetype_count++] = arch;
	// The embedded empty signature has a permanent slot. It does not need a
	// second allocation in the signature index, especially for empty worlds.
	if (!embedded && ced_ArchetypeMap_put(&w->_archetype_index, &arch->mask, arch) == KAMA_ERROR)
		abort();
	ced_changed(w);
	return arch;
}

ced_Archetype *ced_find_archetype(ced_World *w, const ced_Component *type,
								  size_t count) {
	if (!count) return w->archetypes[0];
	ced_Mask mask = {0};
	for (size_t i = 0; i < count; i++)
		ced_mask_set(&mask, type[i]);
	ced_Archetype *arch = NULL;
	ced_ArchetypeMap_get(&w->_archetype_index, &mask, &arch);
	return arch;
}

ced_Archetype *ced_archetype_for(ced_World *w, const ced_Component *type,
								 size_t count) {
	if (count > w->component_count || (count && !type))
		return NULL;
	if (!count) return w->archetypes[0];
	ced_Mask mask = {0};
	for (size_t i = 0; i < count; i++) {
		if (!ced_valid_component(w, type[i]) || ced_mask_has(&mask, type[i]))
			return NULL;
		ced_mask_set(&mask, type[i]);
	}
	ced_Archetype *arch = NULL;
	ced_ArchetypeMap_get(&w->_archetype_index, &mask, &arch);
	return arch ? arch : ced_archetype_new(w, type, count);
}

// Deferred runs already have a validated signature; avoid rebuilding its mask.
ced_Archetype *ced_archetype_for_mask(ced_World *w, const ced_Mask *mask) {
	bool empty = true;
	for (size_t i = 0; i < ced_mask_words; i++) empty &= mask->bits[i] == 0;
	if (empty) return w->archetypes[0];
	ced_Archetype *arch = NULL;
	if (ced_ArchetypeMap_get(&w->_archetype_index, mask, &arch))
		return arch;
	ced_Component local[64];
	ced_Component *ids = w->component_count <= 64 ? local :
		ced_xmalloc(ced_bytes(w->component_count, sizeof(*ids)));
	size_t count = 0;
	for (size_t c = 0; c < w->component_count; c++)
		if (ced_mask_has(mask, (ced_Component)c))
			ids[count++] = (ced_Component)c;
	arch = ced_archetype_new(w, ids, count);
	if (ids != local)
		free(ids);
	return arch;
}

void ced_archetype_reserve(ced_World *w, ced_Archetype *arch, size_t needed) {
	if (w->indexed) {
		ced_reserve_entities(w, needed);
		for (size_t i = 0; i < arch->type_count; i++)
			ced_indexed_reserve(w, arch->type[i]);
		arch->_indexed_ready=true;
		arch->_initialized_rows=w->entity_capacity;
		return;
	}
	if (needed <= arch->capacity)
		return;
	if(arch->_inline_rows && !arch->capacity && needed==1) {
		uintptr_t row=((uintptr_t)(arch->_column_allocations+arch->type_count)+_Alignof(ced_Entity)-1)&~(uintptr_t)(_Alignof(ced_Entity)-1);
		arch->entities=(ced_Entity *)row;
		uintptr_t start=((uintptr_t)(arch->entities+1)+ced_align-1)&~(uintptr_t)(ced_align-1);
		for(size_t c=0;c<arch->type_count;c++) {
			size_t size=arch->columns[c].component_size;
			if(!size)continue;
			arch->columns[c].data=(unsigned char *)start;
			arch->_column_allocations[c]=(void *)start;
			start+=(size+ced_align-1)&~(size_t)(ced_align-1);
		}
		arch->capacity=1;
		arch->_initialized_rows=arch->_zero_tail ? arch->count : 1;
		ced_changed(w);return;
	}
	bool was_inline=arch->_inline_rows;
	size_t row_bytes = sizeof(ced_Entity);
	for (size_t i = 0; i < arch->type_count; i++) {
		size_t size = arch->columns[i].component_size;
		if (size > SIZE_MAX - row_bytes)
			abort();
		row_bytes += size;
	}
	size_t initial = ced_block_size / row_bytes;
	size_t power = 1;
	while (power <= initial / 2)
		power *= 2;
	initial = power;
	size_t cap =
		ced_storage_capacity(arch->capacity && !was_inline ? arch->capacity : (initial ? initial : 1), needed);
    if(arch->_bounded_rows && needed>16384) {
        cap=needed>SIZE_MAX-4095 ? needed : (needed+4095)&~(size_t)4095;
    }
	const ced_Entity *old_entities=arch->entities;
	arch->entities = ced_storage_resize(was_inline ? NULL : arch->entities,
        was_inline ? 0 : ced_archetype_storage_bytes(arch,arch->capacity,sizeof(*arch->entities),0),
        was_inline ? 0 : ced_bytes(arch->count, sizeof(*arch->entities)),
        ced_archetype_storage_bytes(arch,cap,sizeof(*arch->entities),0), _Alignof(ced_Entity));
	if(was_inline && arch->count)memcpy(arch->entities,old_entities,ced_bytes(arch->count,sizeof(*arch->entities)));
	bool retained_dirty=false;
	for (size_t c = 0; c < arch->type_count; c++) {
		ced_Column *column = &arch->columns[c];
		size_t size = column->component_size;
        if (!size) continue;
        size_t alignment = w->components[arch->type[c]]->alignment;
        if (alignment < ced_align) alignment = ced_align;
#if defined(__APPLE__) && defined(__aarch64__)
        size_t spacing = 128;
#else
        size_t spacing = 64;
#endif
        // Offset each SoA column by a cache line, avoiding congruent starts
        // for equally-sized power-of-two allocations. Keep the true base for
        // realloc/free; component alignment is still honored.
        size_t prefix = (c & 31) * spacing;
        prefix = (prefix + alignment - 1) & ~(alignment - 1);
        size_t payload = ced_bytes(cap, size), live = ced_bytes(arch->count, size);
        if (prefix > SIZE_MAX - payload) abort();
        void *old = arch->_column_allocations[c];
        size_t old_bytes = old && !was_inline ? ced_archetype_storage_bytes(arch,arch->capacity,size,prefix) : 0;
        size_t live_bytes = old && !was_inline ? prefix + live : 0;
        size_t new_bytes=ced_archetype_storage_bytes(arch,cap,size,prefix);
        void *base=old;
        retained_dirty |= !was_inline && new_bytes==old_bytes && arch->count<arch->_initialized_rows;
        // The unused backing tail has never been published as live rows.
        // Keep it when only logical capacity changes, avoiding repeated clears.
        if(was_inline || new_bytes!=old_bytes)base = arch->_zero_tail ?
            ced_storage_resize_zero(was_inline ? NULL : old, old_bytes, live_bytes, new_bytes, alignment) :
            ced_storage_resize(was_inline ? NULL : old, old_bytes, live_bytes, new_bytes, alignment);
        if(was_inline && live)memcpy((unsigned char *)base+prefix,column->data,live);
        arch->_column_allocations[c] = base;
        column->data = (unsigned char *)base + prefix;
	}
	arch->capacity = cap;
	arch->_inline_rows=false;
    arch->_initialized_rows = arch->_zero_tail ? (retained_dirty ? arch->_initialized_rows : arch->count) : cap;
	ced_changed(w);
}

ced_ArchetypeEdge *ced_edge(ced_Archetype *arch, ced_Component c, bool create) {
	ced_ArchetypeEdge *edge = arch->_edge_cache[c & 7];
	if (edge && edge->component == c)
		return edge;
	if (ced_EdgeMap_get(&arch->edges, c, &edge)) {
		arch->_edge_cache[c & 7] = edge;
		return edge;
	}
	if (!create)
		return NULL;
	edge = ced_xcalloc(1, sizeof(*edge));
	edge->component = c;
	if (ced_EdgeMap_put(&arch->edges, c, edge) == KAMA_ERROR)
		abort();
	arch->_edge_cache[c & 7] = edge;
	return edge;
}

static ced_ColumnMapping *ced_column_map(ced_Archetype *from, ced_Archetype *to,
										 ced_Component component, size_t *count) {
	if (from->indexed) {
		// Indexed moves keep data in place. One immutable descriptor records
		// the changed component instead of allocating unused shared-column maps.
		ced_ColumnMapping *descriptor = ced_xmalloc(sizeof(*descriptor));
		*descriptor = (ced_ColumnMapping){component, 0};
		*count = 0;
		return descriptor;
	}
	ced_ColumnMapping *map = ced_xmalloc(ced_bytes(from->type_count, sizeof(*map)));
	*count = 0;
	for (size_t i = 0; i < from->type_count; i++) {
		ced_Component dst = to->column_for[from->type[i]];
		if (dst != ced_component_none)
			map[(*count)++] = (ced_ColumnMapping){(ced_Component)i, dst};
	}
	return map;
}

static void ced_link(ced_Archetype *src, ced_Archetype *dst, ced_Component c,
					 bool add) {
	ced_ArchetypeEdge *edge = ced_edge(src, c, true);
	ced_ArchetypeEdge *rev = ced_edge(dst, c, true);
	if (add) {
		edge->add = dst;
		free(edge->add_map);
		edge->add_map = ced_column_map(src, dst, c, &edge->add_map_count);
		rev->remove = src;
		free(rev->remove_map);
		rev->remove_map = ced_column_map(dst, src, c, &rev->remove_map_count);
	} else {
		edge->remove = dst;
		free(edge->remove_map);
		edge->remove_map = ced_column_map(src, dst, c, &edge->remove_map_count);
		rev->add = src;
		free(rev->add_map);
		rev->add_map = ced_column_map(dst, src, c, &rev->add_map_count);
	}
}

ced_Archetype *ced_archetype_with_added(ced_World *w, ced_Archetype *src,
										ced_Component c) {
	ced_ArchetypeEdge *edge = ced_edge(src, c, false);
	if (edge && edge->add)
		return edge->add;
	ced_Component *type = ced_xmalloc(ced_bytes(src->type_count + 1, sizeof(*type)));
	if (src->type_count)
		memcpy(type, src->type, src->type_count * sizeof(*type));
	type[src->type_count] = c;
	ced_Archetype *dst = ced_archetype_for(w, type, src->type_count + 1);
	free(type);
	ced_link(src, dst, c, true);
	return dst;
}

ced_Archetype *ced_archetype_with_removed(ced_World *w, ced_Archetype *src,
										  ced_Component c) {
	ced_ArchetypeEdge *edge = ced_edge(src, c, false);
	if (edge && edge->remove)
		return edge->remove;
	ced_Component *type = ced_xmalloc(ced_bytes(src->type_count, sizeof(*type)));
	size_t count = 0;
	for (size_t i = 0; i < src->type_count; i++)
		if (src->type[i] != c)
			type[count++] = src->type[i];
	ced_Archetype *dst = ced_archetype_for(w, type, count);
	free(type);
	ced_link(src, dst, c, false);
	return dst;
}

ced_Entity ced_archetype_swap_remove(ced_Archetype *arch, size_t row) {
	if (row >= arch->count)
		return ced_entity_none;
	size_t last = --arch->count;
	if (row == last)
		return ced_entity_none;
	for (size_t c = 0; c < arch->type_count; c++) {
		size_t size = arch->columns[c].component_size;
		if (size)
			ced_copy_value(arch->columns[c].data + row * size,
						   arch->columns[c].data + last * size, size);
	}
	return arch->entities[row] = arch->entities[last];
}

void ced_move_entity_mapped(ced_World *w, ced_Entity entity, ced_Archetype *src,
							size_t row, ced_Archetype *dst,
							const ced_ColumnMapping *map, size_t count) {
	if (w->indexed) {
		ced_indexed_move(w, entity, &w->entities[ced_entity_index(entity)], dst, map[0].src_col);
		return;
	}
	size_t dst_row = ced_append(w, dst, entity);
	for (size_t i = 0; i < count; i++) {
		size_t size = src->columns[map[i].src_col].component_size;
		if (size)
			ced_copy_value(dst->columns[map[i].dst_col].data + dst_row * size,
						   src->columns[map[i].src_col].data + row * size, size);
	}
	for (size_t i = 0; i < dst->type_count; i++) {
		size_t size = dst->columns[i].component_size;
		if (size && !ced_mask_has(&src->mask, dst->type[i]))
			ced_zero_value(dst->columns[i].data + dst_row * size, size);
	}
	ced_Entity moved = ced_archetype_swap_remove(src, row);
	if (moved != ced_entity_none)
		w->entities[ced_entity_index(moved)].row = row;
	w->entities[ced_entity_index(entity)].archetype = dst;
	w->entities[ced_entity_index(entity)].row = dst_row;
	ced_changed(w);
}

void ced_archetype_free(ced_Archetype *arch) {
	if (!arch->indexed && !arch->_inline_rows)
		for (size_t i = 0; i < arch->type_count; i++) {
            void *base = arch->_column_allocations[i];
            size_t prefix = base ? (size_t)(arch->columns[i].data - (unsigned char *)base) : 0;
            ced_storage_free(base, ced_archetype_storage_bytes(arch,arch->capacity,arch->columns[i].component_size,prefix));
        }
	kama_foreach (&arch->edges, i) {
		ced_ArchetypeEdge *edge = ced_EdgeMap_val(&arch->edges, i);
		free(edge->add_map);
		free(edge->remove_map);
		free(edge);
	}
	ced_EdgeMap_free(&arch->edges);
	if(!arch->_inline_rows)ced_storage_free(arch->entities, ced_archetype_storage_bytes(arch,arch->capacity,sizeof(*arch->entities),0));
	if (!arch->_embedded)
		free(arch);
}
