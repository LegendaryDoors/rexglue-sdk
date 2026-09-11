#pragma once
/**
 * @file        png_write.h
 * @brief       Minimal dependency-free PNG writer for diagnostic captures.
 *
 * @remarks     Writes valid PNGs using deflate "stored" blocks, so there is
 *              no compression library to link. Files are larger than a
 *              compressed PNG, which does not matter for a debug capture.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <filesystem>

namespace rex {

// Writes 8-bit RGBA pixels as a PNG. `stride` is the byte distance between
// rows; the last row need not be padded. False on invalid input or I/O.
bool WritePng(const std::filesystem::path& path, const uint8_t* rgba, uint32_t width,
              uint32_t height, size_t stride);

}  // namespace rex
