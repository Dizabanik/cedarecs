#ifndef CEDARECS_ACCESS_H
#define CEDARECS_ACCESS_H

// Storage metadata for the C/C++ inline read interface. It requires no C
// atomics, hashmap implementation, or C++ object construction.
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#ifdef __cplusplus
#include <array>
#include <bit>
#include <type_traits>
#endif

#if defined(__GNUC__) || defined(__clang__)
#define CEDARECS_PURE __attribute__((pure, nothrow))
#if defined(__clang__)
#define CEDARECS_ALLOCATOR __attribute__((malloc, returns_nonnull, nothrow))
#else
#define CEDARECS_ALLOCATOR __attribute__((returns_nonnull, nothrow))
#endif
#else
#define CEDARECS_PURE
#define CEDARECS_ALLOCATOR
#endif

typedef uint16_t ced_Component;
typedef uint64_t ced_Entity;
typedef uint32_t ced_ArchetypeId;
typedef struct ced_Archetype ced_Archetype;
typedef struct ced_World ced_World;
typedef struct ced_Column {
    uint8_t *data;
    size_t component_size;
} ced_Column;

// The first member of every archetype. C and C++ readers share this metadata
// without copying it or reinterpreting the world layout.
typedef struct ced_ArchetypeAccess {
    const ced_Column *columns;
    const ced_Component *column_for;
} ced_ArchetypeAccess;
typedef struct ced_Record {
    ced_Archetype *archetype;
    uint32_t row, generation;
} ced_Record;

// Optional entity-indexed storage keeps full owner handles separate from data.
// UINT64_MAX is absent: the engine never allocates entity index UINT32_MAX.
// This metadata object never moves; its allocations may grow between accesses.
typedef struct ced_IndexedAccess ced_IndexedAccess;
typedef struct ced_IndexedColumn {
    unsigned char *data;
    ced_Entity *owners;
    size_t capacity;
    ced_World *_world;
    ced_IndexedAccess *_accesses;
} ced_IndexedColumn;
enum { ced_indexed_partial_column_min_size = 128 };
typedef struct ced_IndexedReader {
    const ced_IndexedColumn *column;
    size_t size;
} ced_IndexedReader;
// Presence includes zero-sized tags; all 64 owner bits are checked.
static inline bool ced_indexed_contains(const ced_IndexedReader *reader, ced_Entity entity) {
    uint32_t index = (uint32_t)entity;
    const ced_IndexedColumn *column = reader ? reader->column : NULL;
    return column && index < column->capacity && column->owners[index] == entity;
}
static inline const void *ced_indexed_get(const ced_IndexedReader *reader, ced_Entity entity) {
    uint32_t index = (uint32_t)entity;
    const ced_IndexedColumn *column = reader ? reader->column : NULL;
    if (!column || index >= column->capacity || column->owners[index] != entity || !reader->size)
        return NULL;
    return column->data + (size_t)index * reader->size;
}

// Keep tracked access at a stable address. Storage growth updates data, owners
// and capacity. Zero-initialize before first init; init/copy may rebind an active
// node. Reset before its address expires; do not copy nodes with assignment.
struct ced_IndexedAccess {
    unsigned char *data;
    const ced_Entity *owners;
    size_t capacity;
    ced_IndexedAccess *_prev, *_next;
    const ced_IndexedColumn *_source;
};

typedef struct ced_ReadAccess {
    ced_Record *const *records;
    const size_t *entity_count;
    const size_t *component_count;
} ced_ReadAccess;

// Access is bound to its world's lifetime. It follows record-array growth and
// entity recycling; returned component pointers still expire on structural
// writes. Use the same synchronization rules as ced_get_component_const.
static inline const void *ced_read_get(const ced_ReadAccess *access,
                                      ced_Entity entity, ced_Component component) {
    uint32_t index = (uint32_t)entity;
    if (!access || !access->records || component >= *access->component_count ||
        index >= *access->entity_count)
        return NULL;
    const ced_Record *record = &(*access->records)[index];
    if (!record->archetype || record->generation != (uint32_t)(entity >> 32))
        return NULL;
    // A struct and its first member have the same address (checked by cedar.h).
    const ced_ArchetypeAccess *storage = (const ced_ArchetypeAccess *)(const void *)record->archetype;
    ced_Component col = storage->column_for[component];
    if (col == UINT16_MAX)
        return NULL;
    const ced_Column *column = &storage->columns[col];
    return column->component_size ? column->data + (size_t)record->row * column->component_size : NULL;
}

// A writable binding can only be obtained from a mutable world. It has the
// same synchronization and pointer lifetime rules as ced_get_component.
typedef struct ced_Access { ced_ReadAccess read; } ced_Access;
static inline void *ced_access_get(const ced_Access *access,
                                  ced_Entity entity, ced_Component component) {
    return (void *)ced_read_get(access ? &access->read : NULL, entity, component);
}

// Owner-only write scopes exclude worker dispatch until explicitly ended.
// Keep a scope at a stable address and do not copy active scopes or bindings.
typedef struct ced_IndexedWriter {
    ced_World *world;
    ced_ReadAccess access;
    uint64_t *revision;
    bool active;
} ced_IndexedWriter;
typedef struct ced_IndexedBinding {
    ced_IndexedWriter *writer;
    ced_IndexedReader reader;
    ced_Component component;
    ced_Archetype *with_component, *without_component;
    size_t *with_count, *without_count;
} ced_IndexedBinding;

#ifdef __cplusplus
extern "C" {
#endif
CEDARECS_PURE ced_ReadAccess ced_read_access(const ced_World *world);
CEDARECS_PURE ced_Access ced_access(ced_World *world);
CEDARECS_PURE ced_IndexedReader ced_indexed_reader(const ced_World *world, ced_Component component);
void ced_indexed_access_init(ced_IndexedAccess *access, ced_IndexedReader reader);
void ced_indexed_access_copy(ced_IndexedAccess *access, const ced_IndexedAccess *source);
void ced_indexed_access_reset(ced_IndexedAccess *access);
ced_IndexedWriter ced_indexed_write_begin(ced_World *world);
bool ced_indexed_write_end(ced_IndexedWriter *writer);
CEDARECS_PURE ced_IndexedBinding ced_indexed_bind(ced_IndexedWriter *writer, ced_Component component);
// Explicitly reserve storage and cache the empty/single-component transition.
// This may invalidate borrowed pointers, like other reserve operations.
bool ced_indexed_prepare(ced_IndexedBinding *binding);
void *ced_indexed_add_cold(ced_IndexedBinding *binding, ced_Entity entity);
void ced_indexed_remove_cold(ced_IndexedBinding *binding, ced_Entity entity);
// Handles aliased input, including when a scoped put needs to grow storage.
bool ced_add_set_component(ced_World *world, ced_Entity entity, ced_Component component,
                           const void *value);
#ifdef __cplusplus
}
#endif

static inline ced_Record *ced_indexed_write_record(const ced_IndexedBinding *binding,
                                                   ced_Entity entity) {
    if (!binding || !binding->writer || !binding->writer->active || !binding->reader.column)
        return NULL;
    const ced_ReadAccess *access = &binding->writer->access;
    uint32_t index = (uint32_t)entity;
    if (index >= *access->entity_count)
        return NULL;
    ced_Record *rec = &(*access->records)[index];
    return rec->archetype && rec->generation == (uint32_t)(entity >> 32) ? rec : NULL;
}
// Add is idempotent; new data is zeroed. Existing data is preserved. Tags have
// presence but return NULL. Binding caches do not bypass generation checks.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
static inline void *ced__indexed_add_record(ced_IndexedBinding *binding, ced_Entity entity,
                                           ced_Record *rec, size_t size, bool clear) {
    if (size >= ced_indexed_partial_column_min_size &&
        (uint32_t)entity >= binding->reader.column->capacity)
        return ced_indexed_add_cold(binding, entity);
    if (rec->archetype == binding->without_component) {
        // Prepared transitions cannot relocate storage. Read the destination
        // before writing metadata so possible aliases do not force a reload
        // of the column binding.
        const ced_IndexedColumn *column = binding->reader.column;
        void *out = size ? column->data + (size_t)(uint32_t)entity * size : NULL;
        rec->archetype = binding->with_component;
        (*binding->without_count)--; (*binding->with_count)++;
        column->owners[(uint32_t)entity] = entity;
        (*binding->writer->revision)++;
        if (clear && size)
            memset(out, 0, size);
        return out;
    } else if (rec->archetype != binding->with_component)
        return ced_indexed_add_cold(binding, entity);
    return size ? binding->reader.column->data + (size_t)(uint32_t)entity * size : NULL;
}
static inline void *ced__indexed_add_size(ced_IndexedBinding *binding, ced_Entity entity, size_t size, bool clear) {
    ced_Record *rec = ced_indexed_write_record(binding, entity);
    return rec ? ced__indexed_add_record(binding, entity, rec, size, clear) : NULL;
}
static inline void *ced_indexed_add(ced_IndexedBinding *binding, ced_Entity entity) {
    return ced__indexed_add_size(binding, entity, binding ? binding->reader.size : 0, true);
}
static inline bool ced__indexed_put_size(ced_IndexedBinding *binding, ced_Entity entity,
                                        const void *value, size_t size) {
    ced_Record *rec = ced_indexed_write_record(binding, entity);
    if (!rec || (size && !value)) return false;
    // A live record in the prepared signature already owns this column row.
    // No storage growth or structural transition is needed for replacement.
    if (rec->archetype == binding->with_component) {
        if (size)
            memmove(binding->reader.column->data + (size_t)(uint32_t)entity * size, value, size);
        return true;
    }
    // Input may point into a column that growth will move. The general path
    // saves it before growing; prepared writes stay inline.
    if (size >= ced_indexed_partial_column_min_size &&
        (uint32_t)entity >= binding->reader.column->capacity)
        return ced_add_set_component(binding->writer->world, entity, binding->component, value);
    void *out = ced__indexed_add_record(binding, entity, rec, size, false);
    if (!size) return true;
    if (!out) return false;
    memmove(out, value, size);
    return true;
}
// Checked scoped put. Null values are accepted only for zero-sized tags.
static inline bool ced_indexed_put(ced_IndexedBinding *binding, ced_Entity entity, const void *value) {
    return ced__indexed_put_size(binding, entity, value, binding ? binding->reader.size : 0);
}
static inline void ced_indexed_remove(ced_IndexedBinding *binding, ced_Entity entity) {
    ced_Record *rec = ced_indexed_write_record(binding, entity);
    if (!rec)
        return;
    if (rec->archetype == binding->with_component) {
        rec->archetype = binding->without_component;
        (*binding->with_count)--; (*binding->without_count)++;
        binding->reader.column->owners[(uint32_t)entity] = UINT64_MAX;
        (*binding->writer->revision)++;
    } else if (rec->archetype != binding->without_component)
        ced_indexed_remove_cold(binding, entity);
}

#ifdef __cplusplus
// Checked type bindings resolve the stride once and keep it constant on reads.
// Return bytes so callers can memcpy into a live C++ object.
template<class T> class ced_IndexedRead {
    inline static const ced_Entity empty_owner_ = UINT64_MAX;
    mutable ced_IndexedAccess access_{nullptr, &empty_owner_, 0, nullptr, nullptr, nullptr};
public:
    ced_IndexedRead() = default;
    explicit ced_IndexedRead(ced_IndexedReader reader) {
        if (reader.column && reader.size == sizeof(T)) ced_indexed_access_init(&access_, reader);
    }
    ~ced_IndexedRead() { if (access_._source) ced_indexed_access_reset(&access_); }
    ced_IndexedRead(const ced_IndexedRead &other) { ced_indexed_access_copy(&access_, &other.access_); }
    ced_IndexedRead(ced_IndexedRead &&other) noexcept {
        ced_indexed_access_copy(&access_, &other.access_);
        ced_indexed_access_reset(&other.access_);
    }
    ced_IndexedRead &operator=(const ced_IndexedRead &other) {
        if (this != &other) { ced_indexed_access_reset(&access_); ced_indexed_access_copy(&access_, &other.access_); }
        return *this;
    }
    ced_IndexedRead &operator=(ced_IndexedRead &&other) noexcept {
        if (this != &other) { ced_indexed_access_reset(&access_); ced_indexed_access_copy(&access_, &other.access_); ced_indexed_access_reset(&other.access_); }
        return *this;
    }
    const void *get(ced_Entity entity) const {
        if (!access_.data) return nullptr;
        uint32_t index = (uint32_t)entity;
        size_t bounded = index < access_.capacity ? index : 0;
        if (!((index < access_.capacity) & (access_.owners[bounded] == entity))) return nullptr;
        return access_.data + (size_t)index * sizeof(T);
    }
    // Explicitly unchecked; callers guarantee live, present, synchronized data.
    T value_unchecked(ced_Entity entity) const {
        static_assert(std::is_trivially_copyable_v<T>);
        std::array<unsigned char, sizeof(T)> bytes;
        memcpy(bytes.data(), access_.data + (size_t)(uint32_t)entity * sizeof(T), sizeof(T));
        return std::bit_cast<T>(bytes);
    }
    T value(ced_Entity entity) const {
        static_assert(std::is_trivially_copyable_v<T>);
        uint32_t index = (uint32_t)entity;
        if (index >= access_.capacity || access_.owners[index] != entity) abort();
        std::array<unsigned char, sizeof(T)> bytes;
        memcpy(bytes.data(), access_.data + (size_t)index * sizeof(T), sizeof(T));
        return std::bit_cast<T>(bytes);
    }
};
template<class T> class ced_IndexedMutate {
    ced_IndexedBinding *const binding_ = nullptr;
public:
    ced_IndexedMutate() = default;
    explicit ced_IndexedMutate(ced_IndexedBinding &binding) :
        binding_(binding.reader.size == sizeof(T) ? &binding : nullptr) {}
    void *add(ced_Entity entity) const { return ced__indexed_add_size(binding_, entity, sizeof(T), true); }
    bool put(ced_Entity entity, const T &value) const {
        static_assert(std::is_trivially_copyable_v<T>);
        return ced__indexed_put_size(binding_, entity, &value, sizeof(T));
    }
    void remove(ced_Entity entity) const { ced_indexed_remove(binding_, entity); }
};
#endif
#endif
