#include "theme/live_wallpaper_palette.h"

#include "theme/scheme.h"

#include <array>

namespace noctalia::theme {

  std::expected<LoadedImage, std::string>
  buildLiveWallpaperMosaic(const std::vector<std::string>& framePaths, Scheme scheme) {
    std::vector<LoadedImage> decoded;
    decoded.reserve(4);
    for (const auto& path : framePaths) {
      if (auto image = loadAndResize(path, scheme); image.has_value()) {
        decoded.push_back(std::move(*image));
      }
      if (decoded.size() == 4) {
        break;
      }
    }
    if (decoded.empty()) {
      return std::unexpected("no extracted live wallpaper frame could be decoded");
    }

    LoadedImage mosaic;
    mosaic.rgb.resize(112 * 112 * 3);
    for (std::size_t quadrant = 0; quadrant < 4; ++quadrant) {
      const auto& source = decoded[std::min(quadrant, decoded.size() - 1)];
      const int offsetX = quadrant % 2 == 0 ? 0 : 56;
      const int offsetY = quadrant < 2 ? 0 : 56;
      for (int y = 0; y < 56; ++y) {
        for (int x = 0; x < 56; ++x) {
          const std::size_t sourceIndex = static_cast<std::size_t>(((y * 2) * 112 + x * 2) * 3);
          const std::size_t targetIndex =
              static_cast<std::size_t>(((offsetY + y) * 112 + offsetX + x) * 3);
          mosaic.rgb[targetIndex] = source.rgb[sourceIndex];
          mosaic.rgb[targetIndex + 1] = source.rgb[sourceIndex + 1];
          mosaic.rgb[targetIndex + 2] = source.rgb[sourceIndex + 2];
        }
      }
    }
    return mosaic;
  }

} // namespace noctalia::theme
