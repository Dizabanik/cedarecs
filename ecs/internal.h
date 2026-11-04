#ifndef CEDARECS_INTERNAL_H
#define CEDARECS_INTERNAL_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#define CEDARECS_NO_INLINE_ACCESSORS
#include "../cedar.h"
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#include <malloc.h>
typedef CRITICAL_SECTION ced_Mutex;
typedef CONDITION_VARIABLE ced_Cond;
typedef HANDLE ced_Thread;
static inline void ced_mutex_init(ced_Mutex *m) { InitializeCriticalSection(m); }
static inline void ced_mutex_free(ced_Mutex *m) { DeleteCriticalSection(m); }
static inline void ced_lock(ced_Mutex *m) { EnterCriticalSection(m); }
static inline void ced_unlock(ced_Mutex *m) { LeaveCriticalSection(m); }
static inline void ced_cond_init(ced_Cond *c) { InitializeConditionVariable(c); }
static inline void ced_cond_free(ced_Cond *c) { (void)c; }
static inline void ced_cond_wait(ced_Cond *c, ced_Mutex *m) {
	SleepConditionVariableCS(c, m, INFINITE);
}
static inline void ced_cond_broadcast(ced_Cond *c) { WakeAllConditionVariable(c); }
#else
#include <pthread.h>
typedef pthread_mutex_t ced_Mutex;
typedef pthread_cond_t ced_Cond;
typedef pthread_t ced_Thread;
static inline void ced_mutex_init(ced_Mutex *m) {
	if (pthread_mutex_init(m, NULL))
		abort();
}
static inline void ced_mutex_free(ced_Mutex *m) { pthread_mutex_destroy(m); }
static inline void ced_lock(ced_Mutex *m) { pthread_mutex_lock(m); }
static inline void ced_unlock(ced_Mutex *m) { pthread_mutex_unlock(m); }
static inline void ced_cond_init(ced_Cond *c) {
	if (pthread_cond_init(c, NULL))
		abort();
}
static inline void ced_cond_free(ced_Cond *c) { pthread_cond_destroy(c); }
static inline void ced_cond_wait(ced_Cond *c, ced_Mutex *m) { pthread_cond_wait(c, m); }
static inline void ced_cond_broadcast(ced_Cond *c) { pthread_cond_broadcast(c); }
#endif

// Allocation failures are fail-fast. Never publish partial storage or lose the
// old allocation on failure. All sizes pass through checked multiplication.
static inline size_t ced_bytes(size_t count, size_t size) {
	if (size && count > SIZE_MAX / size)
		abort();
	return count * size;
}
static inline void *ced_xmalloc(size_t size) {
	void *p = malloc(size ? size : 1);
	if (!p)
		abort();
	return p;
}
static inline void *ced_xcalloc(size_t count, size_t size) {
	size_t bytes = ced_bytes(count, size);
	void *p = calloc(1, bytes ? bytes : 1);
	if (!p)
		abort();
	return p;
}
static inline void *ced_xrealloc(void *old, size_t count, size_t size) {
	size_t bytes = ced_bytes(count, size);
	void *p = realloc(old, bytes ? bytes : 1);
	if (!p)
		abort();
	return p;
}
static inline size_t ced_capacity(size_t current, size_t needed) {
	size_t cap = current ? current : 8;
	while (cap < needed) {
		if (cap > SIZE_MAX / 2)
			return needed;
		cap *= 2;
	}
	return cap;
}
#if (defined(__APPLE__) || defined(__linux__)) && !defined(CEDARECS_NO_VM)
#define CEDARECS_VM_STORAGE 1
#else
#define CEDARECS_VM_STORAGE 0
#endif
#define CEDARECS_VM_THRESHOLD (1024u * 1024u)
static inline size_t ced_archetype_storage_bytes(const ced_Archetype *arch,
                                               size_t capacity, size_t size, size_t prefix) {
    size_t payload=ced_bytes(capacity,size);
    if(prefix>SIZE_MAX-payload)abort();
    if(arch->_bounded_rows && payload+prefix<CEDARECS_VM_THRESHOLD) {
        payload=ced_bytes(ced_capacity(0,capacity),size);
        if(prefix>SIZE_MAX-payload)abort();
    }
    return prefix+payload;
}
static inline size_t ced_storage_capacity(size_t current, size_t needed) {
    if (!CEDARECS_VM_STORAGE || needed <= 65536) return ced_capacity(current, needed);
    const size_t rows = 4096;
    return needed > SIZE_MAX - (rows - 1) ? needed : (needed + rows - 1) & ~(rows - 1);
}
void *ced_storage_resize(void *old, size_t old_size, size_t live_size,
                         size_t size, size_t alignment);
void *ced_storage_resize_zero(void *old, size_t old_size, size_t live_size,
                              size_t size, size_t alignment);
void ced_storage_free(void *data, size_t size);
size_t ced_storage_virtual_bytes(const void *data, size_t size);
size_t ced_storage_logical_bytes(const void *data, size_t size);
static inline void *ced_aligned_alloc(size_t size, size_t alignment) {
	if (!size)
		return NULL;
	if (alignment < ced_align)
		alignment = ced_align;
	void *p = NULL;
#if defined(_WIN32)
	p = _aligned_malloc(size, alignment);
#else
	if (alignment <= _Alignof(max_align_t))
		p = malloc(size);
	else if (posix_memalign(&p, alignment, size))
		abort();
#endif
	if (!p)
		abort();
	return p;
}
static inline void ced_aligned_free(void *p) {
#if defined(_WIN32)
	_aligned_free(p);
#else
	free(p);
#endif
}

// Fundamental-alignment columns use malloc/realloc, allowing the allocator to
// extend/remap large allocations instead of copying every live row on growth.
static inline void *ced_aligned_resize(void *old, size_t live_size, size_t size,
									   size_t alignment) {
	if (!size) {
		ced_aligned_free(old);
		return NULL;
	}
	if (alignment < ced_align)
		alignment = ced_align;
#if defined(_WIN32)
	void *p = _aligned_realloc(old, size, alignment);
	if (!p)
		abort();
	return p;
#else
	if (alignment <= _Alignof(max_align_t))
		return ced_xrealloc(old, size, 1);
	void *p = ced_aligned_alloc(size, alignment);
	if (live_size)
		memcpy(p, old, live_size);
	ced_aligned_free(old);
	return p;
#endif
}

// Reserve zero-filled unused rows once. The allocator can supply fresh zero
// pages; existing allocations retain live data and clear only their unused tail.
static inline void *ced_aligned_resize_zero(void *old, size_t live_size, size_t size,
                                           size_t alignment) {
    if (!size) { ced_aligned_free(old); return NULL; }
#if !defined(_WIN32)
    size_t effective = alignment < ced_align ? ced_align : alignment;
    if (!old && effective <= _Alignof(max_align_t)) return ced_xcalloc(size, 1);
#endif
    void *data = ced_aligned_resize(old, live_size, size, alignment);
    memset((unsigned char *)data + live_size, 0, size - live_size);
    return data;
}

typedef struct ced_ThreadBuffer {
	ced_Mutex mutex;
	uint64_t owner;
	size_t scope_depth; // read and written only by this queue's producer
	ced_CmdBuffer pending, spare;
	ced_View *views;
	struct ced_ThreadBuffer *next;
} ced_ThreadBuffer;

kama_u64(ced_ThreadMap, ced_ThreadBuffer *)

typedef struct ced_Runtime {
	ced_Mutex mutex;
	ced_Record inline_records[256];
	ced_ComponentInfo inline_components[4];
	ced_ComponentInfo *inline_component_ptrs[8];
	ced_Archetype inline_empty;
	ced_Archetype *inline_archetypes[8];
	ced_ThreadBuffer *buffers;
	ced_ThreadMap_t threads;
	struct ced_WorkerPool *pool;
	atomic_bool parallel, has_buffers, indexed_entities_ready;
	uint64_t view_sequence;
	size_t indexed_writers;
	ced_Entity empty_owner;
	const void **deferred_values;
	ced_Component *deferred_touched;
	size_t deferred_capacity;
	bool flushing;
} ced_Runtime;

typedef struct ced_Allocation {
	ced_World world;
	ced_Runtime runtime;
} ced_Allocation;

// Records at the last geometric capacity occupy exactly the VM threshold.
// Keep that boundary native so short-lived worlds can reuse allocator pages.
// The next bounded increment transitions once to growable virtual storage.
static inline bool ced_records_use_native(size_t capacity) {
    return CEDARECS_VM_STORAGE && capacity <= CEDARECS_VM_THRESHOLD / sizeof(ced_Record);
}

static inline size_t ced_indexed_owner_bytes(const ced_ComponentInfo *info, size_t capacity) {
    (void)info;
    return ced_bytes(capacity, sizeof(ced_Entity));
}
static inline void ced_indexed_cached_owner_add(ced_World *w, ced_Component c,
                                               void *owners, size_t size, ced_Entity entity) {
    (void)w; (void)c; (void)size;
    ((ced_Entity *)owners)[(uint32_t)entity] = entity;
}
static inline void ced_indexed_cached_owner_remove(ced_World *w, ced_Component c,
                                                  void *owners, size_t size, ced_Entity entity) {
    (void)w; (void)c; (void)size;
    ((ced_Entity *)owners)[(uint32_t)entity] = UINT64_MAX;
}
static inline bool ced_mutable(const ced_World *w) {
	return w && !atomic_load_explicit(&w->_runtime->parallel, memory_order_acquire);
}
static inline bool ced_valid_component(const ced_World *w, ced_Component c) {
	return w && c < w->component_count;
}
// A live record always points at an archetype, including the empty signature.
static inline const ced_Record *ced_record_const(const ced_World *w, ced_Entity e) {
	uint32_t index = (uint32_t)e;
	if (!w || index >= w->entity_count)
		return NULL;
	const ced_Record *rec = &w->entities[index];
	return rec->archetype && rec->generation == (uint32_t)(e >> 32) ? rec : NULL;
}
static inline ced_Record *ced_record(ced_World *w, ced_Entity e) {
	return (ced_Record *)ced_record_const(w, e);
}
// Constant-size memmoves retain overlap safety while avoiding a libc call for
// common component sizes. Other sizes use the ordinary portable fallback.
#if defined(__GNUC__) || defined(__clang__)
#define CEDARECS_ALWAYS_INLINE static inline __attribute__((always_inline))
#else
#define CEDARECS_ALWAYS_INLINE static inline
#endif
#if defined(__has_builtin) && !defined(CEDARECS_PORTABLE_MEMORY)
#if __has_builtin(__builtin_memcpy_inline)
// Snapshot the complete source before writing, retaining memmove overlap safety.
#define CEDARECS_COPY_FIXED(bytes) do { \
    unsigned char value[bytes]; \
    __builtin_memcpy_inline(value, src, bytes); \
    __builtin_memcpy_inline(dst, value, bytes); \
} while (0)
#endif
#endif
#ifndef CEDARECS_COPY_FIXED
#define CEDARECS_COPY_FIXED(bytes) memmove(dst, src, bytes)
#endif
CEDARECS_ALWAYS_INLINE void ced_copy_value(void *dst, const void *src, size_t size) {
    switch (size) {
    case 0: return;
    case 1: CEDARECS_COPY_FIXED(1); return;
    case 2: CEDARECS_COPY_FIXED(2); return;
    case 4: CEDARECS_COPY_FIXED(4); return;
    case 8: CEDARECS_COPY_FIXED(8); return;
    case 12: CEDARECS_COPY_FIXED(12); return;
    case 16: CEDARECS_COPY_FIXED(16); return;
    case 24: CEDARECS_COPY_FIXED(24); return;
    case 32: CEDARECS_COPY_FIXED(32); return;
    case 64: CEDARECS_COPY_FIXED(64); return;
    default: memmove(dst, src, size); return;
    }
}
#undef CEDARECS_COPY_FIXED
#undef CEDARECS_ALWAYS_INLINE
// Clang can merge constant memset switch arms back into a variable-size libc
// call. Keep short component initialization inline, with a portable fallback.
#if defined(__has_builtin) && !defined(CEDARECS_PORTABLE_MEMORY)
#if __has_builtin(__builtin_memset_inline)
#define CEDARECS_ZERO_FIXED(dst, size) __builtin_memset_inline(dst, 0, size)
#endif
#endif
#ifndef CEDARECS_ZERO_FIXED
#define CEDARECS_ZERO_FIXED(dst, size) memset(dst, 0, size)
#endif
static inline void ced_zero_value(void *dst, size_t size) {
    switch (size) {
    case 0: return;
    case 8: CEDARECS_ZERO_FIXED(dst, 8); return;
    case 16: CEDARECS_ZERO_FIXED(dst, 16); return;
    case 32: CEDARECS_ZERO_FIXED(dst, 32); return;
    case 64: CEDARECS_ZERO_FIXED(dst, 64); return;
    case 128: CEDARECS_ZERO_FIXED(dst, 128); return;
    case 256: CEDARECS_ZERO_FIXED(dst, 256); return;
    default: memset(dst, 0, size); return;
    }
}
// Resolve a homogeneous signature's width once per row, rather than once per
// component. Mixed signatures and arbitrary widths keep the general path.
static inline void ced_zero_row(ced_Archetype *arch, size_t row) {
    const size_t count = arch->type_count;
    const ced_Column *columns = arch->columns;
#define CEDARECS_ZERO_ROW(bytes) case bytes: \
    for (size_t i = 0; i < count; i++) \
        CEDARECS_ZERO_FIXED(columns[i].data + row * bytes, bytes); \
    return
    switch (arch->_uniform_size) {
    case 0: return;
    CEDARECS_ZERO_ROW(8);
    CEDARECS_ZERO_ROW(16);
    CEDARECS_ZERO_ROW(32);
    CEDARECS_ZERO_ROW(64);
    CEDARECS_ZERO_ROW(128);
    CEDARECS_ZERO_ROW(256);
    default:
        for (size_t i = 0; i < count; i++) {
            size_t size = columns[i].component_size;
            if (size) ced_zero_value(columns[i].data + row * size, size);
        }
    }
#undef CEDARECS_ZERO_ROW
}
#undef CEDARECS_ZERO_FIXED
static inline void ced_changed(ced_World *w) { w->storage_revision++; }

ced_Archetype *ced_archetype_new(ced_World *world, const ced_Component *type,
								 size_t count);
ced_Archetype *ced_find_archetype(ced_World *world, const ced_Component *type,
								  size_t count);
ced_Archetype *ced_archetype_for(ced_World *world, const ced_Component *type,
								 size_t count);
ced_Archetype *ced_archetype_for_mask(ced_World *world, const ced_Mask *mask);
ced_Archetype *ced_archetype_with_added(ced_World *world, ced_Archetype *src,
										ced_Component c);
ced_Archetype *ced_archetype_with_removed(ced_World *world, ced_Archetype *src,
										  ced_Component c);
ced_ArchetypeEdge *ced_edge(ced_Archetype *arch, ced_Component c, bool create);
void ced_archetype_reserve(ced_World *world, ced_Archetype *arch, size_t needed);
static inline size_t ced_append(ced_World *world, ced_Archetype *arch,
								ced_Entity entity) {
	if (arch->count == arch->capacity)
		ced_archetype_reserve(world, arch, arch->count + 1);
	size_t row = arch->count++;
    if (arch->_initialized_rows < arch->count) arch->_initialized_rows = arch->count;
	arch->entities[row] = entity;
	return row;
}
static inline size_t ced_archetype_push(ced_World *world, ced_Archetype *arch,
										ced_Entity entity) {
	size_t row = ced_append(world, arch, entity);
	for (size_t c = 0; c < arch->type_count; c++) {
		size_t size = arch->columns[c].component_size;
		if (size)
			ced_zero_value(arch->columns[c].data + row * size, size);
	}
	return row;
}
ced_Entity ced_archetype_swap_remove(ced_Archetype *arch, size_t row);
void ced_archetype_free(ced_Archetype *arch);
void ced_move_entity_mapped(ced_World *world, ced_Entity entity, ced_Archetype *src,
							size_t row, ced_Archetype *dst,
							const ced_ColumnMapping *map, size_t count);
void ced_default_update(ced_World *world);
void ced_indexed_reserve(ced_World *world, ced_Component component);
void ced_indexed_reserve_slot(ced_World *world, ced_Component component, uint32_t index);
// For indexed signatures this field bounds the slots prepared in every
// component, rather than tracking prior packed-row initialization.
static inline size_t ced_indexed_row_capacity(const ced_World *world,
                                               const ced_Archetype *arch) {
    size_t capacity = world->entity_capacity;
    for (size_t i = 0; i < arch->type_count; i++) {
        size_t available = world->components[arch->type[i]]->indexed.capacity;
        if (available < capacity) capacity = available;
    }
    return capacity;
}
void ced_indexed_access_invalidate(ced_IndexedColumn *column);
void ced_indexed_grow(ced_World *world, size_t old_capacity);
void ced_indexed_materialize_entities(ced_World *world);
void ced_indexed_move(ced_World *world, ced_Entity entity, ced_Record *record,
                      ced_Archetype *dst, ced_Component component);
ced_ThreadBuffer *ced_get_thread_buffer(ced_World *world);
void ced_buffers_free(ced_World *world);
void ced_workers_free(ced_World *world);
void ced_view_update(ced_View *view);

#endif
