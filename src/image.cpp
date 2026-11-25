#include "vw/image.hpp"
#include "image_decode.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace vw {
namespace {

// Decoded pixels: eight bits a channel, or sixteen when the file has more.
struct DecodedImage {
  int width = 0;
  int height = 0;
  std::filesystem::file_time_type modified;
  Pixels rgba;
  DeepPixels rgba16;
};

bool is_svg(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return extension == ".svg" || extension == ".svgz";
}

std::shared_ptr<const DecodedImage> decode_image(const std::filesystem::path& path,
                                                  float requested_width, float requested_height) {
  static std::mutex mutex;
  static std::unordered_map<std::string, std::shared_ptr<const DecodedImage>> cache;
  const bool svg = is_svg(path);
  const int target_width = requested_width > 0 ? static_cast<int>(std::ceil(requested_width)) : 0;
  const int target_height = requested_height > 0 ? static_cast<int>(std::ceil(requested_height)) : 0;
  const std::string key = path.string() + (svg ? ":" + std::to_string(target_width) +
                                                    "x" + std::to_string(target_height) : "");
  const auto modified = std::filesystem::last_write_time(path);
  std::lock_guard lock(mutex);
  if (const auto found = cache.find(key);
      found != cache.end() && found->second->modified == modified) return found->second;

  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("could not open image " + path.string());
  const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  detail::DecodedRaster raster;
  try {
    raster = svg ? detail::render_svg(bytes, target_width, target_height, path.parent_path())
                 : detail::decode_raster(bytes);
  } catch (const std::exception& error) {
    throw std::runtime_error(std::string(svg ? "could not render SVG " : "could not decode image ") + key + ": " +
                             error.what());
  }
  auto decoded = std::make_shared<DecodedImage>();
  decoded->modified = modified;
  decoded->width = raster.width;
  decoded->height = raster.height;
  if (raster.deep()) decoded->rgba16 = std::make_shared<const std::vector<std::uint64_t>>(std::move(raster.rgba16));
  else decoded->rgba = std::make_shared<const std::vector<std::uint32_t>>(std::move(raster.rgba));
  // Huge photos (wallpapers) are only ever drawn fitted, and the fitted
  // result is cached: keeping a 100 MB decode around would only cost memory.
  constexpr std::size_t retained_pixels = 16U * 1024U * 1024U;
  if (static_cast<std::size_t>(raster.width) * static_cast<std::size_t>(raster.height) <= retained_pixels)
    cache[key] = decoded;
  return decoded;
}

}  // namespace

namespace {

struct Crop {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

// The source region `crop`, area-averaged down to target_width x
// target_height when it is larger (never upscaled: the raster shader
// stretches). Averaging per channel is enough for photos and icons.
template <typename Pixel>
std::vector<Pixel> resample(const std::vector<Pixel>& source, int source_width, const Crop& crop,
                            int target_width, int target_height) {
  constexpr int bits = sizeof(Pixel) * 2;
  constexpr Pixel mask = (Pixel{1} << bits) - 1;
  std::vector<Pixel> pixels(static_cast<std::size_t>(target_width) * static_cast<std::size_t>(target_height));
  const double step_x = static_cast<double>(crop.width) / target_width;
  const double step_y = static_cast<double>(crop.height) / target_height;
  for (int y = 0; y < target_height; ++y) {
    const int y0 = crop.y + static_cast<int>(y * step_y);
    const int y1 = std::max(y0 + 1, std::min(crop.y + crop.height, crop.y + static_cast<int>((y + 1) * step_y)));
    for (int x = 0; x < target_width; ++x) {
      const int x0 = crop.x + static_cast<int>(x * step_x);
      const int x1 = std::max(x0 + 1, std::min(crop.x + crop.width, crop.x + static_cast<int>((x + 1) * step_x)));
      std::uint64_t sum[4] = {0, 0, 0, 0};
      for (int sy = y0; sy < y1; ++sy)
        for (int sx = x0; sx < x1; ++sx) {
          const Pixel pixel = source[static_cast<std::size_t>(sy) * static_cast<std::size_t>(source_width) + static_cast<std::size_t>(sx)];
          for (int channel = 0; channel < 4; ++channel) sum[channel] += (pixel >> (channel * bits)) & mask;
        }
      const auto count = static_cast<std::uint64_t>((x1 - x0) * (y1 - y0));
      Pixel result = 0;
      for (int channel = 0; channel < 4; ++channel)
        result |= static_cast<Pixel>((sum[channel] + count / 2) / count) << (channel * bits);
      pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(target_width) + static_cast<std::size_t>(x)] = result;
    }
  }
  return pixels;
}

// Fitted images by file, modification time, fit and size, so a fitted
// wallpaper is decoded, cropped and scaled once rather than on every build.
struct Fitted {
  Pixels pixels;
  DeepPixels deep;
  std::uint32_t pixel_width = 0;
  std::uint32_t pixel_height = 0;
  float width = 0;
  float height = 0;
};

std::mutex fitted_mutex;
std::vector<std::pair<std::string, Fitted>>& fitted_cache() {
  static std::vector<std::pair<std::string, Fitted>> cache;
  return cache;
}


// Crops and downscales `source` to width x height (either 0 keeps the
// natural size) the way `fit` asks.
Fitted fit_image(const DecodedImage& source_image, float width, float height, ImageFit fit) {
  const DecodedImage* source = &source_image;
  const bool sized = width > 0 && height > 0;
  std::uint32_t source_width = static_cast<std::uint32_t>(source->width);
  std::uint32_t source_height = static_cast<std::uint32_t>(source->height);
  Pixels pixels = source->rgba;
  DeepPixels deep = source->rgba16;
  const auto scale = [&](const Crop& crop, int target_width, int target_height) {
    if (deep) {
      deep = std::make_shared<const std::vector<std::uint64_t>>(
          resample(*source->rgba16, source->width, crop, target_width, target_height));
    } else {
      pixels = std::make_shared<const std::vector<std::uint32_t>>(
          resample(*source->rgba, source->width, crop, target_width, target_height));
    }
  };
  if (sized && source->width > 0 && source->height > 0) {
    const double box = static_cast<double>(width) / height;
    const double image = static_cast<double>(source->width) / source->height;
    Crop crop{0, 0, source->width, source->height};
    if (fit == ImageFit::contain) {
      if (image > box) height = static_cast<float>(width / image);
      else width = static_cast<float>(height * image);
    } else if (fit == ImageFit::cover && std::abs(image - box) > 1e-6) {
      if (image > box) {
        crop.width = std::max(1, static_cast<int>(std::lround(source->height * box)));
        crop.x = (source->width - crop.width) / 2;
      } else {
        crop.height = std::max(1, static_cast<int>(std::lround(source->width / box)));
        crop.y = (source->height - crop.height) / 2;
      }
    }
    const int target_width = std::max(1, static_cast<int>(std::ceil(width)));
    const int target_height = std::max(1, static_cast<int>(std::ceil(height)));
    if (crop.width > target_width || crop.height > target_height) {
      const int scaled_width = std::min(crop.width, target_width);
      const int scaled_height = std::min(crop.height, target_height);
      scale(crop, scaled_width, scaled_height);
      source_width = static_cast<std::uint32_t>(scaled_width);
      source_height = static_cast<std::uint32_t>(scaled_height);
    } else if (crop.width != source->width || crop.height != source->height) {
      scale(crop, crop.width, crop.height);
      source_width = static_cast<std::uint32_t>(crop.width);
      source_height = static_cast<std::uint32_t>(crop.height);
    }
  }
  const unsigned int output_width = static_cast<unsigned int>(
      width > 0 ? width : static_cast<float>(source->width));
  const unsigned int output_height = static_cast<unsigned int>(
      height > 0 ? height : static_cast<float>(source->height));
  if (output_width == 0 || output_height == 0 || output_width > 8192 || output_height > 8192) {
    throw std::runtime_error("image dimensions must be between 1 and 8192 pixels");
  }
  const Fitted fitted{deep ? Pixels{} : std::move(pixels), std::move(deep), source_width, source_height,
                      static_cast<float>(output_width), static_cast<float>(output_height)};
  return fitted;
}
}  // namespace

namespace {

void push_fitted(Scene& scene, const Fitted& fitted, const Rectangle& style) {
  Rectangle carrier = style;
  carrier.width = fitted.width;
  carrier.height = fitted.height;
  carrier.visible = false;
  const std::size_t carrier_index = scene.rectangles.size();
  scene.add(carrier);
  Raster raster;
  raster.carrier = carrier_index;
  raster.width = fitted.pixel_width;
  raster.height = fitted.pixel_height;
  raster.rgba = fitted.pixels;
  raster.rgba16 = fitted.deep;
  scene.rasters.push_back(std::move(raster));
}

// Fitted bitmaps by their pixels, fit and size: a capture redrawn on every
// build is downscaled once. Holding the pixels keeps the key unambiguous.
struct FittedBitmap {
  Pixels source;
  DeepPixels deep_source;
  float width = 0;
  float height = 0;
  ImageFit fit = ImageFit::stretch;
  Fitted fitted;
};

std::vector<FittedBitmap>& fitted_bitmaps() {
  static std::vector<FittedBitmap> cache;
  return cache;
}

}  // namespace

std::pair<float, float> add_image(Scene& scene, const Bitmap& bitmap,
                                  float width, float height, const Rectangle& style,
                                  ImageFit fit) {
  const std::size_t count = static_cast<std::size_t>(bitmap.width) * bitmap.height;
  if (bitmap.empty() || (bitmap.rgba16 ? bitmap.rgba16->size() : bitmap.rgba->size()) != count)
    throw std::invalid_argument("bitmap pixels do not match its size");
  const Pixels pixels = bitmap.rgba16 ? Pixels{} : bitmap.rgba;
  std::lock_guard lock(fitted_mutex);
  auto& cache = fitted_bitmaps();
  const auto found = std::ranges::find_if(cache, [&](const FittedBitmap& entry) {
    return entry.source == pixels && entry.deep_source == bitmap.rgba16 && entry.width == width &&
           entry.height == height && entry.fit == fit;
  });
  if (found == cache.end()) {
    const DecodedImage source{static_cast<int>(bitmap.width), static_cast<int>(bitmap.height), {}, pixels,
                              bitmap.rgba16};
    if (cache.size() >= 16) cache.erase(cache.begin());
    cache.push_back({pixels, bitmap.rgba16, width, height, fit, fit_image(source, width, height, fit)});
  }
  const Fitted& fitted = found == cache.end() ? cache.back().fitted : found->fitted;
  push_fitted(scene, fitted, style);
  return {fitted.width, fitted.height};
}

std::pair<float, float> add_image(Scene& scene, const std::filesystem::path& path,
                                  float width, float height, const Rectangle& style,
                                  ImageFit fit) {
  const auto push = [&](const Fitted& fitted) {
    push_fitted(scene, fitted, style);
    return std::pair{fitted.width, fitted.height};
  };
  const bool sized = width > 0 && height > 0;
  std::string key;
  if (sized) {
    key = path.string() + '\x1f' + std::to_string(static_cast<int>(fit)) + '\x1f' +
          std::to_string(width) + 'x' + std::to_string(height) + '\x1f' +
          std::to_string(std::filesystem::last_write_time(path).time_since_epoch().count());
    std::lock_guard lock(fitted_mutex);
    for (const auto& [entry_key, fitted] : fitted_cache())
      if (entry_key == key) return push(fitted);
  }

  const Fitted fitted = fit_image(*decode_image(path, width, height), width, height, fit);
  if (sized) {
    std::lock_guard lock(fitted_mutex);
    auto& cache = fitted_cache();
    if (cache.size() >= 64) cache.erase(cache.begin());
    cache.emplace_back(key, fitted);
  }
  return push(fitted);
}

}  // namespace vw
