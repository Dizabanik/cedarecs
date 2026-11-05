#include "wexds.h"
#include <limits.h>
#include <string.h>

static size_t wex_bytes(size_t count, size_t size) {
	if (size && count > SIZE_MAX / size)
		abort();
	return count * size;
}
static void *wex_alloc(size_t count, size_t size) {
	size_t bytes = wex_bytes(count, size);
	void *p = wex_malloc(bytes ? bytes : 1);
	if (!p)
		abort();
	memset(p, 0, bytes);
	return p;
}
static void *wex_resize(void *old, size_t count, size_t size) {
	size_t bytes = wex_bytes(count, size);
	void *p = wex_realloc(old, bytes ? bytes : 1);
	if (!p)
		abort();
	return p;
}
static size_t wex_capacity(size_t current, size_t needed) {
	size_t cap = current ? current : 8;
	while (cap < needed) {
		if (cap > SIZE_MAX / 2)
			return needed;
		cap *= 2;
	}
	return cap;
}

linked_list *init_llist(void) { return wex_alloc(1, sizeof(linked_list)); }
linked_node *add_node(linked_list *list, void *value) {
	if (!list)
		return NULL;
	linked_node *node = wex_alloc(1, sizeof(*node));
	node->data = value;
	node->next = list->head;
	list->head = node;
	list->size++;
	return node;
}
linked_node *get_node(linked_list *list, int id) {
	for (linked_node *node = list ? list->head : NULL; node; node = node->next)
		if (node->data && *(int *)node->data == id)
			return node;
	return NULL;
}
void *get_data(linked_list *list, int id) {
	linked_node *node = get_node(list, id);
	return node ? node->data : NULL;
}
void remove_node(linked_list *list, int id) {
	if (!list)
		return;
	linked_node **slot = &list->head;
	while (*slot && (!(*slot)->data || *(int *)(*slot)->data != id))
		slot = &(*slot)->next;
	if (*slot) {
		linked_node *node = *slot;
		*slot = node->next;
		free(node);
		list->size--;
	}
}
void free_llist(linked_list *list) {
	if (!list)
		return;
	linked_node *node = list->head;
	while (node) {
		linked_node *next = node->next;
		free(node);
		node = next;
	}
	free(list);
}

list *init_list(size_t size) {
	(void)size;
	return wex_alloc(1, sizeof(list));
}
size_t push_back(list *list, void *value) {
	if (!list)
		return SIZE_MAX;
	if (list->count == list->capacity) {
		list->capacity = wex_capacity(list->capacity, list->count + 1);
		list->elements =
			wex_resize(list->elements, list->capacity, sizeof(*list->elements));
	}
	size_t index = list->count++;
	list->elements[index] = value;
	return index;
}
static int wex_int_ptr_cmp(const void *a, const void *b) {
	const int *x = *(int *const *)a, *y = *(int *const *)b;
	if (!x || !y)
		return (x != NULL) - (y != NULL);
	return (*x > *y) - (*x < *y);
}
void sort_list(list *list) {
	if (list && list->count > 1)
		qsort(list->elements, list->count, sizeof(void *), wex_int_ptr_cmp);
}
bool contains(list *list, int id) {
	return list && id >= 0 && (size_t)id < list->count && list->elements[id];
}
void pop_back(list *list) {
	if (list && list->count) {
		free(list->elements[--list->count]);
		list->elements[list->count] = NULL;
	}
}
void pop_at(list *list, size_t index) {
	if (!list || index >= list->count)
		return;
	free(list->elements[index]);
	list->elements[index] = list->elements[--list->count];
	list->elements[list->count] = NULL;
}
void free_list(list *list) {
	if (!list)
		return;
	while (list->count)
		pop_back(list);
	free(list->elements);
	free(list);
}

int ihash(int key, size_t capacity) {
	return capacity ? (int)((unsigned int)key % capacity) : 0;
}
ihmap *ihmap_init(size_t capacity) {
	ihmap *map = wex_alloc(1, sizeof(*map));
	if (!wex_IntMap_init(&map->map, capacity))
		abort();
	map->capacity = map->map.capacity;
	return map;
}
ihmap_kv *ihmap_create_pair(int key, void *value) {
	ihmap_kv *pair = wex_alloc(1, sizeof(*pair));
	*pair = (ihmap_kv){key, value};
	return pair;
}
bool ihmap_insert(ihmap *map, int key, void *value) {
	if (!map)
		return false;
	wex_IntMap_entry_t entry = wex_IntMap_entry(&map->map, (uint32_t)key);
	if (!entry.result)
		abort();
	*entry.value = value;
	map->size = wex_IntMap_count(&map->map);
	map->capacity = map->map.capacity;
	return entry.inserted;
}
void *ihmap_get(ihmap *map, int key) {
	void *value = NULL;
	if (map)
		wex_IntMap_get(&map->map, (uint32_t)key, &value);
	return value;
}
bool ihmap_contains(ihmap *map, int key) {
	return map && wex_IntMap_contains(&map->map, (uint32_t)key);
}
void ihmap_free(ihmap *map) {
	if (map) {
		wex_IntMap_free(&map->map);
		free(map);
	}
}

bitset *bitset_init(size_t size) {
	bitset *set = wex_alloc(1, sizeof(*set));
	set->bytes = wex_alloc(size, 1);
	set->count = size;
	return set;
}
bool bitset_get(bitset *set, size_t index) {
	return set && index / 8 < set->count &&
		   (set->bytes[index / 8] & (1u << (index % 8)));
}
void bitset_set(bitset *set, size_t index, bool value) {
	if (!set || index / 8 >= set->count)
		return;
	uint8_t mask = (uint8_t)(1u << (index % 8));
	if (value)
		set->bytes[index / 8] |= mask;
	else
		set->bytes[index / 8] &= (uint8_t)~mask;
}
void bitset_add(bitset *set, size_t bytes) {
	if (!set || !bytes)
		return;
	if (bytes > SIZE_MAX - set->count)
		abort();
	set->bytes = wex_resize(set->bytes, set->count + bytes, 1);
	memset(set->bytes + set->count, 0, bytes);
	set->count += bytes;
}
void bitset_free(bitset *set) {
	if (set) {
		free(set->bytes);
		free(set);
	}
}

tableu16 *tableu16_init(void) { return wex_alloc(1, sizeof(tableu16)); }
uint16_t tableu16_get(tableu16 *table, size_t x, size_t y) {
	return table && x < table->sizeX && y < table->sizeY
			   ? table->arr[y * table->sizeX + x]
			   : 0;
}
void tableu16_insert(tableu16 *table, uint16_t value, size_t x, size_t y) {
	if (!table)
		return;
	if (x == SIZE_MAX || y == SIZE_MAX)
		abort();
	if (x >= table->sizeX || y >= table->sizeY) {
		size_t width = x < table->sizeX ? table->sizeX : x + 1;
		size_t height = y < table->sizeY ? table->sizeY : y + 1;
		uint16_t *arr = wex_alloc(wex_bytes(width, height), sizeof(*arr));
		for (size_t row = 0; row < table->sizeY; row++)
			if (table->sizeX)
				memcpy(arr + row * width, table->arr + row * table->sizeX,
					   table->sizeX * sizeof(*arr));
		free(table->arr);
		table->arr = arr;
		table->sizeX = width;
		table->sizeY = height;
	}
	table->arr[y * table->sizeX + x] = value;
}
void tableu16_insert_col(tableu16 *table, uint16_t value, size_t x) {
	if (table)
		for (size_t y = 0; y < table->sizeY; y++)
			tableu16_insert(table, value, x, y);
}
void tableu16_free(tableu16 *table) {
	if (table) {
		free(table->arr);
		free(table);
	}
}

wex_stack *wex_stack_init(size_t capacity) {
	wex_stack *stack = wex_alloc(1, sizeof(*stack));
	stack->capacity = capacity;
	stack->items = wex_alloc(capacity, sizeof(*stack->items));
	return stack;
}
void wex_stack_push(wex_stack *stack, void *value) {
	if (!stack)
		return;
	if (stack->top == stack->capacity) {
		stack->capacity = wex_capacity(stack->capacity, stack->top + 1);
		stack->items = wex_resize(stack->items, stack->capacity, sizeof(*stack->items));
	}
	stack->items[stack->top++] = value;
}
void wex_stack_push_s(wex_stack *stack, void *value) {
	if (stack && stack->top < stack->capacity)
		stack->items[stack->top++] = value;
}
void *wex_stack_pop(wex_stack *stack) {
	return stack && stack->top ? stack->items[--stack->top] : NULL;
}
void wex_stack_free(wex_stack *stack) {
	if (stack) {
		free(stack->items);
		free(stack);
	}
}
wex_stack_size_t *wex_stack_size_t_init(size_t capacity) {
	wex_stack_size_t *stack = wex_alloc(1, sizeof(*stack));
	stack->capacity = capacity;
	stack->items = wex_alloc(capacity, sizeof(*stack->items));
	return stack;
}
void wex_stack_size_t_push(wex_stack_size_t *stack, size_t value) {
	if (!stack)
		return;
	if (stack->top == stack->capacity) {
		stack->capacity = wex_capacity(stack->capacity, stack->top + 1);
		stack->items = wex_resize(stack->items, stack->capacity, sizeof(*stack->items));
	}
	stack->items[stack->top++] = value;
}
void wex_stack_size_t_push_s(wex_stack_size_t *stack, size_t value) {
	if (stack && stack->top < stack->capacity)
		stack->items[stack->top++] = value;
}
size_t wex_stack_size_t_pop(wex_stack_size_t *stack) {
	return stack && stack->top ? stack->items[--stack->top] : SIZE_MAX;
}
void wex_stack_size_t_free(wex_stack_size_t *stack) {
	if (stack) {
		free(stack->items);
		free(stack);
	}
}

wex_queue *wex_queue_init(size_t capacity) {
	wex_queue *queue = wex_alloc(1, sizeof(*queue));
	queue->size = capacity ? capacity : 1;
	queue->items = wex_alloc(queue->size, sizeof(*queue->items));
	return queue;
}
void wex_enqueue(wex_queue *queue, void *value) {
	if (!queue)
		return;
	if (queue->count == queue->size) {
		size_t capacity = wex_capacity(queue->size, queue->size + 1);
		void **items = wex_alloc(capacity, sizeof(*items));
		for (size_t i = 0; i < queue->count; i++)
			items[i] = queue->items[(queue->front + i) % queue->size];
		free(queue->items);
		queue->items = items;
		queue->front = 0;
		queue->size = capacity;
	}
	queue->rear = (queue->front + queue->count) % queue->size;
	queue->items[queue->rear] = value;
	queue->count++;
}
void *wex_dequeue(wex_queue *queue) {
	if (!queue || !queue->count)
		return NULL;
	void *value = queue->items[queue->front];
	queue->items[queue->front] = NULL;
	queue->front = (queue->front + 1) % queue->size;
	queue->count--;
	return value;
}
void wex_enqueue_c(wex_queue *queue, void *value) {
	if (!queue)
		return;
	if (queue->count == queue->size)
		(void)wex_dequeue(queue);
	wex_enqueue(queue, value);
}
void *wex_dequeue_c(wex_queue *queue) { return wex_dequeue(queue); }
void wex_enqueue_d(wex_queue *queue, void *value) { wex_enqueue(queue, value); }
void *wex_dequeue_d(wex_queue *queue) { return wex_dequeue(queue); }
void wex_queue_free(wex_queue *queue) {
	if (queue) {
		free(queue->items);
		free(queue);
	}
}
void wex_queue_full_free(wex_queue *queue) {
	if (!queue)
		return;
	while (queue->count)
		free(wex_dequeue(queue));
	wex_queue_free(queue);
}

dhasht *dhasht_init(dhasht_key capacity) {
	if (capacity > SIZE_MAX)
		abort();
	dhasht *map = wex_alloc(1, sizeof(*map));
	kama_options_t options = {.copy_keys = 1};
	if (!wex_StringMap_init_ex(&map->map, (size_t)capacity, &options))
		abort();
	map->size = map->map.capacity;
	return map;
}
void dhasht_resize(dhasht *map, dhasht_key capacity) {
	if (!map)
		return;
	if (capacity > SIZE_MAX || !wex_StringMap_rehash(&map->map, (size_t)capacity))
		abort();
	map->size = map->map.capacity;
}
void dhasht_insert(dhasht *map, const char *key, void *value) {
	if (!map || !key)
		return;
	if (wex_StringMap_put(&map->map, key, strlen(key), value) == KAMA_ERROR)
		abort();
	map->count = wex_StringMap_count(&map->map);
	map->size = map->map.capacity;
}
void *dhasht_get(dhasht *map, const char *key) {
	void *value = NULL;
	if (map && key)
		wex_StringMap_get(&map->map, key, strlen(key), &value);
	return value;
}
static void wex_dhasht_remove(dhasht *map, const char *key, bool own) {
	if (!map || !key)
		return;
	size_t index = wex_StringMap_find_idx(&map->map, key, strlen(key));
	if (index == map->map.capacity)
		return;
	if (own)
		free(wex_StringMap_val(&map->map, index));
	wex_StringMap_erase_idx(&map->map, index);
	map->count = wex_StringMap_count(&map->map);
}
void dhasht_remove(dhasht *map, const char *key) { wex_dhasht_remove(map, key, false); }
void dhasht_remove_wdata(dhasht *map, const char *key) {
	wex_dhasht_remove(map, key, true);
}
void dhasht_destroy(dhasht *map) {
	if (map) {
		wex_StringMap_free(&map->map);
		free(map);
	}
}
void dhasht_destroy_wdata(dhasht *map) {
	if (!map)
		return;
	kama_foreach (&map->map, i)
		free(wex_StringMap_val(&map->map, i));
	dhasht_destroy(map);
}
