#ifndef CIC_TYPES_H
#define CIC_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CIC_FORMAT_AUTO = 0,
    CIC_FORMAT_WEBP,
    CIC_FORMAT_AVIF
} CICFormat;

typedef enum {
    CIC_PIXEL_FORMAT_RGB,
    CIC_PIXEL_FORMAT_RGBA,
    CIC_PIXEL_FORMAT_BGR,
    CIC_PIXEL_FORMAT_BGRA,
    CIC_PIXEL_FORMAT_YUV420,
    CIC_PIXEL_FORMAT_YUV444,
} CICPixelFormat;

typedef struct {
    uint32_t width;
    uint32_t height;
    CICPixelFormat format;
    uint8_t bit_depth;        // 8 or 10
    uint8_t* data;
    size_t stride;            // Bytes per row
    size_t data_size;
} CICImageBuffer;

typedef enum {
    CIC_METADATA_EXIF,
    CIC_METADATA_ICC_PROFILE,
    CIC_METADATA_XMP,
    CIC_METADATA_ORIENTATION,
} CICMetadataType;

typedef struct {
    CICMetadataType type;
    uint8_t* data;
    size_t size;
} CICMetadataItem;

typedef struct {
    CICMetadataItem* items;
    size_t count;
} CICMetadata;

typedef enum {
    CIC_METADATA_PRESERVE,    // Keep all metadata
    CIC_METADATA_STRIP,       // Remove all metadata
    CIC_METADATA_MINIMAL,     // Keep only orientation and color profile
} CICMetadataOptions;

typedef struct {
    int value; // 0-100
    bool lossless;
} CICQualityParams;

typedef void (*CICProgressCallback)(double percent_complete, size_t bytes_processed, double estimated_time_remaining, void* user_data);

typedef enum {
    CIC_LOG_LEVEL_NONE = 0,
    CIC_LOG_LEVEL_ERROR,
    CIC_LOG_LEVEL_WARNING,
    CIC_LOG_LEVEL_INFO,
    CIC_LOG_LEVEL_DEBUG
} CICLogLevel;

#ifdef __cplusplus
}
#endif

#endif /* CIC_TYPES_H */
