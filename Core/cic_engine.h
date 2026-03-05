#ifndef CIC_ENGINE_H
#define CIC_ENGINE_H

#include "cic_error.h"
#include "cic_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CICEngine CICEngine;
typedef void *CICJobHandle;
typedef void *CICBatchHandle;
typedef struct CICFormatHandler CICFormatHandler;

typedef struct {
  uint32_t thread_count; // 0 = auto-detect
  size_t max_memory_mb;  // Memory limit per job
  bool enable_simd;      // Enable SIMD optimizations
  CICLogLevel log_level; // Logging verbosity
} CICConfig;

typedef struct {
  const char *input_path;       // Input file path
  const char *output_path;      // Output file path
  CICImageBuffer *input_buffer; // Raw pixels directly from Swift (optional,
                                // overrides input_path decode)
  CICFormat input_format;       // Auto-detect if CIC_FORMAT_AUTO
  CICFormat output_format;      // Target format
  CICQualityParams quality;     // Quality settings
  CICMetadataOptions metadata;  // Metadata handling
  CICProgressCallback progress; // Progress callback
  void *user_data;              // User context
} CICJobParams;

typedef struct {
  uint64_t decode_time_us; // Microseconds
  uint64_t encode_time_us;
  uint64_t total_time_us;
  size_t peak_memory_bytes;
  size_t input_size_bytes;
  size_t output_size_bytes;
  uint32_t thread_count_used;
} CICMetrics;

// Initialize the engine with configuration
CICEngine *cic_engine_create(const CICConfig *config);

// Register a format handler
CICError cic_engine_register_handler(CICEngine *engine,
                                     CICFormatHandler *handler);

// Submit a conversion job
CICJobHandle cic_engine_submit_job(CICEngine *engine,
                                   const CICJobParams *params);

// Submit batch operation
CICBatchHandle cic_engine_submit_batch(CICEngine *engine,
                                       const CICJobParams *jobs[],
                                       size_t count);

// Cancel a job
CICError cic_engine_cancel_job(CICEngine *engine, CICJobHandle handle);

// Query performance metrics
CICMetrics cic_engine_get_metrics(CICEngine *engine, CICJobHandle handle);

// Cleanup
void cic_engine_destroy(CICEngine *engine);

#ifdef __cplusplus
}
#endif

#endif /* CIC_ENGINE_H */
