#include "cic_format.h"
#include <string.h>

CICFormat cic_format_detect_from_buffer(const uint8_t *data, size_t size) {
  if (size < 12)
    return CIC_FORMAT_AUTO; // Too small to identify

  // Detect WebP: RIFF...WEBP
  if (memcmp(data, "RIFF", 4) == 0 && size >= 12 &&
      memcmp(data + 8, "WEBP", 4) == 0) {
    return CIC_FORMAT_WEBP;
  }

  // Detect AVIF: ....ftypavif or avis
  if (size >= 12 && memcmp(data + 4, "ftyp", 4) == 0) {
    if (memcmp(data + 8, "avif", 4) == 0 || memcmp(data + 8, "avis", 4) == 0) {
      return CIC_FORMAT_AVIF;
    }
  }

  return CIC_FORMAT_AUTO; // Unknown
}

CICFormat cic_format_from_extension(const char *extension) {
  if (!extension)
    return CIC_FORMAT_AUTO;

  if (strcasecmp(extension, "webp") == 0) {
    return CIC_FORMAT_WEBP;
  } else if (strcasecmp(extension, "avif") == 0) {
    return CIC_FORMAT_AVIF;
  }

  return CIC_FORMAT_AUTO;
}
