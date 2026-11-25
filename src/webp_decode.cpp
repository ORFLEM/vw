// WebP: lossy (VP8, RFC 6386), lossless (VP8L), the ALPH chunk, and the
// first frame of an animation.
#include "image_decode_internal.hpp"
#include "webp_tables.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace vw::detail {
namespace {

// ---------------------------------------------------------------------------
// VP8L

class LsbReader {
 public:
  explicit LsbReader(std::span<const std::uint8_t> data) : p_(data.data()), end_(data.data() + data.size()) {}
  std::uint32_t peek(int n) {
    refill();
    return static_cast<std::uint32_t>(buffer_ & ((1ULL << n) - 1U));
  }
  void consume(int n) {
    buffer_ >>= n;
    count_ -= n;
    if (count_ < padded_ * 8) fail("WebP data is truncated");
  }
  std::uint32_t bits(int n) {
    if (n == 0) return 0;
    const std::uint32_t value = peek(n);
    consume(n);
    return value;
  }

 private:
  void refill() {
    while (count_ <= 56) {
      std::uint64_t byte = 0;
      if (p_ < end_) byte = *p_++;
      else ++padded_;
      buffer_ |= byte << count_;
      count_ += 8;
    }
  }
  const std::uint8_t* p_;
  const std::uint8_t* end_;
  std::uint64_t buffer_ = 0;
  int count_ = 0;
  int padded_ = 0;
};

// Canonical prefix code read least significant bit first. One used symbol
// makes a code of zero bits.
class PrefixCode {
 public:
  static constexpr int fast_bits = 8;
  void build(const std::vector<std::uint8_t>& lengths) {
    std::array<int, 16> count{};
    int used = 0, last = 0;
    for (std::size_t i = 0; i < lengths.size(); ++i) {
      if (lengths[i] == 0) continue;
      if (lengths[i] > 15) fail("invalid WebP prefix code");
      ++count[lengths[i]];
      ++used;
      last = static_cast<int>(i);
    }
    if (used == 0) fail("empty WebP prefix code");
    single_ = used == 1 ? last : -1;
    if (single_ >= 0) return;
    int left = 1;
    for (int length = 1; length < 16; ++length) {
      left = (left << 1) - count[static_cast<std::size_t>(length)];
      if (left < 0) fail("invalid WebP prefix code");
    }
    std::array<int, 16> offset{};
    for (int length = 1; length < 15; ++length) offset[static_cast<std::size_t>(length + 1)] = offset[static_cast<std::size_t>(length)] + count[static_cast<std::size_t>(length)];
    symbols_.assign(static_cast<std::size_t>(used), 0);
    for (std::size_t i = 0; i < lengths.size(); ++i)
      if (lengths[i] != 0) symbols_[static_cast<std::size_t>(offset[lengths[i]]++)] = static_cast<std::uint16_t>(i);
    count_ = count;
    fast_.fill(0);
    std::array<int, 16> next{};
    int code = 0;
    count[0] = 0;
    for (int length = 1; length < 16; ++length) {
      code = (code + count[static_cast<std::size_t>(length - 1)]) << 1;
      next[static_cast<std::size_t>(length)] = code;
    }
    for (std::size_t i = 0; i < lengths.size(); ++i) {
      const int length = lengths[i];
      if (length == 0) continue;
      const int value = next[static_cast<std::size_t>(length)]++;
      if (length > fast_bits) continue;
      int reversed = 0;
      for (int bit = 0; bit < length; ++bit) reversed |= ((value >> bit) & 1) << (length - 1 - bit);
      for (int r = reversed; r < (1 << fast_bits); r += 1 << length)
        fast_[static_cast<std::size_t>(r)] = static_cast<std::uint32_t>(length) << 16U | static_cast<std::uint32_t>(i);
    }
  }
  int decode(LsbReader& in) const {
    if (single_ >= 0) return single_;
    const std::uint32_t window = in.peek(15);
    if (const std::uint32_t entry = fast_[window & ((1U << fast_bits) - 1U)]) {
      in.consume(static_cast<int>(entry >> 16U));
      return static_cast<int>(entry & 0xFFFFU);
    }
    int code = 0, first = 0, index = 0;
    for (int length = 1; length < 16; ++length) {
      code |= static_cast<int>((window >> (length - 1)) & 1U);
      const int count = count_[static_cast<std::size_t>(length)];
      if (code - first < count) {
        in.consume(length);
        return symbols_[static_cast<std::size_t>(index + code - first)];
      }
      index += count;
      first = (first + count) << 1;
      code <<= 1;
    }
    fail("invalid WebP prefix code");
  }

 private:
  int single_ = -1;
  std::array<std::uint32_t, 1 << fast_bits> fast_{};
  std::array<int, 16> count_{};
  std::vector<std::uint16_t> symbols_;
};

PrefixCode read_prefix_code(LsbReader& in, int alphabet) {
  std::vector<std::uint8_t> lengths(static_cast<std::size_t>(alphabet), 0);
  if (in.bits(1) != 0) {
    const int count = static_cast<int>(in.bits(1)) + 1;
    const int first = static_cast<int>(in.bits(in.bits(1) != 0 ? 8 : 1));
    if (first >= alphabet) fail("invalid WebP prefix code");
    lengths[static_cast<std::size_t>(first)] = 1;
    if (count == 2) {
      const int second = static_cast<int>(in.bits(8));
      if (second >= alphabet) fail("invalid WebP prefix code");
      lengths[static_cast<std::size_t>(second)] = 1;
    }
  } else {
    static constexpr std::array<std::uint8_t, 19> order{17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    std::vector<std::uint8_t> code_lengths(19, 0);
    const int count = static_cast<int>(in.bits(4)) + 4;
    for (int i = 0; i < count; ++i) code_lengths[order[static_cast<std::size_t>(i)]] = static_cast<std::uint8_t>(in.bits(3));
    PrefixCode lengths_code;
    lengths_code.build(code_lengths);
    int max_symbol = alphabet;
    if (in.bits(1) != 0) {
      const int width = 2 + 2 * static_cast<int>(in.bits(3));
      max_symbol = 2 + static_cast<int>(in.bits(width));
      if (max_symbol > alphabet) fail("invalid WebP prefix code");
    }
    int symbol = 0;
    std::uint8_t previous = 8;
    while (symbol < alphabet) {
      if (max_symbol-- == 0) break;
      const int length = lengths_code.decode(in);
      if (length < 16) {
        lengths[static_cast<std::size_t>(symbol++)] = static_cast<std::uint8_t>(length);
        if (length != 0) previous = static_cast<std::uint8_t>(length);
        continue;
      }
      static constexpr std::array<int, 3> extra{2, 3, 7};
      static constexpr std::array<int, 3> base{3, 3, 11};
      const auto slot = static_cast<std::size_t>(length - 16);
      const int repeat = static_cast<int>(in.bits(extra[slot])) + base[slot];
      if (symbol + repeat > alphabet) fail("invalid WebP prefix code");
      const std::uint8_t value = length == 16 ? previous : 0;
      for (int i = 0; i < repeat; ++i) lengths[static_cast<std::size_t>(symbol++)] = value;
    }
  }
  PrefixCode code;
  code.build(lengths);
  return code;
}

constexpr std::array<std::uint8_t, 120> code_to_plane{
    0x18, 0x07, 0x17, 0x19, 0x28, 0x06, 0x27, 0x29, 0x16, 0x1a, 0x26, 0x2a, 0x38, 0x05, 0x37, 0x39, 0x15, 0x1b, 0x36, 0x3a,
    0x25, 0x2b, 0x48, 0x04, 0x47, 0x49, 0x14, 0x1c, 0x35, 0x3b, 0x46, 0x4a, 0x24, 0x2c, 0x58, 0x45, 0x4b, 0x34, 0x3c, 0x03,
    0x57, 0x59, 0x13, 0x1d, 0x56, 0x5a, 0x23, 0x2d, 0x44, 0x4c, 0x55, 0x5b, 0x33, 0x3d, 0x68, 0x02, 0x67, 0x69, 0x12, 0x1e,
    0x66, 0x6a, 0x22, 0x2e, 0x54, 0x5c, 0x43, 0x4d, 0x65, 0x6b, 0x32, 0x3e, 0x78, 0x01, 0x77, 0x79, 0x53, 0x5d, 0x11, 0x1f,
    0x64, 0x6c, 0x42, 0x4e, 0x76, 0x7a, 0x21, 0x2f, 0x75, 0x7b, 0x31, 0x3f, 0x63, 0x6d, 0x52, 0x5e, 0x00, 0x74, 0x7c, 0x41,
    0x4f, 0x10, 0x20, 0x62, 0x6e, 0x30, 0x73, 0x7d, 0x51, 0x5f, 0x40, 0x72, 0x7e, 0x61, 0x6f, 0x50, 0x71, 0x7f, 0x60, 0x70};

std::size_t div_round_up(std::size_t value, int bits) { return (value + (1U << bits) - 1) >> bits; }

std::uint32_t add_pixels(std::uint32_t a, std::uint32_t b) {
  const std::uint32_t alpha_green = (a & 0xFF00FF00U) + (b & 0xFF00FF00U);
  const std::uint32_t red_blue = (a & 0x00FF00FFU) + (b & 0x00FF00FFU);
  return (alpha_green & 0xFF00FF00U) | (red_blue & 0x00FF00FFU);
}

std::uint32_t average2(std::uint32_t a, std::uint32_t b) { return (((a ^ b) & 0xFEFEFEFEU) >> 1U) + (a & b); }

std::uint32_t channel(std::uint32_t pixel, int shift) { return (pixel >> shift) & 255U; }

std::uint32_t select(std::uint32_t top, std::uint32_t left, std::uint32_t top_left) {
  int difference = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    const int t = static_cast<int>(channel(top, shift)), l = static_cast<int>(channel(left, shift)),
              c = static_cast<int>(channel(top_left, shift));
    difference += std::abs(l - c) - std::abs(t - c);
  }
  return difference <= 0 ? top : left;
}

std::uint32_t clamped_add_subtract_full(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
  std::uint32_t result = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    const int value = static_cast<int>(channel(a, shift)) + static_cast<int>(channel(b, shift)) -
                      static_cast<int>(channel(c, shift));
    result |= static_cast<std::uint32_t>(std::clamp(value, 0, 255)) << shift;
  }
  return result;
}

std::uint32_t clamped_add_subtract_half(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
  const std::uint32_t average = average2(a, b);
  std::uint32_t result = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    const int x = static_cast<int>(channel(average, shift));
    const int y = static_cast<int>(channel(c, shift));
    result |= static_cast<std::uint32_t>(std::clamp(x + (x - y) / 2, 0, 255)) << shift;
  }
  return result;
}

class Vp8lDecoder {
 public:
  explicit Vp8lDecoder(std::span<const std::uint8_t> data) : in_(data) {}

  // The whole image stream (transforms included) as ARGB.
  std::vector<std::uint32_t> image(std::size_t width, std::size_t height) {
    return stream(width, height, true);
  }

  std::uint32_t bits(int n) { return in_.bits(n); }

 private:
  struct Transform {
    int type = 0;
    std::size_t width = 0;
    int bits = 0;
    std::vector<std::uint32_t> data;
  };
  struct Group {
    std::array<PrefixCode, 5> codes;
  };

  std::vector<std::uint32_t> stream(std::size_t width, std::size_t height, bool top) {
    std::vector<Transform> transforms;
    std::size_t coded_width = width;
    if (top) {
      std::array<bool, 4> seen{};
      while (in_.bits(1) != 0) {
        Transform transform;
        transform.type = static_cast<int>(in_.bits(2));
        if (seen[static_cast<std::size_t>(transform.type)]) fail("repeated WebP transform");
        seen[static_cast<std::size_t>(transform.type)] = true;
        transform.width = coded_width;
        if (transform.type == 0 || transform.type == 1) {
          transform.bits = static_cast<int>(in_.bits(3)) + 2;
          transform.data = stream(div_round_up(coded_width, transform.bits), div_round_up(height, transform.bits), false);
        } else if (transform.type == 3) {
          const std::size_t colors = in_.bits(8) + 1;
          transform.bits = colors > 16 ? 0 : colors > 4 ? 1 : colors > 2 ? 2 : 3;
          std::vector<std::uint32_t> palette = stream(colors, 1, false);
          for (std::size_t i = 1; i < palette.size(); ++i) palette[i] = add_pixels(palette[i], palette[i - 1]);
          palette.resize(256, 0);
          transform.data = std::move(palette);
          coded_width = div_round_up(coded_width, transform.bits);
        }
        transforms.push_back(std::move(transform));
      }
    }

    int cache_bits = 0;
    if (in_.bits(1) != 0) {
      cache_bits = static_cast<int>(in_.bits(4));
      if (cache_bits < 1 || cache_bits > 11) fail("invalid WebP color cache");
    }
    int meta_bits = 0;
    std::size_t meta_width = 0;
    std::vector<std::uint32_t> meta;
    std::size_t groups = 1;
    if (top && in_.bits(1) != 0) {
      meta_bits = static_cast<int>(in_.bits(3)) + 2;
      meta_width = div_round_up(coded_width, meta_bits);
      meta = stream(meta_width, div_round_up(height, meta_bits), false);
      for (std::uint32_t& value : meta) {
        value = (value >> 8U) & 0xFFFFU;
        groups = std::max<std::size_t>(groups, value + 1);
      }
    }
    const int cache_size = cache_bits > 0 ? 1 << cache_bits : 0;
    std::vector<Group> codes(groups);
    for (Group& group : codes) {
      group.codes[0] = read_prefix_code(in_, 256 + 24 + cache_size);
      for (std::size_t i = 1; i < 4; ++i) group.codes[i] = read_prefix_code(in_, 256);
      group.codes[4] = read_prefix_code(in_, 40);
    }

    const std::size_t total = coded_width * height;
    if (total > (1ULL << 28)) fail("image is too large");
    std::vector<std::uint32_t> pixels(total);
    std::vector<std::uint32_t> cache(static_cast<std::size_t>(cache_size), 0);
    const auto remember = [&](std::uint32_t pixel) {
      if (cache_bits > 0) cache[(0x1E35A7BDU * pixel) >> (32 - cache_bits)] = pixel;
    };
    const auto copy_value = [&](int symbol) -> std::size_t {
      if (symbol < 4) return static_cast<std::size_t>(symbol) + 1;
      const int extra = (symbol - 2) >> 1;
      const std::size_t offset = static_cast<std::size_t>(2 + (symbol & 1)) << extra;
      return offset + in_.bits(extra) + 1;
    };
    std::size_t pos = 0;
    while (pos < total) {
      const std::size_t x = pos % coded_width, y = pos / coded_width;
      const Group& group = meta.empty() ? codes[0] : codes[meta[(y >> meta_bits) * meta_width + (x >> meta_bits)]];
      const int green = group.codes[0].decode(in_);
      if (green < 256) {
        const auto red = static_cast<std::uint32_t>(group.codes[1].decode(in_));
        const auto blue = static_cast<std::uint32_t>(group.codes[2].decode(in_));
        const auto alpha = static_cast<std::uint32_t>(group.codes[3].decode(in_));
        const std::uint32_t pixel = alpha << 24U | red << 16U | static_cast<std::uint32_t>(green) << 8U | blue;
        pixels[pos++] = pixel;
        remember(pixel);
      } else if (green < 256 + 24) {
        const std::size_t length = copy_value(green - 256);
        const std::size_t code = copy_value(group.codes[4].decode(in_));
        std::size_t distance = 0;
        if (code > 120) {
          distance = code - 120;
        } else {
          const std::uint8_t plane = code_to_plane[code - 1];
          const std::int64_t d = static_cast<std::int64_t>(plane >> 4U) * static_cast<std::int64_t>(coded_width) +
                                 (8 - static_cast<std::int64_t>(plane & 15U));
          distance = d >= 1 ? static_cast<std::size_t>(d) : 1;
        }
        if (distance > pos || length > total - pos) fail("corrupt WebP backward reference");
        for (std::size_t i = 0; i < length; ++i, ++pos) {
          pixels[pos] = pixels[pos - distance];
          remember(pixels[pos]);
        }
      } else if (green - 280 < cache_size) {
        pixels[pos++] = cache[static_cast<std::size_t>(green - 280)];
      } else {
        fail("corrupt WebP color cache index");
      }
    }

    for (auto transform = transforms.rbegin(); transform != transforms.rend(); ++transform)
      pixels = invert(*transform, std::move(pixels), height);
    return pixels;
  }

  static std::vector<std::uint32_t> invert(const Transform& t, std::vector<std::uint32_t> pixels, std::size_t height) {
    const std::size_t width = t.width;
    switch (t.type) {
      case 0: {
        const std::size_t tiles = div_round_up(width, t.bits);
        for (std::size_t y = 0; y < height; ++y)
          for (std::size_t x = 0; x < width; ++x) {
            std::uint32_t* p = pixels.data() + y * width + x;
            std::uint32_t predicted = 0;
            if (y == 0) {
              predicted = x == 0 ? 0xFF000000U : p[-1];
            } else if (x == 0) {
              predicted = p[-static_cast<std::ptrdiff_t>(width)];
            } else {
              const std::uint32_t left = p[-1];
              const std::uint32_t* up = p - width;
              const std::uint32_t mode = (t.data[(y >> t.bits) * tiles + (x >> t.bits)] >> 8U) & 15U;
              switch (mode) {
                case 1: predicted = left; break;
                case 2: predicted = up[0]; break;
                case 3: predicted = up[1]; break;
                case 4: predicted = up[-1]; break;
                case 5: predicted = average2(average2(left, up[1]), up[0]); break;
                case 6: predicted = average2(left, up[-1]); break;
                case 7: predicted = average2(left, up[0]); break;
                case 8: predicted = average2(up[-1], up[0]); break;
                case 9: predicted = average2(up[0], up[1]); break;
                case 10: predicted = average2(average2(left, up[-1]), average2(up[0], up[1])); break;
                case 11: predicted = select(up[0], left, up[-1]); break;
                case 12: predicted = clamped_add_subtract_full(left, up[0], up[-1]); break;
                case 13: predicted = clamped_add_subtract_half(left, up[0], up[-1]); break;
                default: predicted = 0xFF000000U;
              }
            }
            *p = add_pixels(*p, predicted);
          }
        return pixels;
      }
      case 1: {
        const std::size_t tiles = div_round_up(width, t.bits);
        const auto delta = [](std::uint32_t predictor, std::uint32_t color) {
          return (static_cast<int>(static_cast<std::int8_t>(predictor)) * static_cast<int>(static_cast<std::int8_t>(color))) >> 5;
        };
        for (std::size_t y = 0; y < height; ++y)
          for (std::size_t x = 0; x < width; ++x) {
            const std::uint32_t element = t.data[(y >> t.bits) * tiles + (x >> t.bits)];
            const std::uint32_t green_to_red = element & 255U, green_to_blue = (element >> 8U) & 255U,
                                red_to_blue = (element >> 16U) & 255U;
            std::uint32_t& p = pixels[y * width + x];
            const std::uint32_t green = (p >> 8U) & 255U;
            const std::uint32_t red = static_cast<std::uint32_t>(static_cast<int>((p >> 16U) & 255U) + delta(green_to_red, green)) & 255U;
            std::uint32_t blue = static_cast<std::uint32_t>(static_cast<int>(p & 255U) + delta(green_to_blue, green)) & 255U;
            blue = static_cast<std::uint32_t>(static_cast<int>(blue) + delta(red_to_blue, red)) & 255U;
            p = (p & 0xFF00FF00U) | red << 16U | blue;
          }
        return pixels;
      }
      case 2:
        for (std::uint32_t& p : pixels) {
          const std::uint32_t green = (p >> 8U) & 255U;
          p = (p & 0xFF00FF00U) | ((((p >> 16U) + green) & 255U) << 16U) | ((p + green) & 255U);
        }
        return pixels;
      default: {
        const std::size_t coded = div_round_up(width, t.bits);
        std::vector<std::uint32_t> out(width * height);
        const int per_pixel = 8 >> t.bits;
        const std::uint32_t mask = (1U << per_pixel) - 1U;
        for (std::size_t y = 0; y < height; ++y)
          for (std::size_t x = 0; x < width; ++x) {
            const std::uint32_t packed = (pixels[y * coded + (x >> t.bits)] >> 8U) & 255U;
            const std::uint32_t index =
                (packed >> ((x & ((1U << t.bits) - 1U)) * static_cast<std::size_t>(per_pixel))) & mask;
            out[y * width + x] = t.data[index];
          }
        return out;
      }
    }
  }

  LsbReader in_;
};

DecodedRaster decode_vp8l(std::span<const std::uint8_t> data) {
  if (data.empty() || data[0] != 0x2F) fail("invalid WebP lossless signature");
  Vp8lDecoder decoder(data.subspan(1));
  const std::size_t width = decoder.bits(14) + 1;
  const std::size_t height = decoder.bits(14) + 1;
  (void)decoder.bits(1);
  if (decoder.bits(3) != 0) fail("unsupported WebP lossless version");
  DecodedRaster image = blank_raster(width, height);
  const std::vector<std::uint32_t> argb = decoder.image(width, height);
  for (std::size_t i = 0; i < argb.size(); ++i) {
    const std::uint32_t p = argb[i];
    image.rgba[i] = pack_rgba((p >> 16U) & 255U, (p >> 8U) & 255U, p & 255U, p >> 24U);
  }
  return image;
}

// ---------------------------------------------------------------------------
// VP8

class BoolDecoder {
 public:
  BoolDecoder() = default;
  explicit BoolDecoder(std::span<const std::uint8_t> data) : p_(data.data()), end_(data.data() + data.size()) {
    value_ = static_cast<std::uint32_t>(next()) << 8U;
    value_ |= next();
  }
  int bit(int probability) {
    const std::uint32_t split = 1 + (((range_ - 1) * static_cast<std::uint32_t>(probability)) >> 8U);
    const std::uint32_t big_split = split << 8U;
    int result = 0;
    if (value_ >= big_split) {
      result = 1;
      range_ -= split;
      value_ -= big_split;
    } else {
      range_ = split;
    }
    while (range_ < 128) {
      value_ <<= 1U;
      range_ <<= 1U;
      if (++bit_count_ == 8) {
        bit_count_ = 0;
        value_ |= next();
      }
    }
    return result;
  }
  std::uint32_t literal(int n) {
    std::uint32_t value = 0;
    while (n-- > 0) value = value << 1U | static_cast<std::uint32_t>(bit(128));
    return value;
  }
  int signed_literal(int n) {
    const int value = static_cast<int>(literal(n));
    return bit(128) != 0 ? -value : value;
  }
  int optional_signed(int n) { return bit(128) != 0 ? signed_literal(n) : 0; }

 private:
  std::uint8_t next() { return p_ < end_ ? *p_++ : 0; }
  const std::uint8_t* p_ = nullptr;
  const std::uint8_t* end_ = nullptr;
  std::uint32_t value_ = 0;
  std::uint32_t range_ = 255;
  int bit_count_ = 0;
};

enum : int {
  b_dc = 0, b_tm, b_ve, b_he, b_rd, b_vr, b_ld, b_vl, b_hd, b_hu,
  dc_pred = b_dc, v_pred = b_ve, h_pred = b_he, tm_pred = b_tm,
};

constexpr std::array<std::int8_t, 18> bmode_tree{
    -b_dc, 1, -b_tm, 2, -b_ve, 3, 4, 6, -b_he, 5, -b_rd, -b_vr, -b_ld, 7, -b_vl, 8, -b_hd, -b_hu};
constexpr std::array<std::uint8_t, 17> bands{0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0};
constexpr std::array<std::uint8_t, 16> zigzag4{0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
constexpr std::array<std::uint8_t, 4> cat3{173, 148, 140, 0};
constexpr std::array<std::uint8_t, 5> cat4{176, 155, 140, 135, 0};
constexpr std::array<std::uint8_t, 6> cat5{180, 157, 141, 134, 130, 0};
constexpr std::array<std::uint8_t, 12> cat6{254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0};

constexpr int bps = 32;

std::uint8_t clip8(int value) { return static_cast<std::uint8_t>(std::clamp(value, 0, 255)); }

int mul1(int a) { return ((a * 20091) >> 16) + a; }
int mul2(int a) { return (a * 35468) >> 16; }

void inverse_transform(const std::int16_t* in, std::uint8_t* dst) {
  std::array<int, 16> c{};
  for (int i = 0; i < 4; ++i) {
    const int a = in[i] + in[8 + i];
    const int b = in[i] - in[8 + i];
    const int cc = mul2(in[4 + i]) - mul1(in[12 + i]);
    const int d = mul1(in[4 + i]) + mul2(in[12 + i]);
    c[static_cast<std::size_t>(i * 4 + 0)] = a + d;
    c[static_cast<std::size_t>(i * 4 + 1)] = b + cc;
    c[static_cast<std::size_t>(i * 4 + 2)] = b - cc;
    c[static_cast<std::size_t>(i * 4 + 3)] = a - d;
  }
  for (int i = 0; i < 4; ++i) {
    const int dc = c[static_cast<std::size_t>(i)] + 4;
    const int a = dc + c[static_cast<std::size_t>(8 + i)];
    const int b = dc - c[static_cast<std::size_t>(8 + i)];
    const int cc = mul2(c[static_cast<std::size_t>(4 + i)]) - mul1(c[static_cast<std::size_t>(12 + i)]);
    const int d = mul1(c[static_cast<std::size_t>(4 + i)]) + mul2(c[static_cast<std::size_t>(12 + i)]);
    std::uint8_t* row = dst + i * bps;
    row[0] = clip8(row[0] + ((a + d) >> 3));
    row[1] = clip8(row[1] + ((b + cc) >> 3));
    row[2] = clip8(row[2] + ((b - cc) >> 3));
    row[3] = clip8(row[3] + ((a - d) >> 3));
  }
}

void inverse_wht(const std::int16_t* in, std::int16_t* out) {
  std::array<int, 16> t{};
  for (int i = 0; i < 4; ++i) {
    const int a0 = in[i] + in[12 + i];
    const int a1 = in[4 + i] + in[8 + i];
    const int a2 = in[4 + i] - in[8 + i];
    const int a3 = in[i] - in[12 + i];
    t[static_cast<std::size_t>(i)] = a0 + a1;
    t[static_cast<std::size_t>(8 + i)] = a0 - a1;
    t[static_cast<std::size_t>(4 + i)] = a3 + a2;
    t[static_cast<std::size_t>(12 + i)] = a3 - a2;
  }
  for (int i = 0; i < 4; ++i) {
    const int dc = t[static_cast<std::size_t>(i * 4)] + 3;
    const int a0 = dc + t[static_cast<std::size_t>(i * 4 + 3)];
    const int a1 = t[static_cast<std::size_t>(i * 4 + 1)] + t[static_cast<std::size_t>(i * 4 + 2)];
    const int a2 = t[static_cast<std::size_t>(i * 4 + 1)] - t[static_cast<std::size_t>(i * 4 + 2)];
    const int a3 = dc - t[static_cast<std::size_t>(i * 4 + 3)];
    std::int16_t* o = out + i * 64;
    o[0] = static_cast<std::int16_t>((a0 + a1) >> 3);
    o[16] = static_cast<std::int16_t>((a3 + a2) >> 3);
    o[32] = static_cast<std::int16_t>((a0 - a1) >> 3);
    o[48] = static_cast<std::int16_t>((a3 - a2) >> 3);
  }
}

// Whole-block predictions, with the edge variants of DC.
void predict_block(std::uint8_t* dst, int size, int mode, bool has_top, bool has_left) {
  const std::uint8_t* top = dst - bps;
  if (mode == dc_pred) {
    int sum = 0, count = 0;
    if (has_top) {
      for (int i = 0; i < size; ++i) sum += top[i];
      count += size;
    }
    if (has_left) {
      for (int i = 0; i < size; ++i) sum += dst[i * bps - 1];
      count += size;
    }
    const int value = count == 0 ? 128 : (sum + count / 2) / count;
    for (int y = 0; y < size; ++y) std::memset(dst + y * bps, value, static_cast<std::size_t>(size));
  } else if (mode == v_pred) {
    for (int y = 0; y < size; ++y) std::memcpy(dst + y * bps, top, static_cast<std::size_t>(size));
  } else if (mode == h_pred) {
    for (int y = 0; y < size; ++y) std::memset(dst + y * bps, dst[y * bps - 1], static_cast<std::size_t>(size));
  } else {
    const int corner = top[-1];
    for (int y = 0; y < size; ++y)
      for (int x = 0; x < size; ++x) dst[y * bps + x] = clip8(top[x] + dst[y * bps - 1] - corner);
  }
}

int avg3(int a, int b, int c) { return (a + 2 * b + c + 2) >> 2; }
int avg2(int a, int b) { return (a + b + 1) >> 1; }

void predict_subblock(std::uint8_t* dst, int mode) {
  const auto set = [dst](int x, int y, int value) { dst[x + y * bps] = static_cast<std::uint8_t>(value); };
  const std::uint8_t* top = dst - bps;
  const int X = top[-1];
  const int A = top[0], B = top[1], C = top[2], D = top[3], E = top[4], F = top[5], G = top[6], H = top[7];
  const int I = dst[-1], J = dst[bps - 1], K = dst[2 * bps - 1], L = dst[3 * bps - 1];
  switch (mode) {
    case b_dc: {
      int dc = 4;
      for (int i = 0; i < 4; ++i) dc += top[i] + dst[i * bps - 1];
      for (int y = 0; y < 4; ++y) std::memset(dst + y * bps, dc >> 3, 4);
      break;
    }
    case b_tm:
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) dst[y * bps + x] = clip8(top[x] + dst[y * bps - 1] - X);
      break;
    case b_ve: {
      const std::array<int, 4> row{avg3(X, A, B), avg3(A, B, C), avg3(B, C, D), avg3(C, D, E)};
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) set(x, y, row[static_cast<std::size_t>(x)]);
      break;
    }
    case b_he: {
      const std::array<int, 4> column{avg3(X, I, J), avg3(I, J, K), avg3(J, K, L), avg3(K, L, L)};
      for (int y = 0; y < 4; ++y) std::memset(dst + y * bps, column[static_cast<std::size_t>(y)], 4);
      break;
    }
    case b_rd:
      set(0, 3, avg3(J, K, L));
      set(1, 3, avg3(I, J, K)); set(0, 2, avg3(I, J, K));
      set(2, 3, avg3(X, I, J)); set(1, 2, avg3(X, I, J)); set(0, 1, avg3(X, I, J));
      set(3, 3, avg3(A, X, I)); set(2, 2, avg3(A, X, I)); set(1, 1, avg3(A, X, I)); set(0, 0, avg3(A, X, I));
      set(3, 2, avg3(B, A, X)); set(2, 1, avg3(B, A, X)); set(1, 0, avg3(B, A, X));
      set(3, 1, avg3(C, B, A)); set(2, 0, avg3(C, B, A));
      set(3, 0, avg3(D, C, B));
      break;
    case b_ld:
      set(0, 0, avg3(A, B, C));
      set(1, 0, avg3(B, C, D)); set(0, 1, avg3(B, C, D));
      set(2, 0, avg3(C, D, E)); set(1, 1, avg3(C, D, E)); set(0, 2, avg3(C, D, E));
      set(3, 0, avg3(D, E, F)); set(2, 1, avg3(D, E, F)); set(1, 2, avg3(D, E, F)); set(0, 3, avg3(D, E, F));
      set(3, 1, avg3(E, F, G)); set(2, 2, avg3(E, F, G)); set(1, 3, avg3(E, F, G));
      set(3, 2, avg3(F, G, H)); set(2, 3, avg3(F, G, H));
      set(3, 3, avg3(G, H, H));
      break;
    case b_vr:
      set(0, 0, avg2(X, A)); set(1, 2, avg2(X, A));
      set(1, 0, avg2(A, B)); set(2, 2, avg2(A, B));
      set(2, 0, avg2(B, C)); set(3, 2, avg2(B, C));
      set(3, 0, avg2(C, D));
      set(0, 3, avg3(K, J, I));
      set(0, 2, avg3(J, I, X));
      set(0, 1, avg3(I, X, A)); set(1, 3, avg3(I, X, A));
      set(1, 1, avg3(X, A, B)); set(2, 3, avg3(X, A, B));
      set(2, 1, avg3(A, B, C)); set(3, 3, avg3(A, B, C));
      set(3, 1, avg3(B, C, D));
      break;
    case b_vl:
      set(0, 0, avg2(A, B));
      set(1, 0, avg2(B, C)); set(0, 2, avg2(B, C));
      set(2, 0, avg2(C, D)); set(1, 2, avg2(C, D));
      set(3, 0, avg2(D, E)); set(2, 2, avg2(D, E));
      set(0, 1, avg3(A, B, C));
      set(1, 1, avg3(B, C, D)); set(0, 3, avg3(B, C, D));
      set(2, 1, avg3(C, D, E)); set(1, 3, avg3(C, D, E));
      set(3, 1, avg3(D, E, F)); set(2, 3, avg3(D, E, F));
      set(3, 2, avg3(E, F, G));
      set(3, 3, avg3(F, G, H));
      break;
    case b_hd:
      set(0, 0, avg2(I, X)); set(2, 1, avg2(I, X));
      set(0, 1, avg2(J, I)); set(2, 2, avg2(J, I));
      set(0, 2, avg2(K, J)); set(2, 3, avg2(K, J));
      set(0, 3, avg2(L, K));
      set(3, 0, avg3(A, B, C));
      set(2, 0, avg3(X, A, B));
      set(1, 0, avg3(I, X, A)); set(3, 1, avg3(I, X, A));
      set(1, 1, avg3(J, I, X)); set(3, 2, avg3(J, I, X));
      set(1, 2, avg3(K, J, I)); set(3, 3, avg3(K, J, I));
      set(1, 3, avg3(L, K, J));
      break;
    default:
      set(0, 0, avg2(I, J));
      set(2, 0, avg2(J, K)); set(0, 1, avg2(J, K));
      set(2, 1, avg2(K, L)); set(0, 2, avg2(K, L));
      set(1, 0, avg3(I, J, K));
      set(3, 0, avg3(J, K, L)); set(1, 1, avg3(J, K, L));
      set(3, 1, avg3(K, L, L)); set(1, 2, avg3(K, L, L));
      set(3, 2, L); set(2, 2, L); set(0, 3, L); set(1, 3, L); set(2, 3, L); set(3, 3, L);
  }
}

// Loop filter (RFC 6386 section 15).
int sclip1(int v) { return std::clamp(v, -128, 127); }
int sclip2(int v) { return std::clamp(v, -16, 15); }

void filter2(std::uint8_t* p, int step) {
  const int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
  const int a = 3 * (q0 - p0) + sclip1(p1 - q1);
  const int a1 = sclip2((a + 4) >> 3);
  const int a2 = sclip2((a + 3) >> 3);
  p[-step] = clip8(p0 + a2);
  p[0] = clip8(q0 - a1);
}

void filter4(std::uint8_t* p, int step) {
  const int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
  const int a = 3 * (q0 - p0);
  const int a1 = sclip2((a + 4) >> 3);
  const int a2 = sclip2((a + 3) >> 3);
  const int a3 = (a1 + 1) >> 1;
  p[-2 * step] = clip8(p1 + a3);
  p[-step] = clip8(p0 + a2);
  p[0] = clip8(q0 - a1);
  p[step] = clip8(q1 - a3);
}

void filter6(std::uint8_t* p, int step) {
  const int p2 = p[-3 * step], p1 = p[-2 * step], p0 = p[-step];
  const int q0 = p[0], q1 = p[step], q2 = p[2 * step];
  const int a = sclip1(3 * (q0 - p0) + sclip1(p1 - q1));
  const int a1 = (27 * a + 63) >> 7;
  const int a2 = (18 * a + 63) >> 7;
  const int a3 = (9 * a + 63) >> 7;
  p[-3 * step] = clip8(p2 + a3);
  p[-2 * step] = clip8(p1 + a2);
  p[-step] = clip8(p0 + a1);
  p[0] = clip8(q0 - a1);
  p[step] = clip8(q1 - a2);
  p[2 * step] = clip8(q2 - a3);
}

bool high_edge_variance(const std::uint8_t* p, int step, int threshold) {
  return std::abs(p[-2 * step] - p[-step]) > threshold || std::abs(p[step] - p[0]) > threshold;
}

bool needs_filter(const std::uint8_t* p, int step, int t) {
  return 4 * std::abs(p[-step] - p[0]) + std::abs(p[-2 * step] - p[step]) <= t;
}

bool needs_filter2(const std::uint8_t* p, int step, int t, int it) {
  const int p3 = p[-4 * step], p2 = p[-3 * step], p1 = p[-2 * step], p0 = p[-step];
  const int q0 = p[0], q1 = p[step], q2 = p[2 * step], q3 = p[3 * step];
  if (4 * std::abs(p0 - q0) + std::abs(p1 - q1) > t) return false;
  return std::abs(p3 - p2) <= it && std::abs(p2 - p1) <= it && std::abs(p1 - p0) <= it &&
         std::abs(q3 - q2) <= it && std::abs(q2 - q1) <= it && std::abs(q1 - q0) <= it;
}

void simple_edge(std::uint8_t* p, int step, int along, int threshold) {
  const int t = 2 * threshold + 1;
  for (int i = 0; i < 16; ++i)
    if (needs_filter(p + i * along, step, t)) filter2(p + i * along, step);
}

void complex_edge(std::uint8_t* p, int step, int along, int size, int threshold, int interior, int hev,
                  bool macroblock_edge) {
  const int t = 2 * threshold + 1;
  for (int i = 0; i < size; ++i, p += along) {
    if (!needs_filter2(p, step, t, interior)) continue;
    if (high_edge_variance(p, step, hev)) filter2(p, step);
    else if (macroblock_edge) filter6(p, step);
    else filter4(p, step);
  }
}

struct Plane {
  std::vector<std::uint8_t> data;
  int stride = 0;
  std::uint8_t* at(int x, int y) { return data.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(x); }
};

struct Vp8Frame {
  int width = 0, height = 0;
  int mb_wide = 0, mb_high = 0;
  Plane y, u, v;
};

Vp8Frame decode_vp8_frame(std::span<const std::uint8_t> data) {
  Reader header(data);
  const std::uint32_t tag = header.le24();
  if ((tag & 1U) != 0) fail("WebP frame is not a key frame");
  if (((tag >> 1U) & 7U) > 3) fail("unsupported VP8 profile");
  const std::uint32_t first_size = tag >> 5U;
  const std::uint8_t* start = header.take(3);
  if (start[0] != 0x9D || start[1] != 0x01 || start[2] != 0x2A) fail("invalid VP8 start code");
  Vp8Frame frame;
  frame.width = header.le16() & 0x3FFF;
  frame.height = header.le16() & 0x3FFF;
  (void)blank_raster(static_cast<std::uint64_t>(frame.width), static_cast<std::uint64_t>(frame.height));
  if (first_size > header.left()) fail("WebP data is truncated");
  BoolDecoder bits(std::span(header.take(first_size), first_size));

  (void)bits.literal(2);  // color space, clamping
  bool segments_enabled = bits.bit(128) != 0;
  bool update_map = false, absolute = false;
  std::array<int, 4> segment_quant{}, segment_filter{};
  std::array<int, 3> segment_probs{255, 255, 255};
  if (segments_enabled) {
    update_map = bits.bit(128) != 0;
    if (bits.bit(128) != 0) {
      absolute = bits.bit(128) != 0;
      for (int& q : segment_quant) q = bits.optional_signed(7);
      for (int& f : segment_filter) f = bits.optional_signed(6);
    }
    if (update_map)
      for (int& p : segment_probs) p = bits.bit(128) != 0 ? static_cast<int>(bits.literal(8)) : 255;
  }
  const bool simple_filter = bits.bit(128) != 0;
  const int filter_level = static_cast<int>(bits.literal(6));
  const int sharpness = static_cast<int>(bits.literal(3));
  std::array<int, 4> ref_delta{}, mode_delta{};
  const bool use_deltas = bits.bit(128) != 0;
  if (use_deltas && bits.bit(128) != 0) {
    for (int& d : ref_delta) d = bits.optional_signed(6);
    for (int& d : mode_delta) d = bits.optional_signed(6);
  }
  const int partition_count = 1 << bits.literal(2);

  std::vector<BoolDecoder> partitions;
  {
    const std::size_t sizes_bytes = 3 * static_cast<std::size_t>(partition_count - 1);
    const std::uint8_t* sizes = header.take(sizes_bytes);
    std::size_t remaining = header.left();
    const std::uint8_t* part = header.take(remaining);
    for (int i = 0; i < partition_count; ++i) {
      std::size_t size = remaining;
      if (i + 1 < partition_count) {
        const std::uint8_t* s = sizes + 3 * i;
        size = static_cast<std::size_t>(s[0] | s[1] << 8 | s[2] << 16);
        if (size > remaining) fail("WebP data is truncated");
      }
      partitions.emplace_back(std::span(part, size));
      part += size;
      remaining -= size;
    }
  }

  const int base_q = static_cast<int>(bits.literal(7));
  const int y1_dc = bits.optional_signed(4), y2_dc = bits.optional_signed(4), y2_ac = bits.optional_signed(4),
            uv_dc = bits.optional_signed(4), uv_ac = bits.optional_signed(4);
  struct Quant { int y1[2], y2[2], uv[2]; };
  std::array<Quant, 4> quant{};
  for (int s = 0; s < 4; ++s) {
    int q = base_q;
    if (segments_enabled) q = segment_quant[static_cast<std::size_t>(s)] + (absolute ? 0 : base_q);
    const auto index = [](int v, int maximum) { return static_cast<std::size_t>(std::clamp(v, 0, maximum)); };
    Quant& m = quant[static_cast<std::size_t>(s)];
    m.y1[0] = vp8_dc_quant[index(q + y1_dc, 127)];
    m.y1[1] = vp8_ac_quant[index(q, 127)];
    m.y2[0] = vp8_dc_quant[index(q + y2_dc, 127)] * 2;
    m.y2[1] = std::max(8, (vp8_ac_quant[index(q + y2_ac, 127)] * 101581) >> 16);
    m.uv[0] = vp8_dc_quant[index(q + uv_dc, 117)];
    m.uv[1] = vp8_ac_quant[index(q + uv_ac, 127)];
  }
  (void)bits.bit(128);  // refresh entropy probabilities
  std::array<std::array<std::array<std::array<std::uint8_t, 11>, 3>, 8>, 4> probs{};
  for (int t = 0; t < 4; ++t)
    for (int b = 0; b < 8; ++b)
      for (int c = 0; c < 3; ++c)
        for (int p = 0; p < 11; ++p)
          probs[static_cast<std::size_t>(t)][static_cast<std::size_t>(b)][static_cast<std::size_t>(c)][static_cast<std::size_t>(p)] =
              bits.bit(vp8_coeff_update_probs[((t * 8 + b) * 3 + c) * 11 + p]) != 0
                  ? static_cast<std::uint8_t>(bits.literal(8))
                  : vp8_coeff_probs[((t * 8 + b) * 3 + c) * 11 + p];
  const bool use_skip = bits.bit(128) != 0;
  const int skip_prob = use_skip ? static_cast<int>(bits.literal(8)) : 0;

  struct Strength { int limit = 0, interior = 0, hev = 0; bool inner = false; };
  std::array<std::array<Strength, 2>, 4> strengths{};
  const int filter_type = filter_level == 0 ? 0 : simple_filter ? 1 : 2;
  for (int s = 0; s < 4; ++s) {
    int base = filter_level;
    if (segments_enabled) base = segment_filter[static_cast<std::size_t>(s)] + (absolute ? 0 : filter_level);
    for (int i4 = 0; i4 < 2; ++i4) {
      int level = base;
      if (use_deltas) {
        level += ref_delta[0];
        if (i4 != 0) level += mode_delta[0];
      }
      level = std::clamp(level, 0, 63);
      Strength& info = strengths[static_cast<std::size_t>(s)][static_cast<std::size_t>(i4)];
      info.inner = i4 != 0;
      if (level == 0) continue;
      int interior = level;
      if (sharpness > 0) {
        interior >>= sharpness > 4 ? 2 : 1;
        interior = std::min(interior, 9 - sharpness);
      }
      interior = std::max(interior, 1);
      info.interior = interior;
      info.limit = 2 * level + interior;
      info.hev = level >= 40 ? 2 : level >= 15 ? 1 : 0;
    }
  }

  frame.mb_wide = (frame.width + 15) / 16;
  frame.mb_high = (frame.height + 15) / 16;
  frame.y.stride = frame.mb_wide * 16;
  frame.u.stride = frame.v.stride = frame.mb_wide * 8;
  frame.y.data.assign(static_cast<std::size_t>(frame.y.stride) * static_cast<std::size_t>(frame.mb_high) * 16U, 0);
  frame.u.data.assign(static_cast<std::size_t>(frame.u.stride) * static_cast<std::size_t>(frame.mb_high) * 8U, 0);
  frame.v.data = frame.u.data;

  std::vector<std::array<std::uint8_t, 4>> top_modes(static_cast<std::size_t>(frame.mb_wide));
  std::vector<std::array<std::uint8_t, 4>> top_nz_y(static_cast<std::size_t>(frame.mb_wide));
  std::vector<std::array<std::uint8_t, 2>> top_nz_u(static_cast<std::size_t>(frame.mb_wide)), top_nz_v(static_cast<std::size_t>(frame.mb_wide));
  std::vector<std::uint8_t> top_nz_dc(static_cast<std::size_t>(frame.mb_wide));
  std::vector<Strength> filters(static_cast<std::size_t>(frame.mb_wide) * static_cast<std::size_t>(frame.mb_high));

  std::array<std::uint8_t, bps * 17> ywork{};
  std::array<std::uint8_t, bps * 9> uwork{}, vwork{};
  std::uint8_t* const yd = ywork.data() + bps + 8;
  std::uint8_t* const ud = uwork.data() + bps + 8;
  std::uint8_t* const vd = vwork.data() + bps + 8;
  std::array<std::int16_t, 384> coeffs{};

  const auto read_coeffs = [&](BoolDecoder& br, int type, int context, const int* dq, int n, std::int16_t* out) -> int {
    const auto& table = probs[static_cast<std::size_t>(type)];
    const std::uint8_t* p = table[bands[static_cast<std::size_t>(n)]][static_cast<std::size_t>(context)].data();
    for (; n < 16; ++n) {
      if (br.bit(p[0]) == 0) return n;
      while (br.bit(p[1]) == 0) {
        p = table[bands[static_cast<std::size_t>(++n)]][0].data();
        if (n == 16) return 16;
      }
      const auto& next = table[bands[static_cast<std::size_t>(n + 1)]];
      int value = 0;
      if (br.bit(p[2]) == 0) {
        value = 1;
        p = next[1].data();
      } else {
        if (br.bit(p[3]) == 0) {
          value = br.bit(p[4]) == 0 ? 2 : 3 + br.bit(p[5]);
        } else if (br.bit(p[6]) == 0) {
          value = br.bit(p[7]) == 0 ? 5 + br.bit(159) : 7 + 2 * br.bit(165) + br.bit(145);
        } else {
          const int high = br.bit(p[8]);
          const int low = br.bit(p[9 + high]);
          const int category = 2 * high + low;
          const std::uint8_t* extra = category == 0 ? cat3.data() : category == 1 ? cat4.data() : category == 2 ? cat5.data() : cat6.data();
          value = 0;
          for (; *extra != 0; ++extra) value += value + br.bit(*extra);
          value += 3 + (8 << category);
        }
        p = next[2].data();
      }
      if (br.bit(128) != 0) value = -value;
      out[zigzag4[static_cast<std::size_t>(n)]] = static_cast<std::int16_t>(value * dq[n > 0 ? 1 : 0]);
    }
    return 16;
  };

  for (int mby = 0; mby < frame.mb_high; ++mby) {
    BoolDecoder& tokens = partitions[static_cast<std::size_t>(mby & (partition_count - 1))];
    std::array<std::uint8_t, 4> left_modes{};
    std::array<std::uint8_t, 4> left_nz_y{};
    std::array<std::uint8_t, 2> left_nz_u{}, left_nz_v{};
    std::uint8_t left_nz_dc = 0;
    for (int mbx = 0; mbx < frame.mb_wide; ++mbx) {
      const auto ux = static_cast<std::size_t>(mbx);
      // Modes, from the first partition.
      int segment = 0;
      if (update_map)
        segment = bits.bit(segment_probs[0]) == 0 ? bits.bit(segment_probs[1]) : 2 + bits.bit(segment_probs[2]);
      const bool skip = use_skip && bits.bit(skip_prob) != 0;
      const bool i4 = bits.bit(145) == 0;
      int ymode = dc_pred;
      std::array<std::uint8_t, 16> sub_modes{};
      if (!i4) {
        ymode = bits.bit(156) != 0 ? (bits.bit(128) != 0 ? tm_pred : h_pred) : (bits.bit(163) != 0 ? v_pred : dc_pred);
        top_modes[ux].fill(static_cast<std::uint8_t>(ymode));
        left_modes.fill(static_cast<std::uint8_t>(ymode));
      } else {
        for (int y = 0; y < 4; ++y) {
          int left = left_modes[static_cast<std::size_t>(y)];
          for (int x = 0; x < 4; ++x) {
            const std::uint8_t* prob = vp8_bmode_probs + (top_modes[ux][static_cast<std::size_t>(x)] * 10 + left) * 9;
            int i = bmode_tree[static_cast<std::size_t>(bits.bit(prob[0]))];
            while (i > 0) i = bmode_tree[static_cast<std::size_t>(2 * i + bits.bit(prob[i]))];
            left = -i;
            top_modes[ux][static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(left);
            sub_modes[static_cast<std::size_t>(y * 4 + x)] = static_cast<std::uint8_t>(left);
          }
          left_modes[static_cast<std::size_t>(y)] = static_cast<std::uint8_t>(left);
        }
      }
      const int uv_mode = bits.bit(142) == 0 ? dc_pred : bits.bit(114) == 0 ? v_pred : bits.bit(183) != 0 ? tm_pred : h_pred;

      // Residuals, from this row's token partition.
      coeffs.fill(0);
      const Quant& q = quant[static_cast<std::size_t>(segment)];
      bool any_nonzero = false;
      if (!skip) {
        int first = 0;
        int ac_type = 3;
        if (!i4) {
          std::array<std::int16_t, 16> dc{};
          const int nz = read_coeffs(tokens, 1, top_nz_dc[ux] + left_nz_dc, q.y2, 0, dc.data());
          top_nz_dc[ux] = left_nz_dc = nz > 0 ? 1 : 0;
          if (nz > 1) {
            inverse_wht(dc.data(), coeffs.data());
          } else {
            const int dc0 = (dc[0] + 3) >> 3;
            for (int i = 0; i < 16; ++i) coeffs[static_cast<std::size_t>(i * 16)] = static_cast<std::int16_t>(dc0);
          }
          first = 1;
          ac_type = 0;
        }
        for (int y = 0; y < 4; ++y)
          for (int x = 0; x < 4; ++x) {
            std::int16_t* block = coeffs.data() + (y * 4 + x) * 16;
            const int context = left_nz_y[static_cast<std::size_t>(y)] + top_nz_y[ux][static_cast<std::size_t>(x)];
            const int nz = read_coeffs(tokens, ac_type, context, q.y1, first, block);
            const std::uint8_t flag = nz > first ? 1 : 0;
            left_nz_y[static_cast<std::size_t>(y)] = top_nz_y[ux][static_cast<std::size_t>(x)] = flag;
            any_nonzero = any_nonzero || nz > 1 || block[0] != 0;
          }
        for (int plane = 0; plane < 2; ++plane) {
          auto& top = plane == 0 ? top_nz_u[ux] : top_nz_v[ux];
          auto& left = plane == 0 ? left_nz_u : left_nz_v;
          for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x) {
              std::int16_t* block = coeffs.data() + 256 + plane * 64 + (y * 2 + x) * 16;
              const int nz = read_coeffs(tokens, 2, left[static_cast<std::size_t>(y)] + top[static_cast<std::size_t>(x)], q.uv, 0, block);
              left[static_cast<std::size_t>(y)] = top[static_cast<std::size_t>(x)] = nz > 0 ? 1 : 0;
              any_nonzero = any_nonzero || nz > 1 || block[0] != 0;
            }
        }
      } else {
        left_nz_y.fill(0);
        top_nz_y[ux].fill(0);
        left_nz_u.fill(0);
        left_nz_v.fill(0);
        top_nz_u[ux].fill(0);
        top_nz_v[ux].fill(0);
        if (!i4) top_nz_dc[ux] = left_nz_dc = 0;
      }
      if (filter_type > 0) {
        Strength strength = strengths[static_cast<std::size_t>(segment)][i4 ? 1 : 0];
        strength.inner = strength.inner || any_nonzero;
        filters[static_cast<std::size_t>(mby) * static_cast<std::size_t>(frame.mb_wide) + ux] = strength;
      }

      // Neighbours: 127 above the frame, 129 left of it.
      const int px = mbx * 16, py = mby * 16;
      for (int i = -1; i < 20; ++i) {
        std::uint8_t value = 127;
        if (mby > 0) {
          if (i < 0) value = mbx > 0 ? *frame.y.at(px - 1, py - 1) : 129;
          else if (i < 16) value = *frame.y.at(px + i, py - 1);
          else value = mbx < frame.mb_wide - 1 ? *frame.y.at(px + i, py - 1) : *frame.y.at(px + 15, py - 1);
        }
        yd[-bps + i] = value;
      }
      for (int r = 0; r < 16; ++r) yd[r * bps - 1] = mbx > 0 ? *frame.y.at(px - 1, py + r) : 129;
      for (int r = 1; r < 4; ++r) std::memcpy(yd + (4 * r - 1) * bps + 16, yd - bps + 16, 4);
      const int cx = mbx * 8, cy = mby * 8;
      for (int plane = 0; plane < 2; ++plane) {
        Plane& source = plane == 0 ? frame.u : frame.v;
        std::uint8_t* d = plane == 0 ? ud : vd;
        for (int i = -1; i < 8; ++i) {
          std::uint8_t value = 127;
          if (mby > 0) value = i < 0 ? (mbx > 0 ? *source.at(cx - 1, cy - 1) : 129) : *source.at(cx + i, cy - 1);
          d[-bps + i] = value;
        }
        for (int r = 0; r < 8; ++r) d[r * bps - 1] = mbx > 0 ? *source.at(cx - 1, cy + r) : 129;
      }

      if (i4) {
        for (int n = 0; n < 16; ++n) {
          std::uint8_t* dst = yd + (n / 4) * 4 * bps + (n % 4) * 4;
          predict_subblock(dst, sub_modes[static_cast<std::size_t>(n)]);
          inverse_transform(coeffs.data() + n * 16, dst);
        }
      } else {
        predict_block(yd, 16, ymode, mby > 0, mbx > 0);
        for (int n = 0; n < 16; ++n) inverse_transform(coeffs.data() + n * 16, yd + (n / 4) * 4 * bps + (n % 4) * 4);
      }
      predict_block(ud, 8, uv_mode, mby > 0, mbx > 0);
      predict_block(vd, 8, uv_mode, mby > 0, mbx > 0);
      for (int n = 0; n < 4; ++n) {
        inverse_transform(coeffs.data() + 256 + n * 16, ud + (n / 2) * 4 * bps + (n % 2) * 4);
        inverse_transform(coeffs.data() + 320 + n * 16, vd + (n / 2) * 4 * bps + (n % 2) * 4);
      }
      for (int r = 0; r < 16; ++r) std::memcpy(frame.y.at(px, py + r), yd + r * bps, 16);
      for (int r = 0; r < 8; ++r) {
        std::memcpy(frame.u.at(cx, cy + r), ud + r * bps, 8);
        std::memcpy(frame.v.at(cx, cy + r), vd + r * bps, 8);
      }
    }
  }

  if (filter_type > 0) {
    for (int mby = 0; mby < frame.mb_high; ++mby)
      for (int mbx = 0; mbx < frame.mb_wide; ++mbx) {
        const Strength& f = filters[static_cast<std::size_t>(mby) * static_cast<std::size_t>(frame.mb_wide) + static_cast<std::size_t>(mbx)];
        if (f.limit == 0) continue;
        std::uint8_t* y = frame.y.at(mbx * 16, mby * 16);
        const int ys = frame.y.stride;
        if (filter_type == 1) {
          if (mbx > 0) simple_edge(y, 1, ys, f.limit + 4);
          if (f.inner)
            for (int k = 4; k < 16; k += 4) simple_edge(y + k, 1, ys, f.limit);
          if (mby > 0) simple_edge(y, ys, 1, f.limit + 4);
          if (f.inner)
            for (int k = 4; k < 16; k += 4) simple_edge(y + k * ys, ys, 1, f.limit);
          continue;
        }
        std::uint8_t* u = frame.u.at(mbx * 8, mby * 8);
        std::uint8_t* v = frame.v.at(mbx * 8, mby * 8);
        const int cs = frame.u.stride;
        if (mbx > 0) {
          complex_edge(y, 1, ys, 16, f.limit + 4, f.interior, f.hev, true);
          complex_edge(u, 1, cs, 8, f.limit + 4, f.interior, f.hev, true);
          complex_edge(v, 1, cs, 8, f.limit + 4, f.interior, f.hev, true);
        }
        if (f.inner) {
          for (int k = 4; k < 16; k += 4) complex_edge(y + k, 1, ys, 16, f.limit, f.interior, f.hev, false);
          complex_edge(u + 4, 1, cs, 8, f.limit, f.interior, f.hev, false);
          complex_edge(v + 4, 1, cs, 8, f.limit, f.interior, f.hev, false);
        }
        if (mby > 0) {
          complex_edge(y, ys, 1, 16, f.limit + 4, f.interior, f.hev, true);
          complex_edge(u, cs, 1, 8, f.limit + 4, f.interior, f.hev, true);
          complex_edge(v, cs, 1, 8, f.limit + 4, f.interior, f.hev, true);
        }
        if (f.inner) {
          for (int k = 4; k < 16; k += 4) complex_edge(y + k * ys, ys, 1, 16, f.limit, f.interior, f.hev, false);
          complex_edge(u + 4 * cs, cs, 1, 8, f.limit, f.interior, f.hev, false);
          complex_edge(v + 4 * cs, cs, 1, 8, f.limit, f.interior, f.hev, false);
        }
      }
  }
  return frame;
}

// BT.601 limited range, chroma upsampled with 9:3:3:1 weights.
DecodedRaster vp8_to_rgba(Vp8Frame& frame) {
  DecodedRaster image = blank_raster(static_cast<std::uint64_t>(frame.width), static_cast<std::uint64_t>(frame.height));
  const int chroma_wide = (frame.width + 1) / 2, chroma_high = (frame.height + 1) / 2;
  const auto upsample = [&](Plane& plane, int x, int y) {
    const int cx = x >> 1, cy = y >> 1;
    const int nx = std::clamp((x & 1) != 0 ? cx + 1 : cx - 1, 0, chroma_wide - 1);
    const int ny = std::clamp((y & 1) != 0 ? cy + 1 : cy - 1, 0, chroma_high - 1);
    return (9 * *plane.at(cx, cy) + 3 * *plane.at(nx, cy) + 3 * *plane.at(cx, ny) + *plane.at(nx, ny) + 8) >> 4;
  };
  const auto mult_hi = [](int v, int coefficient) { return (v * coefficient) >> 8; };
  const auto clip = [](int v) { return (v & ~16383) == 0 ? v >> 6 : v < 0 ? 0 : 255; };
  for (int y = 0; y < frame.height; ++y)
    for (int x = 0; x < frame.width; ++x) {
      const int luma = *frame.y.at(x, y);
      const int u = upsample(frame.u, x, y), v = upsample(frame.v, x, y);
      const int r = clip(mult_hi(luma, 19077) + mult_hi(v, 26149) - 14234);
      const int g = clip(mult_hi(luma, 19077) - mult_hi(u, 6419) - mult_hi(v, 13320) + 8708);
      const int b = clip(mult_hi(luma, 19077) + mult_hi(u, 33050) - 17685);
      image.rgba[static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) + static_cast<std::size_t>(x)] =
          pack_rgba(static_cast<unsigned>(r), static_cast<unsigned>(g), static_cast<unsigned>(b));
    }
  return image;
}

void apply_alpha(DecodedRaster& image, std::span<const std::uint8_t> chunk) {
  if (chunk.empty()) fail("invalid WebP alpha");
  const unsigned header = chunk[0];
  const unsigned compression = header & 3U, filter = (header >> 2U) & 3U;
  const auto width = static_cast<std::size_t>(image.width), height = static_cast<std::size_t>(image.height);
  std::vector<std::uint8_t> alpha(width * height);
  if (compression == 0) {
    if (chunk.size() - 1 < alpha.size()) fail("WebP alpha is truncated");
    std::memcpy(alpha.data(), chunk.data() + 1, alpha.size());
  } else if (compression == 1) {
    Vp8lDecoder decoder(chunk.subspan(1));
    const std::vector<std::uint32_t> argb = decoder.image(width, height);
    for (std::size_t i = 0; i < alpha.size(); ++i) alpha[i] = static_cast<std::uint8_t>(argb[i] >> 8U);
  } else {
    fail("unsupported WebP alpha compression");
  }
  for (std::size_t y = 0; y < height && filter != 0; ++y)
    for (std::size_t x = 0; x < width; ++x) {
      std::uint8_t* p = alpha.data() + y * width + x;
      int predicted = 0;
      if (y == 0) predicted = x == 0 ? 0 : p[-1];
      else if (x == 0) predicted = p[-static_cast<std::ptrdiff_t>(width)];
      else if (filter == 1) predicted = p[-1];
      else if (filter == 2) predicted = p[-static_cast<std::ptrdiff_t>(width)];
      else predicted = std::clamp(p[-1] + p[-static_cast<std::ptrdiff_t>(width)] - p[-static_cast<std::ptrdiff_t>(width) - 1], 0, 255);
      *p = static_cast<std::uint8_t>(*p + predicted);
    }
  for (std::size_t i = 0; i < alpha.size(); ++i)
    image.rgba[i] = (image.rgba[i] & 0x00FFFFFFU) | static_cast<std::uint32_t>(alpha[i]) << 24U;
}

struct Chunk {
  std::uint32_t kind = 0;
  std::span<const std::uint8_t> data;
};

constexpr std::uint32_t chunk_id(const char (&name)[5]) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(name[0])) |
         static_cast<std::uint32_t>(static_cast<unsigned char>(name[1])) << 8U |
         static_cast<std::uint32_t>(static_cast<unsigned char>(name[2])) << 16U |
         static_cast<std::uint32_t>(static_cast<unsigned char>(name[3])) << 24U;
}

std::vector<Chunk> read_chunks(std::span<const std::uint8_t> data) {
  std::vector<Chunk> chunks;
  Reader reader(data);
  while (reader.left() >= 8) {
    const std::uint32_t kind = reader.le32();
    const std::uint32_t size = reader.le32();
    const std::size_t available = std::min<std::size_t>(size, reader.left());
    chunks.push_back({kind, std::span(reader.take(available), available)});
    if (reader.left() > 0 && (size & 1U) != 0) reader.skip(1);
  }
  return chunks;
}

// A frame from its image chunk and optional ALPH chunk.
DecodedRaster decode_frame(const std::vector<Chunk>& chunks) {
  std::span<const std::uint8_t> alpha;
  bool has_alpha = false;
  for (const Chunk& chunk : chunks) {
    if (chunk.kind == chunk_id("ALPH")) {
      alpha = chunk.data;
      has_alpha = true;
    } else if (chunk.kind == chunk_id("VP8 ")) {
      Vp8Frame frame = decode_vp8_frame(chunk.data);
      DecodedRaster image = vp8_to_rgba(frame);
      if (has_alpha) apply_alpha(image, alpha);
      return image;
    } else if (chunk.kind == chunk_id("VP8L")) {
      return decode_vp8l(chunk.data);
    }
  }
  fail("WebP has no image data");
}

}  // namespace

DecodedRaster decode_webp(std::span<const std::uint8_t> bytes) {
  Reader reader(bytes);
  reader.skip(4);
  const std::uint32_t riff_size = reader.le32();
  reader.skip(4);
  const std::size_t end = std::min<std::size_t>(bytes.size(), 8 + static_cast<std::size_t>(riff_size));
  const std::vector<Chunk> chunks = read_chunks(bytes.subspan(12, end > 12 ? end - 12 : 0));
  if (chunks.empty()) fail("WebP has no image data");
  if (chunks[0].kind != chunk_id("VP8X")) return decode_frame(chunks);

  Reader extended(chunks[0].data);
  extended.skip(4);
  const std::uint32_t canvas_width = extended.le24() + 1;
  const std::uint32_t canvas_height = extended.le24() + 1;
  for (const Chunk& chunk : chunks) {
    if (chunk.kind != chunk_id("ANMF")) continue;
    // An animation shows its first frame on a transparent canvas.
    Reader frame_header(chunk.data);
    const std::uint32_t left = frame_header.le24() * 2;
    const std::uint32_t top = frame_header.le24() * 2;
    frame_header.skip(10);
    const DecodedRaster frame = decode_frame(read_chunks(chunk.data.subspan(16)));
    DecodedRaster canvas = blank_raster(canvas_width, canvas_height);
    for (int y = 0; y < frame.height; ++y) {
      const std::uint64_t cy = top + static_cast<std::uint64_t>(y);
      if (cy >= canvas_height) break;
      for (int x = 0; x < frame.width; ++x) {
        const std::uint64_t cx = left + static_cast<std::uint64_t>(x);
        if (cx >= canvas_width) break;
        canvas.rgba[cy * canvas_width + cx] =
            frame.rgba[static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) + static_cast<std::size_t>(x)];
      }
    }
    return canvas;
  }
  return decode_frame(chunks);
}

}  // namespace vw::detail
