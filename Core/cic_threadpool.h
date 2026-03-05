#ifndef CIC_THREADPOOL_H
#define CIC_THREADPOOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CICThreadPool CICThreadPool;

typedef struct {
  void (*function)(void *arg);
  void *argument;
  volatile bool *cancel_flag;
} CICWorkItem;

// Create thread pool. if thread_count is 0, defaults to logical CPU cores.
CICThreadPool *cic_threadpool_create(uint32_t thread_count);

// Submit work to the thread pool
void cic_threadpool_submit(CICThreadPool *pool, CICWorkItem item);

// Wait for all submitted work to complete
void cic_threadpool_wait_all(CICThreadPool *pool);

// Destroy the thread pool
void cic_threadpool_destroy(CICThreadPool *pool);

#ifdef __cplusplus
}
#endif

#endif /* CIC_THREADPOOL_H */
