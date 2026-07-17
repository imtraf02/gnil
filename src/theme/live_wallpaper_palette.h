#pragma once

#include "theme/image_loader.h"

#include <expected>
#include <string>
#include <vector>

namespace gnil::theme {

  enum class Scheme;

  struct LiveWallpaperPaletteSource {
    // Includes canonical path and file metadata, so replacing a video at the
    // same path invalidates both extraction and generated-palette caches.
    std::string identity;
    std::vector<std::string> framePaths;
  };

  // Builds one deterministic 112x112 sample from up to four representative
  // video frames. Missing quadrants repeat the last successfully decoded
  // frame, keeping extraction useful for short or unusual media files.
  [[nodiscard]] std::expected<LoadedImage, std::string>
  buildLiveWallpaperMosaic(const std::vector<std::string>& framePaths, Scheme scheme);

} // namespace gnil::theme
