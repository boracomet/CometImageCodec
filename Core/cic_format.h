#ifndef CIC_FORMAT_H
#define CIC_FORMAT_H

#include "cic_error.h"
#include "cic_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Decode Context
typedef struct {
  const uint8_t *input_data;
  size_t input_size;
  CICImageBuffer *output_buffer; // Allocated by handler
  CICMetadata *metadata;         // Optional metadata output
  CICProgressCallback progress;
  void *user_data;
  volatile bool *cancel_flag;
} CICDecodeContext;

// Encode Context
typedef struct {
  const CICImageBuffer *input_buffer;
  uint8_t **output_data; // Allocated by handler using cic_malloc
  size_t *output_size;
  CICQualityParams quality;
  const CICMetadata *metadata; // Optional metadata input
  CICProgressCallback progress;
  void *user_data;
  volatile bool *cancel_flag;
} CICEncodeContext;

// Abstract Format Interface
typedef struct CICFormatHandler {
  const char *format_name;
  const char *file_extensions[8];  // e.g., {"webp", NULL}
  const uint8_t magic_numbers[16]; // File signature
  size_t magic_length;

  // Capability queries
  bool (*supports_lossless)(void);
  bool (*supports_lossy)(void);
  bool (*supports_metadata)(CICMetadataType type);

  // Core operations
  CICError (*validate)(const uint8_t *data, size_t size);
  CICError (*decode)(CICDecodeContext *ctx);
  CICError (*encode)(CICEncodeContext *ctx);

  // Cleanup
  void (*destroy)(struct CICFormatHandler *handler);
} CICFormatHandler;

// Format auto-detection helper
CICFormat cic_format_detect_from_buffer(const uint8_t *data, size_t size);
CICFormat cic_format_from_extension(const char *extension);

#ifdef __cplusplus
}
#endif

#endif /* CIC_FORMAT_H */
