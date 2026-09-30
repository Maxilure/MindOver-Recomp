// =============================================================================
// png_writer.h -- save a picture as a .png file, with no library
// =============================================================================
//
// For the F10 photos (native/ab_capture.h): a PNG opens in every image
// viewer and browser, unlike the PPM files the debug captures use.
//
// PNG in brief: an 8-byte signature, then "chunks" (length, 4-letter type,
// data, CRC-32 checksum): IHDR (size, 8-bit RGB), IDAT (the pixel rows,
// each starting with a filter byte, inside a zlib stream), IEND. The zlib
// stream is normally compressed (DEFLATE), but DEFLATE also has "stored"
// blocks that hold data as is (up to 65535 bytes each). Using only those
// keeps this file tiny and dependency-free; the price is size: a 1280x720
// photo is ~2.7 MB, as big as the raw pixels. Any image tool can recompress
// it (the screenshots in docs/images/ are recompressed JPEGs anyway).
// =============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

// Writes `width` x `height` pixels as an 8-bit RGB PNG. `pixels` holds 4
// bytes per pixel (red, green, blue, ignored), `stride` bytes per row.
// False if the file can't be written.
bool WritePng(const std::filesystem::path& path, const uint8_t* pixels, uint32_t width,
              uint32_t height, size_t stride);
