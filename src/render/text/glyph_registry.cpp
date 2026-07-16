#include "render/text/glyph_registry.h"

#include "core/files/resource_paths.h"
#include "core/log.h"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_set>

namespace {

  constexpr Logger kLog("glyph");
  constexpr char32_t kMissingGlyph = 0xEB8B; // Material Symbols question_mark

  // Semantic compatibility is intentionally kept for existing GNIL configs.
  // Every target below is a native Material Symbols name.
  // clang-format off
  const std::unordered_map<std::string, std::string_view> kAliases = {
      {"x", "close"}, {"close", "close"}, {"plus", "add"}, {"add", "add"},
      {"dots-vertical", "more_vert"}, {"more-vertical", "more_vert"},
      {"user", "person"}, {"person", "person"}, {"info", "info"},
      {"mood-smile-beam", "sentiment_very_satisfied"}, {"sort-a-z", "sort_by_alpha"},
      {"file-description", "description"}, {"unpin", "keep_off"}, {"pinned-off", "keep_off"},
      {"image", "image"}, {"photo", "image"}, {"keyboard", "keyboard"},
      {"plugin", "extension"}, {"official-plugin", "verified_user"},
      {"toast-notice", "check_circle"}, {"toast-warning", "warning"},
      {"toast-error", "error"}, {"warning", "warning"},
      {"circle-check", "check_circle"}, {"alert-circle", "warning"}, {"circle-x", "cancel"},
      {"exclamation-circle", "error"},
      {"media-pause", "pause"}, {"player-pause-filled", "pause"},
      {"media-play", "play_arrow"}, {"player-play", "play_arrow"}, {"player-play-filled", "play_arrow"},
      {"media-prev", "skip_previous"}, {"player-skip-back-filled", "skip_previous"},
      {"media-next", "skip_next"}, {"player-skip-forward-filled", "skip_next"},
      {"shuffle", "shuffle"}, {"arrows-shuffle", "shuffle"},
      {"stop", "stop"}, {"player-stop-filled", "stop"},
      {"microphone-mute", "mic_off"}, {"microphone-off", "mic_off"},
      {"volume-high", "volume_up"}, {"volume", "volume_up"},
      {"volume-low", "volume_down"}, {"volume-2", "volume_down"},
      {"volume-mute", "volume_mute"}, {"volume-off", "volume_off"},
      {"volume-x", "no_sound"}, {"volume-zero", "no_sound"}, {"volume-3", "no_sound"},
      {"download-speed", "download"}, {"upload-speed", "upload"},
      {"wifi", "network_wifi"},
      {"wifi-3", "network_wifi_3_bar"}, {"wifi-2", "network_wifi_2_bar"}, {"wifi-1", "network_wifi_1_bar"},
      {"wifi-0", "signal_wifi_0_bar"}, {"wifi-question", "wifi_find"},
      {"wifi-exclamation", "signal_wifi_bad"},
      {"cpu-intensive", "warning"}, {"cpu-usage", "speed"}, {"brand-speedtest", "speed"},
      {"cpu-temperature", "device_thermostat"}, {"flame", "local_fire_department"},
      {"gpu-usage", "developer_board"}, {"device-desktop", "desktop_windows"},
      {"memory", "memory"}, {"cpu", "memory"}, {"storage", "database"},
      {"database", "database"}, {"busy", "hourglass_empty"}, {"hourglass-empty", "hourglass_empty"},
      {"performance", "speed"}, {"gauge", "speed"}, {"balanced", "balance"},
      {"activity-heartbeat", "cardiology"}, {"activity", "monitoring"},
      {"temperature", "device_thermostat"}, {"temperature-sun", "thermostat"},
      {"layers-intersect", "layers"}, {"arrows-exchange", "sync_alt"},
      {"tool", "build"}, {"bug", "bug_report"},
      {"scale", "balance"}, {"powersaver", "energy_savings_leaf"}, {"leaf", "energy_savings_leaf"},
      {"shutdown", "power_settings_new"}, {"power", "power_settings_new"},
      {"reboot", "restart_alt"}, {"refresh", "refresh"},
      {"suspend", "sleep"}, {"player-pause", "pause"}, {"hibernate", "bedtime"}, {"zzz", "bedtime"},
      {"nightlight-on", "nightlight"}, {"nightlight-off", "nightlight_round"},
      {"nightlight-forced", "dark_mode"}, {"moon", "dark_mode"}, {"moon-off", "light_mode"},
      {"moon-stars", "dark_mode"}, {"theme-mode", "contrast"}, {"contrast-filled", "contrast"},
      {"caffeine-on", "coffee"}, {"mug-filled", "coffee"},
      {"caffeine-off", "coffee_maker"}, {"mug", "coffee_maker"},
      {"brightness-low", "brightness_low"}, {"brightness-down-filled", "brightness_low"},
      {"brightness-high", "brightness_high"}, {"brightness-up-filled", "brightness_high"},
      {"wallpaper-selector", "wallpaper"}, {"library-photo", "photo_library"},
      {"battery-0", "battery_0_bar"}, {"battery", "battery_0_bar"},
      {"battery-plugged", "battery_charging_full"}, {"battery-charging-2", "battery_charging_full"},
      {"battery-exclamation", "battery_alert"},
      {"bluetooth-device-generic", "bluetooth"}, {"bluetooth-device-gamepad", "sports_esports"},
      {"device-gamepad-2", "sports_esports"}, {"bluetooth-device-microphone", "mic"},
      {"microphone", "mic"}, {"bluetooth-device-headset", "headset"}, {"headset", "headset"},
      {"bluetooth-device-earbuds", "earbuds"}, {"device-airpods", "earbuds"},
      {"bluetooth-device-headphones", "headphones"}, {"headphones", "headphones"},
      {"bluetooth-device-mouse", "mouse"}, {"mouse-2", "mouse"},
      {"bluetooth-device-keyboard", "keyboard"}, {"bluetooth-device-phone", "smartphone"},
      {"device-mobile", "smartphone"}, {"bluetooth-device-watch", "watch"}, {"device-watch", "watch"},
      {"bluetooth-device-speaker", "speaker"}, {"device-speaker", "speaker"},
      {"bluetooth-device-tv", "tv"}, {"device-tv", "tv"},
      {"weather-sun", "sunny"}, {"sun", "sunny"}, {"weather-moon", "dark_mode"},
      {"weather-moon-stars", "dark_mode"}, {"weather-cloud", "cloud"},
      {"weather-cloud-off", "cloud_off"}, {"cloud-off", "cloud_off"},
      {"weather-cloud-haze", "foggy"}, {"cloud-fog", "foggy"},
      {"weather-cloud-lightning", "thunderstorm"}, {"cloud-bolt", "thunderstorm"},
      {"wind", "air"}, {"mountain", "landscape"},
      {"weather-cloud-rain", "rainy"}, {"cloud-rain", "rainy"},
      {"weather-cloud-snow", "weather_snowy"}, {"cloud-snow", "weather_snowy"},
      {"weather-cloud-sun", "partly_cloudy_day"}, {"cloud-sun", "partly_cloudy_day"},
      {"weather-sunrise", "wb_twilight"}, {"sunrise", "wb_twilight"},
      {"weather-sunset", "wb_twilight"}, {"sunset", "wb_twilight"},
      {"chevron-left", "chevron_left"}, {"chevron-right", "chevron_right"},
      {"chevron-up", "keyboard_arrow_up"}, {"chevron-down", "keyboard_arrow_down"},
      {"arrow-left", "arrow_back"}, {"arrow-right", "arrow_forward"},
      {"arrow-up", "arrow_upward"}, {"arrow-down", "arrow_downward"},
      {"arrow-big-up", "arrow_upward"}, {"external-link", "open_in_new"},
      {"alert-triangle", "warning"}, {"app-window", "web_asset"},
      {"arrows-horizontal", "swap_horiz"}, {"battery-4", "battery_4_bar"},
      {"bell", "notifications"}, {"bell-off", "notifications_off"},
      {"border-corner-pill", "rounded_corner"}, {"brand-apple", "devices"},
      {"brand-git", "code"}, {"brand-google", "language"},
      {"calendar-cog", "calendar_month"}, {"calendar-event", "event"}, {"calendar-off", "event_busy"},
      {"camera-off", "no_photography"}, {"checkbox", "check_box"},
      {"circuit-pushbutton", "radio_button_checked"}, {"clipboard", "content_paste"},
      {"clock", "schedule"}, {"color-picker", "colorize"}, {"copy-plus", "library_add"},
      {"device-floppy", "save"}, {"disc-filled", "album"},
      {"eye", "visibility"}, {"eye-off", "visibility_off"}, {"flask", "science"},
      {"flip-horizontal", "flip"}, {"flip-vertical", "flip"}, {"glyph", "font_download"},
      {"grid-dots", "apps"}, {"layout-grid", "grid_view"}, {"map-pin-off", "location_off"},
      {"menu-2", "menu"}, {"noctalia", "auto_awesome"}, {"photo-off", "hide_image"},
      {"pin", "keep"}, {"plug-off", "power_off"}, {"puzzle", "extension"},
      {"screen-share-off", "stop_screen_share"}, {"shield-lock", "admin_panel_settings"},
      {"stack-2", "layers"}, {"stack-back", "layers"}, {"stack-front", "layers"},
      {"stack-pop", "layers_clear"}, {"star-filled", "star"}, {"trash", "delete"},
      {"video", "videocam"}, {"wave-sine", "graphic_eq"}, {"world", "public"},
      {"circle-dot", "radio_button_checked"}, {"circle-filled", "circle"}, {"pentagon-filled", "circle"},
      {"michelin-star-filled", "circle"}, {"square-rounded-filled", "circle"},
      {"guitar-pick-filled", "circle"}, {"blob-filled", "circle"}, {"triangle-filled", "circle"},
  };
  // clang-format on

  [[nodiscard]] std::optional<char32_t> parseCodepointLiteral(std::string_view value) {
    if (value.size() < 3) {
      return std::nullopt;
    }
    std::string_view hex;
    if ((value[0] == 'U' || value[0] == 'u') && value[1] == '+') {
      hex = value.substr(2);
    } else if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
      hex = value.substr(2);
    } else {
      return std::nullopt;
    }
    std::uint32_t codepoint = 0;
    const auto result = std::from_chars(hex.data(), hex.data() + hex.size(), codepoint, 16);
    if (result.ec != std::errc{} || result.ptr != hex.data() + hex.size() || codepoint == 0 || codepoint > 0x10FFFF) {
      return std::nullopt;
    }
    return static_cast<char32_t>(codepoint);
  }

  [[nodiscard]] std::unordered_map<std::string, char32_t> loadMaterialSymbols() {
    std::unordered_map<std::string, char32_t> icons;
    const std::filesystem::path path = paths::assetPath("fonts/material-symbols-rounded.codepoints");
    std::ifstream file(path);
    if (!file.is_open()) {
      kLog.warn("failed to open Material Symbols codepoints: {}", path.string());
      return icons;
    }
    std::string name;
    std::string hex;
    while (file >> name >> hex) {
      std::uint32_t codepoint = 0;
      const auto result = std::from_chars(hex.data(), hex.data() + hex.size(), codepoint, 16);
      if (result.ec == std::errc{} && result.ptr == hex.data() + hex.size() && codepoint > 0 && codepoint <= 0x10FFFF) {
        icons.emplace(std::move(name), static_cast<char32_t>(codepoint));
      }
    }
    kLog.debug("loaded {} Material Symbol names from {}", icons.size(), path.string());
    return icons;
  }

  [[nodiscard]] std::string normalizedMaterialName(std::string_view name) {
    std::string normalized{name};
    for (char& c : normalized) {
      if (c == '-') {
        c = '_';
      }
    }
    return normalized;
  }

} // namespace

const std::unordered_map<std::string, char32_t>& GlyphRegistry::materialSymbols() {
  static const std::unordered_map<std::string, char32_t> icons = loadMaterialSymbols();
  return icons;
}

bool GlyphRegistry::contains(std::string_view name) {
  if (parseCodepointLiteral(name).has_value()) {
    return true;
  }
  const auto& symbols = materialSymbols();
  const std::string key{name};
  if (const auto alias = kAliases.find(key); alias != kAliases.end()) {
    return symbols.contains(std::string(alias->second));
  }
  return symbols.contains(normalizedMaterialName(name));
}

char32_t GlyphRegistry::lookup(std::string_view name) {
  if (auto codepoint = parseCodepointLiteral(name)) {
    return *codepoint;
  }
  const auto& symbols = materialSymbols();
  const std::string key{name};
  const std::string resolved = [&]() {
    if (const auto alias = kAliases.find(key); alias != kAliases.end()) {
      return std::string(alias->second);
    }
    return normalizedMaterialName(name);
  }();
  if (const auto it = symbols.find(resolved); it != symbols.end()) {
    return it->second;
  }
  static std::unordered_set<std::string> warned;
  if (warned.insert(key).second) {
    kLog.warn("unknown Material Symbol '{}'; using question_mark", name);
  }
  return kMissingGlyph;
}

const std::unordered_map<std::string, std::string_view>& GlyphRegistry::aliases() { return kAliases; }
