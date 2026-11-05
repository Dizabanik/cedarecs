#include "internal.h"

#ifndef ced_worker_spin_us
#define ced_worker_spin_us 64
#endif
_Static_assert(ced_worker_spin_us >= 0, "Spin budget cannot be negative");

#ifndef ced_worker_cache_line
#if defined(__APPLE__) && defined(__aarch64__)
#define ced_worker_cache_line 128
#else
#define ced_worker_cache_line 64
#endif
#endif
_Static_assert(ced_worker_cache_line >= 64 &&
				   !(ced_worker_cache_line & (ced_worker_cache_line - 1)),
			   "Worker cache-line alignment must be a power of two at least 64");

typedef struct ced_Job {
	const ced_Batch *source;
	size_t offset, count;
} ced_Job;

typedef struct ced_Worker {
	_Alignas(ced_worker_cache_line) struct ced_WorkerPool *pool;
	ced_Thread thread;
	void **columns;
	size_t column_capacity;
	size_t index, end;
	_Alignas(ced_worker_cache_line) atomic_size_t next;
} ced_Worker;

typedef struct ced_WorkerPool {
	ced_World *world;
	ced_Mutex mutex;
	ced_Cond wake, done;
	ced_Worker *workers;
	ced_Worker caller;
	size_t thread_count, parked;
	atomic_bool stop, owner_waiting;
	ced_Job *jobs;
    const unsigned char *serial_jobs;
    size_t serial_stride;
	size_t job_count, claim_size;
	ced_BatchFn fn;
	void *context;
	_Alignas(ced_worker_cache_line) atomic_uint_fast64_t generation;
	_Alignas(ced_worker_cache_line) atomic_size_t remaining;
} ced_WorkerPool;

#if ced_worker_spin_us > 0
static uint64_t ced_worker_now(void) {
#if defined(_WIN32)
	LARGE_INTEGER now, frequency;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&frequency);
	return (uint64_t)((double)now.QuadPart * 1e9 / (double)frequency.QuadPart);
#else
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
#endif
}
static inline void ced_relax(void) {
#if defined(_WIN32)
	YieldProcessor();
#elif defined(__aarch64__) || defined(__arm__)
	__asm__ __volatile__("yield");
#elif defined(__i386__) || defined(__x86_64__)
	__asm__ __volatile__("pause");
#else
	atomic_signal_fence(memory_order_seq_cst);
#endif
}

#endif

static void ced_execute_range(ced_Worker *worker, size_t begin, size_t end) {
	ced_WorkerPool *pool = worker->pool;
	for (size_t index = begin; index < end; index++) {
		const ced_Job *job = &pool->jobs[index];
		ced_Batch batch = *job->source;
		batch.offset += job->offset;
		batch.entities += job->offset;
		batch.count = job->count;
		batch.column_ptrs = worker->columns;
		for (size_t c = 0; c < batch.query->component_count; c++)
			worker->columns[c] = job->source->column_ptrs[c]
									 ? (unsigned char *)job->source->column_ptrs[c] +
										   job->offset * batch.query->_field_sizes[c]
									 : NULL;
		pool->fn(pool->world, &batch, pool->context);
	}
}
static void ced_execute_serial_unbound(ced_Worker *worker) {
	ced_WorkerPool *pool = worker->pool;
    ced_BatchFn fn = pool->fn;
    ced_World *world = pool->world;
    void *context = pool->context;
	size_t index = 0;
	while (index < pool->job_count) {
		const ced_Batch *source = pool->jobs[index].source;
		ced_Batch batch = *source;
		batch.column_ptrs = worker->columns;
        // Update systems often bind two fields. Keep their strides and cursors
        // in registers while passing checked batches to callbacks and honoring
        // the requested maximum grain. Other field counts use the loop.
        if (batch.query->component_count == 2 && source->column_ptrs[0] && source->column_ptrs[1]) {
            unsigned char *first = source->column_ptrs[0], *second = source->column_ptrs[1];
            size_t first_size = batch.query->_field_sizes[0], second_size = batch.query->_field_sizes[1];
            do {
                size_t count = pool->jobs[index].count;
                batch.count = count;
                worker->columns[0] = first; worker->columns[1] = second;
                fn(world, &batch, context);
                index++;
                batch.offset += count; batch.entities += count;
                first += count * first_size; second += count * second_size;
            } while (index < pool->job_count && pool->jobs[index].source == source);
            continue;
        }
		for (size_t c = 0; c < batch.query->component_count; c++)
			worker->columns[c] = source->column_ptrs[c];
		for (;;) {
			batch.count = pool->jobs[index].count;
			fn(world, &batch, context);
			index++;
			if (index == pool->job_count || pool->jobs[index].source != source)
				break;
			batch.offset += batch.count;
			batch.entities += batch.count;
			for (size_t c = 0; c < batch.query->component_count; c++)
				if (worker->columns[c])
					worker->columns[c] =
						(unsigned char *)worker->columns[c] +
						batch.count * batch.query->_field_sizes[c];
		}
	}
}
static void ced_execute_serial(ced_Worker *worker) {
    ced_WorkerPool *pool = worker->pool;
    if (!pool->serial_jobs) { ced_execute_serial_unbound(worker); return; }
    ced_BatchFn fn = pool->fn;
    ced_World *world = pool->world;
    void *context = pool->context;
    const unsigned char *job = pool->serial_jobs;
    for (size_t i = 0; i < pool->job_count; i++, job += pool->serial_stride)
        fn(world, (const ced_Batch *)(const void *)job, context);
}
static ced_Worker *ced_worker_at(ced_WorkerPool *pool, size_t index) {
	return index ? &pool->workers[index - 1] : &pool->caller;
}
static bool ced_claim(ced_Worker *queue, size_t amount, size_t *begin, size_t *end) {
	size_t first = atomic_load_explicit(&queue->next, memory_order_relaxed);
	while (first < queue->end) {
		size_t count = queue->end - first;
		if (count > amount)
			count = amount;
		size_t last = first + count;
		if (atomic_compare_exchange_weak_explicit(&queue->next, &first, last,
												  memory_order_relaxed,
												  memory_order_relaxed)) {
			*begin = first;
			*end = last;
			return true;
		}
	}
	return false;
}
static void ced_execute_jobs(ced_Worker *worker) {
	ced_WorkerPool *pool = worker->pool;
	size_t columns = pool->job_count ? pool->jobs[0].source->query->component_count : 0;
	if (columns > worker->column_capacity) {
		worker->column_capacity = ced_capacity(worker->column_capacity, columns);
		worker->columns =
			ced_xrealloc(worker->columns, worker->column_capacity, sizeof(void *));
	}
	if (!pool->thread_count) {
		ced_execute_serial(worker);
		return;
	}
	size_t begin, end;
	while (ced_claim(worker, pool->claim_size, &begin, &end))
		ced_execute_range(worker, begin, end);
	// Workers normally use private queues. Only the balancing tail uses CAS
	// across workers, avoiding contention on a shared counter for every slice.
	size_t total = pool->thread_count + 1;
	for (size_t offset = 1; offset < total; offset++) {
		size_t victim = (worker->index + offset) % total;
		ced_Worker *queue = ced_worker_at(pool, victim);
		while (ced_claim(queue, pool->claim_size, &begin, &end))
			ced_execute_range(worker, begin, end);
	}
}

#if defined(_WIN32)
static DWORD WINAPI ced_worker_main(LPVOID context) {
#else
static void *ced_worker_main(void *context) {
#endif
	ced_Worker *worker = context;
	ced_WorkerPool *pool = worker->pool;
	uint64_t seen = 0;
	for (;;) {
		uint64_t generation =
			atomic_load_explicit(&pool->generation, memory_order_acquire);
		if (atomic_load_explicit(&pool->stop, memory_order_relaxed))
			break;
		if (generation != seen) {
			ced_execute_jobs(worker);
			seen = generation;
			if (atomic_fetch_sub_explicit(&pool->remaining, 1, memory_order_seq_cst) ==
					1 &&
				atomic_load_explicit(&pool->owner_waiting, memory_order_seq_cst)) {
				ced_lock(&pool->mutex);
				ced_cond_broadcast(&pool->done);
				ced_unlock(&pool->mutex);
			}
			continue;
		}
		// Short repeated dispatches avoid a kernel park/wake on every frame.
		// Idle workers still park after a bounded, configurable spin budget.
#if ced_worker_spin_us > 0
		uint64_t deadline = ced_worker_now() + (uint64_t)ced_worker_spin_us * 1000;
		do {
			for (int i = 0; i < 64; i++) {
				generation =
					atomic_load_explicit(&pool->generation, memory_order_acquire);
				if (generation != seen ||
					atomic_load_explicit(&pool->stop, memory_order_relaxed))
					break;
				ced_relax();
			}
			if (generation != seen ||
				atomic_load_explicit(&pool->stop, memory_order_relaxed))
				break;
		} while (ced_worker_now() < deadline);
		if (generation != seen)
			continue;
#endif
		ced_lock(&pool->mutex);
		while (!atomic_load_explicit(&pool->stop, memory_order_relaxed) &&
			   atomic_load_explicit(&pool->generation, memory_order_acquire) == seen) {
			pool->parked++;
			ced_cond_wait(&pool->wake, &pool->mutex);
			pool->parked--;
		}
		ced_unlock(&pool->mutex);
	}
	return 0;
}

static ced_WorkerPool *ced_pool_new(ced_World *w, size_t total) {
	ced_WorkerPool *pool = ced_aligned_alloc(sizeof(*pool), _Alignof(ced_WorkerPool));
	memset(pool, 0, sizeof(*pool));
	pool->world = w;
	pool->caller.pool = pool;
	atomic_init(&pool->caller.next, 0);
	atomic_init(&pool->generation, 0);
	atomic_init(&pool->remaining, 0);
	atomic_init(&pool->stop, false);
	atomic_init(&pool->owner_waiting, false);
	ced_mutex_init(&pool->mutex);
	ced_cond_init(&pool->wake);
	ced_cond_init(&pool->done);
	pool->workers =
		total > 1 ? ced_aligned_alloc(ced_bytes(total - 1, sizeof(*pool->workers)),
									  _Alignof(ced_Worker))
				  : NULL;
	if (total > 1)
		memset(pool->workers, 0, (total - 1) * sizeof(*pool->workers));
	return pool;
}
static void ced_pool_drop(ced_WorkerPool *pool) {
	if (!pool)
		return;
	ced_lock(&pool->mutex);
	atomic_store_explicit(&pool->stop, true, memory_order_relaxed);
	ced_cond_broadcast(&pool->wake);
	ced_unlock(&pool->mutex);
	for (size_t i = 0; i < pool->thread_count; i++) {
#if defined(_WIN32)
		WaitForSingleObject(pool->workers[i].thread, INFINITE);
		CloseHandle(pool->workers[i].thread);
#else
		pthread_join(pool->workers[i].thread, NULL);
#endif
		free(pool->workers[i].columns);
	}
	free(pool->caller.columns);
	ced_aligned_free(pool->workers);
	ced_cond_free(&pool->wake);
	ced_cond_free(&pool->done);
	ced_mutex_free(&pool->mutex);
	ced_aligned_free(pool);
}
void ced_workers_free(ced_World *w) {
	ced_pool_drop(w->_runtime->pool);
	w->_runtime->pool = NULL;
}
size_t ced_thread_count(const ced_World *w) {
	return w && w->_runtime->pool ? w->_runtime->pool->thread_count + 1 : 1;
}
bool ced_set_threads(ced_World *w, size_t total) {
	if (!ced_mutable(w) || total < 1 || total > 256)
		return false;
	if (total == ced_thread_count(w))
		return true;
	ced_WorkerPool *pool = ced_pool_new(w, total);
	for (size_t i = 0; i + 1 < total; i++) {
		ced_Worker *worker = &pool->workers[i];
		worker->pool = pool;
		worker->index = i + 1;
		atomic_init(&worker->next, 0);
#if defined(_WIN32)
		worker->thread = CreateThread(NULL, 0, ced_worker_main, worker, 0, NULL);
		bool failed = worker->thread == NULL;
#else
		bool failed =
			pthread_create(&worker->thread, NULL, ced_worker_main, worker) != 0;
#endif
		if (failed) {
			ced_pool_drop(pool);
			return false;
		}
		pool->thread_count++;
	}
	ced_workers_free(w);
	w->_runtime->pool = pool;
	return true;
}

static void ced_prepare_jobs(ced_WorkerPool *pool, ced_View *view, size_t grain) {
	if (view->_dispatch_revision != view->world->storage_revision ||
		view->_dispatch_grain != grain) {
		view->_dispatch_count = 0;
		for (size_t i = 0; i < view->match_count; i++) {
			const ced_Batch *source = &view->matches[i];
			for (size_t offset = 0; offset < source->count;) {
				size_t count = source->count - offset;
				if (count > grain)
					count = grain;
				if (view->_dispatch_count == view->_dispatch_capacity) {
					view->_dispatch_capacity = ced_capacity(view->_dispatch_capacity,
					                                        view->_dispatch_count + 1);
					view->_dispatch_jobs = ced_xrealloc(view->_dispatch_jobs,
					    view->_dispatch_capacity, sizeof(*view->_dispatch_jobs));
				}
				view->_dispatch_jobs[view->_dispatch_count++] = (ced_Job){source, offset, count};
				offset += count;
			}
		}
		view->_dispatch_revision = view->world->storage_revision;
		view->_dispatch_grain = grain;
	}
    if (!pool->thread_count) {
        pool->serial_jobs = NULL;
        bool stable = view->_serial_observed_revision == view->world->storage_revision &&
                      view->_serial_observed_grain == grain;
        if (stable) {
        if (view->_serial_revision != view->world->storage_revision || view->_serial_grain != grain) {
            size_t fields = ced_bytes(view->component_count, sizeof(void *));
            if (fields > SIZE_MAX - sizeof(ced_Batch)) abort();
            view->_serial_stride = sizeof(ced_Batch) + fields;
            size_t bytes = ced_bytes(view->_dispatch_count, view->_serial_stride);
            if (bytes > view->_serial_capacity) {
                view->_serial_jobs = ced_xrealloc(view->_serial_jobs, bytes, 1);
                view->_serial_capacity = bytes;
            }
            for (size_t i = 0; i < view->_dispatch_count; i++) {
                const ced_Job *job = &view->_dispatch_jobs[i];
                ced_Batch *batch = (ced_Batch *)(void *)(view->_serial_jobs + i * view->_serial_stride);
                *batch = *job->source;
                batch->offset += job->offset;
                batch->entities += job->offset;
                batch->count = job->count;
                batch->column_ptrs = (void **)(void *)(batch + 1);
                for (size_t c = 0; c < view->component_count; c++)
                    batch->column_ptrs[c] = job->source->column_ptrs[c] ?
                        (unsigned char *)job->source->column_ptrs[c] + job->offset * view->_field_sizes[c] : NULL;
            }
            view->_serial_revision = view->world->storage_revision;
            view->_serial_grain = grain;
        }
        pool->serial_jobs = view->_serial_jobs;
        pool->serial_stride = view->_serial_stride;
        }
        view->_serial_observed_revision = view->world->storage_revision;
        view->_serial_observed_grain = grain;
    }
	pool->jobs = view->_dispatch_jobs;
	pool->job_count = view->_dispatch_count;
	pool->claim_size = pool->job_count / ((pool->thread_count + 1) * 16);
	if (!pool->claim_size)
		pool->claim_size = 1;
	if (pool->claim_size > 4)
		pool->claim_size = 4;
}

enum { ced_single_batch_fields = 16 };
// Keep callback metadata and column scratch off the multi-job dispatch stack.
// The caller checks the field count before entry.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static void ced_execute_single_batch(ced_World *w, const ced_Batch *source,
                                      ced_BatchFn fn, void *context) {
    ced_Batch batch = *source;
    void *columns[ced_single_batch_fields];
    for (size_t c = 0; c < source->query->component_count; c++)
        columns[c] = source->column_ptrs[c];
    batch.column_ptrs = columns;
    fn(w, &batch, context);
}

static bool ced_parallel_dispatch(ced_View *view, size_t grain, bool automatic, ced_BatchFn fn, void *context) {
	if (!view || !fn)
		return false;
	ced_World *w = view->world;
	if (w->_runtime->indexed_writers)
		return false;
	bool expected = false;
	if (!atomic_compare_exchange_strong_explicit(&w->_runtime->parallel, &expected,
												 true, memory_order_acq_rel,
												 memory_order_acquire))
		return false;
	ced_view_update(view);
    if (automatic) {
        size_t threads = ced_thread_count(w);
        if (threads == 1) grain = SIZE_MAX;
        else {
            size_t rows = 0;
            for (size_t i = 0; i < view->match_count; i++) rows += view->matches[i].count;
            size_t parts = threads > SIZE_MAX / 8 ? SIZE_MAX : threads * 8;
            grain = rows / parts + (rows % parts != 0);
            if (grain < 64) grain = 64;
        }
    } else if (!grain) grain = 1024;
    // Run a single indivisible slice locally, honoring the requested grain,
    // mutation exclusion and final flush. No worker wakeup or default pool is
    // needed. Empty views still flush commands.
    if (!view->match_count) goto finished;
    if (view->match_count == 1 && view->matches[0].count <= grain &&
        view->component_count <= ced_single_batch_fields) {
        if (view->matches[0].count)
            ced_execute_single_batch(w, &view->matches[0], fn, context);
        goto finished;
    }
	if (!w->_runtime->pool)
		w->_runtime->pool = ced_pool_new(w, 1);
	ced_WorkerPool *pool = w->_runtime->pool;
	ced_prepare_jobs(pool, view, grain);
	pool->fn = fn;
	pool->context = context;
	if (pool->thread_count) {
		size_t total = pool->thread_count + 1;
		size_t span = pool->job_count / total, remainder = pool->job_count % total;
		size_t begin = 0;
		for (size_t i = 0; i < total; i++) {
			ced_Worker *queue = ced_worker_at(pool, i);
			size_t count = span + (i < remainder);
			queue->end = begin + count;
			atomic_store_explicit(&queue->next, begin, memory_order_relaxed);
			begin += count;
		}
		atomic_store_explicit(&pool->remaining, pool->thread_count,
							  memory_order_relaxed);
		// The same mutex protects the park predicate: a worker cannot go to
		// sleep between checking the old generation and publishing its wait.
		ced_lock(&pool->mutex);
		atomic_fetch_add_explicit(&pool->generation, 1, memory_order_release);
		if (pool->parked)
			ced_cond_broadcast(&pool->wake);
		ced_unlock(&pool->mutex);
	}
	ced_execute_jobs(&pool->caller);
	if (pool->thread_count) {
#if ced_worker_spin_us > 0
		uint64_t deadline = ced_worker_now() + (uint64_t)ced_worker_spin_us * 1000;
		do {
			for (int i = 0; i < 64; i++) {
				if (!atomic_load_explicit(&pool->remaining, memory_order_acquire))
					break;
				ced_relax();
			}
			if (!atomic_load_explicit(&pool->remaining, memory_order_acquire))
				break;
		} while (ced_worker_now() < deadline);
#endif
		if (atomic_load_explicit(&pool->remaining, memory_order_acquire)) {
			ced_lock(&pool->mutex);
			// SC store/load pairs with the last worker's SC decrement/load.
			// Either it sees owner_waiting or this check sees completion, so
			// skipping the wake on the fast path cannot lose a notification.
			atomic_store_explicit(&pool->owner_waiting, true, memory_order_seq_cst);
			while (atomic_load_explicit(&pool->remaining, memory_order_seq_cst))
				ced_cond_wait(&pool->done, &pool->mutex);
			atomic_store_explicit(&pool->owner_waiting, false, memory_order_seq_cst);
			ced_unlock(&pool->mutex);
		}
	}
finished:
	atomic_store_explicit(&w->_runtime->parallel, false, memory_order_release);
	ced_flush_commands(w);
	return true;
}

bool ced_parallel_each(ced_View *view, size_t grain, ced_BatchFn fn, void *context) {
    return ced_parallel_dispatch(view, grain, false, fn, context);
}
bool ced_parallel_each_auto(ced_View *view, ced_BatchFn fn, void *context) {
    return ced_parallel_dispatch(view, 0, true, fn, context);
}
