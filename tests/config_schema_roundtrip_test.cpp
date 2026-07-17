// Round-trip + golden tests for the declarative config schema.
//
// The schema is now the single source for both serialize (config_export::serialize →
// writeTable) and parse (parseConfigTable → readInto), so there is no legacy code
// to compare against. What still earns its keep:
//   - read inverse — readInto(writeTable(x)) == x for every section: the schema's
//                    read and write are mutual inverses (catches a field whose read
//                    key != write key, or a lossy codec).
//   - bar golden   — config_export::serialize(probe)["bar"] stays byte-identical to a captured
//                    reference (locks the resolve-and-flatten monitor-override emit).
//   - clamp goldens — pin parse-time range behavior.

#include "config/config_export.h"
#include "config/config_types.h"
#include "config/schema/config_schema.h"
#include "config/schema/config_sections.h"
#include "config/schema/engine.h"
#include "core/input/key_chord.h"
#include "core/toml.h"

#include <print>
#include <set>
#include <sstream>
#include <string>

using namespace gnil::config::schema;

namespace {

  int g_failures = 0;

  void fail(const std::string& message) {
    std::println(stderr, "config_schema_roundtrip: FAIL: {}", message);
    ++g_failures;
  }

  // Mirror of ConfigService::formatToml so serialized output matches exactly.
  std::string formatToml(const toml::table& table) {
    std::ostringstream out;
    out << toml::toml_formatter{
        table, toml::toml_formatter::default_flags & ~toml::format_flags::allow_literal_strings
    };
    return out.str();
  }

  void checkIdleActionResolution() {
    const IdleBehaviorConfig screenOff{
        .name = "screen-off",
        .enabled = true,
        .timeoutSeconds = 60,
        .action = "screen_off",
        .command = {},
        .resumeCommand = "notify-send resumed",
    };
    const ResolvedIdleBehavior resolvedScreenOff = resolveIdleBehaviorActions(screenOff);
    if (resolvedScreenOff.idleAction.kind != IdleActionKind::ScreenOff) {
      fail("idle: screen_off did not resolve to native screen-off");
    }
    if (resolvedScreenOff.resumeAction.kind != IdleActionKind::ScreenOn) {
      fail("idle: screen_off did not retain native screen-on with a custom resume command");
    }
    if (resolvedScreenOff.resumeCommand != screenOff.resumeCommand) {
      fail("idle: screen_off did not retain its additional resume command");
    }

    const IdleBehaviorConfig custom{
        .name = "custom",
        .enabled = true,
        .timeoutSeconds = 60,
        .action = "command",
        .command = "notify-send idle",
        .resumeCommand = "notify-send resumed",
    };
    const ResolvedIdleBehavior resolvedCustom = resolveIdleBehaviorActions(custom);
    if (resolvedCustom.idleAction.kind != IdleActionKind::Command
        || resolvedCustom.idleAction.command != custom.command) {
      fail("idle: custom command did not resolve to its configured idle command");
    }
    if (resolvedCustom.resumeAction.kind != IdleActionKind::None) {
      fail("idle: custom command gained an implicit native resume action");
    }
    if (resolvedCustom.resumeCommand != custom.resumeCommand) {
      fail("idle: custom command did not retain its configured resume command");
    }
  }

  // A fully-specified bar with a fully-specified monitor override. Every override
  // optional is set so the resolve-and-flatten write round-trips back into the
  // same override on read (a partial override would come back fully resolved).
  BarConfig makeProbeBar() {
    BarConfig bar;
    bar.name = "default";
    bar.enabled = false;
    bar.autoHide = true;
    bar.smartAutoHide = false;
    bar.showOnWorkspaceSwitch = true;
    bar.layer = "overlay";
    bar.thickness = 44;
    bar.deadZone.command = "notify-send bar-left";
    bar.deadZone.rightCommand = "notify-send bar-right";
    bar.deadZone.middleCommand = "notify-send bar-middle";
    bar.deadZone.scrollUpCommand = "notify-send bar-scroll-up";
    bar.deadZone.scrollDownCommand = "notify-send bar-scroll-down";
    bar.padding = 12;
    bar.widgetSpacing = 8;
    bar.capsuleThickness = 0.5f;
    bar.scale = 2.0f;
    bar.fontWeight = 600;
    bar.fontFamily = "Inter";
    bar.startWidgets = {"launcher"};
    bar.centerWidgets = {"clock", "weather"};
    bar.endWidgets = {"battery"};
    bar.widgetCapsuleDefault = true;
    bar.widgetCapsuleFill = colorSpecFromConfigString("#abcdef");
    bar.widgetCapsuleForeground = colorSpecFromConfigString("#fedcba");
    bar.widgetColor = colorSpecFromConfigString("#0a0b0c");
    bar.widgetIconColor = colorSpecFromConfigString("#0c0b0a");
    bar.widgetCapsulePadding = 16.0f;
    bar.widgetCapsuleRadius = 12.0;
    bar.widgetCapsuleOpacity = 0.9f;
    bar.widgetCapsuleBorderSpecified = true;
    bar.widgetCapsuleBorder = colorSpecFromConfigString("#111213");
    bar.hoverHighlight = false;
    BarCapsuleGroupStyle group;
    group.id = "grp1";
    group.members = {"clock", "weather"};
    group.fill = colorSpecFromConfigString("#222324");
    group.borderSpecified = true;
    group.border = colorSpecFromConfigString("#333435");
    group.foreground = colorSpecFromConfigString("#444546");
    group.padding = 20.0f;
    group.radius = 14.0f;
    group.opacity = 0.8f;
    bar.widgetCapsuleGroups = {group};

    BarMonitorOverride ovr;
    ovr.match = "DP-1";
    ovr.enabled = true;
    ovr.autoHide = false;
    ovr.showOnHover = false;
    ovr.smartAutoHide = false;
    ovr.showOnWorkspaceSwitch = true;
    ovr.layer = "top";
    ovr.thickness = 50;
    ovr.deadZone.command = "notify-send bar-left";
    ovr.deadZone.rightCommand = "notify-send bar-right";
    ovr.deadZone.middleCommand = "notify-send monitor-middle";
    ovr.deadZone.scrollUpCommand = "notify-send monitor-scroll-up";
    ovr.deadZone.scrollDownCommand = "notify-send bar-scroll-down";
    ovr.padding = 11;
    ovr.widgetSpacing = 7;
    ovr.capsuleThickness = 0.25f;
    ovr.scale = 1.5f;
    ovr.fontFamily = "Fira Sans";
    ovr.startWidgets = std::vector<std::string>{"tray"};
    ovr.centerWidgets = std::vector<std::string>{"media"};
    ovr.endWidgets = std::vector<std::string>{"volume"};
    ovr.widgetCapsuleDefault = false;
    ovr.widgetCapsuleFill = colorSpecFromConfigString("#b1b2b3");
    ovr.widgetCapsuleBorderSpecified = true;
    ovr.widgetCapsuleBorder = colorSpecFromConfigString("#c1c2c3");
    ovr.widgetCapsuleForeground = colorSpecFromConfigString("#d1d2d3");
    ovr.widgetColor = colorSpecFromConfigString("#e1e2e3");
    ovr.widgetIconColor = colorSpecFromConfigString("#e3e2e1");
    ovr.hoverHighlight = true;
    BarCapsuleGroupStyle ogroup;
    ogroup.id = "ogrp";
    ogroup.members = {"volume"};
    ogroup.fill = colorSpecFromConfigString("#f1f2f3");
    ogroup.borderSpecified = true;
    ogroup.border = colorSpecFromConfigString("#0f0e0d");
    ogroup.foreground = colorSpecFromConfigString("#0c0b0a");
    ogroup.padding = 18.0f;
    ogroup.radius = 9.0f;
    ogroup.opacity = 0.6f;
    ovr.widgetCapsuleGroups = std::vector<BarCapsuleGroupStyle>{ogroup};
    ovr.widgetCapsulePadding = 24.0;
    ovr.widgetCapsuleRadius = 30.0;
    ovr.widgetCapsuleOpacity = 0.5;
    bar.monitorOverrides = {ovr};
    return bar;
  }

  // Build a config whose migrated sections hold non-default values, so parity
  // checks exercise real serialization rather than all-defaults.
  Config makeProbe() {
    Config c;
    c.audio = AudioConfig{true, true, 0.73f, "change.ogg", "notify.ogg"};
    c.weather = WeatherConfig{false, false, 17, "imperial"};
    c.osd.position = "bottom_left";
    c.osd.positionVertical = "top_right";
    c.osd.orientation = "vertical";
    c.osd.scale = 1.4f;
    c.osd.backgroundOpacity = 0.42f;
    c.osd.offsetX = 33;
    c.osd.offsetY = 11;
    c.osd.monitors = {"DP-1", "HDMI-A-1"};
    c.osd.kinds.lockKeys = false;
    c.osd.kinds.keyboardLayout = false;
    c.backdrop = BackdropConfig{true, 0.8f, 0.2f};
    c.lockscreen =
        LockscreenConfig{.blurredDesktop = true, .blurIntensity = 0.6f, .tintIntensity = 0.25f, .monitors = {"DP-1"}};
    c.system.monitor.enabled = false;
    c.system.monitor.cpuTempSensorPath = "/sys/class/hwmon/hwmon3/temp1_input";
    c.system.monitor.cpuPollSeconds = 5.0f;
    c.system.monitor.gpuPollSeconds = 4.0f;
    c.system.monitor.memoryPollSeconds = 6.0f;
    c.system.monitor.networkPollSeconds = 7.0f;
    c.system.monitor.diskPollSeconds = 12.0f;
    c.nightlight = NightLightConfig{true, true, 6000, 3500}; // gap satisfied
    c.location.address = "Berlin";
    c.location.customSchedule = true;
    c.location.sunset = "20:30";
    c.location.sunrise = "06:15";
    c.location.latitude = 52.52;
    c.location.longitude = 13.405;
    c.notification = NotificationConfig{
        .enableDaemon = false,
        .showAppName = false,
        .showActions = false,
        .monitors = {"DP-2"},
        .collapseOnDismiss = false,
        .clearThreshold = 0.45f,
        .expandThreshold = 36,
        .groupPreviewCount = 5,
        .openExpanded = true,
        .filters = {NotificationFilterConfig{
            .name = "discord",
            .enabled = true,
            .match = "discord",
            .showToast = false,
            .saveHistory = false,
            .playSound = false,
            .allowPermanent = false,
            .allowedUrgencies = {"normal", "critical"},
        }},
    };
    c.sidebar = SidebarConfig{
        .enabled = false,
        .showOnHover = true,
        .minHoverThresholdMs = 350,
        .dragThreshold = 96,
    };
    c.brightness.enableDdcutil = true;
    c.brightness.ddcutilIgnoreMmids = {"ABC123"};
    c.brightness.monitorOverrides = {
        {"DP-1", BrightnessBackendPreference::Ddcutil},
        {"eDP-1", std::nullopt},
    };
    c.battery.warningThreshold = 15;
    c.battery.deviceThresholds = {{"BAT0", 10}, {"hidpp:1", 25}};
    c.calendar.enabled = true;
    c.calendar.refreshMinutes = 30;
    c.calendar.showEventsCard = false;
    c.calendar.accounts = {
        {"acc1", "google", "Work", "#ff0000", "", "", "", {}},
        {"acc2", "caldav", "Home", "", "custom", "https://dav.example.com/remote.php/dav/", "user", {"personal"}},
    };
    // Explicit chords so write→read round-trips (empty would emit defaults instead).
    c.keybinds.validate = {*parseKeyChordSpec("Return")};
    c.keybinds.cancel = {*parseKeyChordSpec("Escape")};
    c.keybinds.left = {*parseKeyChordSpec("Left")};
    c.keybinds.right = {*parseKeyChordSpec("Right")};
    c.keybinds.up = {*parseKeyChordSpec("Up")};
    c.keybinds.down = {*parseKeyChordSpec("Down")};
    c.keybinds.tabNext = defaultKeybindSet(KeybindAction::TabNext);
    c.keybinds.tabPrevious = defaultKeybindSet(KeybindAction::TabPrevious);
    c.hooks.commands[0] = {"notify-send hi"};
    c.hooks.commands[2] = {"cmd-a", "cmd-b"};
    c.idle.preActionFadeSeconds = 3.0f;
    // Explicit normalized actions so normalizeIdleBehaviorAction is a no-op on read.
    c.idle.behaviors = {
        {"dim", true, 60, "lock", "", "", true},
        {"off", false, 300, "screen_off", "", "", true},
    };
    c.wallpaper.enabled = false;
    c.wallpaper.fillColor = colorSpecFromConfigString("#ff8800");
    c.wallpaper.transitions = {WallpaperTransition::Wipe, WallpaperTransition::Zoom};
    c.wallpaper.transitionDurationMs = 2000.0f;
    c.wallpaper.edgeSmoothness = 0.5f;
    c.wallpaper.directory = "/srv/wallpapers"; // absolute: expandUserPath leaves it unchanged
    c.wallpaper.automation.enabled = true;
    c.wallpaper.automation.intervalSeconds = 30;
    c.wallpaper.automation.order = WallpaperAutomationConfig::Order::Alphabetical;
    c.wallpaper.monitorOverrides = {
        {"DP-1", true, colorSpecFromConfigString("#00ff00"), std::string("/srv/wp1"), std::nullopt, std::nullopt},
    };
    c.accessibility.uiScale = 1.25f;
    c.nexus.wallpapersPerRow = 5;
    c.nexus.networkRescanIntervalMs = 24000;
    c.defaultApps.terminal = "foot.desktop";
    c.defaultApps.audio = "com.github.wwmm.easyeffects.desktop";
    c.defaultApps.mediaPlayback = "org.gnome.Music.desktop";
    c.defaultApps.fileManager = "org.gnome.Nautilus.desktop";
    c.shell.buttonBorders = false;
    c.shell.fontFamily = "Inter";
    c.shell.timeFormat = "{:%H:%M:%S}";
    c.shell.passwordMaskStyle = PasswordMaskStyle::RandomIcons;
    c.shell.clipboardHistoryMaxEntries = 80;
    c.shell.clipboardAutoPaste = ClipboardAutoPasteMode::CtrlV;
    c.shell.avatarPath = "/home/u/face.png";
    c.shell.chrome.frameThickness = 12.0f;
    c.shell.chrome.rounding = 28.0f;
    c.shell.chrome.smoothing = 16.0f;
    c.shell.chrome.deformScale = 0.06f;
    c.shell.panel.sizes = {
        {.id = "network", .width = 520},
        {.id = "sidebar", .width = 460},
    };
    c.shell.animation.speed = 1.5f;
    c.shell.launcher.compact = true;
    c.shell.launcher.sessionSearch = true;
    c.shell.launcher.sortByUsage = false;
    DmenuEntryConfig notifyDmenu;
    notifyDmenu.id = "notify";
    notifyDmenu.exec = std::string("notify-send \"{query}\"");
    notifyDmenu.prefix = std::string("/notify");
    notifyDmenu.label = std::string("Notify");
    notifyDmenu.glyph = std::string("bell");
    notifyDmenu.freeform = true;
    c.shell.launcher.dmenu.entries = {notifyDmenu};
    c.shell.screenCorners.enabled = true;
    c.shell.screenCorners.size = 24;
    c.shell.mpris.blacklist = {"firefox"};
    c.shell.screenshot.directory = "/shots";
    c.shell.screenshot.pipeToCommand = true;
    c.shell.session.actions = {
        SessionPanelActionConfig{
            "lock",
            true,
            std::nullopt,
            std::string("Lock"),
            std::string("lock"),
            SessionActionButtonVariant::Primary,
            parseKeyChordSpec("Ctrl+l"),
        },
        SessionPanelActionConfig{
            "shutdown", false, std::nullopt, std::nullopt, std::nullopt, SessionActionButtonVariant::Destructive,
            std::nullopt
        },
    };
    c.shell.session.power.suspend = "zzz";
    c.shell.session.power.reboot = "sudo -n reboot";
    c.shell.session.power.shutdown = "sudo -n poweroff";
    c.theme.source = PaletteSource::Wallpaper;
    c.theme.builtinPalette = "Tokyo";
    c.theme.mode = ThemeMode::Light;
    c.theme.liveWallpaperOutput = "DP-1";
    c.wallpaper.videoOutputs = {
        VideoWallpaperOutput{
            .match = "DP-1", .path = "/videos/aurora.webm", .mute = true, .hardwareDecode = true,
            .autoPause = true, .keepLastFrame = false, .mpvOptions = ""
        },
    };
    c.accessibility.uiScale = 1.25f;
    c.accessibility.highContrast = true;

    c.hotCorners.enabled = true;
    c.hotCorners.topLeft = {.action = "launcher", .command = ""};
    c.hotCorners.bottomRight = {.action = "command", .command = "notify-send corner"};

    c.bars = {makeProbeBar()};
    return c;
  }

  void checkClamps() {
    // sound_volume above the max clamps to 1.0.
    {
      auto t = toml::parse("sound_volume = 2.5");
      AudioConfig a{};
      Diagnostics d;
      readInto(t, a, audioSchema(), "audio", d);
      if (a.soundVolume != 1.0f) {
        fail("audio.sound_volume clamp: expected 1.0");
      }
    }
    // osd.offset_x has a min-only floor at 0.
    {
      auto t = toml::parse("offset_x = -5");
      OsdConfig o{};
      Diagnostics d;
      readInto(t, o, osdSchema(), "osd", d);
      if (o.offsetX != 0) {
        fail("osd.offset_x floor: expected 0");
      }
    }
    // Unknown enum-like string is left untouched on a plain string field (no enum here),
    // so just verify osd.scale below the min clamps up.
    {
      auto t = toml::parse("scale = 0.1");
      OsdConfig o{};
      Diagnostics d;
      readInto(t, o, osdSchema(), "osd", d);
      if (o.scale != 0.5f) {
        fail("osd.scale clamp: expected 0.5");
      }
    }
    // Clipboard history count accepts large text-heavy histories but still has
    // an explicit config ceiling.
    {
      auto t = toml::parse("clipboard_history_max_entries = 25000");
      ShellConfig s{};
      Diagnostics d;
      readInto(t, s, shellSchema(), "shell", d);
      if (s.clipboardHistoryMaxEntries != 10000) {
        fail("shell.clipboard_history_max_entries clamp: expected 10000");
      }
    }
  }

} // namespace

int main() {
  checkIdleActionResolution();
  // Pins the unified-chrome bar serialization. Per-surface geometry and visual
  // overrides must not reappear in either the bar or monitor override output.
  const char* const kBarGolden =
      R"(order = [ "default" ]

[default]
auto_hide = true
auto_hide_collapsed_thickness = 6
capsule = true
capsule_border = "#111213"
capsule_fill = "#ABCDEF"
capsule_foreground = "#FEDCBA"
capsule_opacity = 0.89999997615814209
capsule_padding = 16.0
capsule_radius = 12.0
capsule_thickness = 0.5
center = [ "clock", "weather" ]
color = "#0A0B0C"
enabled = false
end = [ "battery" ]
font_family = "Inter"
font_weight = 600
hover_highlight = false
icon_color = "#0C0B0A"
layer = "overlay"
padding = 12
scale = 2.0
show_on_hover = false
show_on_workspace_switch = true
smart_auto_hide = false
start = [ "launcher" ]
thickness = 44
widget_spacing = 8

    [default.dead_zone]
    command = "notify-send bar-left"
    middle_command = "notify-send bar-middle"
    right_command = "notify-send bar-right"
    scroll_down_command = "notify-send bar-scroll-down"
    scroll_up_command = "notify-send bar-scroll-up"

    [default.monitor.DP-1]
    auto_hide = false
    auto_hide_collapsed_thickness = 6
    capsule = false
    capsule_border = "#C1C2C3"
    capsule_fill = "#B1B2B3"
    capsule_foreground = "#D1D2D3"
    capsule_opacity = 0.5
    capsule_padding = 24.0
    capsule_radius = 30.0
    capsule_thickness = 0.25
    center = [ "media" ]
    color = "#E1E2E3"
    enabled = true
    end = [ "volume" ]
    font_family = "Fira Sans"
    font_weight = 600
    hover_highlight = true
    icon_color = "#E3E2E1"
    layer = "top"
    match = "DP-1"
    padding = 11
    scale = 1.5
    show_on_hover = false
    show_on_workspace_switch = true
    smart_auto_hide = false
    start = [ "tray" ]
    thickness = 50
    widget_spacing = 7

        [default.monitor.DP-1.dead_zone]
        command = "notify-send bar-left"
        middle_command = "notify-send monitor-middle"
        right_command = "notify-send bar-right"
        scroll_down_command = "notify-send bar-scroll-down"
        scroll_up_command = "notify-send monitor-scroll-up"

        [[default.monitor.DP-1.capsule_group]]
        border = "#0F0E0D"
        enabled = true
        fill = "#F1F2F3"
        foreground = "#0C0B0A"
        id = "ogrp"
        members = [ "volume" ]
        opacity = 0.60000002384185791
        padding = 18.0
        radius = 9.0

    [[default.capsule_group]]
    border = "#333435"
    enabled = true
    fill = "#222324"
    foreground = "#444546"
    id = "grp1"
    members = [ "clock", "weather" ]
    opacity = 0.80000001192092896
    padding = 20.0
    radius = 14.0)";

  const Config probe = makeProbe();
  const toml::table serialized = config_export::serialize(probe);

  // Bar: write parity against the captured golden, plus read-inverse via the
  // schemas (reconstructing the bar exactly as config_service does).
  {
    const std::string fresh = formatToml(*serialized["bar"].as_table());
    if (fresh != kBarGolden) {
      fail(
          "bar: serialization drifted from golden\n--- golden ---\n"
          + std::string(kBarGolden)
          + "\n--- fresh ---\n"
          + fresh
      );
    }
  }
  {
    const auto* barTbl = serialized["bar"]["default"].as_table();
    BarConfig rt;
    rt.name = "default";
    Diagnostics diag;
    readInto(*barTbl, rt, barFieldsSchema(), "bar.default", diag);
    if (const auto* monMap = (*barTbl)["monitor"].as_table()) {
      for (const auto& [monName, monNode] : *monMap) {
        if (const auto* monTbl = monNode.as_table()) {
          BarMonitorOverride ovr;
          ovr.match = std::string(monName.str());
          readInto(*monTbl, ovr, barMonitorOverrideSchema(), "bar.default.monitor", diag);
          rt.monitorOverrides.push_back(std::move(ovr));
        }
      }
    }
    if (!(rt == probe.bars[0])) {
      fail("bar: read inverse did not reconstruct the original bar (incl. monitor override)");
    }
  }

  // Every schema-backed section must round-trip, AND the probe must actually populate
  // it. Iterating the section registry rather than a hand-written list means a new
  // section is covered the moment it is declared — and fails here until its probe
  // values are filled in.
  {
    const Config defaults;
    for (const SectionSpec& spec : sections()) {
      const std::string name(spec.name);
      if (spec.sectionEqual(probe, defaults)) {
        fail(name + ": makeProbe leaves this section at its defaults — populate it, or the round-trip is vacuous");
        continue;
      }
      const auto* sectionTbl = serialized[spec.name].as_table();
      if (sectionTbl == nullptr) {
        fail(name + ": config_export::serialize emitted no [" + name + "] table");
        continue;
      }
      Config roundtrip;
      Diagnostics diag;
      spec.read(*sectionTbl, roundtrip, diag);
      if (!spec.sectionEqual(roundtrip, probe)) {
        fail(name + ": read inverse did not reconstruct the original section");
      }
    }
  }

  // Section names must be unique across the registry and the custom root keys, or a
  // lookup would silently resolve to the wrong handler.
  {
    std::set<std::string_view> seen;
    for (const SectionSpec& spec : sections()) {
      if (!seen.insert(spec.name).second) {
        fail(std::string(spec.name) + ": duplicate section name in the registry");
      }
    }
    for (const std::string_view key : customRootKeys()) {
      if (!seen.insert(key).second) {
        fail(std::string(key) + ": custom root key collides with a registry section");
      }
    }
  }

  checkClamps();

  if (g_failures == 0) {
    std::println("config_schema_roundtrip: all checks passed");
    return 0;
  }
  std::println(stderr, "config_schema_roundtrip: {} failure(s)", g_failures);
  return 1;
}
