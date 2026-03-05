#include "cic_engine.h"
#include "cic_format.h"
#include "cic_memory.h"
#include "cic_simd.h"
#include "cic_threadpool.h"
#include <stdio.h>
#include <string.h>

#define MAX_FORMAT_HANDLERS 16

struct CICEngine {
  CICConfig config;
  CICThreadPool *thread_pool;

  // Format Handler Registry
  CICFormatHandler *handlers[MAX_FORMAT_HANDLERS];
  size_t handler_count;
};

CICEngine *cic_engine_create(const CICConfig *config) {
  if (!config)
    return NULL;

  // Initialize subsystem
  cic_simd_init();

  CICEngine *engine = (CICEngine *)cic_malloc(sizeof(CICEngine));
  if (!engine)
    return NULL;

  // Copy config
  engine->config = *config;
  engine->handler_count = 0;

  // Initialize memory bounds (if applicable bounds tracking were necessary,
  // we'd set globals, but memory stats handles it)
  cic_memory_reset_stats();

  // Init Thread Pool
  engine->thread_pool = cic_threadpool_create(config->thread_count);
  if (!engine->thread_pool) {
    cic_free(engine);
    return NULL;
  }

  return engine;
}

CICError cic_engine_register_handler(CICEngine *engine,
                                     CICFormatHandler *handler) {
  if (!engine || !handler)
    return CIC_ERROR_INVALID_PARAMETER;

  if (engine->handler_count >= MAX_FORMAT_HANDLERS) {
    return CIC_ERROR_RESOURCE_LIMIT;
  }

  engine->handlers[engine->handler_count++] = handler;
  return CIC_SUCCESS;
}

// Find a registered handler given an input file or buffer
static CICFormatHandler *
find_handler_for_buffer(CICEngine *engine, const uint8_t *data, size_t size) {
  CICFormat detected_format = cic_format_detect_from_buffer(data, size);
  if (detected_format == CIC_FORMAT_AUTO)
    return NULL;

  // Map enum format conceptually to string
  const char *target_ext = NULL;
  if (detected_format == CIC_FORMAT_WEBP)
    target_ext = "webp";
  else if (detected_format == CIC_FORMAT_AVIF)
    target_ext = "avif";

  if (!target_ext)
    return NULL;

  for (size_t i = 0; i < engine->handler_count; ++i) {
    CICFormatHandler *h = engine->handlers[i];
    for (int j = 0; j < 8 && h->file_extensions[j] != NULL; ++j) {
      if (strcasecmp(h->file_extensions[j], target_ext) == 0) {
        return h;
      }
    }
  }
  return NULL;
}

static CICFormatHandler *find_handler_for_format(CICEngine *engine,
                                                 CICFormat format) {
  const char *target_ext = NULL;
  if (format == CIC_FORMAT_WEBP)
    target_ext = "webp";
  else if (format == CIC_FORMAT_AVIF)
    target_ext = "avif";

  if (!target_ext)
    return NULL;

  for (size_t i = 0; i < engine->handler_count; ++i) {
    CICFormatHandler *h = engine->handlers[i];
    for (int j = 0; j < 8 && h->file_extensions[j] != NULL; ++j) {
      if (strcasecmp(h->file_extensions[j], target_ext) == 0) {
        return h;
      }
    }
  }
  return NULL;
}

typedef struct {
  CICEngine *engine;
  CICJobParams params;
  CICMetrics metrics;
  volatile bool cancel_flag;
  CICError final_status;
} CICJob;

static void engine_worker_function(void *arg) {
  CICJob *job = (CICJob *)arg;

  // Create memory scope
  CICMemoryScope *scope = cic_memory_scope_create();

  CICImageBuffer *img_buf_ptr = NULL;
  CICImageBuffer decoded_buf = {0};

  if (job->params.input_buffer != NULL) {
    // Pipeline starts from a pre-decoded Swift buffer
    img_buf_ptr = job->params.input_buffer;
  } else {
    // Pipeline starts from disk
    FILE *in_file = fopen(job->params.input_path, "rb");
    if (!in_file) {
      job->final_status = CIC_ERROR_INVALID_PARAMETER;
      goto cleanup;
    }

    fseek(in_file, 0, SEEK_END);
    size_t in_size = ftell(in_file);
    fseek(in_file, 0, SEEK_SET);

    uint8_t *in_data = (uint8_t *)cic_malloc(in_size);
    if (!in_data) {
      fclose(in_file);
      job->final_status = CIC_ERROR_OUT_OF_MEMORY;
      goto cleanup;
    }

    fread(in_data, 1, in_size, in_file);
    fclose(in_file);

    job->metrics.input_size_bytes = in_size;

    if (job->cancel_flag) {
      job->final_status = CIC_ERROR_CANCELLED;
      goto cleanup;
    }

    CICFormatHandler *dec_handler =
        find_handler_for_buffer(job->engine, in_data, in_size);
    if (!dec_handler) {
      job->final_status = CIC_ERROR_UNSUPPORTED_FORMAT;
      goto cleanup;
    }

    CICDecodeContext dctx = {.input_data = in_data,
                             .input_size = in_size,
                             .output_buffer = &decoded_buf,
                             .metadata = NULL,
                             .progress = job->params.progress,
                             .user_data = job->params.user_data,
                             .cancel_flag = &job->cancel_flag};

    CICError err = dec_handler->decode(&dctx);
    if (err != CIC_SUCCESS) {
      job->final_status = err;
      goto cleanup;
    }

    img_buf_ptr = &decoded_buf;
  }

  if (job->cancel_flag) {
    job->final_status = CIC_ERROR_CANCELLED;
    goto cleanup;
  }

  CICFormatHandler *enc_handler =
      find_handler_for_format(job->engine, job->params.output_format);
  if (!enc_handler) {
    job->final_status = CIC_ERROR_UNSUPPORTED_FORMAT;
    goto cleanup;
  }

  uint8_t *out_data = NULL;
  size_t out_size = 0;

  CICEncodeContext ectx = {.input_buffer = img_buf_ptr,
                           .output_data = &out_data,
                           .output_size = &out_size,
                           .quality = job->params.quality,
                           .metadata = NULL,
                           .progress = job->params.progress,
                           .user_data = job->params.user_data,
                           .cancel_flag = &job->cancel_flag};

  CICError err = enc_handler->encode(&ectx);
  if (err != CIC_SUCCESS) {
    job->final_status = err;
    goto cleanup;
  }

  job->metrics.output_size_bytes = out_size;

  if (job->cancel_flag) {
    job->final_status = CIC_ERROR_CANCELLED;
    goto cleanup;
  }

  FILE *out_file = fopen(job->params.output_path, "wb");
  if (!out_file) {
    job->final_status = CIC_ERROR_INVALID_PARAMETER;
    goto cleanup;
  }
  fwrite(out_data, 1, out_size, out_file);
  fclose(out_file);

  job->final_status = CIC_SUCCESS;

cleanup:
  // Update progress with final status
  if (job->params.progress) {
    double p = 1.0;
    job->params.progress(p, job->final_status, 0.0, job->params.user_data);
  }

  cic_memory_scope_destroy(scope);
}

CICJobHandle cic_engine_submit_job(CICEngine *engine,
                                   const CICJobParams *params) {
  if (!engine || !params || (!params->input_path && !params->input_buffer) ||
      !params->output_path)
    return NULL;

  CICJob *job = (CICJob *)malloc(sizeof(CICJob));
  if (!job)
    return NULL;

  job->engine = engine;

  // Deep copy paths since caller might free them
  job->params = *params;
  job->params.input_path =
      params->input_path ? strdup(params->input_path) : NULL;
  job->params.output_path = strdup(params->output_path);

  memset(&job->metrics, 0, sizeof(CICMetrics));
  job->cancel_flag = false;
  job->final_status = CIC_SUCCESS;

  CICWorkItem item = {.function = engine_worker_function,
                      .argument = job,
                      .cancel_flag = &job->cancel_flag};

  cic_threadpool_submit(engine->thread_pool, item);
  return (CICJobHandle)job;
}

CICBatchHandle cic_engine_submit_batch(CICEngine *engine,
                                       const CICJobParams *jobs[],
                                       size_t count) {
  // Stub
  return NULL;
}

CICError cic_engine_cancel_job(CICEngine *engine, CICJobHandle handle) {
  if (!engine || !handle)
    return CIC_ERROR_INVALID_PARAMETER;
  CICJob *job = (CICJob *)handle;
  job->cancel_flag = true;
  return CIC_SUCCESS;
}

CICMetrics cic_engine_get_metrics(CICEngine *engine, CICJobHandle handle) {
  CICMetrics m = {0};
  if (handle) {
    CICJob *job = (CICJob *)handle;
    m = job->metrics;
  }
  return m;
}

void cic_engine_destroy(CICEngine *engine) {
  if (!engine)
    return;

  // Wait for all jobs
  if (engine->thread_pool) {
    cic_threadpool_wait_all(engine->thread_pool);
    cic_threadpool_destroy(engine->thread_pool);
    engine->thread_pool = NULL;
  }

  // Destroy handlers
  for (size_t i = 0; i < engine->handler_count; ++i) {
    if (engine->handlers[i] && engine->handlers[i]->destroy) {
      engine->handlers[i]->destroy(engine->handlers[i]);
    }
  }

  cic_free(engine);
}
