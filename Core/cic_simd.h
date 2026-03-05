#ifndef CIC_SIMD_H
#define CIC_SIMD_H

#include "cic_types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  bool has_neon;  // ARM NEON
  bool has_sse42; // Intel SSE4.2
  bool has_avx2;  // Intel AVX2 (future)
} CICCPUFeatures;

// Initialize and detect CPU capabilities
void cic_simd_init(void);

// Get current CPU features
CICCPUFeatures cic_simd_get_features(void);

// Color space conversions (dispatched to optimal implementation)
void cic_simd_rgb_to_yuv(const uint8_t *rgb, uint8_t *yuv, size_t pixel_count);
void cic_simd_yuv_to_rgb(const uint8_t *yuv, uint8_t *rgb, size_t pixel_count);

// Pixel format conversions
void cic_simd_rgba_to_rgb(const uint8_t *rgba, uint8_t *rgb,
                          size_t pixel_count);
void cic_simd_premultiply_alpha(uint8_t *rgba, size_t pixel_count);

// Helper generic fallback functions exposed for tests/internal logic
void cic_scalar_rgb_to_yuv(const uint8_t *rgb, uint8_t *yuv,
                           size_t pixel_count);
void cic_scalar_yuv_to_rgb(const uint8_t *yuv, uint8_t *rgb,
                           size_t pixel_count);
void cic_scalar_rgba_to_rgb(const uint8_t *rgba, uint8_t *rgb,
                            size_t pixel_count);
void cic_scalar_premultiply_alpha(uint8_t *rgba, size_t pixel_count);

#ifdef __cplusplus
}
#endif

#endif /* CIC_SIMD_H */
