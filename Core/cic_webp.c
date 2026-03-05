#include "cic_webp.h"
#include "cic_memory.h"
#include <string.h>
#include <webp/decode.h>
#include <webp/encode.h>
#include <webp/mux.h>

// Capability queries
static bool webp_supports_lossless(void) { return true; }
static bool webp_supports_lossy(void) { return true; }
static bool webp_supports_metadata(CICMetadataType type) {
  // WebP supports EXIF, ICC profiles, and XMP natively via RIFF chunks
  return (type == CIC_METADATA_EXIF || type == CIC_METADATA_ICC_PROFILE ||
          type == CIC_METADATA_XMP);
}

// Input validation
static CICError webp_validate(const uint8_t *data, size_t size) {
  if (size < 16)
    return CIC_ERROR_CORRUPTED_DATA;

  // Check magic number
  if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WEBP", 4) != 0) {
    return CIC_ERROR_INVALID_PARAMETER;
  }

  WebPBitstreamFeatures features;
  VP8StatusCode status = WebPGetFeatures(data, size, &features);

  if (status != VP8_STATUS_OK) {
    return CIC_ERROR_CORRUPTED_DATA;
  }

  if (features.width > 65535 || features.height > 65535) {
    return CIC_ERROR_INVALID_DIMENSIONS;
  }

  return CIC_SUCCESS;
}

// Memory tracking bridge for WebP (WebP supports custom allocators but we'll
// use direct for simplicity and track manually where possible)

static CICError webp_decode(CICDecodeContext *ctx) {
  if (!ctx || !ctx->input_data || !ctx->output_buffer)
    return CIC_ERROR_INVALID_PARAMETER;

  if (ctx->cancel_flag && *ctx->cancel_flag)
    return CIC_ERROR_CANCELLED;

  WebPBitstreamFeatures features;
  if (WebPGetFeatures(ctx->input_data, ctx->input_size, &features) !=
      VP8_STATUS_OK) {
    return CIC_ERROR_DECODE_FAILED;
  }

  int width = features.width;
  int height = features.height;

  // Allocate RGBA buffer
  size_t stride = width * 4;
  size_t buffer_size = stride * height;

  uint8_t *rgba = (uint8_t *)cic_malloc(buffer_size);
  if (!rgba)
    return CIC_ERROR_OUT_OF_MEMORY;

  if (ctx->cancel_flag && *ctx->cancel_flag) {
    cic_free(rgba);
    return CIC_ERROR_CANCELLED;
  }

  // Decode
  uint8_t *decoded = WebPDecodeRGBAInto(ctx->input_data, ctx->input_size, rgba,
                                        (int)buffer_size, (int)stride);
  if (!decoded) {
    cic_free(rgba);
    return CIC_ERROR_DECODE_FAILED;
  }

  // Populate output
  ctx->output_buffer->width = width;
  ctx->output_buffer->height = height;
  ctx->output_buffer->format = CIC_PIXEL_FORMAT_RGBA;
  ctx->output_buffer->bit_depth = 8;
  ctx->output_buffer->stride = stride;
  ctx->output_buffer->data_size = buffer_size;
  ctx->output_buffer->data = rgba;

  // Future: Extract metadata via WebPDemux API

  return CIC_SUCCESS;
}

static CICError webp_encode(CICEncodeContext *ctx) {
  if (!ctx || !ctx->input_buffer || !ctx->output_data || !ctx->output_size)
    return CIC_ERROR_INVALID_PARAMETER;
  if (ctx->cancel_flag && *ctx->cancel_flag)
    return CIC_ERROR_CANCELLED;

  // Only support RGBA/RGB for now (WebP native)
  if (ctx->input_buffer->format != CIC_PIXEL_FORMAT_RGBA &&
      ctx->input_buffer->format != CIC_PIXEL_FORMAT_RGB) {
    return CIC_ERROR_UNSUPPORTED_FORMAT;
  }

  int width = ctx->input_buffer->width;
  int height = ctx->input_buffer->height;
  int stride = (int)ctx->input_buffer->stride;

  WebPConfig config;
  if (!WebPConfigPreset(&config, WEBP_PRESET_DEFAULT, ctx->quality.value)) {
    return CIC_ERROR_INVALID_PARAMETER;
  }

  if (ctx->quality.lossless) {
    config.lossless = 1;
    config.quality = 100;
  }

  if (!WebPValidateConfig(&config))
    return CIC_ERROR_INVALID_PARAMETER;

  WebPPicture pic;
  if (!WebPPictureInit(&pic))
    return CIC_ERROR_ENCODE_FAILED;

  pic.width = width;
  pic.height = height;

  if (ctx->input_buffer->format == CIC_PIXEL_FORMAT_RGBA) {
    if (!WebPPictureImportRGBA(&pic, ctx->input_buffer->data, stride)) {
      WebPPictureFree(&pic);
      return CIC_ERROR_ENCODE_FAILED;
    }
  } else {
    if (!WebPPictureImportRGB(&pic, ctx->input_buffer->data, stride)) {
      WebPPictureFree(&pic);
      return CIC_ERROR_ENCODE_FAILED;
    }
  }

  WebPMemoryWriter memory_writer;
  WebPMemoryWriterInit(&memory_writer);

  pic.writer = WebPMemoryWrite;
  pic.custom_ptr = &memory_writer;

  if (ctx->cancel_flag && *ctx->cancel_flag) {
    WebPPictureFree(&pic);
    return CIC_ERROR_CANCELLED;
  }

  if (!WebPEncode(&config, &pic)) {
    WebPMemoryWriterClear(&memory_writer);
    WebPPictureFree(&pic);
    return CIC_ERROR_ENCODE_FAILED;
  }

  // Copy out to our memory manager
  *ctx->output_size = memory_writer.size;
  *ctx->output_data = (uint8_t *)cic_malloc(memory_writer.size);

  if (!*ctx->output_data) {
    WebPMemoryWriterClear(&memory_writer);
    WebPPictureFree(&pic);
    return CIC_ERROR_OUT_OF_MEMORY;
  }

  memcpy(*ctx->output_data, memory_writer.mem, memory_writer.size);

  // Apply Metadata Muxing if metadata is preserved
  // Future: implement WebPMux integration.

  WebPMemoryWriterClear(&memory_writer);
  WebPPictureFree(&pic);

  return CIC_SUCCESS;
}

static void webp_destroy(CICFormatHandler *handler) {
  if (handler) {
    cic_free(handler);
  }
}

CICFormatHandler *cic_webp_create_handler(void) {
  CICFormatHandler *h =
      (CICFormatHandler *)cic_malloc(sizeof(CICFormatHandler));
  if (!h)
    return NULL;

  h->format_name = "WebP";
  h->file_extensions[0] = "webp";
  h->file_extensions[1] = NULL;

  const uint8_t webp_magic[] = {'R', 'I', 'F', 'F', 0,   0,
                                0,   0,   'W', 'E', 'B', 'P'};
  memcpy((void *)h->magic_numbers, webp_magic, 12);
  h->magic_length = 12;

  h->supports_lossless = webp_supports_lossless;
  h->supports_lossy = webp_supports_lossy;
  h->supports_metadata = webp_supports_metadata;
  h->validate = webp_validate;
  h->decode = webp_decode;
  h->encode = webp_encode;
  h->destroy = webp_destroy;

  return h;
}
