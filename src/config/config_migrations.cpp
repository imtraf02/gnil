#include "config/config_migrations.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <limits>
#include <set>
#include <string>
#include <utility>

namespace gnil::config {
  namespace {

    constexpr int kNegativeBarRadiusMigrationVersion = 1;
    constexpr int kCustomScheduleMigrationVersion = 2;
    constexpr int kUnifiedChromeMigrationVersion = 3;
    constexpr int kNexusLauncherMigrationVersion = 4;
    constexpr int kLockscreenIslandMigrationVersion = 5;
    constexpr int kThemeTemplatesRemovalMigrationVersion = 6;
    constexpr int kStandalonePanelsMigrationVersion = 7;
    constexpr int kDynamicPanelHeightMigrationVersion = 8;
    constexpr int kShadowlessSurfacesMigrationVersion = 9;
    constexpr std::int64_t kMaxBarRadius = 500;
    constexpr std::array<std::string_view, 5> kBarRadiusKeys = {
        "radius", "radius_top_left", "radius_top_right", "radius_bottom_left", "radius_bottom_right",
    };
    constexpr std::array<std::string_view, 16> kLegacyBarChromeKeys = {
        "reserve_space",
        "background_opacity",
        "border",
        "border_width",
        "radius",
        "radius_top_left",
        "radius_top_right",
        "radius_bottom_left",
        "radius_bottom_right",
        "concave_edge_corners",
        "margin_ends",
        "margin_edge",
        "margin_opposite_edge",
        "shadow",
        "contact_shadow",
        "panel_overlap",
    };
    constexpr std::array<std::string_view, 16> kLegacyPanelChromeKeys = {
        "transparency_mode",       "shadow",
        "floating_offset",         "launcher_placement",
        "launcher_position",       "clipboard_placement",
        "clipboard_position",      "wallpaper_placement",
        "wallpaper_position",      "polkit_placement",
        "polkit_position",         "control_center_placement",
        "control_center_position", "session_placement",
        "session_position",        "open_near_click_launcher",
    };
    constexpr std::array<std::string_view, 5> kLegacyPanelOpenNearClickKeys = {
        "open_near_click_clipboard", "open_near_click_wallpaper", "open_near_click_control_center",
        "open_near_click_session",   "open_near_click_polkit",
    };
    constexpr std::array<std::string_view, 6> kLegacyNotificationChromeKeys = {
        "position", "layer", "scale", "background_opacity", "offset_x", "offset_y",
    };

    template <std::size_t N, typename OnRemoved>
    void removeLegacyKeys(
        toml::table& table, const std::array<std::string_view, N>& keys, std::string_view path, OnRemoved&& onRemoved
    ) {
      for (const std::string_view key : keys) {
        if (!table.contains(key)) {
          continue;
        }
        table.erase(key);
        onRemoved(std::format("{}.{}", path, key));
      }
    }

    template <typename OnRemoved> void migrateUnifiedChromeConfig(toml::table& root, OnRemoved&& onRemoved) {
      if (auto* shell = root["shell"].as_table(); shell != nullptr) {
        if (auto* panel = (*shell)["panel"].as_table(); panel != nullptr) {
          removeLegacyKeys(*panel, kLegacyPanelChromeKeys, "shell.panel", onRemoved);
          removeLegacyKeys(*panel, kLegacyPanelOpenNearClickKeys, "shell.panel", onRemoved);
        }
      }

      if (auto* notification = root["notification"].as_table(); notification != nullptr) {
        removeLegacyKeys(*notification, kLegacyNotificationChromeKeys, "notification", onRemoved);
      }

      auto* bars = root["bar"].as_table();
      if (bars == nullptr) {
        return;
      }
      for (auto& [barName, barNode] : *bars) {
        auto* bar = barNode.as_table();
        if (bar == nullptr) {
          continue;
        }
        const std::string barPath = "bar." + std::string(barName.str());
        removeLegacyKeys(*bar, kLegacyBarChromeKeys, barPath, onRemoved);
        if (auto* monitors = (*bar)["monitor"].as_table(); monitors != nullptr) {
          for (auto& [monitorName, monitorNode] : *monitors) {
            if (auto* monitor = monitorNode.as_table(); monitor != nullptr) {
              removeLegacyKeys(
                  *monitor, kLegacyBarChromeKeys, barPath + ".monitor." + std::string(monitorName.str()), onRemoved
              );
            }
          }
        }
      }
    }

    void migrateUnifiedChromeConfigSidecar(toml::table& root, schema::Diagnostics& diag) {
      migrateUnifiedChromeConfig(root, [&diag](const std::string& path) {
        diag.warn(path, "removed legacy visual option; [shell.chrome] now owns panel and frame geometry");
      });
    }

    void migrateLegacyVibrantScheme(toml::table& root, schema::Diagnostics& diag) {
      bool changed = false;
      const auto migrateTable = [&changed](toml::table* table) {
        if (table == nullptr) {
          return;
        }
        const auto value = (*table)["wallpaper_scheme"].value<std::string_view>();
        if (value == "vibrant") {
          table->insert_or_assign("wallpaper_scheme", "custom-vibrant");
          changed = true;
        }
      };

      migrateTable(root["theme"].as_table());
      if (auto* wallpaper = root["wallpaper"].as_table(); wallpaper != nullptr) {
        if (auto* favorites = (*wallpaper)["favorite"].as_table(); favorites != nullptr) {
          for (auto& [name, node] : *favorites) {
            (void)name;
            migrateTable(node.as_table());
          }
        }
      }
      if (changed) {
        diag.warn(
            "theme.wallpaper_scheme",
            "renamed legacy vibrant generator to custom-vibrant; vibrant is now the Material 3 variant"
        );
      }
    }

    void removeLegacyDockConfig(toml::table& root, schema::Diagnostics& diag) {
      if (!root.contains("dock")) {
        return;
      }
      root.erase("dock");
      diag.warn("dock", "removed: GNIL now uses the rail and workspace app icons instead of a dock");
    }

    void migrateNexusLauncherConfig(toml::table& root, schema::Diagnostics& diag) {
      migrateLegacyVibrantScheme(root, diag);
      removeLegacyDockConfig(root, diag);
    }

    void removeLockscreenWidgetsConfig(toml::table& root, schema::Diagnostics& diag) {
      if (!root.contains("lockscreen_widgets")) {
        return;
      }
      root.erase("lockscreen_widgets");
      diag.warn("lockscreen_widgets", "removed: lock screen widgets were replaced by the lock screen island");
    }

    void removeThemeTemplatesConfig(toml::table& root, schema::Diagnostics& diag) {
      auto* theme = root["theme"].as_table();
      if (theme == nullptr || !theme->contains("templates")) {
        return;
      }
      theme->erase("templates");
      diag.warn("theme.templates", "removed: theme templates are no longer supported");
    }

    template <typename OnChanged> void removePanelHeightOverrides(toml::table& root, OnChanged&& onChanged) {
      auto* shell = root["shell"].as_table();
      auto* panel = shell != nullptr ? (*shell)["panel"].as_table() : nullptr;
      auto* sizes = panel != nullptr ? (*panel)["size"].as_table() : nullptr;
      if (sizes == nullptr) {
        return;
      }
      for (auto& [name, node] : *sizes) {
        auto* size = node.as_table();
        if (size == nullptr || !size->contains("height")) {
          continue;
        }
        size->erase("height");
        onChanged("shell.panel.size." + std::string(name.str()) + ".height");
      }
    }

    void removePanelHeightOverridesSidecar(toml::table& root, schema::Diagnostics& diag) {
      removePanelHeightOverrides(root, [&diag](const std::string& path) {
        diag.warn(path, "removed: panel height now follows content and is capped at 720 logical pixels");
      });
    }

    template <typename OnChanged> void removeSurfaceShadowConfig(toml::table& root, OnChanged&& onChanged) {
      auto* shell = root["shell"].as_table();
      if (shell == nullptr || !shell->contains("shadow")) {
        return;
      }
      shell->erase("shadow");
      onChanged("shell.shadow");
    }

    void removeSurfaceShadowConfigSidecar(toml::table& root, schema::Diagnostics& diag) {
      removeSurfaceShadowConfig(root, [&diag](const std::string& path) {
        diag.warn(path, "removed: GNIL surfaces no longer render outer shadows");
      });
    }

    toml::table& ensureTable(toml::table& parent, std::string_view key) {
      if (auto* existing = parent[key].as_table(); existing != nullptr) {
        return *existing;
      }
      parent.insert_or_assign(key, toml::table{});
      return *parent[key].as_table();
    }

    template <typename OnChanged> void migrateStandalonePanels(toml::table& root, OnChanged&& onChanged) {
      if (auto* controlCenter = root["control_center"].as_table(); controlCenter != nullptr) {
        if (const auto width = (*controlCenter)["width"].value<std::int64_t>(); width.has_value()) {
          auto& shell = ensureTable(root, "shell");
          auto& panel = ensureTable(shell, "panel");
          auto& sizes = ensureTable(panel, "size");
          constexpr std::array<std::string_view, 11> panelIds = {
              "media", "audio", "brightness", "night-light", "system", "battery",
              "network", "bluetooth", "weather", "calendar", "screen-time",
          };
          for (const std::string_view id : panelIds) {
            auto& entry = ensureTable(sizes, id);
            if (!entry.contains("width")) {
              entry.insert_or_assign("width", *width);
            }
          }
          onChanged("control_center.width", "migrated to per-panel width overrides under shell.panel.size");
        }
        if (auto* calendar = (*controlCenter)["calendar"].as_table(); calendar != nullptr) {
          if (const auto showEvents = (*calendar)["show_events_card"].value<bool>(); showEvents.has_value()) {
            auto& target = ensureTable(root, "calendar");
            if (!target.contains("show_events_card")) {
              target.insert_or_assign("show_events_card", *showEvents);
            }
          }
        }
        root.erase("control_center");
        onChanged("control_center", "removed dashboard tabs and shortcuts; bar widgets now open standalone panels");
      }

      if (auto* bars = root["bar"].as_table(); bars != nullptr) {
        const auto removeControlCenterWidget = [&onChanged](toml::table& bar, const std::string& path) {
          for (const std::string_view lane : {"start", "center", "end"}) {
            auto* entries = bar[lane].as_array();
            if (entries == nullptr) continue;
            const auto oldSize = entries->size();
            for (auto it = entries->begin(); it != entries->end();) {
              if (it->value<std::string_view>() == "control-center") {
                it = entries->erase(it);
              } else {
                ++it;
              }
            }
            if (entries->size() != oldSize) {
              onChanged(path + "." + std::string(lane), "removed obsolete control-center widget");
            }
          }
        };
        for (auto& [name, node] : *bars) {
          auto* bar = node.as_table();
          if (bar == nullptr) continue;
          const std::string barPath = "bar." + std::string(name.str());
          removeControlCenterWidget(*bar, barPath);
          if (auto* monitors = (*bar)["monitor"].as_table(); monitors != nullptr) {
            for (auto& [match, monitorNode] : *monitors) {
              if (auto* monitor = monitorNode.as_table(); monitor != nullptr) {
                removeControlCenterWidget(*monitor, barPath + ".monitor." + std::string(match.str()));
              }
            }
          }
        }
      }

      if (auto* hotCorners = root["hot_corners"].as_table(); hotCorners != nullptr) {
        for (const std::string_view corner : {"top_left", "top_right", "bottom_left", "bottom_right"}) {
          auto* entry = (*hotCorners)[corner].as_table();
          if (entry != nullptr && (*entry)["action"].value<std::string_view>() == "control_center") {
            entry->insert_or_assign("action", "none");
            onChanged("hot_corners." + std::string(corner) + ".action", "disabled removed control_center action");
          }
        }
      }
    }

    void migrateStandalonePanelsSidecar(toml::table& root, schema::Diagnostics& diag) {
      migrateStandalonePanels(root, [&diag](const std::string& path, const std::string& message) {
        diag.warn(path, message);
      });
    }

    bool migrateNegativeRadii(toml::table& table) {
      bool changed = false;
      for (const std::string_view key : kBarRadiusKeys) {
        const auto radius = table[key].value<std::int64_t>();
        if (!radius.has_value() || *radius >= 0) {
          continue;
        }

        const std::int64_t magnitude = *radius <= -kMaxBarRadius ? kMaxBarRadius : -*radius;
        table.insert_or_assign(key, magnitude);
        changed = true;
      }

      if (changed) {
        table.insert_or_assign("concave_edge_corners", true);
      }
      return changed;
    }

    template <typename OnChanged> void migrateNegativeBarRadii(toml::table& root, OnChanged&& onChanged) {
      auto* bars = root["bar"].as_table();
      if (bars == nullptr) {
        return;
      }

      for (auto& [barName, barNode] : *bars) {
        auto* bar = barNode.as_table();
        if (bar == nullptr) {
          continue;
        }

        const std::string barPath = "bar." + std::string(barName.str());
        if (migrateNegativeRadii(*bar)) {
          onChanged(barPath);
        }

        auto* monitors = (*bar)["monitor"].as_table();
        if (monitors == nullptr) {
          continue;
        }
        for (auto& [monitorName, monitorNode] : *monitors) {
          auto* monitor = monitorNode.as_table();
          if (monitor != nullptr && migrateNegativeRadii(*monitor)) {
            onChanged(barPath + ".monitor." + std::string(monitorName.str()));
          }
        }
      }
    }

    void migrateNegativeBarRadiiSidecar(toml::table& root, schema::Diagnostics& diag) {
      migrateNegativeBarRadii(root, [&diag](const std::string& path) {
        diag.warn(path, "migrated negative corner radii to concave_edge_corners");
      });
    }

    // sunset/sunrise used to schedule day/night on their own whenever no coordinates were available.
    // They now require custom_schedule. Turn it on for configs that relied on the old behavior,
    // i.e. times are set and no coordinate source is.
    template <typename OnChanged> void migrateCustomSchedule(toml::table& root, OnChanged&& onChanged) {
      auto* location = root["location"].as_table();
      if (location == nullptr || location->contains("custom_schedule")) {
        return;
      }

      const auto sunset = (*location)["sunset"].value<std::string>();
      const auto sunrise = (*location)["sunrise"].value<std::string>();
      if (!sunset.has_value() || sunset->empty() || !sunrise.has_value() || sunrise->empty()) {
        return;
      }

      const bool hasCoordinates = (*location)["latitude"].is_number() && (*location)["longitude"].is_number();
      if (hasCoordinates) {
        return;
      }

      location->insert_or_assign("custom_schedule", true);
      onChanged("location");
    }

    void migrateCustomScheduleSidecar(toml::table& root, schema::Diagnostics& diag) {
      migrateCustomSchedule(root, [&diag](const std::string& path) {
        diag.warn(path, "sunset/sunrise now require custom_schedule; enabled it to keep the schedule");
      });
    }

    std::uint64_t stableIssueHash(int migrationVersion, std::string_view path) {
      constexpr std::uint64_t kOffset = 14695981039346656037ULL;
      constexpr std::uint64_t kPrime = 1099511628211ULL;
      std::uint64_t hash = kOffset;
      const auto append = [&hash](std::string_view value) {
        for (const unsigned char byte : value) {
          hash ^= byte;
          hash *= kPrime;
        }
      };
      append(std::to_string(migrationVersion));
      append(":");
      append(path);
      return hash;
    }

    std::set<std::string> splitFingerprint(std::string_view fingerprint) {
      std::set<std::string> entries;
      std::size_t start = 0;
      while (start < fingerprint.size()) {
        const std::size_t end = fingerprint.find(',', start);
        const std::size_t length = end == std::string_view::npos ? fingerprint.size() - start : end - start;
        if (length > 0) {
          entries.emplace(fingerprint.substr(start, length));
        }
        if (end == std::string_view::npos) {
          break;
        }
        start = end + 1;
      }
      return entries;
    }

  } // namespace

  const std::vector<ConfigMigration>& configMigrations() {
    static const std::vector<ConfigMigration> migrations = {
        {
            .toVersion = kNegativeBarRadiusMigrationVersion,
            .summary = "bar: migrate negative corner radii",
            .apply = migrateNegativeBarRadiiSidecar,
        },
        {
            .toVersion = kCustomScheduleMigrationVersion,
            .summary = "location: opt legacy sunset/sunrise schedules into custom_schedule",
            .apply = migrateCustomScheduleSidecar,
        },
        {
            .toVersion = kUnifiedChromeMigrationVersion,
            .summary = "chrome: remove per-surface geometry and visual overrides",
            .apply = migrateUnifiedChromeConfigSidecar,
        },
        {
            .toVersion = kNexusLauncherMigrationVersion,
            .summary = "theme: reserve vibrant for Material 3, remove dock, and add Nexus launcher defaults",
            .apply = migrateNexusLauncherConfig,
        },
        {
            .toVersion = kLockscreenIslandMigrationVersion,
            .summary = "lockscreen: replace widgets with the built-in lock screen island",
            .apply = removeLockscreenWidgetsConfig,
        },
        {
            .toVersion = kThemeTemplatesRemovalMigrationVersion,
            .summary = "theme: remove deprecated template configuration",
            .apply = removeThemeTemplatesConfig,
        },
        {
            .toVersion = kStandalonePanelsMigrationVersion,
            .summary = "panels: replace the control-center dashboard with standalone destinations",
            .apply = migrateStandalonePanelsSidecar,
        },
        {
            .toVersion = kDynamicPanelHeightMigrationVersion,
            .summary = "panels: remove fixed height overrides in favor of intrinsic sizing",
            .apply = removePanelHeightOverridesSidecar,
        },
        {
            .toVersion = kShadowlessSurfacesMigrationVersion,
            .summary = "surfaces: remove outer shadow configuration",
            .apply = removeSurfaceShadowConfigSidecar,
        },
    };
    return migrations;
  }

  int currentConfigVersion() {
    const auto& migrations = configMigrations();
    return migrations.empty() ? 0 : migrations.back().toVersion;
  }

  std::optional<int> storedConfigVersion(const toml::table& root, schema::Diagnostics& diag) {
    const toml::node* node = root.get(kConfigVersionKey);
    if (node == nullptr) {
      return 0;
    }

    const auto value = node->value<std::int64_t>();
    if (!value.has_value()) {
      diag.fatal(std::string(kConfigVersionKey), "expected a non-negative integer", "config.version.invalid");
      return std::nullopt;
    }
    if (*value < 0 || *value > std::numeric_limits<int>::max()) {
      diag.fatal(std::string(kConfigVersionKey), "expected a non-negative integer", "config.version.invalid");
      return std::nullopt;
    }

    const int version = static_cast<int>(*value);
    if (version > currentConfigVersion()) {
      diag.fatal(
          std::string(kConfigVersionKey),
          std::format("version {} is newer than supported version {}", version, currentConfigVersion()),
          "config.version.unsupported"
      );
      return std::nullopt;
    }
    return version;
  }

  int applyPendingConfigMigrations(
      toml::table& root, int storedVersion, schema::Diagnostics& diag, std::span<const ConfigMigration> migrations
  ) {
    int appliedVersion = storedVersion;
    for (const ConfigMigration& migration : migrations) {
      if (migration.toVersion <= storedVersion) {
        continue;
      }
      migration.apply(root, diag);
      appliedVersion = migration.toVersion;
    }
    return appliedVersion;
  }

  void normalizeLegacyConfig(toml::table& root, LegacyConfigIssues& issues) {
    // Strip obsolete visual keys before schema validation. Their previous
    // per-surface geometry cannot be represented by the unified chrome host.
    migrateUnifiedChromeConfig(root, [&issues](const std::string& path) {
      issues.push_back({
          .migrationVersion = kUnifiedChromeMigrationVersion,
          .path = path,
          .message = "this visual option was removed; [shell.chrome] is the single chrome geometry source",
      });
    });
    migrateNegativeBarRadii(root, [&issues](const std::string& path) {
      issues.push_back({
          .migrationVersion = kNegativeBarRadiusMigrationVersion,
          .path = path,
          .message = "negative corner radii are deprecated; use positive radii and concave_edge_corners = true",
      });
    });
    migrateCustomSchedule(root, [&issues](const std::string& path) {
      issues.push_back({
          .migrationVersion = kCustomScheduleMigrationVersion,
          .path = path,
          .message = "sunset/sunrise no longer schedule on their own; set custom_schedule = true",
      });
    });
    if (root.contains("dock")) {
      root.erase("dock");
      issues.push_back({
          .migrationVersion = kNexusLauncherMigrationVersion,
          .path = "dock",
          .message = "the dock was removed; use the rail and workspace application icons",
      });
    }
    if (root.contains("lockscreen_widgets")) {
      root.erase("lockscreen_widgets");
      issues.push_back({
          .migrationVersion = kLockscreenIslandMigrationVersion,
          .path = "lockscreen_widgets",
          .message = "lock screen widgets were removed; the built-in lock screen island now provides this layout",
      });
    }
    if (auto* theme = root["theme"].as_table(); theme != nullptr && theme->contains("templates")) {
      theme->erase("templates");
      issues.push_back({
          .migrationVersion = kThemeTemplatesRemovalMigrationVersion,
          .path = "theme.templates",
          .message = "theme templates were removed; their configuration has been ignored",
      });
    }
    migrateStandalonePanels(root, [&issues](const std::string& path, const std::string& message) {
      issues.push_back({
          .migrationVersion = kStandalonePanelsMigrationVersion,
          .path = path,
          .message = message,
      });
    });
    removePanelHeightOverrides(root, [&issues](const std::string& path) {
      issues.push_back({
          .migrationVersion = kDynamicPanelHeightMigrationVersion,
          .path = path,
          .message = "fixed panel height was removed; panels now follow content up to 720 logical pixels",
      });
    });
    removeSurfaceShadowConfig(root, [&issues](const std::string& path) {
      issues.push_back({
          .migrationVersion = kShadowlessSurfacesMigrationVersion,
          .path = path,
          .message = "surface shadows were removed; this configuration has been ignored",
      });
    });
  }

  std::string legacyConfigIssueFingerprint(const LegacyConfigIssues& issues) {
    std::set<std::string> entries;
    for (const LegacyConfigIssue& issue : issues) {
      entries.insert(std::format("{:016x}", stableIssueHash(issue.migrationVersion, issue.path)));
    }

    std::string fingerprint;
    for (const std::string& entry : entries) {
      if (!fingerprint.empty()) {
        fingerprint.push_back(',');
      }
      fingerprint += entry;
    }
    return fingerprint;
  }

  bool legacyConfigFingerprintHasNewIssues(std::string_view currentFingerprint, std::string_view previousFingerprint) {
    const std::set<std::string> current = splitFingerprint(currentFingerprint);
    const std::set<std::string> previous = splitFingerprint(previousFingerprint);
    return std::ranges::any_of(current, [&previous](const std::string& entry) { return !previous.contains(entry); });
  }

  bool legacyConfigReminderIntervalElapsed(std::int64_t nowEpochSeconds, std::int64_t previousEpochSeconds) {
    return previousEpochSeconds > nowEpochSeconds
        || nowEpochSeconds - previousEpochSeconds >= kLegacyConfigReminderIntervalSeconds;
  }

} // namespace gnil::config
