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
  const auto overviewEnabled = (*runtime)["backdrop"]["enabled"].value<bool>();
  const auto overviewBlur = (*runtime)["backdrop"]["blur_intensity"].value<double>();
  const auto overviewTint = (*runtime)["backdrop"]["tint_intensity"].value<double>();
  const auto dashboardEnabled = (*runtime)["dashboard"]["enabled"].value<bool>();
  ok &= check(frame.has_value() && near(*frame, 6.0), "frame thickness uses Ling default");
  ok &= check(barThickness == 45, "bar thickness derives from frame, inner size and padding");
  ok &= check(autoHide == true && showOnHover == false, "bar collapses into frame and uses edge drag by default");
  ok &= check(liveWallpaperOutput == "auto", "live wallpaper palette source defaults to automatic output selection");
  ok &= check(overviewEnabled == true, "Niri overview backdrop defaults to enabled");
  ok &= check(dashboardEnabled == true, "top-centre dashboard defaults to enabled");
  ok &= check(
      overviewBlur.has_value() && overviewTint.has_value()
          && near(*overviewBlur, 0.5) && near(*overviewTint, 0.3),
      "Niri overview backdrop intensity defaults convert to runtime settings"
  );

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

    auto* backdrop = runtime->get_as<toml::table>("backdrop");
    ok &= check(backdrop != nullptr, "backdrop runtime table exists");
    if (backdrop != nullptr) {
      backdrop->insert_or_assign("enabled", false);
      backdrop->insert_or_assign("blur_intensity", 0.72);
      backdrop->insert_or_assign("tint_intensity", 0.18);
      gnil::settings_document::syncFromRuntimeOverrides(document, *runtime);
      const auto persistedEnabled = document["settings"]["wallpaper"]["overview_enabled"].value<bool>();
      const auto persistedBlur = document["settings"]["wallpaper"]["overview_blur_intensity"].value<double>();
      const auto persistedTint = document["settings"]["wallpaper"]["overview_tint_intensity"].value<double>();
      ok &= check(persistedEnabled == false, "overview backdrop toggle syncs back to the public document");
      ok &= check(
          persistedBlur.has_value() && persistedTint.has_value()
              && near(*persistedBlur, 0.72) && near(*persistedTint, 0.18),
          "overview backdrop intensities sync back to the public document"
      );
    }

    auto* dashboard = runtime->get_as<toml::table>("dashboard");
    auto* dashboardMedia = dashboard != nullptr ? dashboard->get_as<toml::table>("media") : nullptr;
    ok &= check(dashboardMedia != nullptr, "dashboard media runtime table exists");
    if (dashboardMedia != nullptr) {
      document["settings"]["dashboard"].as_table()->insert_or_assign("show_on_hover", true);
      dashboardMedia->insert_or_assign("lyrics_enabled", false);
      gnil::settings_document::syncFromRuntimeOverrides(document, *runtime);
      ok &= check(
          document["settings"]["dashboard"]["media"]["lyrics_enabled"].value<bool>() == false,
          "dashboard settings persist through the public Settings/Style document"
      );
      ok &= check(
          !document["settings"]["dashboard"]["show_on_hover"],
          "legacy dashboard hover setting is removed during synchronization"
      );
    }
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

  // Test capsule_group mapping
  {
    toml::table group;
    group.insert_or_assign("id", "g1");
    toml::array members;
    members.push_back("launcher");
    members.push_back("workspaces");
    group.insert_or_assign("members", std::move(members));
    toml::array groups;
    groups.push_back(std::move(group));
    auto* settingsBar = document.get_as<toml::table>("settings")->get_as<toml::table>("bar");
    settingsBar->insert_or_assign("capsule_group", std::move(groups));

    runtime = gnil::settings_document::toRuntimeOverrides(document);
    ok &= check(
        runtime.has_value()
            && (*runtime)["bar"]["default"]["capsule_group"].as_array() != nullptr
            && (*runtime)["bar"]["default"]["capsule_group"].as_array()->size() == 1,
        "capsule_group survives public-document conversion"
    );

    if (runtime.has_value()) {
      auto* runtimeGroups = (*runtime)["bar"]["default"]["capsule_group"].as_array();
      if (runtimeGroups != nullptr && runtimeGroups->size() == 1) {
        auto* firstGroup = (*runtimeGroups)[0].as_table();
        if (firstGroup != nullptr) {
          firstGroup->insert_or_assign("opacity", 0.5);
        }
      }
      gnil::settings_document::syncFromRuntimeOverrides(document, *runtime);
      const auto persistedOpacity = document["settings"]["bar"]["capsule_group"][0]["opacity"].value<double>();
      ok &= check(persistedOpacity.has_value() && near(*persistedOpacity, 0.5), "capsule_group changes sync back correctly");
    }
  }

  // Legacy light/dark wallpaper folders collapse into one shared source.
  {
    auto legacyDocument = gnil::settings_document::defaults();
    auto* wallpaper = legacyDocument.get_as<toml::table>("settings")->get_as<toml::table>("wallpaper");
    wallpaper->insert_or_assign("directory", "");
    wallpaper->insert_or_assign("directory_light", "/wallpapers/light");
    wallpaper->insert_or_assign("directory_dark", "/wallpapers/shared");
    wallpaper->insert_or_assign("live_wallpaper_directory", "");
    wallpaper->insert_or_assign("live_wallpaper_directory_light", "/live/light");
    wallpaper->insert_or_assign("live_wallpaper_directory_dark", "/live/shared");

    auto migrated = gnil::settings_document::toRuntimeOverrides(legacyDocument);
    ok &= check(
        migrated.has_value()
            && (*migrated)["wallpaper"]["directory"].value<std::string>() == "/wallpapers/shared"
            && (*migrated)["wallpaper"]["live_wallpaper_directory"].value<std::string>() == "/live/shared",
        "legacy light/dark wallpaper folders collapse into shared directories"
    );
    if (migrated.has_value()) {
      gnil::settings_document::syncFromRuntimeOverrides(legacyDocument, *migrated);
      ok &= check(
          legacyDocument["settings"]["wallpaper"]["directory"].value<std::string>() == "/wallpapers/shared"
              && !legacyDocument["settings"]["wallpaper"]["directory_light"]
              && !legacyDocument["settings"]["wallpaper"]["directory_dark"],
          "legacy wallpaper directory keys are removed after synchronization"
      );
      ok &= check(
          legacyDocument["settings"]["wallpaper"]["live_wallpaper_directory"].value<std::string>()
                  == "/live/shared"
              && !legacyDocument["settings"]["wallpaper"]["live_wallpaper_directory_light"]
              && !legacyDocument["settings"]["wallpaper"]["live_wallpaper_directory_dark"],
          "legacy live wallpaper directory keys are removed after synchronization"
      );
    }
  }

  document.insert_or_assign("schema_version", std::int64_t{99});
  std::string error;
  ok &= check(
      !gnil::settings_document::toRuntimeOverrides(document, &error).has_value() && !error.empty(),
      "unknown public schema versions are rejected"
  );

  return ok ? 0 : 1;
}
