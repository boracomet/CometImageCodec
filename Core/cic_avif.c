#include "cic_avif.h"
#include "cic_memory.h"
#include <avif/avif.h>
#include <string.h>

static bool avif_supports_lossless(void) { return true; }
static bool avif_supports_lossy(void) { return true; }
static bool avif_supports_metadata(CICMetadataType type) {
  return (type == CIC_METADATA_EXIF || type == CIC_METADATA_ICC_PROFILE ||
          type == CIC_METADATA_XMP);
}

static CICError avif_validate(const uint8_t *data, size_t size) {
  avifROData roData = {data, size};
  if (avifPeekCompatibleFileType(&roData)) {
    return CIC_SUCCESS;
  }
  return CIC_ERROR_INVALID_PARAMETER;
}

static CICError avif_decode(CICDecodeContext *ctx) {
  if (!ctx || !ctx->input_data || !ctx->output_buffer)
    return CIC_ERROR_INVALID_PARAMETER;
  if (ctx->cancel_flag && *ctx->cancel_flag)
    return CIC_ERROR_CANCELLED;

  avifDecoder *decoder = avifDecoderCreate();
  if (!decoder)
    return CIC_ERROR_DECODE_FAILED;

  // Configure decoder
  decoder->maxThreads =
      1; // Handled structurally by our thread pool across multiple jobs

  avifResult result =
      avifDecoderSetIOMemory(decoder, ctx->input_data, ctx->input_size);
  if (result != AVIF_RESULT_OK) {
    avifDecoderDestroy(decoder);
    return CIC_ERROR_DECODE_FAILED;
  }

  if (ctx->cancel_flag && *ctx->cancel_flag) {
    avifDecoderDestroy(decoder);
    return CIC_ERROR_CANCELLED;
  }

  result = avifDecoderParse(decoder);
  if (result != AVIF_RESULT_OK) {
    avifDecoderDestroy(decoder);
    return CIC_ERROR_DECODE_FAILED;
  }

  result = avifDecoderNextImage(decoder);
  if (result != AVIF_RESULT_OK) {
    avifDecoderDestroy(decoder);
    return CIC_ERROR_DECODE_FAILED;
  }

  // Allocate RGB output buffer
  // AVIF natively decodes to YUV, we need to convert it to RGB.
  avifRGBImage rgb;
  avifRGBImageSetDefaults(&rgb, decoder->image);

  // Default to RGBA 8-bit for MVP compatibility with Swift NSImage/CGImage
  // bridge
  rgb.format = AVIF_RGB_FORMAT_RGBA;
  rgb.depth = 8;

  result = avifRGBImageAllocatePixels(&rgb);
  if (result != AVIF_RESULT_OK) {
    avifDecoderDestroy(decoder);
    return CIC_ERROR_OUT_OF_MEMORY;
  }

  if (ctx->cancel_flag && *ctx->cancel_flag) {
    avifRGBImageFreePixels(&rgb);
    avifDecoderDestroy(decoder);
    return CIC_ERROR_CANCELLED;
  }

  result = avifImageYUVToRGB(decoder->image, &rgb);
  if (result != AVIF_RESULT_OK) {
    avifRGBImageFreePixels(&rgb);
    avifDecoderDestroy(decoder);
    return CIC_ERROR_DECODE_FAILED;
  }

  // Transfer to CIC memory
  size_t buffer_size = rgb.rowBytes * rgb.height;
  uint8_t *rgba = (uint8_t *)cic_malloc(buffer_size);
  if (!rgba) {
    avifRGBImageFreePixels(&rgb);
    avifDecoderDestroy(decoder);
    return CIC_ERROR_OUT_OF_MEMORY;
  }

  memcpy(rgba, rgb.pixels, buffer_size);

  ctx->output_buffer->width = rgb.width;
  ctx->output_buffer->height = rgb.height;
  ctx->output_buffer->stride = rgb.rowBytes;
  ctx->output_buffer->bit_depth = 8;
  ctx->output_buffer->format = CIC_PIXEL_FORMAT_RGBA;
  ctx->output_buffer->data_size = buffer_size;
  ctx->output_buffer->data = rgba;

  // Free AVIF structures
  avifRGBImageFreePixels(&rgb);
  avifDecoderDestroy(decoder);

  return CIC_SUCCESS;
}

static CICError avif_encode(CICEncodeContext *ctx) {
  if (!ctx || !ctx->input_buffer || !ctx->output_data || !ctx->output_size)
    return CIC_ERROR_INVALID_PARAMETER;
  if (ctx->cancel_flag && *ctx->cancel_flag)
    return CIC_ERROR_CANCELLED;

  avifEncoder *encoder = avifEncoderCreate();
  if (!encoder)
    return CIC_ERROR_ENCODE_FAILED;

  encoder->maxThreads = 1; // Thread pool handles multi-tasking
  encoder->speed = 6;      // Good balance of compression and speed

  if (ctx->quality.lossless) {
    encoder->quality = AVIF_QUALITY_LOSSLESS;
    encoder->qualityAlpha = AVIF_QUALITY_LOSSLESS;
  } else {
    // Map 0-100 to AVIF's 0-100 quality
    int qual = ctx->quality.value;
    if (qual < 0)
      qual = 0;
    if (qual > 100)
      qual = 100;
    encoder->quality = qual;
    encoder->qualityAlpha = qual;
  }

  avifImage *image =
      avifImageCreate(ctx->input_buffer->width, ctx->input_buffer->height, 8,
                      AVIF_PIXEL_FORMAT_YUV420);
  if (!image) {
    avifEncoderDestroy(encoder);
    return CIC_ERROR_ENCODE_FAILED;
  }

  avifRGBImage rgb;
  avifRGBImageSetDefaults(&rgb, image);
  rgb.format = (ctx->input_buffer->format == CIC_PIXEL_FORMAT_RGBA)
                   ? AVIF_RGB_FORMAT_RGBA
                   : AVIF_RGB_FORMAT_RGB;
  rgb.pixels = ctx->input_buffer->data;
  rgb.rowBytes = (uint32_t)ctx->input_buffer->stride;

  if (ctx->cancel_flag && *ctx->cancel_flag) {
    avifImageDestroy(image);
    avifEncoderDestroy(encoder);
    return CIC_ERROR_CANCELLED;
  }

  // Convert RGB to YUV for AVIF
  avifResult result = avifImageRGBToYUV(image, &rgb);
  if (result != AVIF_RESULT_OK) {
    avifImageDestroy(image);
    avifEncoderDestroy(encoder);
    return CIC_ERROR_ENCODE_FAILED;
  }

  avifRWData output = AVIF_DATA_EMPTY;
  result = avifEncoderWrite(encoder, image, &output);

  if (result != AVIF_RESULT_OK) {
    avifRWDataFree(&output);
    avifImageDestroy(image);
    avifEncoderDestroy(encoder);
    return CIC_ERROR_ENCODE_FAILED;
  }

  // Transfer memory to our allocator
  *ctx->output_size = output.size;
  *ctx->output_data = (uint8_t *)cic_malloc(output.size);

  if (!*ctx->output_data) {
    avifRWDataFree(&output);
    avifImageDestroy(image);
    avifEncoderDestroy(encoder);
    return CIC_ERROR_OUT_OF_MEMORY;
  }

  memcpy(*ctx->output_data, output.data, output.size);

  avifRWDataFree(&output);
  avifImageDestroy(image);
  avifEncoderDestroy(encoder);

  return CIC_SUCCESS;
}

static void avif_destroy(CICFormatHandler *handler) {
  if (handler) {
    cic_free(handler);
  }
}

CICFormatHandler *cic_avif_create_handler(void) {
  CICFormatHandler *h =
      (CICFormatHandler *)cic_malloc(sizeof(CICFormatHandler));
  if (!h)
    return NULL;

  h->format_name = "AVIF";
  h->file_extensions[0] = "avif";
  h->file_extensions[1] = "avis";
  h->file_extensions[2] = NULL;

  const uint8_t magic[] = {0, 0, 0, 0, 'f', 't', 'y', 'p', 'a', 'v', 'i', 'f'};
  memcpy((void *)h->magic_numbers, magic, 12);
  h->magic_length = 12;

  h->supports_lossless = avif_supports_lossless;
  h->supports_lossy = avif_supports_lossy;
  h->supports_metadata = avif_supports_metadata;
  h->validate = avif_validate;
  h->decode = avif_decode;
  h->encode = avif_encode;
  h->destroy = avif_destroy;

  return h;
}
