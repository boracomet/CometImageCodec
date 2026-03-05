# CometImageCodec (Pure C Core)

A high-performance, zero-dependency image codec engine written in pure C for macOS 13+. CometImageCodec provides an extensible architecture for encoding and decoding modern image formats with a focus on speed, safety, and memory efficiency.

## Key Features

- **Pure C Engine**: Lightweight core with no external runtime dependencies.
- **Native Format Support**: Built-in support for **WebP** and **AVIF**.
- **Performance Optimized**: 
    - **SIMD**: Native acceleration using ARM NEON (Apple Silicon) and SSE4.2 (Intel).
    - **Multi-threading**: Lock-free thread pool for parallel batch processing.
    - **Memory-Efficient**: Streaming I/O and memory-mapped processing for large-scale images.
- **Extensible Architecture**: Pluggable format handler system allowing developers to add new codecs without modifying the core engine.
- **Memory Safety**: Centralized memory management with tracking and bounds checking.
- **Universal Binary**: Fully compatible with both Apple Silicon and Intel-based Macs.

## Supported Formats

| Format | Read (Decode) | Write (Encode) | Features |
| :--- | :---: | :---: | :--- |
| **WebP** | ✅ | ✅ | Lossy, Lossless, Metadata (EXIF/XMP/ICC) |
| **AVIF** | ✅ | ✅ | 8/10-bit depth, High efficiency |

*Note: The core engine is designed to be bridged to higher-level frameworks (like Swift/AppKit) to support additional system formats via ImageIO.*

## Architecture

The engine is divided into several specialized layers:
- **CICEngine**: Core coordinator and job management.
- **FormatHandlers**: Pluggable modules for specific codecs (WebP, AVIF).
- **CICThreadPool**: High-concurrency job scheduler.
- **CICSIMD**: Hardware-specific compute optimizations.
- **CICMemory**: Scoped memory tracking for leak-free operations.

## Integration

### Prerequisites
- macOS 13.0+
- Xcode Command Line Tools
- `libwebp` and `libavif` source/libraries (for static linking)

### Building the Core
CometImageCodec is designed to be statically linked into your application. 

```bash
# Example build steps
mkdir build && cd build
cmake ..
make
```

## Developer Usage (C API)

```c
#include "cic_engine.h"

// 1. Initialize Engine
CICConfig config = { .thread_count = 0, .max_memory_mb = 512 };
CICEngine* engine = cic_engine_create(&config);

// 2. Prepare Job
CICJobParams params = {
    .input_path = "input.jpg",
    .output_path = "output.webp",
    .output_format = CIC_FORMAT_WEBP,
    .quality = { .value = 85, .lossless = false }
};

// 3. Submit Job
CICJobHandle handle = cic_engine_submit_job(engine, &params);

// 4. Wait & Cleanup
cic_engine_wait(engine, handle);
cic_engine_destroy(engine);
```

## License
[Insert License Here - e.g., MIT]

---
Created by **Bora Ata Türkoğlu** & The Comet Editor Team.
