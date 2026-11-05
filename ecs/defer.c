#include "internal.h"

static atomic_uint_fast64_t ced_thread_sequence = 1;
typedef struct ced_ThreadCache {
    uint64_t world;
    ced_ThreadBuffer *buffer;
    uint64_t token;
} ced_ThreadCache;
static _Thread_local ced_ThreadCache ced_tls;

#if CEDARECS_CONCURRENT_REGISTRY == 2
enum { ced_registry_min_producers = 2, ced_registry_promotion_score = 64 };

static void ced_registry_promote(ced_Runtime *runtime) {
    ced_SharedThreadMap_t *shared = ced_xcalloc(1, sizeof(*shared));
    kama_conc_options_t options = {.shards = CEDARECS_REGISTRY_SHARDS};
    if (!ced_SharedThreadMap_init_ex(shared, 0, &options)) abort();
    kama_foreach (&runtime->threads, i) {
        if (!ced_SharedThreadMap_put(shared, ced_ThreadMap_key(&runtime->threads, i),
                                    ced_ThreadMap_val(&runtime->threads, i))) abort();
    }
    atomic_store_explicit(&runtime->shared_threads, shared, memory_order_release);
    ced_ThreadMap_free(&runtime->threads);
}

static ced_ThreadBuffer *ced_registry_shared_buffer(ced_World *w, ced_SharedThreadMap_t *map, uint64_t token) {
    ced_ThreadBuffer *buffer = NULL;
    ced_SharedThreadMap_get(map, token, &buffer);
    if (!buffer) {
        buffer = ced_xcalloc(1, sizeof(*buffer));
        ced_mutex_init(&buffer->mutex);
        buffer->owner = token;
        ced_lock(&w->_runtime->mutex);
        buffer->next = w->_runtime->buffers;
        w->_runtime->buffers = buffer;
        if (!ced_SharedThreadMap_put(map, token, buffer)) abort();
        atomic_store_explicit(&w->_runtime->has_buffers, true, memory_order_release);
        ced_unlock(&w->_runtime->mutex);
    }
    return buffer;
}

static ced_ThreadBuffer *ced_registry_auto_buffer(ced_World *w, uint64_t token) {
    ced_Runtime *runtime = w->_runtime;
    ced_SharedThreadMap_t *shared = atomic_load_explicit(&runtime->shared_threads, memory_order_acquire);
    if (shared) return ced_registry_shared_buffer(w, shared, token);
    ced_lock(&runtime->mutex);
    // Migration may have completed while this producer waited for the mutex.
    shared = atomic_load_explicit(&runtime->shared_threads, memory_order_relaxed);
    if (shared) {
        ced_unlock(&runtime->mutex);
        return ced_registry_shared_buffer(w, shared, token);
    }
    ced_ThreadBuffer *buffer = NULL;
    ced_ThreadMap_get(&runtime->threads, token, &buffer);
    if (!buffer) {
        buffer = ced_xcalloc(1, sizeof(*buffer));
        ced_mutex_init(&buffer->mutex);
        buffer->owner = token;
        buffer->next = runtime->buffers;
        runtime->buffers = buffer;
        if (!ced_ThreadMap_put_unique(&runtime->threads, token, buffer)) abort();
        atomic_store_explicit(&runtime->has_buffers, true, memory_order_release);
    } else if (ced_ThreadMap_count(&runtime->threads) >= ced_registry_min_producers) {
        if (runtime->registry_previous_owner != token) {
            runtime->registry_burst = 0;
            if (++runtime->registry_score == ced_registry_promotion_score) ced_registry_promote(runtime);
        } else if (++runtime->registry_burst == 0 && runtime->registry_score) {
            // Reduce the score every 256 misses from the same owner. Short OS
            // lock convoys retain their score; long runs by one owner decay it.
            --runtime->registry_score;
        }
    }
    runtime->registry_previous_owner = token;
    ced_unlock(&runtime->mutex);
    return buffer;
}
#endif

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static ced_ThreadBuffer *ced_get_thread_buffer_slow(ced_World *w, ced_ThreadCache *cache) {
	if (!cache->token) {
		cache->token =
			atomic_fetch_add_explicit(&ced_thread_sequence, 1, memory_order_relaxed);
		if (!cache->token)
			abort();
	}
#if CEDARECS_CONCURRENT_REGISTRY == 2
    ced_ThreadBuffer *buffer = ced_registry_auto_buffer(w, cache->token);
#else
#if CEDARECS_CONCURRENT_REGISTRY == 1
	// Release/acquire publication protects initialization of the shard handle.
	// Worlds that never use deferred commands allocate no registry shards.
	if (!atomic_load_explicit(&w->_runtime->threads_ready, memory_order_acquire)) {
		ced_lock(&w->_runtime->mutex);
		if (!atomic_load_explicit(&w->_runtime->threads_ready, memory_order_relaxed)) {
			kama_conc_options_t options = {.shards = CEDARECS_REGISTRY_SHARDS};
			if (!ced_ThreadMap_init_ex(&w->_runtime->threads, 0, &options)) abort();
			atomic_store_explicit(&w->_runtime->threads_ready, true, memory_order_release);
		}
		ced_unlock(&w->_runtime->mutex);
	}
#else
	ced_lock(&w->_runtime->mutex);
#endif
	ced_ThreadBuffer *buffer = NULL;
	ced_ThreadMap_get(&w->_runtime->threads, cache->token, &buffer);
	if (!buffer) {
		buffer = ced_xcalloc(1, sizeof(*buffer));
		ced_mutex_init(&buffer->mutex);
		buffer->owner = cache->token;
#if CEDARECS_CONCURRENT_REGISTRY == 1
		// Map operations release their shard lock before taking this lock.
		// The world mutex protects list publication and flush snapshots.
		ced_lock(&w->_runtime->mutex);
#endif
		buffer->next = w->_runtime->buffers;
		w->_runtime->buffers = buffer;
		atomic_store_explicit(&w->_runtime->has_buffers, true, memory_order_release);
#if CEDARECS_CONCURRENT_REGISTRY == 1
		if (ced_ThreadMap_put(&w->_runtime->threads, cache->token, buffer) ==
#else
		// The world mutex keeps this key absent from the lookup through insertion.
		if (ced_ThreadMap_put_unique(&w->_runtime->threads, cache->token, buffer) ==
#endif
			KAMA_ERROR)
			abort();
#if CEDARECS_CONCURRENT_REGISTRY == 1
		ced_unlock(&w->_runtime->mutex);
#endif
	}
#if CEDARECS_CONCURRENT_REGISTRY == 0
	ced_unlock(&w->_runtime->mutex);
#endif
#endif
    cache->world = w->identity;
    cache->buffer = buffer;
    return buffer;
}

// Resolve TLS once. Only cache misses select a map, update scores or register.
ced_ThreadBuffer *ced_get_thread_buffer(ced_World *w) {
    ced_ThreadCache *cache = &ced_tls;
    if (cache->world == w->identity) return cache->buffer;
    return ced_get_thread_buffer_slow(w, cache);
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

// Commands without values skip payload growth and copying. They still validate
// input, lock the queue, check capacity overflow and publish in FIFO order.
static void ced_enqueue_simple(ced_World *w, ced_CmdType type, ced_Entity e, ced_Component c) {
    if (!w || (type != ced_cmd_kill && !ced_valid_component(w,c))) return;
    ced_ThreadBuffer *thread=ced_get_thread_buffer(w);
    if (!thread->scope_depth) ced_lock(&thread->mutex);
    ced_CmdBuffer *buffer=&thread->pending;
    if (buffer->count==buffer->capacity) {
        buffer->capacity=ced_capacity(buffer->capacity,buffer->count+1);
        buffer->cmds=ced_xrealloc(buffer->cmds,buffer->capacity,sizeof(*buffer->cmds));
    }
    buffer->cmds[buffer->count++]=(ced_DeferredCmd){e,buffer->data_size,type,c};
    if (!thread->scope_depth) ced_unlock(&thread->mutex);
}

void ced_defer_add_component(ced_World *w, ced_Entity e, ced_Component c) {
	ced_enqueue_simple(w, ced_cmd_add, e, c);
}
void ced_defer_remove_component(ced_World *w, ced_Entity e, ced_Component c) {
	ced_enqueue_simple(w, ced_cmd_remove, e, c);
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
	ced_enqueue_simple(w, ced_cmd_kill, e, 0);
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
static bool ced_absent_pair(ced_World *w, const ced_DeferredCmd *first,
                           const ced_DeferredCmd *last) {
	if (last->type != ced_cmd_remove || first->component != last->component ||
	    (first->type != ced_cmd_add && first->type != ced_cmd_put && first->type != ced_cmd_set))
		return false;
	ced_Record *rec = ced_record(w, first->entity);
	return rec && !ced_mask_has(&rec->archetype->mask, first->component);
}

static void ced_apply_run(ced_World *w, const ced_CmdBuffer *b,
                         size_t begin, size_t end) {
	ced_Entity entity = b->cmds[begin].entity;
	ced_Record *rec = ced_record(w, entity);
	if (!rec)
		return;
	ced_Archetype *src = rec->archetype;
	ced_Runtime *rt = w->_runtime;
	if (rt->deferred_capacity < w->component_count) {
		rt->deferred_capacity = ced_capacity(rt->deferred_capacity, w->component_count);
		rt->deferred_values = ced_xrealloc(rt->deferred_values, rt->deferred_capacity,
		                                sizeof(*rt->deferred_values));
		rt->deferred_touched = ced_xrealloc(rt->deferred_touched, rt->deferred_capacity,
		                                 sizeof(*rt->deferred_touched));
	}
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
		bool scoped = thread->owner == ced_tls.token && thread->scope_depth;
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
				if (end == i + 2 && ced_absent_pair(w, &buffer->cmds[i], &buffer->cmds[i + 1])) {
					// Packed and indexed storage hide intermediate flush states.
					// Once checked, an absent-component cancellation needs no mutation.
					i = end;
					continue;
				}
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
    if (!atomic_load_explicit(&w->_runtime->has_buffers, memory_order_acquire)) return;
#if CEDARECS_CONCURRENT_REGISTRY == 2
    ced_SharedThreadMap_t *shared = atomic_load_explicit(&w->_runtime->shared_threads, memory_order_relaxed);
    if (shared) { ced_SharedThreadMap_free(shared); free(shared); }
    else ced_ThreadMap_free(&w->_runtime->threads);
#else
    ced_ThreadMap_free(&w->_runtime->threads);
#endif
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
