#include "shell/settings/settings_registry.h"

#include "config/config_types.h"
#include "config/schema/config_schema.h"
#include "config/schema/ranges.h"
#include "core/files/resource_paths.h"
#include "core/log.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "shell/settings/color_spec_picker.h"
#include "shell/settings/font_weight_catalog.h"
#include "shell/wallpaper/wallpaper_paths.h"
#include "system/sysmon_threshold_profile.h"
#include "theme/builtin_palettes.h"
#include "ui/app_icon_colorization.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace settings {
  namespace {

    [[nodiscard]] std::vector<KeyChord>
    effectiveKeybindItems(const std::vector<KeyChord>& configured, KeybindAction action) {
      if (!configured.empty()) {
        return configured;
      }
      return defaultKeybindSet(action);
    }

    constexpr std::array<SettingsSectionDescriptor, 20> kSettingsSections{{
        {SettingsSection::Appearance, "appearance", "tune"},
        {SettingsSection::Wallpaper, "wallpaper", "paint"},
        {SettingsSection::Desktop, "desktop", "layout-board"},
        {SettingsSection::Dock, "dock", "dock_to_bottom"},
        {SettingsSection::Panels, "panels", "dock_to_bottom"},
        {SettingsSection::Launcher, "launcher", "rocket"},
        {SettingsSection::ControlCenter, "control-center", "adjustments"},
        {SettingsSection::Notifications, "notifications", "bell"},
        {SettingsSection::Osd, "osd", "message-circle"},
        {SettingsSection::Shell, "shell", "app-window"},
        {SettingsSection::Keybinds, "keybinds", "keyboard"},
        {SettingsSection::Security, "security", "shield-lock"},
        {SettingsSection::System, "system", "activity-heartbeat"},
        {SettingsSection::Services, "services", "stack-2"},
        {SettingsSection::Location, "location", "map-pin"},
        {SettingsSection::Power, "power", "bolt"},
        {SettingsSection::Hooks, "hooks", "link"},
        {SettingsSection::Niri, "niri", "niri"},
        {SettingsSection::Bar, "bar", "crop-3-2", false},
    }};

    const SettingsSectionDescriptor& descriptorFor(SettingsSection section) {
      const auto it = std::ranges::find(kSettingsSections, section, &SettingsSectionDescriptor::section);
      if (it == kSettingsSections.end()) {
        std::abort();
      }
      return *it;
    }

    // Builds a slider whose bounds come from the shared schema Range — the same
    // constant the parser clamps with — so the UI range and the config clamp are
    // one source. `integerValue` (write as int64) stays explicit: it is a UI/write
    // choice, not implied by the range's numeric type (e.g. transition_duration).
    template <typename V, typename T>
    SliderSetting sliderFor(V value, const gnil::config::schema::Range<T>& range, bool integerValue) {
      return SliderSetting{
          static_cast<double>(value), static_cast<double>(range.min.value()), static_cast<double>(range.max.value()),
          static_cast<double>(range.step.value()), integerValue
      };
    }

    SelectSetting asSegmented(SelectSetting setting) {
      setting.segmented = true;
      return setting;
    }

    std::optional<int> radiusStepperValue(const std::optional<double>& value) {
      if (!value.has_value()) {
        return std::nullopt;
      }
      return std::clamp(static_cast<int>(std::lround(*value)), 0, 80);
    }

    int radiusStepperFallback(const std::optional<double>& value) { return radiusStepperValue(value).value_or(8); }

    template <typename T, std::size_t N> SelectSetting enumSelect(const EnumOption<T> (&options)[N], T selected) {
      std::vector<SelectOption> opts;
      opts.reserve(N);
      std::string selectedValue;
      for (const auto& option : options) {
        std::string key(option.key);
        if (option.value == selected) {
          selectedValue = key;
        }
        opts.push_back(SelectOption{std::move(key), i18n::tr(option.labelKey)});
      }
      if (selectedValue.empty() && N > 0) {
        selectedValue = std::string(options[0].key);
      }
      return SelectSetting{std::move(opts), std::move(selectedValue)};
    }

    SelectSetting
    plainSelect(std::initializer_list<std::pair<std::string_view, std::string_view>> items, std::string_view selected) {
      std::vector<SelectOption> opts;
      opts.reserve(items.size());
      for (const auto& [value, labelKey] : items) {
        opts.push_back(SelectOption{std::string(value), i18n::tr(labelKey)});
      }
      return SelectSetting{std::move(opts), std::string(selected)};
    }

    [[nodiscard]] std::string barAutoHideMode(bool autoHide, bool smartAutoHide) {
      if (smartAutoHide) {
        return "smart";
      }
      if (autoHide) {
        return "on";
      }
      return "off";
    }

    [[nodiscard]] SelectSetting autoHideModeSelect(std::string_view mode, std::vector<std::string> smartPath) {
      auto select = asSegmented(plainSelect(
          {{"off", "settings.options.bar.auto-hide.off"},
           {"on", "settings.options.bar.auto-hide.on"},
           {"smart", "settings.options.bar.auto-hide.smart"}},
          mode
      ));
      select.linkedPath = std::move(smartPath);
      select.groupedCommit = [](std::string_view value, const std::vector<std::string>& primaryPath) {
        auto companionPath = primaryPath;
        companionPath.back() = "smart_auto_hide";
        return std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>{
            {primaryPath, ConfigOverrideValue{value == "on"}},
            {std::move(companionPath), ConfigOverrideValue{value == "smart"}},
        };
      };
      return select;
    }

    ColorSwatchPreview palettePreviewFromPalette(const ::Palette& palette) {
      return ColorSwatchPreview{
          .surface = fixedColorSpec(palette.surface),
          .swatches = {
              fixedColorSpec(palette.primary),
              fixedColorSpec(palette.secondary),
              fixedColorSpec(palette.tertiary),
              fixedColorSpec(palette.error),
          },
      };
    }

    ColorSwatchPreview builtinPalettePreview(const gnil::theme::BuiltinPalette& palette, ThemeMode mode) {
      return palettePreviewFromPalette(mode == ThemeMode::Light ? palette.light.palette : palette.dark.palette);
    }

    SelectSetting builtinPaletteSelect(std::string_view selected, ThemeMode mode) {
      std::vector<SelectOption> opts;
      opts.reserve(gnil::theme::builtinPalettes().size());
      for (const auto& palette : gnil::theme::builtinPalettes()) {
        opts.push_back(
            SelectOption{
                .value = std::string(palette.name),
                .label = std::string(palette.name),
                .description = {},
                .preview = builtinPalettePreview(palette, mode),
            }
        );
      }
      return SelectSetting{
          .options = std::move(opts), .selectedValue = std::string(selected), .preferredWidth = 240.0f
      };
    }

    SelectSetting wallpaperSchemeSelect(std::string_view selected) {
      return plainSelect(
          {{"m3-content", "theme.scheme.m3-content"},
           {"m3-tonal-spot", "theme.scheme.m3-tonal-spot"},
           {"m3-fruit-salad", "theme.scheme.m3-fruit-salad"},
           {"m3-rainbow", "theme.scheme.m3-rainbow"},
           {"m3-monochrome", "theme.scheme.m3-monochrome"},
           {"vibrant", "theme.scheme.vibrant"},
           {"faithful", "theme.scheme.faithful"},
           {"soft", "theme.scheme.soft"},
           {"dysfunctional", "theme.scheme.dysfunctional"},
           {"muted", "theme.scheme.muted"}},
          selected
      );
    }

    ColorSpecPickerSetting
    colorSpecPicker(const std::optional<ColorSpec>& selected, bool allowNone, std::string noneLabel = {}) {
      return ColorSpecPickerSetting{
          .roles = {},
          .selectedValue = optionalColorSpecConfigValue(selected),
          .allowNone = allowNone,
          .allowCustomColor = true,
          .noneLabel = std::move(noneLabel),
      };
    }

    ColorSpecPickerSetting
    colorSpecPicker(const ColorSpec& selected, bool allowNone = false, std::string noneLabel = {}) {
      return colorSpecPicker(std::optional<ColorSpec>{selected}, allowNone, std::move(noneLabel));
    }

    std::string pathText(const std::vector<std::string>& path) {
      std::string out;
      for (const auto& part : path) {
        if (!out.empty()) {
          out.push_back('.');
        }
        out += part;
      }
      return out;
    }

    SettingEntry makeEntry(
        SettingsSection section, std::string group, std::string title, std::string subtitle,
        std::vector<std::string> path, SettingControl control, std::string tags = {}, bool advanced = false
    ) {
      std::string searchText = std::string(settingsSectionId(section))
          + " "
          + group
          + " "
          + title
          + " "
          + subtitle
          + " "
          + pathText(path)
          + " "
          + tags;
      if (advanced) {
        searchText += " advanced";
      }
      return SettingEntry{
          .section = section,
          .group = std::move(group),
          .title = std::move(title),
          .subtitle = std::move(subtitle),
          .path = std::move(path),
          .control = std::move(control),
          .advanced = advanced,
          .searchText = StringUtils::toLower(searchText),
      };
    }

  } // namespace

  const BarConfig* findBar(const Config& cfg, std::string_view name) {
    for (const auto& bar : cfg.bars) {
      if (bar.name == name) {
        return &bar;
      }
    }
    return nullptr;
  }

  const BarMonitorOverride* findMonitorOverride(const BarConfig& bar, std::string_view match) {
    for (const auto& ovr : bar.monitorOverrides) {
      if (ovr.match == match) {
        return &ovr;
      }
    }
    return nullptr;
  }

  std::vector<std::string> barNames(const Config& cfg) {
    std::vector<std::string> names;
    names.reserve(cfg.bars.size());
    for (const auto& bar : cfg.bars) {
      names.push_back(bar.name);
    }
    return names;
  }

  std::string normalizedSettingQuery(std::string_view query) { return StringUtils::toLower(query); }

  bool matchesNormalizedSettingQuery(const SettingEntry& entry, std::string_view normalizedQuery) {
    if (normalizedQuery.empty()) {
      return true;
    }
    return entry.searchText.contains(normalizedQuery);
  }

  bool matchesSettingQuery(const SettingEntry& entry, std::string_view query) {
    return matchesNormalizedSettingQuery(entry, normalizedSettingQuery(query));
  }

  bool isBarMonitorOverrideSettingPath(const std::vector<std::string>& path) {
    return path.size() >= 5 && path[0] == "bar" && path[2] == "monitor";
  }

  bool settingEntryMatchesBarNavigation(
      const SettingEntry& entry, std::string_view selectedBarName, std::string_view selectedMonitorOverride
  ) {
    if (entry.section != SettingsSection::Bar || entry.path.size() < 2 || entry.path[0] != "bar") {
      return false;
    }
    if (selectedBarName.empty() || entry.path[1] != selectedBarName) {
      return false;
    }
    const bool monitorEntry = isBarMonitorOverrideSettingPath(entry.path);
    if (selectedMonitorOverride.empty()) {
      return !monitorEntry;
    }
    return monitorEntry && entry.path[3] == selectedMonitorOverride;
  }

  std::string barSettingContentSectionKey(const SettingEntry& entry) {
    if (entry.section != SettingsSection::Bar || entry.path.size() < 2) {
      return std::string(settingsSectionId(entry.section));
    }
    std::string key = "bar:" + entry.path[1];
    if (isBarMonitorOverrideSettingPath(entry.path)) {
      key += ":monitor:" + entry.path[3];
    }
    return key;
  }

  std::span<const SettingsSectionDescriptor> settingsSectionDescriptors() { return kSettingsSections; }

  namespace {
    const std::array<ParentCategoryDescriptor, 5> kParentCategories{{
        {
            "group_system",
            "settings.navigation.parents.system.title",
            "settings.navigation.parents.system.subtitle",
            "settings",
            {
                SettingsSection::System,
                SettingsSection::Power,
                SettingsSection::Security,
                SettingsSection::Niri,
                SettingsSection::Shell,
                SettingsSection::Osd,
                SettingsSection::Keybinds,
                SettingsSection::Hooks
            }
        },
        {
            "group_personalization",
            "settings.navigation.parents.personalization.title",
            "settings.navigation.parents.personalization.subtitle",
            "palette",
            {
                SettingsSection::Appearance,
                SettingsSection::Wallpaper,
                SettingsSection::Desktop,
                SettingsSection::Dock
            }
        },
        {
            "group_layout",
            "settings.navigation.parents.layout.title",
            "settings.navigation.parents.layout.subtitle",
            "dashboard",
            {
                SettingsSection::Bar,
                SettingsSection::Panels,
                SettingsSection::Launcher,
                SettingsSection::ControlCenter
            }
        },
        {
            "group_services",
            "settings.navigation.parents.services.title",
            "settings.navigation.parents.services.subtitle",
            "public",
            {
                SettingsSection::Services,
                SettingsSection::Location
            }
        },
        {
            "group_notifications",
            "settings.navigation.parents.notifications.title",
            "settings.navigation.parents.notifications.subtitle",
            "bell",
            {
                SettingsSection::Notifications
            }
        }
    }};
  } // namespace

  std::span<const ParentCategoryDescriptor> parentCategories() { return kParentCategories; }

  std::optional<ParentCategoryDescriptor> findParentCategory(std::string_view id) {
    const auto it = std::ranges::find(kParentCategories, id, &ParentCategoryDescriptor::id);
    if (it == kParentCategories.end()) {
      return std::nullopt;
    }
    return *it;
  }

  std::optional<ParentCategoryDescriptor> findParentForSection(SettingsSection section) {
    for (const auto& parent : kParentCategories) {
      if (std::ranges::contains(parent.subSections, section)) {
        return parent;
      }
    }
    return std::nullopt;
  }

  std::string_view settingsSectionId(SettingsSection section) { return descriptorFor(section).id; }

  std::string settingsSectionLabelKey(SettingsSection section) {
    return "settings.navigation.sections." + std::string(settingsSectionId(section));
  }

  std::string_view sectionGlyph(SettingsSection section) { return descriptorFor(section).glyph; }

  std::optional<SettingsSection> settingsSectionFromId(std::string_view id) {
    const auto it = std::ranges::find(kSettingsSections, id, &SettingsSectionDescriptor::id);
    if (it == kSettingsSections.end()) {
      return std::nullopt;
    }
    return it->section;
  }

  std::vector<SettingEntry> buildSettingsRegistry(
      const Config& cfg, const BarConfig* selectedBar, const BarMonitorOverride* selectedMonitorOverride,
      const RegistryEnvironment& env
  ) {
    (void)selectedBar;
    (void)selectedMonitorOverride;
    using i18n::tr;
    std::vector<SettingEntry> entries;

    // GNIL exposes the compact Ling Settings/Style model.  The runtime paths
    // below are adapted by settings_document.cpp; keeping the renderer-facing
    // Config type here avoids duplicating state throughout every panel.
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "theme", "Theme mode", "Choose the light or dark appearance.",
        {"theme", "mode"}, asSegmented(enumSelect(kThemeModes, cfg.theme.mode)), "appearance light dark"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "theme", "Dynamic colours",
        "Generate colours from the committed wallpaper.", {"theme", "source"},
        asSegmented(SelectSetting{
            .options = {{.value = "builtin", .label = "Static"}, {.value = "wallpaper", .label = "Dynamic"}},
            .selectedValue = cfg.theme.source == PaletteSource::Wallpaper ? "wallpaper" : "builtin",
        }),
        "wallpaper palette dynamic"
    ));
    if (cfg.theme.source == PaletteSource::Wallpaper) {
      std::vector<SelectOption> liveOutputOptions = {
          {.value = "auto", .label = "Automatic", .description = "Prefer a global live wallpaper assignment."},
      };
      for (const auto& output : env.availableOutputs) {
        liveOutputOptions.push_back({.value = output.value, .label = output.label, .description = output.description});
      }
      entries.push_back(makeEntry(
          SettingsSection::Appearance, "theme", "Live wallpaper source",
          "Choose which monitor supplies stable dynamic colours for video wallpaper.",
          {"theme", "live_wallpaper_output"},
          SelectSetting{.options = std::move(liveOutputOptions), .selectedValue = cfg.theme.liveWallpaperOutput},
          "wallpaper video monitor palette"
      ));
    }
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "font", "Interface font", "Font used by the shell and bar.",
        {"shell", "font_family"}, TextSetting{.value = cfg.shell.fontFamily, .placeholder = "Rubik"}, "font sans"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Appearance, "profile", "Avatar image", "Image shown for the current user.",
        {"shell", "avatar_path"},
        TextSetting{
            .value = cfg.shell.avatarPath,
            .placeholder = "",
            .browseMode = TextSettingBrowseMode::OpenFile,
            .browseFileExtensions = {".png", ".jpg", ".jpeg", ".webp"},
        },
        "profile user image"
    ));

    entries.push_back(makeEntry(
        SettingsSection::Panels, "frame", "Frame thickness",
        "The desktop frame thickness; the expanded left bar replaces this edge.",
        {"shell", "chrome", "frame_thickness"},
        SliderSetting{cfg.shell.chrome.frameThickness, 0.0, 32.0, 1.0, false}, "frame border bar thickness"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Panels, "frame", "Corner radius",
        "Radius shared by the frame, bar aperture and attached panels.", {"shell", "chrome", "rounding"},
        SliderSetting{cfg.shell.chrome.rounding, 0.0, 64.0, 1.0, false}, "round frame radius"
    ));

    struct PanelSizeCatalogEntry {
      std::string_view id;
      std::string_view label;
      int naturalWidth;
    };
    constexpr std::array panelSizeCatalog = {
        PanelSizeCatalogEntry{"media", "Media", 360},
        PanelSizeCatalogEntry{"audio", "Audio", 400},
        PanelSizeCatalogEntry{"brightness", "Brightness", 400},
        PanelSizeCatalogEntry{"night-light", "Night Light", 440},
        PanelSizeCatalogEntry{"system", "System monitor", 480},
        PanelSizeCatalogEntry{"battery", "Battery", 400},
        PanelSizeCatalogEntry{"network", "Network", 620},
        PanelSizeCatalogEntry{"bluetooth", "Bluetooth", 480},
        PanelSizeCatalogEntry{"weather", "Weather", 480},
        PanelSizeCatalogEntry{"calendar", "Calendar", 360},
        PanelSizeCatalogEntry{"screen-time", "Screen time", 520},
        PanelSizeCatalogEntry{"launcher", "Launcher", 630},
        PanelSizeCatalogEntry{"wallpaper", "Wallpaper picker", 360},
        PanelSizeCatalogEntry{"clipboard", "Clipboard", 720},
        PanelSizeCatalogEntry{"sidebar", "Notification sidebar", 420},
        PanelSizeCatalogEntry{"settings", "Settings", 1120},
        PanelSizeCatalogEntry{"session", "Session", 420},
        PanelSizeCatalogEntry{"tray-drawer", "Tray drawer", 360},
        PanelSizeCatalogEntry{"tray-menu", "Tray menu", 300},
    };
    for (const auto& panel : panelSizeCatalog) {
      const auto configured = std::ranges::find(
          cfg.shell.panel.sizes, panel.id, &ShellConfig::PanelConfig::SizeOverride::id
      );
      const std::optional<int> width = configured != cfg.shell.panel.sizes.end() && configured->width.has_value()
          ? std::optional<int>{*configured->width}
          : std::nullopt;
      entries.push_back(makeEntry(
          SettingsSection::Panels, "panel-sizes", std::string(panel.label) + " width",
          "Auto follows the panel's natural layout; Custom is stored in logical pixels.",
          {"shell", "panel", "size", std::string(panel.id), "width"},
          OptionalStepperSetting{
              .value = width,
              .minValue = 240,
              .maxValue = 2400,
              .step = 10,
              .fallbackValue = panel.naturalWidth,
              .unsetLabel = "Auto",
              .customLabel = "Custom",
          },
          std::string(panel.id) + " panel width size auto custom"
      ));
    }

    // Dashboard — kept in its own settings section so the Panels page can
    // reveal it as one coherent room instead of scattering tab switches among
    // unrelated standalone-panel sizing controls.
    entries.push_back(makeEntry(
        SettingsSection::ControlCenter, "general", "Enabled",
        "Enable the top-centre dashboard.", {"dashboard", "enabled"},
        ToggleSetting{cfg.dashboard.enabled}, "dashboard top center"
    ));
    {
      auto e = makeEntry(
          SettingsSection::ControlCenter, "general", "Drag threshold",
          "Distance dragged from the top edge before the dashboard opens.",
          {"dashboard", "drag_threshold"},
          StepperSetting{.value = cfg.dashboard.dragThreshold, .minValue = 1, .maxValue = 500, .step = 5,
                         .valueSuffix = "px"},
          "dashboard touch swipe gesture"
      );
      e.visibleWhen = [](const Config& c) { return c.dashboard.enabled; };
      entries.push_back(std::move(e));
    }

    const auto addDashboardToggle = [&entries](std::string group, std::string label, std::vector<std::string> path,
                                                bool checked, std::string search) {
      auto e = makeEntry(
          SettingsSection::ControlCenter, std::move(group), std::move(label),
          "Show this room in the dashboard tab strip.", std::move(path), ToggleSetting{checked}, std::move(search)
      );
      e.visibleWhen = [](const Config& c) { return c.dashboard.enabled; };
      entries.push_back(std::move(e));
    };
    addDashboardToggle("tabs", "Dashboard", {"dashboard", "show_dashboard"}, cfg.dashboard.showDashboard, "home");
    addDashboardToggle("tabs", "Media", {"dashboard", "show_media"}, cfg.dashboard.showMedia, "music player");
    addDashboardToggle(
        "tabs", "Performance", {"dashboard", "show_performance"}, cfg.dashboard.showPerformance,
        "cpu gpu memory storage network"
    );
    addDashboardToggle("tabs", "Weather", {"dashboard", "show_weather"}, cfg.dashboard.showWeather, "forecast");

    {
      auto e = makeEntry(
          SettingsSection::ControlCenter, "media", "Synced lyrics",
          "Fetch and cache time-synchronised lyrics for the active MPRIS track.",
          {"dashboard", "media", "lyrics_enabled"}, ToggleSetting{cfg.dashboard.media.lyricsEnabled},
          "lyrics lrc lrclib"
      );
      e.visibleWhen = [](const Config& c) { return c.dashboard.enabled && c.dashboard.showMedia; };
      entries.push_back(std::move(e));
    }

    const auto addPerformanceToggle = [&entries](std::string label, std::string key, bool checked) {
      auto e = makeEntry(
          SettingsSection::ControlCenter, "performance", std::move(label),
          "Show this card in the Performance room.", {"dashboard", "performance", std::move(key)},
          ToggleSetting{checked}, "dashboard performance widget"
      );
      e.visibleWhen = [](const Config& c) { return c.dashboard.enabled && c.dashboard.showPerformance; };
      entries.push_back(std::move(e));
    };
    addPerformanceToggle("Battery", "show_battery", cfg.dashboard.performance.showBattery);
    addPerformanceToggle("GPU", "show_gpu", cfg.dashboard.performance.showGpu);
    addPerformanceToggle("CPU", "show_cpu", cfg.dashboard.performance.showCpu);
    addPerformanceToggle("Memory", "show_memory", cfg.dashboard.performance.showMemory);
    addPerformanceToggle("Storage", "show_storage", cfg.dashboard.performance.showStorage);
    addPerformanceToggle("Network", "show_network", cfg.dashboard.performance.showNetwork);

    const BarConfig* lingBar = selectedBar;
    if (lingBar == nullptr && !cfg.bars.empty()) {
      lingBar = &cfg.bars.front();
    }
    if (lingBar != nullptr) {
      std::vector<SelectOption> widgetOptions;
      widgetOptions.reserve(cfg.widgets.size());
      for (const auto& [name, _] : cfg.widgets) {
        widgetOptions.push_back(SelectOption{.value = name, .label = name});
      }
      std::ranges::sort(widgetOptions, {}, &SelectOption::label);
      const std::vector<std::string> root = {"bar", lingBar->name};
      const auto barPath = [&root](std::string field) {
        auto path = root;
        path.push_back(std::move(field));
        return path;
      };
      entries.push_back(makeEntry(
          SettingsSection::Bar, "visibility", "Auto-hide", "Collapse the bar back into the desktop frame.",
          barPath("auto_hide"), ToggleSetting{lingBar->autoHide}, "persistent hide reveal drag"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Bar, "visibility", "Reveal on hover",
          "When disabled, reveal the collapsed bar by dragging right from the frame.", barPath("show_on_hover"),
          ToggleSetting{lingBar->showOnHover}, "hover drag reveal"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Bar, "layout", "Content padding", "Space between widgets and the shared frame.",
          barPath("padding"), SliderSetting{lingBar->padding, 0.0, 32.0, 1.0, false}, "padding inset"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Bar, "layout", "Widget spacing", "Gap between adjacent widgets.",
          barPath("widget_spacing"), SliderSetting{lingBar->widgetSpacing, 0.0, 32.0, 1.0, false}, "gap spacing"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Bar, "widgets", "Top widgets", "Widgets placed from the top edge.", barPath("start"),
          ListSetting{.items = lingBar->startWidgets, .suggestedOptions = widgetOptions}, "widgets order top"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Bar, "widgets", "Centre widgets", "Widgets centred on the rail.", barPath("center"),
          ListSetting{.items = lingBar->centerWidgets, .suggestedOptions = widgetOptions}, "widgets order centre"
      ));
      entries.push_back(makeEntry(
          SettingsSection::Bar, "widgets", "Bottom widgets", "Widgets placed from the bottom edge.", barPath("end"),
          ListSetting{.items = lingBar->endWidgets, .suggestedOptions = widgetOptions}, "widgets order bottom"
      ));
    }



    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "transition", "Transition duration", "Duration of a committed wallpaper change.",
        {"wallpaper", "transition_duration"},
        SliderSetting{cfg.wallpaper.transitionDurationMs, 0.0, 5000.0, 50.0, false}, "animation milliseconds"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "transition", "Edge softness", "Softness at the transition boundary.",
        {"wallpaper", "edge_smoothness"}, SliderSetting{cfg.wallpaper.edgeSmoothness, 0.0, 1.0, 0.01, false},
        "animation edge"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "directories", tr("settings.schema.wallpaper.directory.label"),
        tr("settings.schema.wallpaper.directory.description"), {"wallpaper", "directory"},
        TextSetting{
            .value = cfg.wallpaper.directory,
            .placeholder = std::string(wallpaper::kDefaultWallpaperDirectory),
            .browseMode = TextSettingBrowseMode::SelectFolder,
            .browseFileExtensions = {}
        },
        "folder path"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "directories", tr("settings.schema.wallpaper.live-wallpaper-directory.label"),
        tr("settings.schema.wallpaper.live-wallpaper-directory.description"), {"wallpaper", "live_wallpaper_directory"},
        TextSetting{
            .value = cfg.wallpaper.liveWallpaperDirectory,
            .placeholder = std::string(wallpaper::kDefaultLiveWallpaperDirectory),
            .browseMode = TextSettingBrowseMode::SelectFolder,
            .browseFileExtensions = {}
        },
        "live wallpaper folder path"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "directories", tr("settings.schema.wallpaper.per-monitor-directories.label"),
        tr("settings.schema.wallpaper.per-monitor-directories.description"), {"wallpaper", "per_monitor_directories"},
        ToggleSetting{cfg.wallpaper.perMonitorDirectories}, "per display folder"
    ));
    const auto allVideo = [&cfg]() {
      VideoWallpaperOutput result;
      for (const auto& output : cfg.wallpaper.videoOutputs) {
        if (output.match == "all" || output.match == "*") {
          return output;
        }
      }
      return result;
    }();
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "live", "Enable live wallpaper",
        "Use the configured video instead of the static wallpaper on every output.",
        {"wallpaper", "video", "all", "enabled"}, ToggleSetting{allVideo.enabled},
        "live video wallpaper mpv mpvpaper"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "live", "Live wallpaper",
        "Play a local video on every output through the bundled mpvpaper integration.",
        {"wallpaper", "video", "all", "path"},
        TextSetting{
            .value = allVideo.path,
            .placeholder = "Choose a video file",
            .browseMode = TextSettingBrowseMode::OpenFile,
            .browseFileExtensions = {".mp4", ".webm", ".mkv", ".mov", ".gif"},
        },
        "live video wallpaper mpv mpvpaper"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "live", "Mute live wallpaper", "Start live wallpapers without audio.",
        {"wallpaper", "video", "all", "mute"}, ToggleSetting{allVideo.mute}, "live video wallpaper audio"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "live", "Hardware decoding", "Use hardware video decoding when available.",
        {"wallpaper", "video", "all", "hardware_decode"}, ToggleSetting{allVideo.hardwareDecode},
        "live video wallpaper gpu"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Wallpaper, "live", "Pause when obscured",
        "Pause the player while the output is not visible to reduce GPU usage.",
        {"wallpaper", "video", "all", "auto_pause"}, ToggleSetting{allVideo.autoPause},
        "live video wallpaper performance"
    ));

    entries.push_back(makeEntry(
        SettingsSection::Launcher, "results", "Maximum results", "Maximum application results shown at once.",
        {"shell", "launcher", "max_shown"},
        SliderSetting{cfg.shell.launcher.maxShown, 1, 30, 1, true}, "launcher results"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Launcher, "wallpapers", "Wallpaper cards", "Maximum wallpaper cards in the launcher.",
        {"shell", "launcher", "max_wallpapers"},
        SliderSetting{cfg.shell.launcher.maxWallpapers, 1, 20, 1, true}, "launcher wallpaper carousel"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Launcher, "apps", "Hidden applications", "Application IDs omitted from search.",
        {"shell", "launcher", "hidden_apps"}, ListSetting{.items = cfg.shell.launcher.hiddenApps}, "launcher hidden"
    ));

    entries.push_back(makeEntry(
        SettingsSection::Notifications, "toasts", "Notification daemon", "Receive and display desktop notifications.",
        {"notification", "enable_daemon"}, ToggleSetting{cfg.notification.enableDaemon}, "notifications daemon"
    ));
    entries.push_back(makeEntry(
        SettingsSection::Notifications, "toasts", "Show actions", "Show notification action buttons when provided.",
        {"notification", "show_actions"}, ToggleSetting{cfg.notification.showActions}, "notification buttons"
    ));

    entries.push_back(makeEntry(
        SettingsSection::Security, "lock", "Fingerprint", "Allow fingerprint authentication on the lock screen.",
        {"lockscreen", "fingerprint"}, ToggleSetting{cfg.lockscreen.fingerprint}, "lock biometric"
    ));
    entries.push_back(makeEntry(
        SettingsSection::System, "brightness", "DDC monitor support", "Control supported external displays via DDC.",
        {"brightness", "enable_ddcutil"}, ToggleSetting{cfg.brightness.enableDdcutil}, "brightness external ddc"
    ));
    entries.push_back(makeEntry(
        SettingsSection::System, "monitor", "System monitor", "Collect the metrics used by performance widgets.",
        {"system", "monitor", "enabled"}, ToggleSetting{cfg.system.monitor.enabled}, "cpu memory gpu"
    ));

    return entries;

  }

} // namespace settings
