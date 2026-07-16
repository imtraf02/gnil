#include "config/settings_document.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <utility>

namespace {

bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
  }
  return condition;
}

bool near(double lhs, double rhs) { return std::abs(lhs - rhs) < 0.001; }

} // namespace

int main() {
  bool ok = true;
  toml::table document = gnil::settings_document::defaults();
  auto runtime = gnil::settings_document::toRuntimeOverrides(document);
  ok &= check(runtime.has_value(), "default Settings/Style document converts");
  if (!runtime.has_value()) {
    return 1;
  }

  const auto frame = (*runtime)["shell"]["chrome"]["frame_thickness"].value<double>();
  const auto barThickness = (*runtime)["bar"]["default"]["thickness"].value<std::int64_t>();
  const auto autoHide = (*runtime)["bar"]["default"]["auto_hide"].value<bool>();
  const auto showOnHover = (*runtime)["bar"]["default"]["show_on_hover"].value<bool>();
  const auto liveWallpaperOutput = (*runtime)["theme"]["live_wallpaper_output"].value<std::string>();
  ok &= check(frame.has_value() && near(*frame, 6.0), "frame thickness uses Ling default");
  ok &= check(barThickness == 45, "bar thickness derives from frame, inner size and padding");
  ok &= check(autoHide == true && showOnHover == false, "bar collapses into frame and uses edge drag by default");
  ok &= check(liveWallpaperOutput == "auto", "live wallpaper palette source defaults to automatic output selection");

  auto* settings = document.get_as<toml::table>("settings");
  auto* appearance = settings != nullptr ? settings->get_as<toml::table>("appearance") : nullptr;
  ok &= check(appearance != nullptr, "appearance table exists");
  if (appearance != nullptr) {
    appearance->insert_or_assign("thickness", 12.0);
  }
  runtime = gnil::settings_document::toRuntimeOverrides(document);
  ok &= check(runtime.has_value(), "edited Settings/Style document converts");
  if (runtime.has_value()) {
    const auto updatedBar = (*runtime)["bar"]["default"]["thickness"].value<std::int64_t>();
    ok &= check(updatedBar == 57, "bar total tracks a thicker left frame without a stale offset");

    auto* bar = runtime->get_as<toml::table>("bar");
    auto* defaultBar = bar != nullptr ? bar->get_as<toml::table>("default") : nullptr;
    if (defaultBar != nullptr) {
      defaultBar->insert_or_assign("show_on_hover", true);
    }
    gnil::settings_document::syncFromRuntimeOverrides(document, *runtime);
    const auto persistedHover = document["settings"]["bar"]["show_on_hover"].value<bool>();
    ok &= check(persistedHover == true, "runtime UI mutation writes back to the public document");
  }

  toml::table networkSize;
  networkSize.insert_or_assign("width", std::int64_t{520});
  toml::table panelSizes;
  panelSizes.insert_or_assign("network", std::move(networkSize));
  settings = document.get_as<toml::table>("settings");
  ok &= check(settings != nullptr, "settings table survives runtime synchronization");
  if (settings == nullptr) {
    return 1;
  }
  settings->insert_or_assign("panels", std::move(panelSizes));
  runtime = gnil::settings_document::toRuntimeOverrides(document);
  ok &= check(
      runtime.has_value()
          && (*runtime)["shell"]["panel"]["size"]["network"]["width"].value<std::int64_t>() == 520,
      "per-panel width survives public-document conversion"
  );
  if (runtime.has_value()) {
    runtime->get_as<toml::table>("shell")
        ->get_as<toml::table>("panel")
        ->get_as<toml::table>("size")
        ->get_as<toml::table>("network")
        ->insert_or_assign("width", "auto");
    gnil::settings_document::syncFromRuntimeOverrides(document, *runtime);
    ok &= check(
        !document["settings"]["panels"]["network"]["width"],
        "Auto clears the panel width override"
    );
  }

  document.insert_or_assign("schema_version", std::int64_t{99});
  std::string error;
  ok &= check(
      !gnil::settings_document::toRuntimeOverrides(document, &error).has_value() && !error.empty(),
      "unknown public schema versions are rejected"
  );

  return ok ? 0 : 1;
}
