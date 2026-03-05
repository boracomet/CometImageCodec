#ifndef CIC_MEMORY_H
#define CIC_MEMORY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  size_t current_usage;
  size_t peak_usage;
  size_t allocation_count;
  size_t deallocation_count;
} CICMemoryStats;

typedef struct CICMemoryScope CICMemoryScope;

// Core allocation functions
void *cic_malloc(size_t size);
void *cic_calloc(size_t count, size_t size);
void *cic_realloc(void *ptr, size_t new_size);
void cic_free(void *ptr);

// Memory tracking
CICMemoryStats cic_memory_get_stats(void);
void cic_memory_reset_stats(void);

// Job-scoped allocations
CICMemoryScope *cic_memory_scope_create(void);
void cic_memory_scope_destroy(CICMemoryScope *scope);

#ifdef __cplusplus
}
#endif

#endif /* CIC_MEMORY_H */
