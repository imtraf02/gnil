#pragma once


#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gnil::theme {

  struct GeneratedPalette;
  struct AvailablePalette {
    struct PreviewMode {
      std::string surface;
      std::vector<std::string> accents;
    };
    struct Preview {
      PreviewMode dark;
      PreviewMode light;
    };

    std::string name;
    std::string md5;
    Preview preview;
  };

  [[nodiscard]] std::filesystem::path customPaletteDir();
  [[nodiscard]] std::filesystem::path customPalettePath(std::string_view name);
  [[nodiscard]] std::vector<AvailablePalette> availableCustomPalettes();

  [[nodiscard]] std::string suggestCustomPaletteName(std::string_view wallpaperPath, std::string_view scheme);
  [[nodiscard]] std::string allocateCustomPaletteName(std::string_view wallpaperPath, std::string_view scheme);
  [[nodiscard]] bool saveCustomPaletteFromGenerated(
      std::string_view name, const GeneratedPalette& palette, std::string* errorOut = nullptr
  );

} // namespace gnil::theme
