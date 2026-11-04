#include "internal.h"

ced_Component ced_component_find(const ced_World *w, const char *name) {
	if (!w || !name)
		return ced_component_none;
	ced_Component id = ced_component_none;
	ced_NameMap_get(&w->_component_names, name, strlen(name), &id);
	return id;
}

ced_Component ced_component_lookup(const ced_World *w, ced_Component hint,
								   const char *name) {
	if (ced_valid_component(w, hint) && name && w->components[hint]->name &&
		!strcmp(w->components[hint]->name, name))
		return hint;
	return ced_component_find(w, name);
}

const char *ced_component_name(const ced_World *w, ced_Component c) {
	return ced_valid_component(w, c) ? w->components[c]->name : NULL;
}

ced_Component ced_component_init(ced_World *w, size_t size) {
	if (!ced_mutable(w) || w->component_count >= ced_max_components)
		return ced_component_none;
	if (w->component_count == w->component_capacity) {
        size_t cap = ced_capacity(w->component_capacity, w->component_count + 1);
        if (cap > ced_max_components) cap = ced_max_components;
        if (w->components == w->_runtime->inline_component_ptrs) {
            ced_ComponentInfo **table = ced_xmalloc(ced_bytes(cap, sizeof(*table)));
            memcpy(table, w->components, ced_bytes(w->component_count, sizeof(*table)));
            w->components = table;
        } else w->components = ced_xrealloc(w->components, cap, sizeof(*w->components));
        w->component_capacity = cap;
    }
	ced_Component id = (ced_Component)w->component_count++;
	w->components[id] = id < 4 ? &w->_runtime->inline_components[id] : ced_xmalloc(sizeof(ced_ComponentInfo));
    *w->components[id] = (ced_ComponentInfo){.size = size, .alignment = 1};
	if (w->indexed) {
		w->components[id]->indexed.owners = &w->_runtime->empty_owner;
        w->components[id]->indexed._world = w;
	}
	return id;
}

ced_Component ced_component_register(ced_World *w, const char *name, size_t size,
									 size_t alignment) {
	if (!ced_mutable(w) || !name || !*name || !alignment ||
		(alignment & (alignment - 1)) || size % alignment)
		return ced_component_none;
	ced_Component id = ced_component_find(w, name);
	if (id != ced_component_none) {
		const ced_ComponentInfo *info = w->components[id];
		return info->size == size && info->alignment == alignment ? id
																  : ced_component_none;
	}
	id = ced_component_init(w, size);
	if (id == ced_component_none)
		return id;
	w->components[id]->alignment = alignment;
	size_t len = strlen(name) + 1;
	w->components[id]->name = len <= sizeof(w->components[id]->_short_name) ?
		w->components[id]->_short_name : ced_xmalloc(len);
	memcpy(w->components[id]->name, name, len);
	if (ced_NameMap_put(&w->_component_names, w->components[id]->name, len - 1, id) ==
		KAMA_ERROR)
		abort();
	return id;
}

static inline ced_Column *ced_component_column(const ced_World *w,
											   const ced_Record *rec, ced_Component c) {
	if (!ced_valid_component(w, c) || !rec)
		return NULL;
	ced_Component col = rec->archetype->column_for[c];
	return col == ced_component_none ? NULL : &rec->archetype->columns[col];
}

bool ced_has_component(ced_World *w, ced_Entity entity, ced_Component c) {
	const ced_Record *rec = ced_record_const(w, entity);
	return rec && ced_valid_component(w, c) &&
		rec->archetype->column_for[c] != ced_component_none;
}
const void *ced_get_component_const(const ced_World *w, ced_Entity entity,
									ced_Component c) {
	const ced_Record *rec = ced_record_const(w, entity);
	ced_Column *col = ced_component_column(w, rec, c);
	return col && col->component_size
			   ? col->data + (size_t)rec->row * col->component_size
			   : NULL;
}
void *ced_get_component(ced_World *w, ced_Entity entity, ced_Component c) {
	return (void *)ced_get_component_const(w, entity, c);
}

static void ced_add_record(ced_World *w, ced_Entity entity, ced_Record *rec,
						   ced_Component c) {
	ced_Archetype *src = rec->archetype;
	ced_ArchetypeEdge *edge = ced_edge(src, c, false);
	if (!edge || !edge->add) {
		ced_archetype_with_added(w, src, c);
		edge = ced_edge(src, c, false);
	}
	ced_move_entity_mapped(w, entity, src, rec->row, edge->add, edge->add_map,
						   edge->add_map_count);
}
void ced_add_component(ced_World *w, ced_Entity entity, ced_Component c) {
	if (!ced_mutable(w) || !ced_valid_component(w, c))
		return;
	ced_Record *rec = ced_record(w, entity);
	if (!rec || rec->archetype->column_for[c] != ced_component_none)
		return;
	ced_add_record(w, entity, rec, c);
}
void ced_remove_component(ced_World *w, ced_Entity entity, ced_Component c) {
	if (!ced_mutable(w) || !ced_valid_component(w, c))
		return;
	ced_Record *rec = ced_record(w, entity);
	if (!rec || rec->archetype->column_for[c] == ced_component_none)
		return;
	ced_Archetype *src = rec->archetype;
	ced_ArchetypeEdge *edge = ced_edge(src, c, false);
	if (!edge || !edge->remove) {
		ced_archetype_with_removed(w, src, c);
		edge = ced_edge(src, c, false);
	}
	ced_move_entity_mapped(w, entity, src, rec->row, edge->remove, edge->remove_map,
						   edge->remove_map_count);
}
static inline bool ced_store_column(ced_Record *rec, ced_Column *col,
									const void *value) {
	if (!col)
		return false;
	if (!col->component_size)
		return true;
	if (!value)
		return false;
	ced_copy_value(col->data + (size_t)rec->row * col->component_size, value,
				   col->component_size);
	return true;
}
bool ced_set_component(ced_World *w, ced_Entity entity, ced_Component c,
					   const void *value) {
	ced_Record *rec = ced_record(w, entity);
	return ced_store_column(rec, ced_component_column(w, rec, c), value);
}
bool ced_add_set_component(ced_World *w, ced_Entity entity, ced_Component c,
						   const void *value) {
	ced_Record *rec = ced_record(w, entity);
	if (!rec || !ced_valid_component(w, c))
		return false;
	ced_Column *col = ced_component_column(w, rec, c);
	if (col)
		return ced_store_column(rec, col, value);
	if (!ced_mutable(w))
		return false;
	size_t size = w->components[c]->size;
	if (size && !value)
		return false;
	// Protect values that alias any row compacted by the structural move.
	unsigned char local[256];
	void *copy = size > sizeof(local) ? ced_xmalloc(size) : local;
	ced_copy_value(copy, value, size);
	ced_add_record(w, entity, rec, c);
	col = &rec->archetype->columns[rec->archetype->column_for[c]];
	bool ok = ced_store_column(rec, col, copy);
	if (copy != local)
		free(copy);
	return ok;
}
