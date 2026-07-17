#include "system/location_service.h"

#include "config/config_service.h"

#include <cmath>
#include <utility>

LocationService::LocationService(ConfigService& configService) : m_configService(configService) {}

void LocationService::initialize() {
  m_config = m_configService.config().location;
  m_configService.addReloadCallback([this]() { onConfigReload(); }, "location");
}

void LocationService::addChangeCallback(ChangeCallback callback) {
  m_callbacks.push_back(std::move(callback));
}

bool LocationService::coordinatesValid() const noexcept {
  return m_config.latitude.has_value()
      && m_config.longitude.has_value()
      && std::isfinite(*m_config.latitude)
      && std::isfinite(*m_config.longitude)
      && *m_config.latitude >= -90.0
      && *m_config.latitude <= 90.0
      && *m_config.longitude >= -180.0
      && *m_config.longitude <= 180.0;
}

bool LocationService::hasResolvedLocation() const noexcept { return coordinatesValid(); }

std::optional<ResolvedLocation> LocationService::resolvedLocation() const noexcept {
  if (!coordinatesValid()) {
    return std::nullopt;
  }
  return ResolvedLocation{
      .latitude = *m_config.latitude,
      .longitude = *m_config.longitude,
      .name = m_config.address,
      .sourceLabel = "manual",
  };
}

void LocationService::onConfigReload() {
  const LocationConfig next = m_configService.config().location;
  if (next == m_config) {
    return;
  }
  m_config = next;
  notifyChanged();
}

void LocationService::notifyChanged() {
  for (const auto& callback : m_callbacks) {
    callback();
  }
}
