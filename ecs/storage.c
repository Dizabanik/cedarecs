#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif
#include "internal.h"
#if CEDARECS_VM_STORAGE
#include <sys/mman.h>
#include <unistd.h>
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CEDARECS_STORAGE_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(CEDARECS_STORAGE_ASAN)
#include <sanitizer/asan_interface.h>
#define ced_storage_poison(p,n) __asan_poison_memory_region((p),(n))
#define ced_storage_unpoison(p,n) __asan_unpoison_memory_region((p),(n))
#else
#define ced_storage_poison(p,n) ((void)0)
#define ced_storage_unpoison(p,n) ((void)0)
#endif
enum { ced_storage_threshold = CEDARECS_VM_THRESHOLD };
typedef struct ced_VirtualStorage {
    void *mapping;
    size_t mapped_bytes, available, logical_bytes, high_water_bytes;
} ced_VirtualStorage;
static size_t ced_storage_native_backing(size_t size) {
    if (size < ced_storage_threshold / 2) return size;
    size_t backing=ced_capacity(0,size);
    // A small prefix for alignment or cache coloring should not double a
    // power-of-two allocation. Larger increases still use geometric growth.
    size_t previous=backing/2;
    return size-previous<=8192 ? size : backing;
}
static ced_VirtualStorage *ced_storage_header(const void *p) {
    return (ced_VirtualStorage *)((unsigned char *)p - sizeof(ced_VirtualStorage));
}
static void *ced_storage_map(size_t size, size_t alignment) {
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    if (!page || (page & (page - 1))) abort();
    if (alignment < ced_align) alignment = ced_align;
    if (alignment < page) alignment = page;
    size_t available = 256u * 1024u * 1024u;
    while (available < size) {
        if (available > SIZE_MAX / 2) { available = size; break; }
        available *= 2;
    }
    if (available > SIZE_MAX - alignment - page) abort();
    size_t total = (available + alignment + page - 1) & ~(page - 1);
    void *base = mmap(NULL, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        // If the spare address range cannot be reserved, try just the requested size.
        if (size > SIZE_MAX - alignment - page) abort();
        total = (size + alignment + page - 1) & ~(page - 1);
        base = mmap(NULL, total, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED) abort();
    }
    uintptr_t value = ((uintptr_t)base + sizeof(ced_VirtualStorage) + alignment - 1) & ~(uintptr_t)(alignment - 1);
    void *data = (void *)value;
    size_t prefix = (size_t)((unsigned char *)data - (unsigned char *)base);
    ced_VirtualStorage *h = ced_storage_header(data);
    *h = (ced_VirtualStorage){base, total, total - prefix, size, size};
    // Fresh reserved pages are demand-zero. Unused logical rows are not yet
    // resident; ASan still rejects access past the current logical allocation.
    ced_storage_poison((unsigned char *)data + size, h->available - size);
    return data;
}
static void ced_storage_unmap(void *p) {
    ced_VirtualStorage h = *ced_storage_header(p);
    ced_storage_unpoison(h.mapping, h.mapped_bytes);
    if (munmap(h.mapping, h.mapped_bytes)) abort();
}
#endif
void *ced_storage_resize(void *old, size_t old_size, size_t live_size,
                         size_t size, size_t alignment) {
    if (live_size > old_size || (live_size && !old)) abort();
    if (!size) { ced_storage_free(old, old_size); return NULL; }
    if (live_size > size) live_size = size;
#if CEDARECS_VM_STORAGE
    bool mapped = old && old_size >= ced_storage_threshold;
    if (mapped) {
        ced_VirtualStorage *h = ced_storage_header(old);
        if (h->logical_bytes != old_size) abort();
        if (size >= ced_storage_threshold && size <= h->available &&
            !((uintptr_t)old & (alignment - 1))) {
            if (size > old_size) ced_storage_unpoison((unsigned char *)old + old_size, size - old_size);
            else ced_storage_poison((unsigned char *)old + size, old_size - size);
            h->logical_bytes = size;
            if(size>h->high_water_bytes)h->high_water_bytes=size;
            return old;
        }
    }
    if (size >= ced_storage_threshold || mapped) {
        void *data = size >= ced_storage_threshold ? ced_storage_map(size, alignment) : ced_aligned_alloc(size, alignment);
        if (live_size > size) live_size = size;
        if (live_size) memcpy(data, old, live_size);
        if (mapped) ced_storage_unmap(old);
        else ced_aligned_free(old);
        return data;
    }
#endif
#if CEDARECS_VM_STORAGE
    // Native buffers keep geometric backing below the mapping threshold.
    // Logical growth can stay bounded without repeatedly copying small data.
    size_t backing = ced_storage_native_backing(size);
    size_t old_backing = old ? ced_storage_native_backing(old_size) : 0;
    if (old && backing == old_backing && !((uintptr_t)old & (alignment - 1))) {
        if (size > old_size) ced_storage_unpoison((unsigned char *)old + old_size, size - old_size);
        else ced_storage_poison((unsigned char *)old + size, old_size - size);
        return old;
    }
    if (old) ced_storage_unpoison(old, old_backing);
    void *data = ced_aligned_resize(old, live_size, backing, alignment);
    ced_storage_poison((unsigned char *)data + size, backing - size);
    return data;
#else
    return ced_aligned_resize(old, live_size, size, alignment);
#endif
}
void *ced_storage_resize_zero(void *old, size_t old_size, size_t live_size,
                              size_t size, size_t alignment) {
    if (live_size > old_size || (live_size && !old)) abort();
    if (live_size > size) live_size = size;
    size_t clear_until=size;
#if CEDARECS_VM_STORAGE
    if (!old && size && size < ced_storage_threshold) {
        size_t backing = ced_storage_native_backing(size);
        void *data = ced_aligned_resize_zero(NULL, 0, backing, alignment);
        ced_storage_poison((unsigned char *)data + size, backing - size);
        return data;
    }
    if(size>=ced_storage_threshold) {
        bool fresh=!old || old_size<ced_storage_threshold;
        if(!fresh) {
            const ced_VirtualStorage *h=ced_storage_header(old);
            fresh=size>h->available || ((uintptr_t)old&(alignment-1));
            if(!fresh && clear_until>h->high_water_bytes)clear_until=h->high_water_bytes;
        }
        // Anonymous mappings and never-published spare pages are demand-zero.
        // Previously published bytes can be dirty after a shrink, so retain
        // the high-water mark and clear those bytes before exposing them again.
        if(fresh)clear_until=live_size;
    }
#else
    return ced_aligned_resize_zero(old, live_size, size, alignment);
#endif
    void *data = ced_storage_resize(old, old_size, live_size, size, alignment);
    if (clear_until > live_size) memset((unsigned char *)data + live_size, 0, clear_until - live_size);
    return data;
}
void ced_storage_free(void *p, size_t size) {
    if (!p) return;
#if CEDARECS_VM_STORAGE
    if (size >= ced_storage_threshold) { ced_storage_unmap(p); return; }
#else
    (void)size;
#endif
    ced_aligned_free(p);
}
size_t ced_storage_virtual_bytes(const void *p, size_t size) {
#if CEDARECS_VM_STORAGE
    if (p && size >= ced_storage_threshold) return ced_storage_header(p)->mapped_bytes;
#else
    (void)p; (void)size;
#endif
    return 0;
}
size_t ced_storage_logical_bytes(const void *p, size_t size) {
#if CEDARECS_VM_STORAGE
    if (p && size >= ced_storage_threshold) {
        const ced_VirtualStorage *h = ced_storage_header(p);
        return (size_t)((const unsigned char *)p - (const unsigned char *)h->mapping) + size;
    }
#else
    (void)p; (void)size;
#endif
    return 0;
}
