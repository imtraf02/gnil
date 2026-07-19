#include "shell/wallpaper/wallpaper_paths.h"

#include "config/config_types.h"
#include "util/file_utils.h"

const WallpaperMonitorOverride*
wallpaper::findWallpaperMonitorOverride(const WallpaperConfig& config, const WaylandOutput& output) {
  for (const auto& ovr : config.monitorOverrides) {
    if (outputMatchesSelector(ovr.match, output)) {
      return &ovr;
    }
  }
  return nullptr;
}

std::string wallpaper::resolveWallpaperDirectory(const WallpaperConfig& config, const WaylandOutput& output) {
  if (config.perMonitorDirectories) {
    if (const auto* ovr = findWallpaperMonitorOverride(config, output); ovr != nullptr) {
      if (ovr->directory.has_value() && !ovr->directory->empty()) {
        return *ovr->directory;
      }
    }
  }
  return resolveGlobalWallpaperDirectory(config);
}

std::string wallpaper::resolveGlobalWallpaperDirectory(const WallpaperConfig& config) {
  if (!config.directory.empty()) {
    return config.directory;
  }
  return FileUtils::expandUserPath(std::string(kDefaultWallpaperDirectory)).string();
}

std::string wallpaper::resolveGlobalLiveWallpaperDirectory(const WallpaperConfig& config) {
  if (!config.liveWallpaperDirectory.empty()) {
    return config.liveWallpaperDirectory;
  }
  return FileUtils::expandUserPath(std::string(kDefaultLiveWallpaperDirectory)).string();
}
