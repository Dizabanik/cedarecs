#include "internal.h"

static bool ced_filter(ced_World *w, ced_Mask *mask, size_t count,
					   const ced_Component *ids) {
	if (count > w->component_count || (count && !ids))
		return false;
	for (size_t i = 0; i < count; i++) {
		if (!ced_valid_component(w, ids[i]) || ced_mask_has(mask, ids[i]))
			return false;
		ced_mask_set(mask, ids[i]);
	}
	return true;
}

ced_View *ced_view_init(ced_World *w, const ced_ViewDesc *desc) {
	if (!w || !desc)
		return NULL;
	ced_Mask all = {0}, none = {0}, any = {0};
	if (!ced_filter(w, &all, desc->all_count, desc->all) ||
		!ced_filter(w, &none, desc->none_count, desc->none) ||
		!ced_filter(w, &any, desc->any_count, desc->any))
		return NULL;
	ced_View *q = ced_xcalloc(1, sizeof(*q));
	q->world = w;
	q->mask = all;
	q->exclude_mask = none;
	q->any_mask = any;
	q->has_any = desc->any_count != 0;
	q->component_count = desc->all_count;
	q->components = ced_xmalloc(ced_bytes(q->component_count, sizeof(*q->components)));
	if (q->component_count)
		memcpy(q->components, desc->all, q->component_count * sizeof(*q->components));
	q->_field_sizes =
		ced_xmalloc(ced_bytes(q->component_count, sizeof(*q->_field_sizes)));
	for (size_t i = 0; i < q->component_count; i++) {
		q->_field_sizes[i] = w->components[q->components[i]]->size;
	}
	if (w->indexed && (w->entity_count < 2048 || q->component_count == 1)) q->_column_slots=SIZE_MAX;
	q->_revision = UINT64_MAX;
	ced_lock(&w->_runtime->mutex);
	q->_identity = ++w->_runtime->view_sequence;
	if (!q->_identity)
		abort();
	q->_next = w->_views;
	if (q->_next)
		q->_next->_prev = q;
	w->_views = q;
	ced_unlock(&w->_runtime->mutex);
	ced_view_update(q);
	return q;
}

ced_View *ced_view_from_ids(ced_World *w, size_t count, const ced_Component *ids) {
	return ced_view_init(w, &(ced_ViewDesc){.all_count = count, .all = ids});
}

ced_Query *ced_query_init(ced_World *w, size_t count, ...) {
	if (!w || count > w->component_count)
		return NULL;
	ced_Component *ids = ced_xmalloc(ced_bytes(count, sizeof(*ids)));
	va_list args;
	va_start(args, count);
	for (size_t i = 0; i < count; i++)
		ids[i] = (ced_Component)va_arg(args, int);
	va_end(args);
	ced_Query *q = ced_view_from_ids(w, count, ids);
	free(ids);
	return q;
}

static bool ced_matches(const ced_View *q, const ced_Archetype *a) {
	if (!ced_mask_contains_all(&a->mask, &q->mask))
		return false;
	bool any = !q->has_any;
	for (size_t i = 0; i < ced_mask_words; i++) {
		if (a->mask.bits[i] & q->exclude_mask.bits[i])
			return false;
		if (a->mask.bits[i] & q->any_mask.bits[i])
			any = true;
	}
	return any;
}

static void ced_indexed_classic_update(ced_View *q) {
	ced_World *w = q->world;
	ced_indexed_materialize_entities(w);
	// Reuse the batch/pointer allocations as run boundaries change.
	size_t old_count = q->match_count;
	q->match_count = 0;
	for (size_t index = 0; index < w->entity_count;) {
		ced_Archetype *arch = w->entities[index].archetype;
		size_t begin = index++;
		while (index < w->entity_count && w->entities[index].archetype == arch)
			index++;
		if (!arch || !ced_matches(q, arch))
			continue;
		if (q->match_count == q->_match_capacity) {
			size_t previous = q->_match_capacity;
			q->_match_capacity = ced_capacity(previous, q->match_count + 1);
			q->matches = ced_xrealloc(q->matches, q->_match_capacity, sizeof(*q->matches));
			memset(q->matches + previous, 0, (q->_match_capacity - previous) * sizeof(*q->matches));
		}
		ced_Batch *batch = &q->matches[q->match_count++];
		void **columns = batch->column_ptrs;
		if (!columns)
			columns = ced_xmalloc(ced_bytes(q->component_count, sizeof(*columns)));
		*batch = (ced_Batch){.query = q, ._field_count = (uint32_t)q->component_count, .archetype = arch, .column_ptrs = columns,
		                    .entities = w->_indexed_entities + begin,
		                    .offset = begin, .count = index - begin};
		for (size_t c = 0; c < q->component_count; c++)
			columns[c] = ced_batch_col(batch, q->components[c]);
	}
	for (size_t i = q->match_count; i < old_count; i++) {
		free(q->matches[i].column_ptrs);
		q->matches[i].column_ptrs = NULL;
	}
	q->_revision = w->storage_revision;
}


static void ced_indexed_arena_update(ced_View *q) {
    ced_World *w = q->world;
    ced_indexed_materialize_entities(w);
    // Align each row of column pointers to max_align_t.
    const size_t row_align = (_Alignof(max_align_t) + sizeof(void *) - 1) / sizeof(void *);
    size_t stride = (q->component_count + row_align - 1) / row_align * row_align;
    size_t bound_rows = stride ? q->_column_slots / stride : 0;
    q->match_count = 0;
    for (size_t index = 0; index < w->entity_count;) {
        ced_Archetype *arch = w->entities[index].archetype;
        size_t begin = index++;
        while (index < w->entity_count && w->entities[index].archetype == arch) index++;
        if (!arch || !ced_matches(q, arch)) continue;
        if (q->match_count == q->_match_capacity) {
            q->_match_capacity = ced_capacity(q->_match_capacity, q->match_count + 1);
            q->matches = ced_xrealloc(q->matches, q->_match_capacity, sizeof(*q->matches));
        }
        size_t slot = q->match_count++;
        ced_Batch *batch = &q->matches[slot];
        void **columns = slot < bound_rows ? q->_column_storage + slot * stride : NULL;
        *batch = (ced_Batch){.query=q, ._field_count=(uint32_t)q->component_count,
            .archetype=arch, .column_ptrs=columns, .entities=w->_indexed_entities+begin,
            .offset=begin, .count=index-begin};
        // Bind the existing arena while scanning. If the run count stays the
        // same, a structural refresh needs no allocation or second pass.
        if (columns) for (size_t c=0;c<q->component_count;++c)
            columns[c]=ced_batch_col(batch,q->components[c]);
    }
    size_t slots=ced_bytes(q->match_count,stride);
    if (slots!=q->_column_slots) {
        if (!slots) { free(q->_column_storage); q->_column_storage=NULL; }
        else q->_column_storage=ced_xrealloc(q->_column_storage,slots,sizeof(void *));
        q->_column_slots=slots;
        // realloc preserves the bound prefix. Update pointers if it moves,
        // then bind any rows that did not fit in the previous allocation.
        for (size_t i=0;i<q->match_count;++i) {
            ced_Batch *batch=&q->matches[i];
            batch->column_ptrs=q->_column_storage+i*stride;
            if (i>=bound_rows) for (size_t c=0;c<q->component_count;++c)
                batch->column_ptrs[c]=ced_batch_col(batch,q->components[c]);
        }
    }
    q->_revision=w->storage_revision;
}

static void ced_indexed_view_update(ced_View *q) {
    if (q->_column_slots == SIZE_MAX) {
        // Small worlds and single-field views use separate column-pointer arrays.
        if (q->world->entity_count < 2048 || q->component_count == 1) {
            ced_indexed_classic_update(q);
            return;
        }
        for (size_t i=0;i<q->match_count;++i) free(q->matches[i].column_ptrs);
        q->_column_slots=0;
    }
    ced_indexed_arena_update(q);
}

void ced_view_update(ced_View *q) {
	ced_World *w = q->world;
	if (q->_revision == w->storage_revision)
		return;
	if (w->indexed) {
		ced_indexed_view_update(q);
		return;
	}
	for (size_t i = q->_last_archetype_count; i < w->archetype_count; i++) {
		ced_Archetype *arch = w->archetypes[i];
		if (!ced_matches(q, arch))
			continue;
		if (q->match_count == q->_match_capacity) {
			q->_match_capacity = ced_capacity(q->_match_capacity, q->match_count + 1);
			q->matches =
				ced_xrealloc(q->matches, q->_match_capacity, sizeof(*q->matches));
		}
		ced_Batch *batch = &q->matches[q->match_count++];
		*batch = (ced_Batch){.query = q, ._field_count = (uint32_t)q->component_count, .archetype = arch};
		batch->column_ptrs = ced_xmalloc(ced_bytes(q->component_count, sizeof(void *)));
	}
	q->_last_archetype_count = w->archetype_count;
	for (size_t i = 0; i < q->match_count; i++) {
		ced_Batch *batch = &q->matches[i];
		batch->entities = batch->archetype->entities;
		batch->count = batch->archetype->count;
		for (size_t c = 0; c < q->component_count; c++)
			batch->column_ptrs[c] = ced_batch_col(batch, q->components[c]);
	}
	q->_revision = w->storage_revision;
}

void ced_view_refresh(ced_View *q, ced_World *w) {
	// A query is bound to its owning world; never reuse foreign archetypes.
	if (q && q->world == w)
		ced_view_update(q);
}
void ced_query_refresh(ced_Query *q, ced_World *w) { ced_view_refresh(q, w); }

void ced_view_free(ced_View *q) {
	if (!q)
		return;
	ced_World *w = q->world;
	ced_lock(&w->_runtime->mutex);
	if (q->_prev)
		q->_prev->_next = q->_next;
	else
		w->_views = q->_next;
	if (q->_next)
		q->_next->_prev = q->_prev;
	if (q->_cache_head) {
		ced_View **slot = q->_cache_head;
		while (*slot && *slot != q)
			slot = &(*slot)->_cache_next;
		if (*slot)
			*slot = q->_cache_next;
	}
	ced_unlock(&w->_runtime->mutex);
	if (w->indexed && q->_column_slots != SIZE_MAX) free(q->_column_storage);
	else for (size_t i = 0; i < q->match_count; i++)
		free(q->matches[i].column_ptrs);
	free(q->matches);
	free(q->components);
	free(q->_field_sizes);
	free(q->_dispatch_jobs);
    free(q->_serial_jobs);
	free(q);
}
void ced_query_free(ced_Query *q) { ced_view_free(q); }

static bool ced_names_to_ids(ced_World *w, size_t count, const char *const *names,
							 ced_Component *ids) {
	if (!w || count > w->component_count || (count && !names))
		return false;
	for (size_t i = 0; i < count; i++) {
		ids[i] = ced_component_find(w, names[i]);
		if (ids[i] == ced_component_none)
			return false;
	}
	return true;
}

ced_View *ced_view_from_names(ced_World *w, size_t count, const char *const *names) {
	if (count > ced_max_components)
		return NULL;
	ced_Component local[16];
	ced_Component *ids =
		count <= 16 ? local : ced_xmalloc(ced_bytes(count, sizeof(*ids)));
	bool valid = ced_names_to_ids(w, count, names, ids);
	ced_View *view = valid ? ced_view_from_ids(w, count, ids) : NULL;
	if (ids != local)
		free(ids);
	return view;
}

ced_Entity ced_entity_with_names(ced_World *w, size_t count, const char *const *names) {
	if (count > ced_max_components)
		return ced_entity_none;
	ced_Component local[16];
	ced_Component *ids =
		count <= 16 ? local : ced_xmalloc(ced_bytes(count, sizeof(*ids)));
	bool valid = ced_names_to_ids(w, count, names, ids);
	ced_Entity entity = valid ? ced_entity_with_ids(w, count, ids) : ced_entity_none;
	if (ids != local)
		free(ids);
	return entity;
}

ced_View *ced_cached_view(ced_World *w, size_t count, const char *const *names) {
	if (!w || count > ced_max_components || (count && !names))
		return NULL;
	ced_ThreadBuffer *buffer = ced_get_thread_buffer(w);
	// Match by name to reuse cached views without allocating or building ID arrays.
	for (ced_View *q = buffer->views; q; q = q->_cache_next) {
		if (q->component_count != count)
			continue;
		size_t i = 0;
		for (; i < count; i++) {
			const char *name = w->components[q->components[i]]->name;
			if (!name || !names[i] || strcmp(name, names[i]))
				break;
		}
		if (i == count) {
			ced_view_update(q);
			return q;
		}
	}
	ced_View *q = ced_view_from_names(w, count, names);
	if (q) {
		q->_cache_head = &buffer->views;
		q->_cache_next = buffer->views;
		buffer->views = q;
	}
	return q;
}

size_t ced_view_count(ced_View *q) {
	if (q)
		ced_view_update(q);
	size_t count = 0;
	if (q)
		for (size_t i = 0; i < q->match_count; i++)
			count += q->matches[i].count;
	return count;
}
