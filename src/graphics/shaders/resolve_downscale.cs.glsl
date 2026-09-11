/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#version 460

// Produces the 1x image of a resolution-scaled resolve in the shared memory
// buffer. The guest sees resolve output in memory at 1x.
//
// The scaled resolve buffer holds, per guest granule, scale_x * scale_y
// granule-sized sub-copies ordered column-major (sub_x * scale_y + sub_y).

layout(local_size_x = 32, local_size_y = 32, local_size_z = 1) in;

layout(push_constant) uniform ResolveDownscaleConstants {
  uint xe_downscale_scale_x;           // 1 to kMaxDrawResolutionScaleAlongAxis
  uint xe_downscale_scale_y;           // 1 to kMaxDrawResolutionScaleAlongAxis
  uint xe_downscale_pixel_size_log2;   // 0=8bit, 1=16bit, 2=32bit, 3=64bit
  uint xe_downscale_half_pixel_offset;
  // Destination rectangle in pixels of the tiled destination. Left and width
  // are multiples of 8, so a dword of sub-32-bit pixels never straddles it.
  uint xe_downscale_rect_left;
  uint xe_downscale_rect_top;
  uint xe_downscale_rect_width;
  uint xe_downscale_rect_height;
  uint xe_downscale_dest_pitch;        // Pixels, multiple of 32.
  uint xe_downscale_dest_height;       // Pixels, multiple of 32; 3D only.
  // Bit 31 set for a 3D (volume) destination; low bits are the slice.
  uint xe_downscale_dest_slice;
  // The written extent starts this many bytes after the destination base, so
  // tiled offsets become extent-relative by subtracting this.
  uint xe_downscale_extent_offset_bytes;
  uint xe_downscale_extent_length_bytes;
  // Byte offset of the extent start within each buffer binding: the scaled
  // one in the source, the 1x one in the destination.
  uint xe_downscale_source_offset_bytes;
  uint xe_downscale_dest_offset_bytes;
};

// Scaled resolve buffer.
layout(std430, set = 0, binding = 0) readonly buffer SourceBuffer {
  uint data[];
} xe_resolve_source;

// Shared memory buffer, the 1x guest memory image.
layout(std430, set = 0, binding = 1) writeonly buffer DestBuffer {
  uint data[];
} xe_resolve_dest;

// The Xenos tiled address functions, as in the host texture code.
uint XeTiledOffset2D(uint x, uint y, uint pitch, uint bpp_log2) {
  uint macro = ((x >> 5u) + (y >> 5u) * (pitch >> 5u)) << (bpp_log2 + 7u);
  uint micro = ((x & 7u) + ((y & 0xEu) << 2u)) << bpp_log2;
  uint offset = macro + ((micro & ~0xFu) << 1u) + (micro & 0xFu) + ((y & 1u) << 4u);
  return ((offset & ~0x1FFu) << 3u) + ((y & 16u) << 7u) + ((offset & 0x1C0u) << 2u) +
         (((((y & 8u) >> 2u) + (x >> 3u)) & 3u) << 6u) + (offset & 0x3Fu);
}

uint XeTiledOffset3D(uint x, uint y, uint z, uint pitch, uint height, uint bpp_log2) {
  uint macro_outer = ((y >> 4u) + (z >> 2u) * (height >> 4u)) * (pitch >> 5u);
  uint macro = ((((x >> 5u) + macro_outer) << (bpp_log2 + 6u)) & 0xFFFFFFFu) << 1u;
  uint micro = (((x & 7u) + ((y & 6u) << 2u)) << (bpp_log2 + 6u)) >> 6u;
  uint offset_outer = ((y >> 3u) + (z >> 2u)) & 1u;
  uint offset1 = offset_outer + ((((x >> 3u) + (offset_outer << 1u)) & 3u) << 1u);
  uint offset2 = ((macro + (micro & ~15u)) << 1u) + (micro & 15u) +
                 ((z & 3u) << (bpp_log2 + 6u)) + ((y & 1u) << 4u);
  uint address = (offset1 & 1u) << 3u;
  address += (offset2 >> 6u) & 7u;
  address <<= 3u;
  address += offset1 & ~1u;
  address <<= 2u;
  address += offset2 & ~511u;
  address <<= 3u;
  address += offset2 & 63u;
  return address;
}

// Byte offset in the scaled resolve buffer binding of the host sample for the
// 1x pixel at the given extent-relative byte offset.
uint XeScaledSourceOffset(uint dest_rel_byte_offset) {
  uint pixel_size_log2 = xe_downscale_pixel_size_log2;
  uint granule_bytes_log2 = min(4u, 3u + pixel_size_log2);
  uint granule_bytes = 1u << granule_bytes_log2;
  uint granule_pixels_log2 = granule_bytes_log2 - pixel_size_log2;
  uint scale_xy = xe_downscale_scale_x * xe_downscale_scale_y;
  uint phase_x = 0u;
  uint phase_y = 0u;
  if (xe_downscale_half_pixel_offset != 0u && scale_xy > 1u) {
    phase_x = xe_downscale_scale_x >> 1u;
    phase_y = xe_downscale_scale_y >> 1u;
  }
  uint pixel_in_granule =
      (dest_rel_byte_offset & (granule_bytes - 1u)) >> pixel_size_log2;
  uint host_x = xe_downscale_scale_x * pixel_in_granule + phase_x;
  uint sub_x = host_x >> granule_pixels_log2;
  uint host_in_granule = host_x & ((1u << granule_pixels_log2) - 1u);
  return xe_downscale_source_offset_bytes +
         (dest_rel_byte_offset & ~(granule_bytes - 1u)) * scale_xy +
         (sub_x * xe_downscale_scale_y + phase_y) * granule_bytes +
         (host_in_granule << pixel_size_log2);
}

void main() {
  uint pixel_size_log2 = xe_downscale_pixel_size_log2;
  // Pixels per thread: a dword's worth below 32 bits per pixel, one otherwise.
  uint pixels_per_thread_log2 = pixel_size_log2 < 2u ? (2u - pixel_size_log2) : 0u;
  uint x_rel = gl_GlobalInvocationID.x << pixels_per_thread_log2;
  uint y_rel = gl_GlobalInvocationID.y;
  if (x_rel >= xe_downscale_rect_width || y_rel >= xe_downscale_rect_height) {
    return;
  }
  uint x = xe_downscale_rect_left + x_rel;
  uint y = xe_downscale_rect_top + y_rel;

  uint tiled_offset;
  if ((xe_downscale_dest_slice & 0x80000000u) != 0u) {
    tiled_offset = XeTiledOffset3D(x, y, xe_downscale_dest_slice & 0x7FFFFFFFu,
                                   xe_downscale_dest_pitch, xe_downscale_dest_height,
                                   pixel_size_log2);
  } else {
    tiled_offset = XeTiledOffset2D(x, y, xe_downscale_dest_pitch, pixel_size_log2);
  }
  // Within an 8-pixel-aligned run, consecutive pixels are consecutive bytes,
  // so the pixels of one thread share a dword-aligned span at this offset.
  uint dest_rel = tiled_offset - xe_downscale_extent_offset_bytes;
  uint thread_bytes = 1u << (pixel_size_log2 + pixels_per_thread_log2);
  if (dest_rel >= xe_downscale_extent_length_bytes ||
      xe_downscale_extent_length_bytes - dest_rel < thread_bytes) {
    return;
  }
  uint dest_dword = (xe_downscale_dest_offset_bytes + dest_rel) >> 2u;

  switch (pixel_size_log2) {
    case 0u: {
      uint packed = 0u;
      for (uint i = 0u; i < 4u; ++i) {
        uint src_byte_offset = XeScaledSourceOffset(dest_rel + i);
        uint src_word = xe_resolve_source.data[src_byte_offset >> 2u];
        packed |= ((src_word >> ((src_byte_offset & 3u) * 8u)) & 0xFFu) << (i * 8u);
      }
      xe_resolve_dest.data[dest_dword] = packed;
      break;
    }
    case 1u: {
      uint packed = 0u;
      for (uint i = 0u; i < 2u; ++i) {
        uint src_byte_offset = XeScaledSourceOffset(dest_rel + (i << 1u));
        uint src_word = xe_resolve_source.data[src_byte_offset >> 2u];
        packed |= ((src_word >> ((src_byte_offset & 2u) * 8u)) & 0xFFFFu) << (i * 16u);
      }
      xe_resolve_dest.data[dest_dword] = packed;
      break;
    }
    case 2u: {
      uint src_byte_offset = XeScaledSourceOffset(dest_rel);
      xe_resolve_dest.data[dest_dword] = xe_resolve_source.data[src_byte_offset >> 2u];
      break;
    }
    default: {
      uint src_byte_offset = XeScaledSourceOffset(dest_rel);
      uint src_dword = src_byte_offset >> 2u;
      xe_resolve_dest.data[dest_dword] = xe_resolve_source.data[src_dword];
      xe_resolve_dest.data[dest_dword + 1u] = xe_resolve_source.data[src_dword + 1u];
      break;
    }
  }
}
