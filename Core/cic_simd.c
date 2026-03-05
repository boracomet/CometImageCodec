#include "cic_simd.h"
#include <string.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

// Intrinsics definitions
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#if defined(__SSE4_2__)
#include <smmintrin.h>
#endif

static CICCPUFeatures g_features = {false, false, false};

void cic_simd_init(void) {
#if defined(__APPLE__)
#if defined(__ARM_NEON)
  // On Apple Silicon, NEON is always available.
  g_features.has_neon = true;
#elif defined(__x86_64__)
  int sse42 = 0;
  size_t size = sizeof(sse42);
  if (sysctlbyname("hw.optional.sse4_2", &sse42, &size, NULL, 0) == 0) {
    g_features.has_sse42 = (sse42 == 1);
  }
#endif
#endif
}

CICCPUFeatures cic_simd_get_features(void) { return g_features; }

// --- Scalar Fallbacks ---
void cic_scalar_rgb_to_yuv(const uint8_t *rgb, uint8_t *yuv,
                           size_t pixel_count) {
  for (size_t i = 0; i < pixel_count; ++i) {
    int r = rgb[i * 3 + 0];
    int g = rgb[i * 3 + 1];
    int b = rgb[i * 3 + 2];
    yuv[i * 3 + 0] = (uint8_t)((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
    yuv[i * 3 + 1] = (uint8_t)((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
    yuv[i * 3 + 2] = (uint8_t)((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
  }
}

void cic_scalar_yuv_to_rgb(const uint8_t *yuv, uint8_t *rgb,
                           size_t pixel_count) {
  for (size_t i = 0; i < pixel_count; ++i) {
    int y = yuv[i * 3 + 0] - 16;
    int u = yuv[i * 3 + 1] - 128;
    int v = yuv[i * 3 + 2] - 128;

    int r = (298 * y + 409 * v + 128) >> 8;
    int g = (298 * y - 100 * u - 208 * v + 128) >> 8;
    int b = (298 * y + 516 * u + 128) >> 8;

    rgb[i * 3 + 0] = r < 0 ? 0 : (r > 255 ? 255 : r);
    rgb[i * 3 + 1] = g < 0 ? 0 : (g > 255 ? 255 : g);
    rgb[i * 3 + 2] = b < 0 ? 0 : (b > 255 ? 255 : b);
  }
}

void cic_scalar_rgba_to_rgb(const uint8_t *rgba, uint8_t *rgb,
                            size_t pixel_count) {
  for (size_t i = 0; i < pixel_count; ++i) {
    rgb[i * 3 + 0] = rgba[i * 4 + 0];
    rgb[i * 3 + 1] = rgba[i * 4 + 1];
    rgb[i * 3 + 2] = rgba[i * 4 + 2];
  }
}

void cic_scalar_premultiply_alpha(uint8_t *rgba, size_t pixel_count) {
  for (size_t i = 0; i < pixel_count; ++i) {
    int a = rgba[i * 4 + 3];
    if (a < 255) {
      rgba[i * 4 + 0] = (rgba[i * 4 + 0] * a) / 255;
      rgba[i * 4 + 1] = (rgba[i * 4 + 1] * a) / 255;
      rgba[i * 4 + 2] = (rgba[i * 4 + 2] * a) / 255;
    }
  }
}

// --- ARM NEON Impls ---
#if defined(__ARM_NEON)
static void cic_neon_rgba_to_rgb(const uint8_t *rgba, uint8_t *rgb,
                                 size_t pixel_count) {
  size_t i = 0;
  // Process 16 pixels (4 * 16 = 64 bytes) at a time
  for (; i + 15 < pixel_count; i += 16) {
    uint8x16x4_t pixels = vld4q_u8(rgba + i * 4);
    uint8x16x3_t rgb_out = {pixels.val[0], pixels.val[1], pixels.val[2]};
    vst3q_u8(rgb + i * 3, rgb_out);
  }
  // Tail
  cic_scalar_rgba_to_rgb(rgba + i * 4, rgb + i * 3, pixel_count - i);
}

// Standard trick for premultiply in NEON (approx / 255)
static void cic_neon_premultiply_alpha(uint8_t *rgba, size_t pixel_count) {
  size_t i = 0;
  for (; i + 7 < pixel_count; i += 8) {
    uint8x8x4_t p = vld4_u8(rgba + i * 4);

    uint16x8_t r = vmull_u8(p.val[0], p.val[3]);
    uint16x8_t g = vmull_u8(p.val[1], p.val[3]);
    uint16x8_t b = vmull_u8(p.val[2], p.val[3]);

    r = vaddq_u16(r, vdupq_n_u16(128));
    r = vaddq_u16(r, vshrq_n_u16(r, 8));
    p.val[0] = vshrn_n_u16(r, 8);

    g = vaddq_u16(g, vdupq_n_u16(128));
    g = vaddq_u16(g, vshrq_n_u16(g, 8));
    p.val[1] = vshrn_n_u16(g, 8);

    b = vaddq_u16(b, vdupq_n_u16(128));
    b = vaddq_u16(b, vshrq_n_u16(b, 8));
    p.val[2] = vshrn_n_u16(b, 8);

    vst4_u8(rgba + i * 4, p);
  }
  cic_scalar_premultiply_alpha(rgba + i * 4, pixel_count - i);
}
#endif

// --- Intel SSE Impls ---
// Provide basic loop implementations for x86 if SSE4.2 is absent, but logic
// will fallback to scalar if features don't match

// --- Dispatchers ---
void cic_simd_rgb_to_yuv(const uint8_t *rgb, uint8_t *yuv, size_t pixel_count) {
  // Both NEON and SSE versions for color conversions require careful
  // permutation. Given the complexity of writing custom vector math for exactly
  // ITU.R BT.601, and AVIF / WebP generally relying on libyuv, we'll use scalar
  // for direct conversions here unless libyuv is linked. In our architecture,
  // we mainly wrap memory anyway.
  cic_scalar_rgb_to_yuv(rgb, yuv, pixel_count);
}

void cic_simd_yuv_to_rgb(const uint8_t *yuv, uint8_t *rgb, size_t pixel_count) {
  cic_scalar_yuv_to_rgb(yuv, rgb, pixel_count);
}

void cic_simd_rgba_to_rgb(const uint8_t *rgba, uint8_t *rgb,
                          size_t pixel_count) {
#if defined(__ARM_NEON)
  if (g_features.has_neon) {
    cic_neon_rgba_to_rgb(rgba, rgb, pixel_count);
    return;
  }
#endif
  cic_scalar_rgba_to_rgb(rgba, rgb, pixel_count);
}

void cic_simd_premultiply_alpha(uint8_t *rgba, size_t pixel_count) {
#if defined(__ARM_NEON)
  if (g_features.has_neon) {
    cic_neon_premultiply_alpha(rgba, pixel_count);
    return;
  }
#endif
  cic_scalar_premultiply_alpha(rgba, pixel_count);
}
