#pragma once

// vw's text rasterizer for vw's own code that composites text itself (SVG).

#include "vw/scene.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace vw::detail {

struct TextImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  // Coverage, four 8-bit values per element, when alpha_mask; otherwise
  // RGBA (color glyphs), red in the low byte.
  Pixels pixels;
  bool alpha_mask = true;
  // Where the start of the baseline lies in the bitmap, in its pixels.
  float origin_x = 0.0F;
  float baseline_y = 0.0F;
  // Pixels of bitmap per unit of `size`.
  float scale = 1.0F;
  // Logical advance of the whole line, in the units of `size`.
  float advance = 0.0F;
};

// One unwrapped line, as add_text lays it out.
TextImage rasterize_text(std::string_view text, std::string_view font, float size, int weight,
                         const std::array<float, 4>& color);

}  // namespace vw::detail
