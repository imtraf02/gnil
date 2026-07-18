#pragma once

#include "launcher/launcher_provider.h"

class ConfigService;
class WaylandConnection;
class Wallpaper;

class LiveWallpaperProvider : public LauncherProvider {
public:
  LiveWallpaperProvider(ConfigService* config, WaylandConnection* wayland, Wallpaper* wallpaper);

  [[nodiscard]] std::string_view prefix() const override { return "/livewallpaper"; }
  [[nodiscard]] std::string_view id() const override { return "LiveWallpaper"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "smart_display"; }
  [[nodiscard]] bool trackUsage() const override { return true; }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;

  bool activate(const LauncherResult& result) override;

private:
  ConfigService* m_config = nullptr;
  WaylandConnection* m_wayland = nullptr;
  Wallpaper* m_wallpaper = nullptr;
};
