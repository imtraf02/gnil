#pragma once

#include <cstdint>
#include <optional>
#include <string>

struct NightLightConfig {
  // Day temperature must be higher than night temperature by at least this much.
  static constexpr std::int32_t kTemperatureMin = 1000;
  static constexpr std::int32_t kTemperatureMax = 10000;
  static constexpr std::int32_t kTemperatureGap = 100;

  bool enabled = false;
  bool force = false;
  std::int32_t dayTemperature = 6500;
  std::int32_t nightTemperature = 4000;

  bool operator==(const NightLightConfig&) const = default;
};

struct LocationConfig {
  // Local source of truth for weather, night light, and automatic theme mode.
  // GNIL never sends the user's IP or address to a geolocation service.
  // Default: Ho Chi Minh City, Vietnam (IANA timezone: Asia/Ho_Chi_Minh).
  std::string address = "Hồ Chí Minh, Việt Nam";
  bool customSchedule = false; // when true, use sunset/sunrise times instead of coordinates
  std::string sunset;          // HH:MM night start, used only when customSchedule is true
  std::string sunrise;         // HH:MM day start, used only when customSchedule is true
  std::optional<double> latitude = 10.8231;
  std::optional<double> longitude = 106.6297;

  bool operator==(const LocationConfig&) const = default;
};
