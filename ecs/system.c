#include "internal.h"

void ced_add_start_system(ced_World *w, ced_SystemFn sys) {
	if (!ced_mutable(w) || !sys || w->start_systems_size >= INT_MAX)
		return;
	if (w->start_systems_size == w->_start_capacity) {
		w->_start_capacity =
			ced_capacity(w->_start_capacity, w->start_systems_size + 1);
		w->start_systems = ced_xrealloc(w->start_systems, w->_start_capacity,
										sizeof(*w->start_systems));
	}
	w->start_systems[w->start_systems_size++] = sys;
}

static void ced_register_system(ced_World *w, ced_SystemFn sys, double interval,
								bool fixed) {
	if (!ced_mutable(w) || !sys || !isfinite(interval) || interval < 0)
		return;
	ced_SystemRecord **records = fixed ? &w->fixed_systems : &w->update_systems;
	size_t *count = fixed ? &w->fixed_systems_size : &w->update_systems_size;
	size_t *capacity = fixed ? &w->_fixed_capacity : &w->_update_capacity;
	if (*count >= INT_MAX)
		return;
	if (*count == *capacity) {
		*capacity = ced_capacity(*capacity, *count + 1);
		*records = ced_xrealloc(*records, *capacity, sizeof(**records));
	}
	(*records)[(*count)++] = (ced_SystemRecord){.func = sys, .interval = interval};
}

void ced_add_update_system(ced_World *w, ced_SystemFn sys) {
	ced_register_system(w, sys, 0, false);
}
void ced_add_update_system_capped(ced_World *w, ced_SystemFn sys, double fps) {
	if (!isfinite(fps) || fps < 0)
		return;
	ced_register_system(w, sys, fps > 0 ? 1.0 / fps : 0, false);
}
void ced_add_fixed_system(ced_World *w, ced_SystemFn sys, double step) {
	if (step > 0)
		ced_register_system(w, sys, step, true);
}

ced_World *ced_add_systems_impl(ced_World *w, int stage, ...) {
	if (!ced_mutable(w))
		return w;
	va_list args;
	va_start(args, stage);
	size_t i = 0;
	for (ced_SystemFn sys = va_arg(args, ced_SystemFn); sys;
		 sys = va_arg(args, ced_SystemFn), i++) {
		switch (stage) {
		case ced_stage_plugin:
			sys(w, (int)i, 0);
			break;
		case ced_stage_start:
			ced_add_start_system(w, sys);
			break;
		case ced_stage_update:
			ced_add_update_system(w, sys);
			break;
		case ced_stage_fixed_update:
			ced_add_fixed_system(w, sys, 1.0 / 60.0);
			break;
		}
	}
	va_end(args);
	return w;
}

void ced_custom_update(ced_World *w, void (*func)(ced_World *)) {
	if (ced_mutable(w))
		w->update = func;
}
void ced_custom_start(ced_World *w, void (*func)(ced_World *)) {
	if (ced_mutable(w))
		w->start = func;
}

void ced_start(ced_World *w) {
	if (!ced_mutable(w))
		return;
	size_t count = w->start_systems_size;
	for (size_t i = 0; i < count; i++)
		w->start_systems[i](w, (int)i, 0);
	ced_flush_commands(w);
}

void ced_update_frame(ced_World *w, double dt) {
	if (!ced_mutable(w) || !isfinite(dt) || dt < 0)
		return;
	size_t count = w->fixed_systems_size;
	for (size_t i = 0; i < count; i++) {
		ced_SystemRecord *rec = &w->fixed_systems[i];
		rec->accumulator += dt;
		size_t steps = 0;
		while (w->fixed_systems[i].accumulator >= w->fixed_systems[i].interval &&
			   steps < ced_max_fixed_steps) {
			ced_SystemFn fn = w->fixed_systems[i].func;
			double interval = w->fixed_systems[i].interval;
			w->fixed_systems[i].accumulator -= interval;
			fn(w, (int)i, interval);
			steps++;
		}
		rec = &w->fixed_systems[i]; // callbacks may grow the systems array
		if (rec->accumulator >= rec->interval)
			rec->accumulator = fmod(rec->accumulator, rec->interval);
	}
	count = w->update_systems_size;
	for (size_t i = 0; i < count; i++) {
		ced_SystemRecord *rec = &w->update_systems[i];
		ced_SystemFn fn = rec->func;
		if (rec->interval == 0)
			fn(w, (int)i, dt);
		else {
			rec->accumulator += dt;
			rec->elapsed += dt;
			if (rec->accumulator >= rec->interval) {
				double elapsed = rec->elapsed;
				rec->elapsed = 0;
				rec->accumulator = fmod(rec->accumulator, rec->interval);
				fn(w, (int)i, elapsed);
			}
		}
	}
	ced_flush_commands(w);
}

static double ced_now(void) {
#if defined(_WIN32)
	LARGE_INTEGER counter, frequency;
	QueryPerformanceCounter(&counter);
	QueryPerformanceFrequency(&frequency);
	return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

void ced_default_update(ced_World *w) {
	double last = ced_now();
	while (!atomic_load_explicit(&w->ecs_quit, memory_order_relaxed)) {
		double now = ced_now();
		ced_update_frame(w, now - last);
		last = now;
#ifdef USE_FPS_DELAY
		if (isfinite(w->targetFPS) && w->targetFPS > 0) {
			double delay = 1.0 / w->targetFPS - (ced_now() - now);
			if (delay > 0) {
#if defined(_WIN32)
				Sleep((DWORD)(delay * 1000));
#else
				struct timespec pause = {(time_t)delay,
										 (long)((delay - (time_t)delay) * 1e9)};
				nanosleep(&pause, NULL);
#endif
			}
		}
#endif
	}
}

void ced_run_with(ced_World *w, void (*before_start)(void),
				  void (*before_update)(void)) {
	if (!ced_mutable(w))
		return;
	if (before_start)
		before_start();
	if (w->start)
		w->start(w);
	if (before_update)
		before_update();
	if (w->update)
		w->update(w);
}
void ced_run(ced_World *w) { ced_run_with(w, NULL, NULL); }
