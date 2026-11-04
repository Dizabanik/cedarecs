#include "internal.h"

static atomic_uint_fast64_t ced_thread_sequence = 1;
static _Thread_local uint64_t ced_thread_token;
static _Thread_local uint64_t ced_cached_world;
static _Thread_local ced_ThreadBuffer *ced_cached_buffer;

// Registration is per world, without a fixed slot limit. A monotonically
// increasing world identity prevents reuse of a TLS pointer after world free.
ced_ThreadBuffer *ced_get_thread_buffer(ced_World *w) {
	if (ced_cached_world == w->identity)
		return ced_cached_buffer;
	if (!ced_thread_token) {
		ced_thread_token =
			atomic_fetch_add_explicit(&ced_thread_sequence, 1, memory_order_relaxed);
		if (!ced_thread_token)
			abort();
	}
	ced_lock(&w->_runtime->mutex);
	ced_ThreadBuffer *buffer = NULL;
	ced_ThreadMap_get(&w->_runtime->threads, ced_thread_token, &buffer);
	if (!buffer) {
		buffer = ced_xcalloc(1, sizeof(*buffer));
		ced_mutex_init(&buffer->mutex);
		buffer->owner = ced_thread_token;
		buffer->next = w->_runtime->buffers;
		w->_runtime->buffers = buffer;
		atomic_store_explicit(&w->_runtime->has_buffers, true, memory_order_release);
		if (ced_ThreadMap_put(&w->_runtime->threads, ced_thread_token, buffer) ==
			KAMA_ERROR)
			abort();
	}
	ced_unlock(&w->_runtime->mutex);
	ced_cached_world = w->identity;
	ced_cached_buffer = buffer;
	return buffer;
}

bool ced_commands_begin(ced_World *w) {
	if (!w)
		return false;
	ced_ThreadBuffer *thread = ced_get_thread_buffer(w);
	if (!thread->scope_depth)
		ced_lock(&thread->mutex);
	if (thread->scope_depth == SIZE_MAX)
		abort();
	thread->scope_depth++;
	return true;
}
bool ced_commands_end(ced_World *w) {
	if (!w)
		return false;
	ced_ThreadBuffer *thread = ced_get_thread_buffer(w);
	if (!thread->scope_depth)
		return false;
	if (!--thread->scope_depth)
		ced_unlock(&thread->mutex);
	return true;
}

static void ced_enqueue_command(ced_World *w, ced_CmdType type, ced_Entity e,
								ced_Component c, const void *value, bool payload) {
	if (!w || (type != ced_cmd_kill && !ced_valid_component(w, c)))
		return;
	size_t size = payload ? w->components[c]->size : 0;
	if (size && !value)
		return;
	ced_ThreadBuffer *thread = ced_get_thread_buffer(w);
	if (!thread->scope_depth)
		ced_lock(&thread->mutex);
	ced_CmdBuffer *buffer = &thread->pending;
	if (buffer->count == buffer->capacity) {
		buffer->capacity = ced_capacity(buffer->capacity, buffer->count + 1);
		buffer->cmds =
			ced_xrealloc(buffer->cmds, buffer->capacity, sizeof(*buffer->cmds));
	}
	if (size > SIZE_MAX - buffer->data_size)
		abort();
	size_t needed = buffer->data_size + size;
	if (needed > buffer->data_capacity) {
		buffer->data_capacity = ced_capacity(buffer->data_capacity, needed);
		buffer->data = ced_xrealloc(buffer->data, buffer->data_capacity, 1);
	}
	if (size)
		ced_copy_value(buffer->data + buffer->data_size, value, size);
	buffer->cmds[buffer->count++] =
		(ced_DeferredCmd){e, buffer->data_size, type, c};
	buffer->data_size = needed;
	if (!thread->scope_depth)
		ced_unlock(&thread->mutex);
}

void ced_defer_add_component(ced_World *w, ced_Entity e, ced_Component c) {
	ced_enqueue_command(w, ced_cmd_add, e, c, NULL, false);
}
void ced_defer_remove_component(ced_World *w, ced_Entity e, ced_Component c) {
	ced_enqueue_command(w, ced_cmd_remove, e, c, NULL, false);
}
void ced_defer_set_component(ced_World *w, ced_Entity e, ced_Component c,
							 const void *value) {
	ced_enqueue_command(w, ced_cmd_set, e, c, value, true);
}
void ced_defer_add_set_component(ced_World *w, ced_Entity e, ced_Component c,
								 const void *value) {
	ced_enqueue_command(w, ced_cmd_put, e, c, value, true);
}
void ced_defer_destroy_entity(ced_World *w, ced_Entity e) {
	ced_enqueue_command(w, ced_cmd_kill, e, 0, NULL, false);
}

static const void *ced_command_data(ced_World *w, const ced_CmdBuffer *b,
                                    const ced_DeferredCmd *c) {
	return (c->type == ced_cmd_set || c->type == ced_cmd_put) &&
		w->components[c->component]->size ? b->data + c->data_offset : NULL;
}

static void ced_apply_single(ced_World *w, const ced_CmdBuffer *b,
                             const ced_DeferredCmd *c) {
	const void *data = ced_command_data(w, b, c);
	switch (c->type) {
	case ced_cmd_add: ced_add_component(w, c->entity, c->component); break;
	case ced_cmd_remove: ced_remove_component(w, c->entity, c->component); break;
	case ced_cmd_set: ced_set_component(w, c->entity, c->component, data); break;
	case ced_cmd_put: ced_add_set_component(w, c->entity, c->component, data); break;
	case ced_cmd_kill: ced_entity_destroy(w, c->entity); break;
	}
}

// Preserve FIFO results while inserting directly into the final signature.
// No callbacks can observe the intermediate states of a flush. Only adjacent
// commands for the exact same generational handle are folded together.
static void ced_apply_run(ced_World *w, const ced_CmdBuffer *b,
                         size_t begin, size_t end) {
	ced_Entity entity = b->cmds[begin].entity;
	ced_Record *rec = ced_record(w, entity);
	if (!rec)
		return;
	ced_Runtime *rt = w->_runtime;
	if (rt->deferred_capacity < w->component_count) {
		rt->deferred_capacity = ced_capacity(rt->deferred_capacity, w->component_count);
		rt->deferred_values = ced_xrealloc(rt->deferred_values, rt->deferred_capacity,
		                                sizeof(*rt->deferred_values));
		rt->deferred_touched = ced_xrealloc(rt->deferred_touched, rt->deferred_capacity,
		                                 sizeof(*rt->deferred_touched));
	}
	ced_Archetype *src = rec->archetype;
	size_t row = rec->row, touched_count = 0;
	ced_Mask final = src->mask, seen = {0};
	for (size_t i = begin; i < end; i++) {
		const ced_DeferredCmd *cmd = &b->cmds[i];
		if (cmd->type == ced_cmd_kill) {
			ced_entity_destroy(w, entity);
			return;
		}
		ced_Component c = cmd->component;
		if (!ced_mask_has(&seen, c)) {
			ced_mask_set(&seen, c);
			rt->deferred_touched[touched_count++] = c;
			ced_Component col = src->column_for[c];
			size_t size = w->components[c]->size;
			rt->deferred_values[c] = col != ced_component_none && size ?
				src->columns[col].data + row * size : NULL;
		}
		switch (cmd->type) {
		case ced_cmd_add:
			if (!ced_mask_has(&final, c)) {
				ced_mask_set(&final, c);
				rt->deferred_values[c] = NULL;
			}
			break;
		case ced_cmd_remove:
			final.bits[c >> 6] &= ~(UINT64_C(1) << (c & 63));
			rt->deferred_values[c] = NULL;
			break;
		case ced_cmd_put:
			ced_mask_set(&final, c);
			rt->deferred_values[c] = ced_command_data(w, b, cmd);
			break;
		case ced_cmd_set:
			if (ced_mask_has(&final, c))
				rt->deferred_values[c] = ced_command_data(w, b, cmd);
			break;
		case ced_cmd_kill: break;
		}
	}
	bool same = memcmp(&final, &src->mask, sizeof(final)) == 0;
	ced_Archetype *dst = same ? src : ced_archetype_for_mask(w, &final);
	size_t dst_row = same ? row : ced_append(w, dst, entity);
	size_t count = same ? touched_count : dst->type_count;
	for (size_t i = 0; i < count; i++) {
		ced_Component c = same ? rt->deferred_touched[i] : dst->type[i];
		ced_Component col = dst->column_for[c];
		if (col == ced_component_none)
			continue;
		size_t size = dst->columns[col].component_size;
		if (!size)
			continue;
		const void *value = NULL;
		if (ced_mask_has(&seen, c))
			value = rt->deferred_values[c];
		else if (src->column_for[c] != ced_component_none)
			value = src->columns[src->column_for[c]].data + row * size;
		void *out = dst->columns[col].data + dst_row * size;
		if (value) {
			if (out != value)
				ced_copy_value(out, value, size);
		} else
			ced_zero_value(out, size);
	}
	if (!same) {
		ced_Entity moved = ced_archetype_swap_remove(src, row);
		if (moved != ced_entity_none)
			w->entities[ced_entity_index(moved)].row = (uint32_t)row;
		rec->archetype = dst;
		rec->row = (uint32_t)dst_row;
		ced_changed(w);
	}
}

static void ced_process_commands(ced_World *w, bool apply) {
	if (!ced_mutable(w) || w->_runtime->flushing ||
		!atomic_load_explicit(&w->_runtime->has_buffers, memory_order_acquire))
		return;
	w->_runtime->flushing = true;
	ced_lock(&w->_runtime->mutex);
	ced_ThreadBuffer *thread = w->_runtime->buffers;
	ced_unlock(&w->_runtime->mutex);
	for (; thread; thread = thread->next) {
		// Only this queue's producer may inspect scope_depth without its mutex.
		bool scoped = thread->owner == ced_thread_token && thread->scope_depth;
		if (!scoped)
			ced_lock(&thread->mutex);
		ced_CmdBuffer tmp = thread->spare;
		thread->spare = thread->pending;
		thread->pending = tmp;
		if (!scoped)
			ced_unlock(&thread->mutex);
		ced_CmdBuffer *buffer = &thread->spare;
		if (apply)
			for (size_t i = 0; i < buffer->count;) {
				size_t end = i + 1;
				while (end < buffer->count && buffer->cmds[end].entity == buffer->cmds[i].entity)
					end++;
				if (w->indexed) {
					for (size_t command = i; command < end; command++)
						ced_apply_single(w, buffer, &buffer->cmds[command]);
				} else if (end == i + 1)
					ced_apply_single(w, buffer, &buffer->cmds[i]);
				else
					ced_apply_run(w, buffer, i, end);
				i = end;
			}
		buffer->count = buffer->data_size = 0;
	}
	w->_runtime->flushing = false;
}

void ced_flush_commands(ced_World *w) { ced_process_commands(w, true); }
void ced_discard_commands(ced_World *w) { ced_process_commands(w, false); }

void ced_buffers_free(ced_World *w) {
	if (!atomic_load_explicit(&w->_runtime->has_buffers, memory_order_acquire))
		return;
	ced_ThreadMap_free(&w->_runtime->threads);
	free(w->_runtime->deferred_values);
	free(w->_runtime->deferred_touched);
	ced_ThreadBuffer *buffer = w->_runtime->buffers;
	while (buffer) {
		ced_ThreadBuffer *next = buffer->next;
		free(buffer->pending.cmds);
		free(buffer->pending.data);
		free(buffer->spare.cmds);
		free(buffer->spare.data);
		ced_mutex_free(&buffer->mutex);
		free(buffer);
		buffer = next;
	}
}
