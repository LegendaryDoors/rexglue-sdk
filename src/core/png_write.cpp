/**
 * @file        core/png_write.cpp
 * @brief       Minimal dependency-free PNG writer. See png_write.h.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/png_write.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

#include <rex/filesystem.h>
#include <rex/logging.h>

namespace rex {

namespace {

uint32_t Crc32(const uint8_t* data, size_t length) {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      t[n] = c;
    }
    return t;
  }();
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

void PutBigEndian32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(uint8_t(value >> 24));
  out.push_back(uint8_t(value >> 16));
  out.push_back(uint8_t(value >> 8));
  out.push_back(uint8_t(value));
}

void WriteChunk(std::vector<uint8_t>& out, const char tag[4], const std::vector<uint8_t>& payload) {
  PutBigEndian32(out, uint32_t(payload.size()));
  size_t crc_begin = out.size();
  out.insert(out.end(), tag, tag + 4);
  out.insert(out.end(), payload.begin(), payload.end());
  PutBigEndian32(out, Crc32(out.data() + crc_begin, out.size() - crc_begin));
}

}  // namespace

bool WritePng(const std::filesystem::path& path, const uint8_t* rgba, uint32_t width,
              uint32_t height, size_t stride) {
  if (!width || !height || !rgba) {
    return false;
  }

  // Scanlines, each prefixed with filter type 0 (None).
  std::vector<uint8_t> raw;
  raw.reserve(size_t(height) * (1 + size_t(width) * 4));
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);
    const uint8_t* row = rgba + size_t(y) * stride;
    raw.insert(raw.end(), row, row + size_t(width) * 4);
  }

  // zlib container around stored (uncompressed) deflate blocks.
  std::vector<uint8_t> z;
  z.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
  z.push_back(0x78);  // CMF: deflate, 32K window
  z.push_back(0x01);  // FLG: no dict, fastest - checksum-valid pair
  size_t pos = 0;
  do {
    size_t n = std::min<size_t>(65535, raw.size() - pos);
    bool is_final = (pos + n) >= raw.size();
    z.push_back(is_final ? 1 : 0);
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    uint16_t inverse = uint16_t(~uint16_t(n));
    z.push_back(uint8_t(inverse));
    z.push_back(uint8_t(inverse >> 8));
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
  } while (pos < raw.size());

  uint32_t a = 1, b = 0;
  for (uint8_t byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  PutBigEndian32(z, (b << 16) | a);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

  std::vector<uint8_t> ihdr;
  PutBigEndian32(ihdr, width);
  PutBigEndian32(ihdr, height);
  ihdr.push_back(8);  // bit depth
  ihdr.push_back(6);  // colour type: RGBA
  ihdr.push_back(0);  // compression
  ihdr.push_back(0);  // filter
  ihdr.push_back(0);  // interlace
  WriteChunk(png, "IHDR", ihdr);
  WriteChunk(png, "IDAT", z);
  WriteChunk(png, "IEND", {});

  FILE* file = filesystem::OpenFile(path, "wb");
  if (!file) {
    REXLOG_ERROR("WritePng: failed to open {} for writing", rex::path_to_utf8(path));
    return false;
  }
  size_t written = fwrite(png.data(), 1, png.size(), file);
  fclose(file);
  if (written != png.size()) {
    REXLOG_ERROR("WritePng: short write to {} ({} of {} bytes)", rex::path_to_utf8(path), written,
                 png.size());
    return false;
  }
  return true;
}

}  // namespace rex
