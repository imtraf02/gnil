#pragma once

#include "config/config_types.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

class ConfigService;

struct ResolvedLocation {
  double latitude = 0.0;
  double longitude = 0.0;
  std::string name;
  std::string sourceLabel;
};

// Publishes manually configured coordinates shared by weather, night light,
// and automatic theme mode. GNIL performs no IP geolocation or remote geocoding.
class LocationService {
public:
  using ChangeCallback = std::function<void()>;

  explicit LocationService(ConfigService& configService);

  void initialize();
  void addChangeCallback(ChangeCallback callback);

  [[nodiscard]] int pollTimeoutMs() const noexcept { return -1; }
  void tick() noexcept {}

  [[nodiscard]] bool networkResolutionConfigured() const noexcept { return false; }
  [[nodiscard]] bool resolving() const noexcept { return false; }
  [[nodiscard]] bool hasResolvedLocation() const noexcept;
  [[nodiscard]] std::optional<ResolvedLocation> resolvedLocation() const noexcept;
  [[nodiscard]] const std::string& error() const noexcept { return m_error; }

private:
  void onConfigReload();
  void notifyChanged();
  [[nodiscard]] bool coordinatesValid() const noexcept;

  ConfigService& m_configService;
  LocationConfig m_config;
  std::vector<ChangeCallback> m_callbacks;
  std::string m_error;
};
