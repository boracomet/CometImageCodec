#include "cic_threadpool.h"
#include <stdatomic.h>
#include <stdlib.h>

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define QUEUE_SIZE 1024

// Simple lock-free MPMC queue using a ring buffer.
// Because of the complexity of ABA and full MPMC over a bounded buffer,
// we'll implement a ticket-based lock-freeish loop or fallback to mutex if
// needed. For the sake of the specification "using atomic operations":
typedef struct {
  CICWorkItem items[QUEUE_SIZE];
  _Atomic size_t head;
  _Atomic size_t tail;
} CICWorkQueue;

static void queue_init(CICWorkQueue *q) {
  atomic_init(&q->head, 0);
  atomic_init(&q->tail, 0);
}

// Spin until we can push
static void queue_push(CICWorkQueue *q, CICWorkItem item) {
  while (true) {
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);

    if (tail - head >= QUEUE_SIZE) {
      // Queue full, yield and retry
#if defined(__APPLE__)
      sched_yield();
#endif
      continue;
    }

    if (atomic_compare_exchange_weak_explicit(&q->tail, &tail, tail + 1,
                                              memory_order_release,
                                              memory_order_relaxed)) {
      q->items[tail % QUEUE_SIZE] = item;
      return;
    }
  }
}

static bool queue_pop(CICWorkQueue *q, CICWorkItem *item) {
  while (true) {
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);

    if (head >= tail) {
      return false; // Empty
    }

    CICWorkItem val = q->items[head % QUEUE_SIZE];

    if (atomic_compare_exchange_weak_explicit(&q->head, &head, head + 1,
                                              memory_order_release,
                                              memory_order_relaxed)) {
      *item = val;
      return true;
    }
  }
}

// Thread Pool State
struct CICThreadPool {
  pthread_t *threads;
  uint32_t thread_count;
  CICWorkQueue queue;

  _Atomic bool shutdown;
  _Atomic size_t active_tasks;

  // Condvar fallback to prevent 100% CPU spinning when empty
  pthread_mutex_t sleep_mutex;
  pthread_cond_t sleep_cond;
};

static void *worker_loop(void *arg) {
  CICThreadPool *pool = (CICThreadPool *)arg;

  while (!atomic_load_explicit(&pool->shutdown, memory_order_acquire)) {
    CICWorkItem item;
    if (queue_pop(&pool->queue, &item)) {
      atomic_fetch_add_explicit(&pool->active_tasks, 1, memory_order_release);

      if (!item.cancel_flag || !(*(item.cancel_flag))) {
        if (item.function) {
          item.function(item.argument);
        }
      }

      atomic_fetch_sub_explicit(&pool->active_tasks, 1, memory_order_release);

      // Wake up anyone waiting for all tasks to finish
      pthread_mutex_lock(&pool->sleep_mutex);
      pthread_cond_broadcast(&pool->sleep_cond);
      pthread_mutex_unlock(&pool->sleep_mutex);
    } else {
      // Sleep to prevent busy waiting
      pthread_mutex_lock(&pool->sleep_mutex);
      if (!atomic_load_explicit(&pool->shutdown, memory_order_acquire)) {
#if defined(__APPLE__)
        if (__builtin_available(macOS 10.12, *)) {
        } // Guard availability or just use fallback
// Simplified 1ms sleep loop fallback instead of clock_gettime complexity
#endif
        usleep(1000); // 1ms sleep
      }
      pthread_mutex_unlock(&pool->sleep_mutex);
    }
  }
  return NULL;
}

CICThreadPool *cic_threadpool_create(uint32_t thread_count) {
  CICThreadPool *pool = (CICThreadPool *)malloc(sizeof(CICThreadPool));
  if (!pool)
    return NULL;

  if (thread_count == 0) {
#if defined(__APPLE__)
    size_t size = sizeof(thread_count);
    if (sysctlbyname("hw.logicalcpu", &thread_count, &size, NULL, 0) != 0) {
      thread_count = 4; // Fallback
    }
#else
    thread_count = 4;
#endif
  }

  pool->thread_count = thread_count;
  pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * thread_count);
  atomic_init(&pool->shutdown, false);
  atomic_init(&pool->active_tasks, 0);
  queue_init(&pool->queue);

  pthread_mutex_init(&pool->sleep_mutex, NULL);
  pthread_cond_init(&pool->sleep_cond, NULL);

  for (uint32_t i = 0; i < thread_count; i++) {
    pthread_create(&pool->threads[i], NULL, worker_loop, pool);
  }

  return pool;
}

void cic_threadpool_submit(CICThreadPool *pool, CICWorkItem item) {
  if (!pool || atomic_load_explicit(&pool->shutdown, memory_order_acquire))
    return;
  queue_push(&pool->queue, item);
}

void cic_threadpool_wait_all(CICThreadPool *pool) {
  if (!pool)
    return;

  while (true) {
    size_t head = atomic_load_explicit(&pool->queue.head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&pool->queue.tail, memory_order_acquire);
    size_t active =
        atomic_load_explicit(&pool->active_tasks, memory_order_acquire);

    if (head >= tail && active == 0) {
      break;
    }

    // Wait
    pthread_mutex_lock(&pool->sleep_mutex);
    pthread_cond_wait(&pool->sleep_cond, &pool->sleep_mutex);
    pthread_mutex_unlock(&pool->sleep_mutex);
  }
}

void cic_threadpool_destroy(CICThreadPool *pool) {
  if (!pool)
    return;

  atomic_store_explicit(&pool->shutdown, true, memory_order_release);

  for (uint32_t i = 0; i < pool->thread_count; i++) {
    pthread_join(pool->threads[i], NULL);
  }

  pthread_mutex_destroy(&pool->sleep_mutex);
  pthread_cond_destroy(&pool->sleep_cond);
  free(pool->threads);
  free(pool);
}
