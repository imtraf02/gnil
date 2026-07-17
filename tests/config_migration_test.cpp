#include "config/config_migrations.h"
#include "core/toml.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <print>
#include <string>
#include <string_view>

namespace {

  int g_failures = 0;
  int g_syntheticMigrationApplications = 0;

  void countSyntheticMigration(toml::table&, gnil::config::schema::Diagnostics&) {
    ++g_syntheticMigrationApplications;
  }

  void expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "config_migration_test: FAIL: {}", message);
      ++g_failures;
    }
  }

  void checkUnifiedChromeMigration() {
    toml::table root = toml::parse(R"(
[shell.panel]
transparency_mode = "glass"
launcher_placement = "floating"
open_near_click_launcher = true

[shell.shadow]
direction = "down"
alpha = 0.55

[notification]
position = "bottom_left"
background_opacity = 0.5

[bar.main]
radius = -12
radius_top_left = -20
margin_edge = 8
shadow = true

[bar.main.monitor.dp1]
match = "DP-1"
radius = -16
panel_overlap = 2

[dock]
radius = -7
)");

    gnil::config::LegacyConfigIssues issues;
    gnil::config::normalizeLegacyConfig(root, issues);

    expect(!root["bar"]["main"]["radius"], "legacy base radius was not removed");
    expect(!root["bar"]["main"]["radius_top_left"], "legacy per-corner radius was not removed");
    expect(!root["bar"]["main"]["margin_edge"], "legacy bar margin was not removed");
    expect(!root["bar"]["main"]["shadow"], "legacy bar shadow was not removed");
    expect(!root["bar"]["main"]["monitor"]["dp1"]["radius"], "monitor radius was not removed");
    expect(!root["bar"]["main"]["monitor"]["dp1"]["panel_overlap"], "monitor overlap was not removed");
    expect(!root["shell"]["panel"]["launcher_placement"], "panel placement was not removed");
    expect(!root["shell"]["shadow"], "removed surface shadow config survived normalization");
    expect(!root["notification"]["position"], "notification position was not removed");
    expect(!root["dock"], "removed dock configuration survived normalization");
    expect(!issues.empty(), "removed chrome keys did not report legacy issues");

    gnil::config::LegacyConfigIssues secondPassIssues;
    gnil::config::normalizeLegacyConfig(root, secondPassIssues);
    expect(secondPassIssues.empty(), "normalization was not idempotent");
  }

  void checkCustomScheduleMigration() {
    toml::table legacy = toml::parse(R"(
[location]
sunset = "20:30"
sunrise = "07:30"
)");
    gnil::config::LegacyConfigIssues issues;
    gnil::config::normalizeLegacyConfig(legacy, issues);
    expect(
        legacy["location"]["custom_schedule"].value<bool>() == true,
        "a times-only location did not opt into custom scheduling"
    );
    expect(issues.size() == 1, "times-only location did not report a legacy issue");

    gnil::config::LegacyConfigIssues secondPassIssues;
    gnil::config::normalizeLegacyConfig(legacy, secondPassIssues);
    expect(secondPassIssues.empty(), "custom scheduling normalization was not idempotent");

    // Explicit coordinates keep using the astronomical schedule.
    toml::table coords = toml::parse(R"(
[location]
sunset = "20:30"
sunrise = "07:30"
latitude = 52.52
longitude = 13.405
)");
    gnil::config::LegacyConfigIssues coordIssues;
    gnil::config::normalizeLegacyConfig(coords, coordIssues);
    expect(
        !coords["location"]["custom_schedule"].value<bool>().has_value(),
        "a location with coordinates was switched to custom scheduling"
    );
    expect(coordIssues.empty(), "a location with coordinates reported a legacy issue");

    toml::table explicitOff = toml::parse(R"(
[location]
custom_schedule = false
sunset = "20:30"
sunrise = "07:30"
)");
    gnil::config::LegacyConfigIssues offIssues;
    gnil::config::normalizeLegacyConfig(explicitOff, offIssues);
    expect(
        explicitOff["location"]["custom_schedule"].value<bool>() == false,
        "an explicit custom_schedule = false was overwritten"
    );
  }

  void checkTemplateRemovalMigration() {
    toml::table legacy = toml::parse(R"(
[theme.templates]
enable_builtin_templates = true
builtin_ids = ["foot"]
)");
    gnil::config::LegacyConfigIssues issues;
    gnil::config::normalizeLegacyConfig(legacy, issues);
    expect(!legacy["theme"]["templates"], "removed theme templates survived normalization");
    expect(
        std::ranges::any_of(issues, [](const auto& issue) { return issue.path == "theme.templates"; }),
        "removed theme templates did not report a migration issue"
    );
  }

  void checkStandalonePanelMigration() {
    toml::table legacy = toml::parse(R"(
[control_center]
width = 610
sidebar = "compact"
hidden_tabs = ["weather"]

[control_center.calendar]
show_events_card = false

[bar.main]
start = ["launcher", "control-center", "workspaces"]

[bar.main.monitor.dp1]
match = "DP-1"
end = ["control-center", "battery"]

[hot_corners.top_left]
action = "control_center"
)" );
    gnil::config::schema::Diagnostics diag;
    const int applied = gnil::config::applyPendingConfigMigrations(legacy, 6, diag);
    expect(
        applied == gnil::config::currentConfigVersion(),
        "standalone panel migration did not advance through current migrations"
    );
    expect(!legacy["control_center"], "removed control-center table survived migration");
    expect(
        legacy["shell"]["panel"]["size"]["network"]["width"].value<std::int64_t>() == 610,
        "legacy width was not copied to standalone panels"
    );
    expect(
        legacy["calendar"]["show_events_card"].value<bool>() == false,
        "calendar events-card preference was not migrated"
    );
    const auto* lane = legacy["bar"]["main"]["start"].as_array();
    expect(
        lane != nullptr && std::ranges::none_of(*lane, [](const toml::node& item) {
          return item.value<std::string_view>() == "control-center";
        }),
        "obsolete control-center bar widget survived migration"
    );
    const auto* monitorLane = legacy["bar"]["main"]["monitor"]["dp1"]["end"].as_array();
    expect(
        monitorLane != nullptr && std::ranges::none_of(*monitorLane, [](const toml::node& item) {
          return item.value<std::string_view>() == "control-center";
        }),
        "obsolete monitor-specific control-center widget survived migration"
    );
    expect(
        legacy["hot_corners"]["top_left"]["action"].value<std::string_view>() == "none",
        "obsolete control-center hot-corner action was not disabled"
    );
  }

  void checkVersionGating() {
    toml::table legacy = toml::parse(R"(
[bar.main]
radius = -10
)");
    gnil::config::schema::Diagnostics diag;
    const auto stored = gnil::config::storedConfigVersion(legacy, diag);
    expect(stored == 0, "missing config_version was not treated as legacy version 0");
    const int applied = gnil::config::applyPendingConfigMigrations(legacy, stored.value_or(0), diag);
    expect(applied == gnil::config::currentConfigVersion(), "pending migration did not reach current version");
    expect(!legacy["bar"]["main"]["radius"], "sidecar did not remove obsolete radius");

    toml::table current = toml::parse(R"(
config_version = 1
[bar.main]
radius = -10
)");
    gnil::config::schema::Diagnostics currentDiag;
    const auto currentStored = gnil::config::storedConfigVersion(current, currentDiag);
    expect(currentStored == 1, "current config_version was not read");
    (void)gnil::config::applyPendingConfigMigrations(current, currentStored.value_or(0), currentDiag);
    expect(!current["bar"]["main"]["radius"], "pending unified chrome migration did not remove radius");

    toml::table fixedHeight = toml::parse(R"(
config_version = 7
[shell.panel.size.network]
width = 520
height = 640
)");
    gnil::config::schema::Diagnostics fixedHeightDiag;
    const int fixedHeightApplied =
        gnil::config::applyPendingConfigMigrations(fixedHeight, 7, fixedHeightDiag);
    expect(
        fixedHeightApplied == gnil::config::currentConfigVersion(),
        "dynamic-height migration did not reach current version"
    );
    expect(
        fixedHeight["shell"]["panel"]["size"]["network"]["width"].value<std::int64_t>() == 520,
        "dynamic-height migration removed the width override"
    );
    expect(
        !fixedHeight["shell"]["panel"]["size"]["network"]["height"],
        "dynamic-height migration kept the fixed height override"
    );

    toml::table shadowed = toml::parse(R"(
config_version = 8
[shell.shadow]
direction = "up_left"
alpha = 0.7
)");
    gnil::config::schema::Diagnostics shadowDiag;
    const int shadowApplied = gnil::config::applyPendingConfigMigrations(shadowed, 8, shadowDiag);
    expect(
        shadowApplied == gnil::config::currentConfigVersion(),
        "shadowless-surface migration did not reach current version"
    );
    expect(!shadowed["shell"]["shadow"], "shadowless-surface migration kept shell.shadow");

    toml::table invalid = toml::parse("config_version = \"one\"");
    gnil::config::schema::Diagnostics invalidDiag;
    expect(
        !gnil::config::storedConfigVersion(invalid, invalidDiag).has_value(), "invalid config_version was accepted"
    );
    expect(invalidDiag.hasErrors(), "invalid config_version did not produce an error");
    expect(invalidDiag.hasFatalErrors(), "invalid config_version was not document-fatal");

    toml::table future = toml::parse("config_version = 999");
    gnil::config::schema::Diagnostics futureDiag;
    expect(
        !gnil::config::storedConfigVersion(future, futureDiag).has_value(), "future config_version was accepted"
    );
    expect(futureDiag.hasErrors(), "future config_version did not produce an error");
    expect(futureDiag.hasFatalErrors(), "future config_version was not document-fatal");

    gnil::config::schema::Diagnostics baseline;
    baseline.componentError("widget.clock.timezone", "widget.clock", "unknown timezone", "clock.timezone.unknown");
    gnil::config::schema::Diagnostics candidate = baseline;
    candidate.error("accessibility.ui_scale", "expected a number", "config.type.number");
    const auto introduced = candidate.introducedErrorsComparedTo(baseline);
    expect(introduced.entries.size() == 1, "diagnostic comparison did not isolate the new error");
    expect(
        introduced.entries.front().path == "accessibility.ui_scale", "diagnostic comparison returned the wrong error"
    );
  }

  void checkVibrantMigration() {
    toml::table legacy = toml::parse(R"(
config_version = 3
[theme]
wallpaper_scheme = "vibrant"
[wallpaper.favorite.home]
wallpaper_scheme = "vibrant"
[dock]
enabled = true
[lockscreen_widgets]
enabled = true
)");
    gnil::config::schema::Diagnostics diag;
    const int applied = gnil::config::applyPendingConfigMigrations(legacy, 3, diag);
    expect(applied == gnil::config::currentConfigVersion(), "pending migrations did not reach current version");
    expect(
        legacy["theme"]["wallpaper_scheme"].value<std::string_view>() == "custom-vibrant",
        "theme vibrant scheme was not renamed"
    );
    expect(
        legacy["wallpaper"]["favorite"]["home"]["wallpaper_scheme"].value<std::string_view>() == "custom-vibrant",
        "favorite vibrant scheme was not renamed"
    );
    expect(!legacy["dock"], "v4 migration did not remove the dock configuration");
    expect(!legacy["lockscreen_widgets"], "v5 migration did not remove lockscreen widgets");
  }

  void checkReminderFingerprint() {
    const gnil::config::LegacyConfigIssues first = {{1, "bar.main", "message"}};
    const gnil::config::LegacyConfigIssues reordered = {
        {1, "bar.second", "message"},
        {1, "bar.main", "different display message"},
    };
    const gnil::config::LegacyConfigIssues sameReordered = {
        {1, "bar.main", "message"},
        {1, "bar.second", "message"},
    };

    const std::string firstFingerprint = gnil::config::legacyConfigIssueFingerprint(first);
    const std::string expandedFingerprint = gnil::config::legacyConfigIssueFingerprint(reordered);
    expect(
        expandedFingerprint == gnil::config::legacyConfigIssueFingerprint(sameReordered),
        "fingerprint depends on issue ordering or display message"
    );
    expect(
        gnil::config::legacyConfigFingerprintHasNewIssues(expandedFingerprint, firstFingerprint),
        "new issue was not detected"
    );
    expect(
        !gnil::config::legacyConfigFingerprintHasNewIssues(firstFingerprint, expandedFingerprint),
        "removing an issue was treated as introducing one"
    );

    constexpr std::int64_t kStart = 1'000'000;
    expect(
        !gnil::config::legacyConfigReminderIntervalElapsed(
            kStart + gnil::config::kLegacyConfigReminderIntervalSeconds - 1, kStart
        ),
        "reminder became due before three days"
    );
    expect(
        gnil::config::legacyConfigReminderIntervalElapsed(
            kStart + gnil::config::kLegacyConfigReminderIntervalSeconds, kStart
        ),
        "reminder was not due at three days"
    );
    expect(
        gnil::config::legacyConfigReminderIntervalElapsed(kStart - 1, kStart),
        "backward clock change did not make the reminder due"
    );
  }

  void checkRegistryOrdering() {
    int expectedVersion = 1;
    for (const auto& migration : gnil::config::configMigrations()) {
      expect(migration.toVersion == expectedVersion, "migration registry has a gap or is out of order");
      expect(!migration.summary.empty(), "migration registry entry has no summary");
      expect(migration.apply != nullptr, "migration registry entry has no apply function");
      ++expectedVersion;
    }
    expect(
        expectedVersion - 1 == gnil::config::currentConfigVersion(),
        "current config version does not match the registry"
    );
  }

  void checkLargeCurrentRegistrySkipsBodies() {
    std::vector<gnil::config::ConfigMigration> migrations;
    migrations.reserve(100);
    for (int version = 1; version <= 100; ++version) {
      migrations.push_back({
          .toVersion = version,
          .summary = "synthetic migration",
          .apply = countSyntheticMigration,
      });
    }

    toml::table root;
    gnil::config::schema::Diagnostics diag;
    g_syntheticMigrationApplications = 0;
    const int current = gnil::config::applyPendingConfigMigrations(root, 100, diag, migrations);
    expect(current == 100, "synthetic current version changed");
    expect(g_syntheticMigrationApplications == 0, "current sidecar executed historical migration bodies");

    const int upgraded = gnil::config::applyPendingConfigMigrations(root, 99, diag, migrations);
    expect(upgraded == 100, "synthetic upgrade did not reach the current version");
    expect(g_syntheticMigrationApplications == 1, "synthetic upgrade did not execute exactly one pending body");
  }

} // namespace

int main() {
  checkUnifiedChromeMigration();
  checkCustomScheduleMigration();
  checkTemplateRemovalMigration();
  checkStandalonePanelMigration();
  checkVersionGating();
  checkVibrantMigration();
  checkReminderFingerprint();
  checkRegistryOrdering();
  checkLargeCurrentRegistrySkipsBodies();

  if (g_failures == 0) {
    std::println("config_migration_test: all checks passed");
  }
  return g_failures == 0 ? 0 : 1;
}
