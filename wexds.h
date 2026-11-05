#ifndef DATSTR_H_INCLUDED
#define DATSTR_H_INCLUDED
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <kama/kama.h>

// try/catch macros using setjmp/longjmp:
//     TRY { ... } CATCH { ... } ETRY;
#define TRY                                                                            \
	do {                                                                               \
		jmp_buf ex_buf__;                                                              \
		if (!setjmp(ex_buf__)) {
#define CATCH                                                                          \
	}                                                                                  \
	else {
#define ETRY                                                                           \
	}                                                                                  \
	}                                                                                  \
	while (0)
#define THROW longjmp(ex_buf__, 1)

#define wex_malloc(Z) malloc(Z)
#define wex_realloc(X, Z) realloc(X, Z)
#define wex_free(Z) free(Z)

// Compatibility APIs backed by Kama; long string keys are owned by the map.
typedef uint64_t dhasht_key;
kama(wex_StringMap, void *)
kama_u32(wex_IntMap, void *)

typedef struct dhasht {
	wex_StringMap_t map;
	dhasht_key size; // allocated slots
	size_t count;
} dhasht;

dhasht *dhasht_init(dhasht_key size);
void *dhasht_get(dhasht *hash, const char *key);
void dhasht_remove_wdata(dhasht *hash, const char *key);
void dhasht_remove(dhasht *hash, const char *key);
void dhasht_resize(dhasht *hash, dhasht_key newsize);
void dhasht_insert(dhasht *hash, const char *key, void *value);
void dhasht_destroy_wdata(dhasht *hash);
void dhasht_destroy(dhasht *hash);

#define sffree(x) free(x)
static inline void wex_swap_bytes(void *a, void *b, size_t size) {
	unsigned char *x = a, *y = b;
	for (size_t i = 0; i < size; i++) {
		unsigned char t = x[i];
		x[i] = y[i];
		y[i] = t;
	}
}
#define wexswap(x, y) wex_swap_bytes(&(x), &(y), sizeof(x))
#define wexswap_p(x, y) wex_swap_bytes((x), (y), sizeof(*(x)))

typedef struct linked_node {
	void *data;
	struct linked_node *next;
} linked_node;
typedef struct linked_list {
	linked_node *head;
	size_t size;
} linked_list;
typedef struct list {
	void **elements;
	size_t count;
	size_t capacity;
} list;
typedef struct ihmap_kv {
	int key;
	void *value;
} ihmap_kv;

// Legacy integer map API; values remain caller-owned.
typedef struct ihmap {
	wex_IntMap_t map;
	size_t capacity, size;
} ihmap;
typedef struct bitset {
	uint8_t *bytes;
	size_t count;
} bitset;

typedef struct tableu16 {
	uint16_t *arr;
	size_t sizeX;
	size_t sizeY;
} tableu16;
typedef struct wex_stack {
	void **items;
	size_t top; // number of items
	size_t capacity;
} wex_stack;
typedef struct wex_stack_size_t {
	size_t *items;
	size_t top; // number of items
	size_t capacity;
} wex_stack_size_t;
typedef struct wex_queue {
	void **items;
	size_t front;
	size_t rear;
	size_t size; // capacity
	size_t count;
} wex_queue;

wex_stack *wex_stack_init(size_t start_size);
void wex_stack_push(wex_stack *st, void *item);
// Fixed-capacity push: ignored when full. Pop returns NULL when empty.
void wex_stack_push_s(wex_stack *st, void *item);
void *wex_stack_pop(wex_stack *st);
void wex_stack_free(wex_stack *st);

wex_stack_size_t *wex_stack_size_t_init(size_t start_size);
void wex_stack_size_t_push(wex_stack_size_t *st, size_t item);
void wex_stack_size_t_push_s(wex_stack_size_t *st, size_t item);
// Returns SIZE_MAX when empty.
size_t wex_stack_size_t_pop(wex_stack_size_t *st);
void wex_stack_size_t_free(wex_stack_size_t *st);

wex_queue *wex_queue_init(size_t start_size);
// Circular queue that grows as needed.
void wex_enqueue(wex_queue *queue, void *value);
void *wex_dequeue(wex_queue *queue);
// Fixed circular queue. Enqueuing when full overwrites an existing entry.
void wex_enqueue_c(wex_queue *queue, void *value);
void *wex_dequeue_c(wex_queue *queue);
// Dynamic queue without wraparound.
void wex_enqueue_d(wex_queue *queue, void *value);
void *wex_dequeue_d(wex_queue *queue);
void wex_queue_free(wex_queue *queue);
// Free the queue and every value still in it.
void wex_queue_full_free(wex_queue *queue);

tableu16 *tableu16_init(void);
uint16_t tableu16_get(tableu16 *table, size_t x, size_t y);
void tableu16_insert(tableu16 *table, uint16_t value, size_t x, size_t y);
void tableu16_insert_col(tableu16 *table, uint16_t value, size_t x);
void tableu16_free(tableu16 *table);

// uint64_t FNV1a_64(const char* key);
linked_node *add_node(linked_list *list, void *value);
void remove_node(linked_list *list, int id);
linked_node *get_node(linked_list *list, int id);
void *get_data(linked_list *list, int id);
// Pointer lists free elements on removal and in free_list. size reserves capacity.
// Linked lists free their nodes, but leave the values to the caller.
list *init_list(size_t size);
linked_list *init_llist(void);
// Returns the new element's index.
size_t push_back(list *llist, void *item);
bool contains(list *llist, int id);
void sort_list(list *llist);
void pop_back(list *llist);
void pop_at(list *llist, size_t index);
void free_list(list *llist);
void free_llist(linked_list *llist);

ihmap *ihmap_init(size_t capacity);
ihmap_kv *ihmap_create_pair(int key, void *value);
int ihash(int key, size_t capacity);
// Overwrites existing values. Returns true only when inserting a new key.
bool ihmap_insert(ihmap *map, int key, void *value);
void *ihmap_get(ihmap *map, int key);
bool ihmap_contains(ihmap *map, int key);
void ihmap_free(ihmap *map);

bitset *bitset_init(size_t byteSize);
void bitset_add(bitset *bm, size_t bytes);
bool bitset_get(bitset *bm, size_t index);
void bitset_set(bitset *bm, size_t index, bool value);
void bitset_free(bitset *bm);

#endif
