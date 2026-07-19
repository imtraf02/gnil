#pragma once

#include <string>
#include <string_view>

struct WallpaperConfig;
struct WallpaperMonitorOverride;
struct WaylandOutput;

namespace wallpaper {

  inline constexpr std::string_view kDefaultWallpaperDirectory = "~/Pictures/Wallpapers";
  inline constexpr std::string_view kDefaultLiveWallpaperDirectory = "~/Videos/LiveWallpapers";

  [[nodiscard]] const WallpaperMonitorOverride*
  findWallpaperMonitorOverride(const WallpaperConfig& config, const WaylandOutput& output);

  [[nodiscard]] std::string resolveWallpaperDirectory(const WallpaperConfig& config, const WaylandOutput& output);

  [[nodiscard]] std::string resolveGlobalWallpaperDirectory(const WallpaperConfig& config);

  [[nodiscard]] std::string resolveGlobalLiveWallpaperDirectory(const WallpaperConfig& config);

} // namespace wallpaper
