#include "cic_memory.h"
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <pthread.h>
#include <stdatomic.h>
#endif

// Global atomics for memory stats
static _Atomic size_t g_current_usage = 0;
static _Atomic size_t g_peak_usage = 0;
static _Atomic size_t g_allocation_count = 0;
static _Atomic size_t g_deallocation_count = 0;

// Update peak memory
static void update_peak(size_t current) {
  size_t peak = atomic_load_explicit(&g_peak_usage, memory_order_relaxed);
  while (current > peak) {
    if (atomic_compare_exchange_weak_explicit(&g_peak_usage, &peak, current,
                                              memory_order_relaxed,
                                              memory_order_relaxed)) {
      break;
    }
  }
}

// Memory Allocation Node (internal tracking)
typedef struct CICAllocNode {
  void *ptr;
  size_t size;
  struct CICAllocNode *next;
} CICAllocNode;

// Thread-local Memory Scope
struct CICMemoryScope {
  CICAllocNode *head;
};

// Thread-local current scope
static _Thread_local CICMemoryScope *t_current_scope = NULL;

static void track_allocation(void *ptr, size_t size) {
  if (!ptr)
    return;

  atomic_fetch_add_explicit(&g_allocation_count, 1, memory_order_relaxed);
  size_t new_usage =
      atomic_fetch_add_explicit(&g_current_usage, size, memory_order_relaxed) +
      size;
  update_peak(new_usage);

  // If we're inside a scope, record it
  if (t_current_scope) {
    CICAllocNode *node = (CICAllocNode *)malloc(sizeof(CICAllocNode));
    if (node) {
      node->ptr = ptr;
      node->size = size;
      node->next = t_current_scope->head;
      t_current_scope->head = node;
    }
  }
}

static void track_deallocation(size_t size) {
  atomic_fetch_add_explicit(&g_deallocation_count, 1, memory_order_relaxed);
  atomic_fetch_sub_explicit(&g_current_usage, size, memory_order_relaxed);
}

// We need a subtle way to store the 'size' of allocation to correctly track
// current_usage. We'll wrap allocations.

typedef struct {
  size_t size;
  // Align data to 16 bytes for SIMD operations
#if defined(__APPLE__)
  char padding[8];
#else
  char padding[16 - sizeof(size_t)];
#endif
} CICAllocMeta;

// 16 bytes overhead
#define HEADER_SIZE 16

void *cic_malloc(size_t size) {
  if (size == 0)
    return NULL;

  CICAllocMeta *meta = (CICAllocMeta *)malloc(size + HEADER_SIZE);
  if (!meta)
    return NULL;

  meta->size = size;
  void *ptr = (void *)((char *)meta + HEADER_SIZE);

  track_allocation(ptr, size);
  return ptr;
}

void *cic_calloc(size_t count, size_t size) {
  size_t total = count * size;
  if (total == 0)
    return NULL;

  void *ptr = cic_malloc(total);
  if (ptr) {
    memset(ptr, 0, total);
  }
  return ptr;
}

void *cic_realloc(void *ptr, size_t new_size) {
  if (!ptr)
    return cic_malloc(new_size);
  if (new_size == 0) {
    cic_free(ptr);
    return NULL;
  }

  CICAllocMeta *meta = (CICAllocMeta *)((char *)ptr - HEADER_SIZE);
  size_t old_size = meta->size;

  // We can't safely realloc because the new pointer might have a different
  // address and our scope tracking holds the *old* address. Instead of doing
  // actual realloc and updating nodes, we'll malloc/copy/free. This maintains
  // thread-local scope tracking integrity.

  void *new_ptr = cic_malloc(new_size);
  if (new_ptr) {
    size_t copy_size = old_size < new_size ? old_size : new_size;
    memcpy(new_ptr, ptr, copy_size);
    cic_free(ptr);
  }
  return new_ptr;
}

void cic_free(void *ptr) {
  if (!ptr)
    return;

  CICAllocMeta *meta = (CICAllocMeta *)((char *)ptr - HEADER_SIZE);
  size_t size = meta->size;

  track_deallocation(size);

  // Remove from scope tracking if it exists
  if (t_current_scope) {
    CICAllocNode **curr = &t_current_scope->head;
    while (*curr != NULL) {
      if ((*curr)->ptr == ptr) {
        CICAllocNode *to_delete = *curr;
        *curr = to_delete->next;
        free(to_delete);
        break;
      }
      curr = &(*curr)->next;
    }
  }

  free(meta);
}

CICMemoryStats cic_memory_get_stats(void) {
  CICMemoryStats stats;
  stats.current_usage =
      atomic_load_explicit(&g_current_usage, memory_order_relaxed);
  stats.peak_usage = atomic_load_explicit(&g_peak_usage, memory_order_relaxed);
  stats.allocation_count =
      atomic_load_explicit(&g_allocation_count, memory_order_relaxed);
  stats.deallocation_count =
      atomic_load_explicit(&g_deallocation_count, memory_order_relaxed);
  return stats;
}

void cic_memory_reset_stats(void) {
  atomic_store_explicit(&g_current_usage, 0, memory_order_relaxed);
  atomic_store_explicit(&g_peak_usage, 0, memory_order_relaxed);
  atomic_store_explicit(&g_allocation_count, 0, memory_order_relaxed);
  atomic_store_explicit(&g_deallocation_count, 0, memory_order_relaxed);
}

CICMemoryScope *cic_memory_scope_create(void) {
  CICMemoryScope *scope = (CICMemoryScope *)malloc(sizeof(CICMemoryScope));
  if (scope) {
    scope->head = NULL;
    t_current_scope = scope;
  }
  return scope;
}

void cic_memory_scope_destroy(CICMemoryScope *scope) {
  if (!scope)
    return;

  // Free all tracked allocations in this scope using cic_free
  // which handles the tracking cleanup internally.
  // We have to iterate carefully because cic_free modifies the list.

  CICAllocNode *curr = scope->head;
  // Detach list from current tracking immediately to prevent O(N^2) searches
  // in cic_free
  if (t_current_scope == scope) {
    t_current_scope->head = NULL;
  }

  while (curr != NULL) {
    void *mem_ptr = curr->ptr;
    CICAllocNode *next = curr->next;

    // Directly free since we bypassed normal tracking list removal
    if (mem_ptr) {
      CICAllocMeta *meta = (CICAllocMeta *)((char *)mem_ptr - HEADER_SIZE);
      track_deallocation(meta->size);
      free(meta);
    }

    free(curr);
    curr = next;
  }

  if (t_current_scope == scope) {
    t_current_scope = NULL;
  }

  free(scope);
}
