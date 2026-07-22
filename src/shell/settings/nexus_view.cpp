#include "shell/settings/nexus_view.h"

#include "compositors/compositor_detect.h"
#include "compositors/compositor_platform.h"
#include "config/atomic_file.h"
#include "config/config_service.h"
#include "core/build_info.h"
#include "core/deferred_call.h"
#include "dbus/bluetooth/bluetooth_agent.h"
#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/network/inetwork_service.h"
#include "i18n/i18n.h"
#include "pipewire/pipewire_service.h"
#include "render/animation/animation_manager.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/settings_registry.h"
#include "system/desktop_entry.h"
#include "system/distro_info.h"
#include "system/hardware_info.h"
#include "theme/scheme.h"
#include "ui/builders.h"
#include "ui/controls/slider.h"
#include "ui/controls/search_picker.h"
#include "ui/controls/roving_list_nav.h"
#include "ui/controls/toggle.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/palette.h"
#include "ui/scroll_into_view.h"
#include "ui/style.h"
#include "util/file_utils.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <charconv>
#include <cstdlib>
#include <set>
#include <string>
#include <utility>

namespace {
  struct ControlDescriptor {
    NexusPage page;
    std::string_view id;
    std::string_view title;
    std::string_view description;
  };

  struct GroupPresentation {
    NexusPage page;
    std::string_view id;
    std::string_view titleKey;
    std::string_view glyph;
  };

  constexpr auto kGroupPresentations = std::to_array<GroupPresentation>({
      {NexusPage::WallpaperStyle, "theme", "settings.nexus.groups.theme", "palette"},
      {NexusPage::WallpaperStyle, "font", "settings.nexus.groups.font", "text_fields"},
      {NexusPage::WallpaperStyle, "profile", "settings.nexus.groups.profile", "person"},
      {NexusPage::WallpaperStyle, "transition", "settings.nexus.groups.transition", "animation"},
      {NexusPage::WallpaperStyle, "directories", "settings.nexus.groups.directories", "folder"},
      {NexusPage::WallpaperStyle, "live", "settings.nexus.groups.live", "motion_photos_on"},
      {NexusPage::Panels, "frame", "settings.nexus.groups.frame", "crop_free"},
      {NexusPage::Panels, "panel-sizes", "settings.nexus.groups.panel-sizes", "aspect_ratio"},
      {NexusPage::Panels, "visibility", "settings.nexus.groups.visibility", "visibility"},
      {NexusPage::Panels, "layout", "settings.nexus.groups.layout", "dashboard"},
      {NexusPage::Panels, "widgets", "settings.nexus.groups.widgets", "widgets"},
      {NexusPage::Panels, "general", "settings.nexus.groups.general", "tune"},
      {NexusPage::Panels, "tabs", "settings.nexus.groups.tabs", "tab"},
      {NexusPage::Panels, "media", "settings.nexus.groups.media", "music-note"},
      {NexusPage::Panels, "performance", "settings.nexus.groups.performance", "speed"},
      {NexusPage::Apps, "pinned-apps", "settings.nexus.groups.pinned-apps", "push_pin"},
      {NexusPage::Apps, "behavior", "settings.nexus.groups.behavior", "tune"},
      {NexusPage::Apps, "keybinds", "settings.nexus.groups.keybinds", "keyboard"},
      {NexusPage::Services, "clipboard", "settings.nexus.groups.clipboard", "clipboard"},
      {NexusPage::Services, "network", "settings.nexus.groups.network", "shield"},
      {NexusPage::Services, "privacy", "settings.nexus.groups.privacy", "shield-lock"},
      {NexusPage::Services, "toasts", "settings.nexus.groups.notifications", "bell"},
      {NexusPage::LanguageRegion, "time", "settings.nexus.groups.time", "clock"},
      {NexusPage::LanguageRegion, "location", "settings.nexus.groups.location", "map-pin"},
      {NexusPage::LanguageRegion, "weather", "settings.nexus.groups.weather", "weather-sun"},
      {NexusPage::LanguageRegion, "night-light", "settings.nexus.groups.night-light", "weather-moon"},
      {NexusPage::About, "system", "settings.nexus.groups.system", "monitor"},
      {NexusPage::About, "security", "settings.nexus.groups.security", "shield-lock"},
  });

  std::string humanizeGroupId(std::string_view id) {
    std::string title(id);
    std::ranges::replace(title, '-', ' ');
    if (!title.empty()) {
      title.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(title.front())));
    }
    return title;
  }

  std::pair<std::string, std::string> groupPresentation(NexusPage page, std::string_view id) {
    const auto found = std::ranges::find_if(kGroupPresentations, [page, id](const GroupPresentation& item) {
      return item.page == page && item.id == id;
    });
    if (found == kGroupPresentations.end()) {
      return {humanizeGroupId(id), "tune"};
    }
    return {i18n::tr(found->titleKey), std::string(found->glyph)};
  }

  std::string pageTitle(const NexusPageDescriptor& descriptor) { return i18n::tr(descriptor.titleKey); }

  constexpr auto kControls = std::to_array<ControlDescriptor>({
      {NexusPage::WallpaperStyle, "wallpaper", "Wallpaper browser", "Search, sort, favourite and apply wallpapers"},
      {NexusPage::WallpaperStyle, "output", "Output", "Choose which monitor the wallpaper applies to"},
      {NexusPage::WallpaperStyle, "wallpapers-per-row", "Wallpaper columns", "Thumbnail columns in the wallpaper browser"},
      {NexusPage::WallpaperStyle, "scheme", "Color scheme", "Builtin, wallpaper and custom palettes"},
      {NexusPage::WallpaperStyle, "variant", "Material variant", "Nine Material 3 generation styles"},
      {NexusPage::WallpaperStyle, "typography", "Typography", "Font family and interface text sizing"},
      {NexusPage::WallpaperStyle, "appearance", "Interface appearance", "Scale, contrast, corners, borders and motion"},
      {NexusPage::WallpaperStyle, "automation", "Wallpaper automation", "Fill, transition, interval and ordering"},
      {NexusPage::Network, "wifi", "Wi-Fi", "Scan, connect and manage saved networks"},
      {NexusPage::Network, "ethernet", "Ethernet", "Wired connection and device state"},
      {NexusPage::Network, "rescan", "Automatic rescan", "Refresh nearby networks every 15 seconds"},
      {NexusPage::Network, "vpn", "VPN", "Active and saved VPN connections"},
      {NexusPage::ConnectedDevices, "bluetooth", "Bluetooth", "Adapter power and visibility"},
      {NexusPage::ConnectedDevices, "discoverable", "Discoverable", "Allow nearby devices to find this computer"},
      {NexusPage::ConnectedDevices, "pair", "Pair a new device", "Discover, pair and trust nearby devices"},
      {NexusPage::ConnectedDevices, "devices", "Known devices", "Connected, paired and remembered devices"},
      {NexusPage::Audio, "output", "Output", "Default sink and output volume"},
      {NexusPage::Audio, "input", "Input", "Default source and microphone volume"},
      {NexusPage::Audio, "overdrive", "Overdrive", "Allow volume above 100 percent"},
      {NexusPage::Audio, "devices", "Audio devices", "Output and input device volumes and mute state"},
      {NexusPage::Audio, "applications", "Applications", "Per-application stream volume"},
      {NexusPage::Panels, "rail", "Rail", "Panel placement and visible items"},
      {NexusPage::Panels, "frame", "Frame", "Thickness and corner rounding shared with the bar"},
      {NexusPage::Panels, "panel-sizes", "Panel widths", "Auto or custom width; height follows panel content"},
      {NexusPage::Panels, "bar-widgets", "Bar widgets", "Add, reorder and configure widgets in each bar lane"},
      {NexusPage::Panels, "workspace", "Workspace widget", "Display, windows and icon limits"},
      {NexusPage::Panels, "notifications", "Notifications", "Toast and history surfaces"},
      {NexusPage::Apps, "defaults", "Default apps", "Terminal, audio, media and file manager"},
      {NexusPage::Apps, "favourites", "Favourite apps", "Prioritize apps in the launcher"},
      {NexusPage::Apps, "hidden", "Hidden apps", "Remove apps from launcher results"},
      {NexusPage::Apps, "launcher", "Launcher behavior", "Results, layout, Vim keys and actions"},
      {NexusPage::Apps, "reset-usage", "Reset usage", "Clear launcher frequency data"},
      {NexusPage::Services, "notifications", "Notification service", "Daemon and delivery preferences"},
      {NexusPage::Services, "clipboard", "Clipboard", "History, limits and confirmation behavior"},
      {NexusPage::Services, "clear-clipboard", "Clear clipboard history", "Remove saved clipboard entries"},
      {NexusPage::Services, "offline", "Offline mode", "Disable network-backed shell services"},
      {NexusPage::Services, "network-privacy", "Network privacy", "External network lookups"},
      {NexusPage::LanguageRegion, "language", "Language", "GNIL currently uses English UI"},
      {NexusPage::LanguageRegion, "time", "Time format", "Clock and date formatting"},
      {NexusPage::LanguageRegion, "location", "Region", "Location-backed schedules and weather"},
      {NexusPage::LanguageRegion, "weather", "Weather", "Units, effects and refresh interval"},
      {NexusPage::LanguageRegion, "night-light", "Night light", "Schedule and day/night temperatures"},
      {NexusPage::About, "system", "System information", "Operating system and hardware"},
      {NexusPage::About, "shell", "GNIL", "Native C++23 Wayland shell"},
      {NexusPage::About, "support", "Support information", "Diagnostics and configuration paths"},
  });

  std::string pathKey(const std::vector<std::string>& path) {
    std::string key;
    for (const auto& part : path) {
      if (!key.empty()) {
        key += '.';
      }
      key += part;
    }
    return key;
  }

  std::optional<NexusPage> pageForEntry(const settings::SettingEntry& entry) {
    using settings::SettingsSection;
    switch (entry.section) {
    case SettingsSection::Appearance:
    case SettingsSection::Wallpaper:
      return NexusPage::WallpaperStyle;
    case SettingsSection::Panels:
    case SettingsSection::Bar:
    case SettingsSection::Desktop:
    case SettingsSection::Dock:
    case SettingsSection::Osd:
      return NexusPage::Panels;
    case SettingsSection::Launcher:
      return NexusPage::Apps;
    case SettingsSection::Location:
      return NexusPage::LanguageRegion;
    case SettingsSection::Notifications:
      return NexusPage::Services;
    case SettingsSection::Services:
      return entry.group == "audio" ? NexusPage::Audio : NexusPage::Services;
    case SettingsSection::Security:
      if (!entry.path.empty() && entry.path.front() == "shell"
          && (entry.group == "network" || entry.group == "privacy")) {
        return NexusPage::Services;
      }
      return NexusPage::About;
    case SettingsSection::Shell:
      if (entry.group == "clipboard") {
        return NexusPage::Services;
      }
      if (entry.path == std::vector<std::string>{"shell", "time_format"}
          || entry.path == std::vector<std::string>{"shell", "date_format"}) {
        return NexusPage::LanguageRegion;
      }
      return NexusPage::Panels;
    case SettingsSection::ControlCenter:
      return NexusPage::Panels;
    case SettingsSection::Hooks:
    case SettingsSection::Keybinds:
      return NexusPage::Services;
    case SettingsSection::Niri:
      return entry.group == "backdrop" ? NexusPage::WallpaperStyle : NexusPage::Services;
    case SettingsSection::System:
    case SettingsSection::Power:
      return NexusPage::About;
    }
    return std::nullopt;
  }

  settings::SettingsSection representativeSection(NexusPage page) {
    using settings::SettingsSection;
    switch (page) {
    case NexusPage::WallpaperStyle:
      return SettingsSection::Appearance;
    case NexusPage::Panels:
      return SettingsSection::Panels;
    case NexusPage::Apps:
      return SettingsSection::Launcher;
    case NexusPage::LanguageRegion:
      return SettingsSection::Location;
    case NexusPage::Audio:
    case NexusPage::Services:
    case NexusPage::Network:
      return SettingsSection::Services;
    case NexusPage::ConnectedDevices:
    case NexusPage::About:
      return SettingsSection::System;
    }
    return SettingsSection::Services;
  }

  settings::SettingEntry customEntry(
      NexusPage page, std::string group, std::string title, std::string subtitle, std::vector<std::string> path,
      settings::SettingControl control, std::string searchText = {}
  ) {
    return settings::SettingEntry{
        .section = representativeSection(page),
        .group = std::move(group),
        .title = std::move(title),
        .subtitle = std::move(subtitle),
        .path = std::move(path),
        .control = std::move(control),
        .advanced = false,
        .searchText = std::move(searchText),
    };
  }

  std::vector<settings::SettingEntry> nexusEntries(const Config& config, NexusPage page) {
    settings::RegistryEnvironment environment;
    environment.niriBackdropSupported = compositors::isNiri();
    environment.niriOverviewTypeToLaunchSupported = compositors::isNiri();
    auto registry = settings::buildSettingsRegistry(config, nullptr, nullptr, environment);
    std::vector<settings::SettingEntry> entries;
    entries.reserve(registry.size());
    for (auto& entry : registry) {
      const auto mapped = pageForEntry(entry);
      if (!mapped.has_value() || *mapped != page) {
        continue;
      }
      entry.section = representativeSection(page);
      entry.advanced = false;
      entries.push_back(std::move(entry));
    }

    using settings::ListSetting;
    using settings::SliderSetting;
    using settings::ToggleSetting;
    if (page == NexusPage::WallpaperStyle) {
      entries.push_back(customEntry(
          page, "wallpaper", "Wallpaper columns", "Number of thumbnails per row in the wallpaper browser",
          {"nexus", "wallpapers_per_row"},
          SliderSetting{config.nexus.wallpapersPerRow, 1, 12, 1, true}, "wallpaper thumbnail grid columns"
      ));
    } else if (page == NexusPage::Network) {
      entries.push_back(customEntry(
          page, "network", "Automatic rescan interval", "Scan only while this page is active and Wi-Fi is enabled",
          {"nexus", "network_rescan_interval_ms"},
          SliderSetting{config.nexus.networkRescanIntervalMs, 1000, 300000, 1000, true}, "wifi scan refresh interval"
      ));
    } else if (page == NexusPage::Apps) {
      const auto& launcher = config.shell.launcher;
      std::vector<std::string> visibleFavourites = launcher.favouriteApps;
      std::erase_if(visibleFavourites, [&launcher](const std::string& app) {
        return std::ranges::contains(launcher.hiddenApps, app);
      });
      entries.push_back(customEntry(
          page, "pinned-apps", "Favourite apps", "Apps pinned to the top of launcher results",
          {"shell", "launcher", "favourite_apps"}, ListSetting{.items = std::move(visibleFavourites)},
          "favorite pinned apps"
      ));
      entries.push_back(customEntry(
          page, "pinned-apps", "Hidden apps", "Apps excluded from launcher results; hidden always wins",
          {"shell", "launcher", "hidden_apps"}, ListSetting{.items = launcher.hiddenApps}, "ignored excluded apps"
      ));
      entries.push_back(customEntry(
          page, "behavior", "Launcher enabled", "Allow launcher panels and shortcuts",
          {"shell", "launcher", "enabled"}, ToggleSetting{launcher.enabled}
      ));
      entries.push_back(customEntry(
          page, "behavior", "Open on hover", "Reveal the launcher when hovering its rail trigger",
          {"shell", "launcher", "show_on_hover"}, ToggleSetting{launcher.showOnHover}
      ));
      entries.push_back(customEntry(
          page, "behavior", "Result limit", "Maximum regular results shown at once",
          {"shell", "launcher", "max_shown"}, SliderSetting{launcher.maxShown, 1, 50, 1, true}
      ));
      entries.push_back(customEntry(
          page, "behavior", "Wallpaper result limit", "Maximum wallpaper results shown at once",
          {"shell", "launcher", "max_wallpapers"}, SliderSetting{launcher.maxWallpapers, 1, 50, 1, true}
      ));
      entries.push_back(customEntry(
          page, "keybinds", "Vim navigation", "Use familiar Vim keys to move through results",
          {"shell", "launcher", "vim_keybinds"}, ToggleSetting{launcher.vimKeybinds}
      ));
      entries.push_back(customEntry(
          page, "keybinds", "Dangerous actions", "Include shutdown and destructive actions in search",
          {"shell", "launcher", "enable_dangerous_actions"}, ToggleSetting{launcher.enableDangerousActions}
      ));
    }
    return entries;
  }
} // namespace

NexusView::NexusView(NexusRoute& route, NexusServices services) : m_route(route), m_services(services) {}

std::unique_ptr<Node> NexusView::build(
    float scale, NexusNavigationMode navigationMode, std::function<void()> requestPip,
    std::function<void()> requestRefresh, std::function<void()> requestClose
) {
  (void)requestClose;
  m_scale = scale;
  m_navigationMode = navigationMode;
  m_aliveGuard = std::make_shared<int>(0);
  m_contentMutationPending = false;
  m_requestRefresh = std::move(requestRefresh);
  m_requestPip = std::move(requestPip);
  m_navButtons.clear();
  auto root = ui::row({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = 0.0f,
      .fill = colorSpecFromRole(ColorRole::Surface),
      .radius = Style::scaledRadiusXl(scale),
      .fillWidth = true,
      .fillHeight = true,
      .clipChildren = true,
  });

  constexpr float kCompactNavigationWidth = 80.0f;
  constexpr float kLabeledNavigationWidth = 224.0f;
  constexpr float kNavigationControlHeight = 44.0f;
  const bool labeledNavigation = navigationMode == NexusNavigationMode::LabeledSidebar;
  const float navigationWidth =
      (labeledNavigation ? kLabeledNavigationWidth : kCompactNavigationWidth) * scale;

  ScrollView* navigationScroll = nullptr;
  std::unique_ptr<ScrollView> navigationViewport;
  std::function<void(const Node*)> scrollNavigationIntoView;
  if (labeledNavigation) {
    navigationViewport = ui::scrollView({
        .out = &navigationScroll,
        .scrollbarVisible = true,
        .viewportPaddingH = 0.0f,
        .viewportPaddingV = 0.0f,
        .fill = colorSpecFromRole(ColorRole::Surface),
        .minWidth = navigationWidth,
        .fillHeight = true,
        .width = navigationWidth,
        .height = 0.0f,
        .configure = [](ScrollView& view) { view.clearBorder(); },
    });
    scrollNavigationIntoView = [navigationScroll, scale](const Node* node) {
      if (navigationScroll != nullptr && node != nullptr) {
        scrollNodeIntoScrollView(*navigationScroll, nullptr, *node, Style::spaceXs * scale);
      }
    };
  }

  auto nav = std::make_unique<RovingListNavHost>(RovingListNavController::Options{
      .axis = RovingListNavAxis::Vertical,
      .mode = RovingListNavMode::FollowFocus,
      .keepItemsInTabOrder = false,
      .wrap = false,
      .scrollIntoView = std::move(scrollNavigationIntoView),
      .syncIndexFromSelection = [this]() { return static_cast<std::size_t>(m_route.page()); },
  });
  nav->setTabFocusKey("settings.navigation");
  m_navigationFocus = nav->focusArea();
  nav->setGap(Style::spaceXs * scale);
  nav->setPadding(Style::spaceMd * scale, Style::spaceSm * scale);
  nav->setFill(colorSpecFromRole(ColorRole::Surface));
  nav->setMinWidth(navigationWidth);
  nav->setMaxWidth(navigationWidth);

  const auto requestHostHandoff = [this]() {
    if (m_requestPip) {
      m_requestPip();
    }
  };
  if (labeledNavigation) {
    const float buttonWidth =
        (kLabeledNavigationWidth - Style::spaceSm * 2.0f) * scale;
    nav->addChild(ui::button({
        .text = i18n::tr("settings.nexus.open-panel"),
        .glyph = "settings",
        .fontSize = Style::fontSizeCaption * scale,
        .glyphSize = Style::fontSizeTitle * scale,
        .controlHeight = kNavigationControlHeight * scale,
        .contentAlign = ButtonContentAlign::Start,
        .variant = ButtonVariant::Ghost,
        .tooltip = i18n::tr("settings.nexus.open-panel"),
        .minWidth = buttonWidth,
        .maxWidth = buttonWidth,
        .paddingH = Style::spaceSm * scale,
        .gap = Style::spaceSm * scale,
        .radius = Style::scaledRadiusLg(scale),
        .onClick = requestHostHandoff,
        .configure = [](Button& button) {
          if (button.label() != nullptr) {
            button.label()->setFontWeight(FontWeight::SemiBold);
          }
        },
    }));
  } else {
    nav->addChild(ui::button({
        .glyph = "settings",
        .glyphSize = Style::fontSizeTitle * scale,
        .variant = ButtonVariant::Ghost,
        .tooltip = i18n::tr("settings.nexus.open-window"),
        .minWidth = kNavigationControlHeight * scale,
        .minHeight = kNavigationControlHeight * scale,
        .padding = Style::spaceSm * scale,
        .radius = Style::scaledRadiusLg(scale),
        .onClick = requestHostHandoff,
    }));
  }
  nav->addChild(ui::box({
      .fill = colorSpecFromRole(ColorRole::Outline, 0.18f),
      .height = Style::borderWidth,
  }));

  const auto addNavItem = [this, nav = nav.get(), scale, labeledNavigation](NexusPage page) {
    const auto& descriptor = nexusPageDescriptor(page);
    const std::string title = pageTitle(descriptor);
    Button* button = nullptr;
    auto activate = [this, page = descriptor.page]() {
      m_route.setFocusKey("settings.navigation");
      selectPage(page);
    };
    if (labeledNavigation) {
      constexpr float kLabeledButtonWidth = kLabeledNavigationWidth - Style::spaceSm * 2.0f;
      nav->addChild(ui::button({
          .out = &button,
          .text = title,
          .glyph = std::string(descriptor.glyph),
          .fontSize = Style::fontSizeCaption * scale,
          .glyphSize = Style::fontSizeBody * scale,
          .controlHeight = kNavigationControlHeight * scale,
          .contentAlign = ButtonContentAlign::Start,
          .variant = descriptor.page == m_route.page() ? ButtonVariant::TabActive : ButtonVariant::Ghost,
          .tooltip = title,
          .minWidth = kLabeledButtonWidth * scale,
          .maxWidth = kLabeledButtonWidth * scale,
          .paddingH = Style::spaceSm * scale,
          .gap = Style::spaceSm * scale,
          .radius = Style::scaledRadiusLg(scale),
          .onClick = activate,
          .configure = [](Button& configuredButton) {
            if (configuredButton.label() != nullptr) {
              configuredButton.label()->setFontWeight(FontWeight::SemiBold);
            }
          },
      }));
    } else {
      nav->addChild(ui::button({
          .out = &button,
          .glyph = std::string(descriptor.glyph),
          .glyphSize = Style::fontSizeBody * scale,
          .variant = descriptor.page == m_route.page() ? ButtonVariant::TabActive : ButtonVariant::Ghost,
          .tooltip = title,
          .minWidth = kNavigationControlHeight * scale,
          .minHeight = kNavigationControlHeight * scale,
          .padding = Style::spaceSm * scale,
          .radius = Style::scaledRadiusLg(scale),
          .onClick = activate,
      }));
    }
    nav->registerItem(button, activate);
    m_navButtons.push_back(button);
  };

  for (const auto& descriptor : nexusPages()) {
    addNavItem(descriptor.page);
  }
  if (navigationViewport != nullptr) {
    navigationViewport->content()->setDirection(FlexDirection::Vertical);
    navigationViewport->content()->setAlign(FlexAlign::Stretch);
    navigationViewport->content()->addChild(std::move(nav));
    root->addChild(std::move(navigationViewport));
  } else {
    root->addChild(std::move(nav));
  }
  root->addChild(ui::box({
      .fill = colorSpecFromRole(ColorRole::Outline, 0.18f),
      .width = Style::borderWidth,
  }));

  auto workspace = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .padding = Style::spaceMd * scale,
      .fillWidth = true,
      .flexGrow = 1.0f,
  });

  auto searchRow = ui::row({
      .align = FlexAlign::Center,
      .fillWidth = true,
  });
  searchRow->addChild(ui::row({.flexGrow = 1.0f}));
  searchRow->addChild(ui::input({
      .out = &m_search,
      .value = m_route.query(),
      .placeholder = i18n::tr("settings.nexus.search-placeholder"),
      .fontSize = Style::fontSizeBody * scale,
      .controlHeight = Style::controlHeight * scale,
      .horizontalPadding = Style::spaceMd * scale,
      .clearButtonEnabled = true,
      .surfaceOpacity = 0.52f,
      .width = 300.0f * scale,
      .onChange = [this](const std::string& query) { search(query); },
      .configure = [this, scale](Input& input) {
        input.setFlatSurfaceFrame(true);
        input.setFrameRadius(Style::controlHeight * 0.5f * scale);
        input.setOnFocusGain([this]() { m_route.setFocusKey("settings.search"); });
        if (input.inputArea() != nullptr) {
          input.inputArea()->setTabFocusKey("settings.search");
        }
      },
  }));
  workspace->addChild(std::move(searchRow));

  auto pageFrame = ui::column({
      .align = FlexAlign::Stretch,
      .gap = 0.0f,
      .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.18f),
      .radius = Style::scaledRadiusXl(scale),
      .border = colorSpecFromRole(ColorRole::Outline, 0.28f),
      .borderWidth = Style::borderWidth,
      .fillWidth = true,
      .clipChildren = true,
      .flexGrow = 1.0f,
  });
  auto pageHeader = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .paddingV = Style::spaceMd * scale,
      .paddingH = Style::spaceLg * scale,
      .fillWidth = true,
  });
  pageHeader->addChild(ui::button({
      .out = &m_backButton,
      .glyph = "arrow-left",
      .glyphSize = Style::baseGlyphSize * scale,
      .variant = ButtonVariant::Ghost,
      .tooltip = i18n::tr("settings.nexus.back"),
      .minWidth = Style::controlHeightSm * scale,
      .minHeight = Style::controlHeightSm * scale,
      .padding = Style::spaceXs * scale,
      .radius = Style::scaledRadiusMd(scale),
      .visible = false,
      .participatesInLayout = false,
      .onClick = [this]() {
        if (m_route.popSubpage()) {
          rebuildContent();
          animateContentReveal();
        }
      },
  }));
  pageHeader->addChild(ui::label({
      .out = &m_title,
      .fontSize = Style::fontSizeHeader * scale,
      .fontWeight = FontWeight::Bold,
      .color = colorSpecFromRole(ColorRole::OnSurface),
      .flexGrow = 1.0f,
  }));
  pageHeader->addChild(ui::button({
      .out = &m_resetButton,
      .text = i18n::tr("settings.nexus.reset-page"),
      .glyph = "restart_alt",
      .fontSize = Style::fontSizeCaption * scale,
      .glyphSize = Style::baseGlyphSize * scale,
      .variant = ButtonVariant::Ghost,
      .minHeight = Style::controlHeightSm * scale,
      .paddingH = Style::spaceSm * scale,
      .radius = Style::scaledRadiusMd(scale),
      .onClick = [this]() {
        if (!m_resetConfirmationPending) {
          m_resetConfirmationPending = true;
          requestContentRefresh();
          return;
        }
        resetCurrentPage();
      },
  }));
  pageFrame->addChild(std::move(pageHeader));
  pageFrame->addChild(ui::box({
      .fill = colorSpecFromRole(ColorRole::Outline, 0.14f),
      .height = Style::borderWidth,
  }));

  auto scroll = ui::scrollView({
      .out = &m_scroll,
      .scrollbarVisible = true,
      .viewportPaddingH = Style::spaceLg * scale,
      .viewportPaddingV = Style::spaceMd * scale,
      .flexGrow = 1.0f,
      .configure = [this, scale](ScrollView& view) {
        view.clearFill();
        view.clearBorder();
        view.setOnScrollChanged([this](float offset) {
          if (m_route.query().empty()) {
            m_route.setScrollOffset(offset);
          }
        });
      },
  });
  m_content = scroll->content();
  m_content->setDirection(FlexDirection::Vertical);
  m_content->setAlign(FlexAlign::Stretch);
  m_content->setGap(Style::spaceMd * scale);
  pageFrame->addChild(std::move(scroll));
  workspace->addChild(std::move(pageFrame));
  root->addChild(std::move(workspace));
  rebuildContent();
  return root;
}

InputArea* NexusView::initialFocusArea() const noexcept {
  if (m_route.focusKey() == "settings.navigation" && m_navigationFocus != nullptr) {
    return m_navigationFocus;
  }
  return m_search != nullptr ? m_search->inputArea() : nullptr;
}

bool NexusView::pageAvailable(NexusPage page) const noexcept {
  switch (page) {
  case NexusPage::Network:
    return m_services.network != nullptr;
  case NexusPage::ConnectedDevices:
    return m_services.bluetooth != nullptr;
  case NexusPage::Audio:
    return m_services.audio != nullptr;
  case NexusPage::WallpaperStyle:
  case NexusPage::Panels:
  case NexusPage::Apps:
  case NexusPage::Services:
  case NexusPage::LanguageRegion:
    return m_services.config != nullptr;
  case NexusPage::About:
    return m_services.dependencies != nullptr || m_services.platform != nullptr;
  }
  return false;
}

void NexusView::selectPage(NexusPage page) {
  if (m_scroll != nullptr) {
    m_route.setScrollOffset(m_scroll->scrollOffset());
  }
  if (m_route.page() == page) {
    while (m_route.popSubpage()) {
    }
    m_route.setSelectedControl({});
    m_route.clearPendingControl();
    m_route.setSelectedEntity({});
  } else {
    m_route.setPage(page);
  }
  m_route.clearQuery();
  if (m_search != nullptr && !m_search->value().empty()) {
    m_search->setValue("");
  }
  m_resetConfirmationPending = false;
  syncNetworkRescan();
  for (std::size_t i = 0; i < m_navButtons.size(); ++i) {
    m_navButtons[i]->setVariant(nexusPages()[i].page == page ? ButtonVariant::TabActive : ButtonVariant::Ghost);
  }
  rebuildContent();
  animateContentReveal();
}

void NexusView::animateContentReveal() {
  if (m_scroll == nullptr) {
    return;
  }
  m_scroll->setOpacity(0.38f);
  m_scroll->setScale(0.992f);
  if (AnimationManager* animations = m_scroll->animationManager(); animations != nullptr) {
    (void)animations->animate(
        0.0f, 1.0f, Style::animNormal, Easing::FluidSpatial,
        [scroll = m_scroll](float progress) {
          scroll->setOpacity(0.38f + progress * 0.62f);
          scroll->setScale(0.992f + progress * 0.008f);
        },
        {}, m_scroll
    );
  } else {
    m_scroll->setOpacity(1.0f);
    m_scroll->setScale(1.0f);
  }
}

void NexusView::selectSearchResult(NexusPage page, std::string controlId) {
  if (m_scroll != nullptr) {
    m_route.setScrollOffset(m_scroll->scrollOffset());
  }
  m_route.setPage(page);
  if (m_services.config != nullptr) {
    const auto entries = nexusEntries(m_services.config->config(), page);
    const auto target = std::ranges::find_if(entries, [&controlId](const settings::SettingEntry& entry) {
      return pathKey(entry.path) == controlId;
    });
    if (target != entries.end() && !target->group.empty()) {
      auto [title, glyph] = groupPresentation(page, target->group);
      (void)glyph;
      m_route.pushSubpage(NexusSubpage{.id = target->group, .title = std::move(title)});
    }
  }
  m_route.setSelectedControl(controlId);
  m_route.setPendingControl(std::move(controlId));
  m_route.clearQuery();
  if (m_search != nullptr) {
    m_search->setValue("");
  }
  for (std::size_t i = 0; i < m_navButtons.size(); ++i) {
    m_navButtons[i]->setVariant(nexusPages()[i].page == page ? ButtonVariant::TabActive : ButtonVariant::Ghost);
  }
  rebuildContent();
  animateContentReveal();
}

void NexusView::deferContentMutation(std::function<void()> mutation) {
  if (m_contentMutationPending || !mutation) {
    return;
  }
  m_contentMutationPending = true;
  const std::weak_ptr<void> aliveGuard = m_aliveGuard;
  DeferredCall::callLater([this, aliveGuard, mutation = std::move(mutation)]() mutable {
    if (aliveGuard.expired()) {
      return;
    }
    m_contentMutationPending = false;
    mutation();
  });
}

void NexusView::requestContentRefresh() {
  m_contentRefreshPending = true;
  if (m_requestRefresh) {
    m_requestRefresh();
  }
}

bool NexusView::contentHasActiveSliderDrag() const {
  const auto containsDrag = [](const auto& self, const Node* node) -> bool {
    if (node == nullptr) {
      return false;
    }
    if (const auto* slider = dynamic_cast<const Slider*>(node); slider != nullptr && slider->dragging()) {
      return true;
    }
    return std::ranges::any_of(node->children(), [&self](const auto& child) { return self(self, child.get()); });
  };
  return containsDrag(containsDrag, m_content);
}

void NexusView::refresh() {
  if (contentHasActiveSliderDrag()) {
    m_deferredRefreshTimer.start(std::chrono::milliseconds(80), [this]() { refresh(); });
    return;
  }
  requestContentRefresh();
}

void NexusView::setActive(bool active) {
  m_active = active;
  if (!m_active) {
    m_deferredRefreshTimer.stop();
  }
  syncNetworkRescan();
}

void NexusView::cancelTransientPrompts() {
  if (m_services.bluetoothAgent != nullptr && m_services.bluetoothAgent->hasPendingRequest()) {
    m_services.bluetoothAgent->cancelPending();
  }
  m_route.setSelectedEntity({});
  m_pendingForgetEntity.clear();
  m_resetConfirmationPending = false;
  m_clipboardClearConfirmationPending = false;
  m_launcherResetConfirmationPending = false;
}

void NexusView::syncNetworkRescan() {
  m_networkRescanTimer.stop();
  if (!m_active || m_route.page() != NexusPage::Network || m_services.network == nullptr
      || m_services.config == nullptr || !m_services.network->state().wirelessEnabled) {
    return;
  }
  const auto interval = std::chrono::milliseconds(
      std::max(1000, m_services.config->config().nexus.networkRescanIntervalMs)
  );
  m_networkRescanTimer.startRepeating(interval, [this]() {
    if (m_active && m_route.page() == NexusPage::Network && m_services.network != nullptr
        && m_services.network->state().wirelessEnabled) {
      m_services.network->requestScan();
    }
  });
}

void NexusView::showError(std::string message) {
  m_errorMessage = std::move(message);
  if (m_errorLabel != nullptr) {
    m_errorLabel->setText(m_errorMessage);
    m_errorLabel->setVisible(!m_errorMessage.empty());
    m_errorLabel->setParticipatesInLayout(!m_errorMessage.empty());
  } else {
    requestContentRefresh();
  }
}

void NexusView::addControlRow(std::string title, std::string description, bool available, std::string controlId) {
  if (m_content == nullptr) {
    return;
  }
  const bool highlighted = !controlId.empty() && controlId == m_route.pendingControl();
  const std::string actionValue = description;
  auto row = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceMd * m_scale,
      .padding = Style::spaceMd * m_scale,
      .fill = highlighted ? colorSpecFromRole(ColorRole::Primary, 0.18f)
                          : colorSpecFromRole(ColorRole::Surface, 0.70f),
      .radius = Style::scaledRadiusXl(m_scale),
      .border = highlighted ? colorSpecFromRole(ColorRole::Primary, 0.46f)
                            : colorSpecFromRole(ColorRole::Outline, 0.22f),
      .borderWidth = Style::borderWidth,
      .minHeight = 66.0f * m_scale,
      .fillWidth = true,
  });
  if (highlighted) {
    m_pendingScrollTarget = row.get();
  }
  Label* detailLabel = nullptr;
  row->addChild(ui::column(
      {.align = FlexAlign::Center,
       .justify = FlexJustify::Center,
       .fill = colorSpecFromRole(
           available ? ColorRole::Primary : ColorRole::SurfaceVariant, available ? 0.09f : 0.45f
       ),
       .radius = 20.0f * m_scale,
       .width = 40.0f * m_scale,
       .height = 40.0f * m_scale},
      ui::glyph({
          .glyph = available ? "check_circle" : "block",
          .glyphSize = Style::baseGlyphSize * m_scale,
          .color = colorSpecFromRole(
              available ? ColorRole::Primary : ColorRole::OnSurfaceVariant, available ? 1.0f : 0.5f
          ),
      })
  ));
  row->addChild(
      ui::column(
          {.align = FlexAlign::Start, .gap = 2.0f * m_scale, .flexGrow = 1.0f},
          ui::label({
              .text = std::move(title),
              .fontSize = Style::fontSizeBody * m_scale,
              .fontWeight = FontWeight::SemiBold,
              .color = colorSpecFromRole(ColorRole::OnSurface, available ? 1.0f : 0.5f),
          }),
          ui::label({
              .out = &detailLabel,
              .text = available ? std::move(description) : "Unavailable on this system",
              .fontSize = Style::fontSizeCaption * m_scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant, available ? 1.0f : 0.5f),
          })
      )
  );
  bool hasTrailingControl = false;
  if (controlId == "wifi" && m_services.network != nullptr) {
    const auto& state = m_services.network->state();
    row->addChild(
        ui::toggle({
            .out = &m_wifiToggle,
            .checked = state.wirelessEnabled,
            .enabled = m_services.network->hasStateSnapshot(),
            .scale = m_scale,
            .onChange = [this, network = m_services.network](bool enabled) {
              network->setWirelessEnabled(enabled);
              if (enabled) {
                m_networkRescanTimer.start(std::chrono::milliseconds(1000), [this]() { syncNetworkRescan(); });
              } else {
                syncNetworkRescan();
              }
            },
        })
    );
    hasTrailingControl = true;
  } else if (controlId == "rescan" && m_services.network != nullptr) {
    row->addChild(
        ui::button({
            .text = "Scan now",
            .enabled = m_services.network->hasStateSnapshot(),
            .variant = ButtonVariant::Ghost,
            .minHeight = Style::controlHeightSm * m_scale,
            .paddingH = Style::spaceSm * m_scale,
            .radius = Style::scaledRadiusMd(m_scale),
            .onClick = [network = m_services.network]() { network->requestScan(); },
        })
    );
    hasTrailingControl = true;
  } else if (controlId == "ethernet" && m_services.network != nullptr) {
    const bool canActivate = m_services.network->canActivateWiredConnection();
    row->addChild(
        ui::button({
            .text = m_services.network->state().kind == NetworkConnectivity::Wired ? "Connected" : "Connect",
            .enabled = canActivate,
            .variant = ButtonVariant::Ghost,
            .minHeight = Style::controlHeightSm * m_scale,
            .paddingH = Style::spaceSm * m_scale,
            .radius = Style::scaledRadiusMd(m_scale),
            .onClick = [network = m_services.network]() { (void)network->activateWiredConnection(); },
        })
    );
    hasTrailingControl = true;
  } else if (controlId == "bluetooth" && m_services.bluetooth != nullptr) {
    const auto& state = m_services.bluetooth->state();
    row->addChild(
        ui::toggle({
            .out = &m_bluetoothToggle,
            .checked = state.powered,
            .enabled = state.adapterPresent && !state.rfkillHardBlocked,
            .scale = m_scale,
            .onChange = [bluetooth = m_services.bluetooth](bool enabled) { bluetooth->setPowered(enabled); },
        })
    );
    hasTrailingControl = true;
  } else if (controlId == "discoverable" && m_services.bluetooth != nullptr) {
    const auto& state = m_services.bluetooth->state();
    row->addChild(
        ui::toggle({
            .out = &m_discoverableToggle,
            .checked = state.discoverable,
            .enabled = state.adapterPresent && state.powered,
            .scale = m_scale,
            .onChange = [bluetooth = m_services.bluetooth](bool enabled) {
              bluetooth->setDiscoverable(enabled);
              bluetooth->setPairable(enabled);
            },
        })
    );
    hasTrailingControl = true;
  } else if (controlId == "pair" && m_services.bluetooth != nullptr) {
    const bool discovering = m_services.bluetooth->state().discovering;
    row->addChild(
        ui::button({
            .text = discovering ? "Stop" : "Discover",
            .enabled = m_services.bluetooth->state().powered,
            .variant = ButtonVariant::Ghost,
            .minHeight = Style::controlHeightSm * m_scale,
            .paddingH = Style::spaceSm * m_scale,
            .radius = Style::scaledRadiusMd(m_scale),
            .onClick = [bluetooth = m_services.bluetooth, discovering]() {
              if (discovering) {
                bluetooth->stopDiscovery();
              } else {
                bluetooth->startDiscovery();
              }
            },
        })
    );
    hasTrailingControl = true;
  } else if (controlId == "clear-clipboard" && m_services.clipboard != nullptr) {
    row->addChild(
        ui::button({
            .text = m_clipboardClearConfirmationPending ? "Confirm clear" : "Clear history",
            .glyph = m_clipboardClearConfirmationPending ? "warning" : "delete",
            .variant = ButtonVariant::Destructive,
            .minHeight = Style::controlHeightSm * m_scale,
            .paddingH = Style::spaceSm * m_scale,
            .radius = Style::scaledRadiusMd(m_scale),
            .onClick = [this]() {
              if (!m_clipboardClearConfirmationPending) {
                m_clipboardClearConfirmationPending = true;
                requestContentRefresh();
                return;
              }
              m_services.clipboard->clearHistory();
              m_clipboardClearConfirmationPending = false;
              requestContentRefresh();
            },
        })
    );
    hasTrailingControl = true;
  } else if (controlId == "reset-usage" && m_services.resetLauncherUsage) {
    row->addChild(ui::button({
        .text = m_launcherResetConfirmationPending ? "Confirm reset" : "Reset usage",
        .glyph = m_launcherResetConfirmationPending ? "warning" : "restart_alt",
        .variant = m_launcherResetConfirmationPending ? ButtonVariant::Destructive : ButtonVariant::Ghost,
        .onClick = [this]() {
          if (!m_launcherResetConfirmationPending) {
            m_launcherResetConfirmationPending = true;
            requestContentRefresh();
            return;
          }
          m_services.resetLauncherUsage();
          m_launcherResetConfirmationPending = false;
          showError("");
          requestContentRefresh();
        },
    }));
    hasTrailingControl = true;
  } else if (controlId.starts_with("support.") && m_services.clipboard != nullptr) {
    const std::string copyValue = controlId == "support.diagnostics" && m_services.config != nullptr
        ? m_services.config->buildSupportReport()
        : actionValue;
    row->addChild(ui::button({
        .text = "Copy",
        .glyph = "content_copy",
        .variant = ButtonVariant::Ghost,
        .onClick = [this, value = std::move(copyValue)]() {
          if (!m_services.clipboard->copyText(value)) {
            showError("The value could not be copied.");
            return;
          }
          showError("");
        },
    }));
    hasTrailingControl = true;
  } else if (controlId == "output" && m_services.audio != nullptr) {
    const AudioNode* sink = m_services.audio->defaultSink();
    row->addChild(
        ui::slider({
            .out = &m_outputSlider,
            .minValue = 0.0,
            .maxValue = 1.5,
            .step = 0.01,
            .value = sink != nullptr ? static_cast<double>(sink->volume) : 0.0,
            .enabled = sink != nullptr,
            .presentation = SliderPresentation::LevelCompact,
            .trackHeight = 18.0f * m_scale,
            .thumbSize = 33.0f * m_scale,
            .controlHeight = 33.0f * m_scale,
            .width = 180.0f * m_scale,
            .onValueChanged = [audio = m_services.audio](double volume) {
              audio->setVolume(static_cast<float>(volume));
            },
        })
    );
    hasTrailingControl = true;
  } else if (controlId == "input" && m_services.audio != nullptr) {
    const AudioNode* source = m_services.audio->defaultSource();
    row->addChild(
        ui::slider({
            .out = &m_inputSlider,
            .minValue = 0.0,
            .maxValue = 1.5,
            .step = 0.01,
            .value = source != nullptr ? static_cast<double>(source->volume) : 0.0,
            .enabled = source != nullptr,
            .presentation = SliderPresentation::LevelCompact,
            .trackHeight = 18.0f * m_scale,
            .thumbSize = 33.0f * m_scale,
            .controlHeight = 33.0f * m_scale,
            .width = 180.0f * m_scale,
            .onValueChanged = [audio = m_services.audio](double volume) {
              audio->setMicVolume(static_cast<float>(volume));
            },
        })
    );
    hasTrailingControl = true;
  } else if (controlId == "scheme" && m_services.config != nullptr) {
    const auto mode = m_services.config->config().theme.mode;
    row->addChild(
        ui::button({
            .text = mode == ThemeMode::Light ? "Light"
                : mode == ThemeMode::Dark    ? "Dark"
                                             : "Auto",
            .enabled = true,
            .variant = ButtonVariant::Ghost,
            .minHeight = Style::controlHeightSm * m_scale,
            .paddingH = Style::spaceSm * m_scale,
            .radius = Style::scaledRadiusMd(m_scale),
            .onClick = [config = m_services.config, mode]() {
              config->setThemeMode(
                  mode == ThemeMode::Dark        ? ThemeMode::Light
                      : mode == ThemeMode::Light ? ThemeMode::Auto
                                                 : ThemeMode::Dark
              );
            },
        })
    );
    hasTrailingControl = true;
  } else if (controlId == "variant" && m_services.config != nullptr) {
    static constexpr std::array<std::string_view, 9> kVariants = {
        "vibrant",        "m3-tonal-spot", "m3-expressive", "m3-fidelity",   "m3-content",
        "m3-fruit-salad", "m3-rainbow",    "m3-neutral",    "m3-monochrome",
    };
    const std::string current = m_services.config->config().theme.wallpaperScheme;
    const auto found = std::ranges::find(kVariants, current);
    const std::size_t next = found == kVariants.end()
        ? 0
        : (static_cast<std::size_t>(std::distance(kVariants.begin(), found)) + 1) % kVariants.size();
    row->addChild(
        ui::button({
            .text = std::string(
                gnil::theme::schemeToString(
                    gnil::theme::schemeFromString(current).value_or(gnil::theme::Scheme::Content)
                )
            ),
            .enabled = true,
            .variant = ButtonVariant::Ghost,
            .minHeight = Style::controlHeightSm * m_scale,
            .paddingH = Style::spaceSm * m_scale,
            .radius = Style::scaledRadiusMd(m_scale),
            .onClick = [config = m_services.config, next]() {
              (void)config->setThemeColorScheme(PaletteSource::Wallpaper, kVariants[next]);
            },
        })
    );
    hasTrailingControl = true;
  } else if (controlId.starts_with("network-ap:") && m_services.network != nullptr) {
    const std::string path = controlId.substr(std::string_view("network-ap:").size());
    const auto found = std::ranges::find_if(m_services.network->accessPoints(), [&path](const AccessPointInfo& ap) {
      return ap.path == path;
    });
    if (found != m_services.network->accessPoints().end()) {
      const AccessPointInfo ap = *found;
      const bool needsPassword = ap.secured && !m_services.network->hasSavedConnection(ap.ssid);
      Button* action = nullptr;
      row->addChild(
          ui::button({
              .out = &action,
              .text = ap.active ? "Connected" : needsPassword ? "Enter password" : "Connect",
              .enabled = !ap.active,
              .variant = ButtonVariant::Ghost,
              .minHeight = Style::controlHeightSm * m_scale,
              .paddingH = Style::spaceSm * m_scale,
              .radius = Style::scaledRadiusMd(m_scale),
              .onClick = [this, network = m_services.network, ap, needsPassword]() {
                if (needsPassword) {
                  m_route.setSelectedEntity("network-password:" + ap.path);
                  requestContentRefresh();
                } else {
                  (void)network->activateAccessPoint(ap);
                }
              },
          })
      );
      if (m_services.network->hasSavedConnection(ap.ssid)) {
        row->addChild(ui::button({
            .text = m_pendingForgetEntity == "wifi:" + ap.path ? "Confirm forget" : "Forget",
            .variant = ButtonVariant::Destructive,
            .minHeight = Style::controlHeightSm * m_scale,
            .paddingH = Style::spaceSm * m_scale,
            .radius = Style::scaledRadiusMd(m_scale),
            .onClick = [this, network = m_services.network, ap]() {
              const std::string key = "wifi:" + ap.path;
              if (m_pendingForgetEntity != key) {
                m_pendingForgetEntity = key;
                requestContentRefresh();
                return;
              }
              network->forgetSsid(ap.ssid);
              m_pendingForgetEntity.clear();
              requestContentRefresh();
            },
        }));
      }
      m_liveRows[controlId] = {.detail = detailLabel, .action = action};
      hasTrailingControl = true;
    }
  } else if (controlId.starts_with("bluetooth-device:") && m_services.bluetooth != nullptr) {
    const std::string path = controlId.substr(std::string_view("bluetooth-device:").size());
    const auto found =
        std::ranges::find_if(m_services.bluetooth->devices(), [&path](const BluetoothDeviceInfo& device) {
          return device.path == path;
        });
    if (found != m_services.bluetooth->devices().end()) {
      const BluetoothDeviceInfo device = *found;
      Button* action = nullptr;
      row->addChild(
          ui::button({
              .out = &action,
              .text = device.connected ? "Disconnect"
                  : device.paired      ? "Connect"
                                       : "Pair",
              .enabled = !device.connecting,
              .variant = ButtonVariant::Ghost,
              .minHeight = Style::controlHeightSm * m_scale,
              .paddingH = Style::spaceSm * m_scale,
              .radius = Style::scaledRadiusMd(m_scale),
              .onClick = [bluetooth = m_services.bluetooth, device]() {
                if (device.connected) {
                  (void)bluetooth->disconnectDevice(device.path);
                } else if (device.paired) {
                  (void)bluetooth->connect(device.path);
                } else {
                  (void)bluetooth->pair(device.path);
                }
              },
          })
      );
      if (device.paired) {
        row->addChild(ui::toggle({
            .checked = device.trusted,
            .scale = m_scale,
            .onChange = [bluetooth = m_services.bluetooth, path = device.path](bool trusted) {
              bluetooth->setTrusted(path, trusted);
            },
        }));
        row->addChild(ui::button({
            .text = m_pendingForgetEntity == "bluetooth:" + device.path ? "Confirm forget" : "Forget",
            .variant = ButtonVariant::Destructive,
            .minHeight = Style::controlHeightSm * m_scale,
            .paddingH = Style::spaceSm * m_scale,
            .radius = Style::scaledRadiusMd(m_scale),
            .onClick = [this, bluetooth = m_services.bluetooth, path = device.path]() {
              const std::string key = "bluetooth:" + path;
              if (m_pendingForgetEntity != key) {
                m_pendingForgetEntity = key;
                requestContentRefresh();
                return;
              }
              bluetooth->forget(path);
              m_pendingForgetEntity.clear();
              requestContentRefresh();
            },
        }));
      }
      m_liveRows[controlId] = {.detail = detailLabel, .action = action};
      hasTrailingControl = true;
    }
  } else if (controlId.starts_with("network-vpn:") && m_services.network != nullptr) {
    const std::string path = controlId.substr(std::string_view("network-vpn:").size());
    const auto found = std::ranges::find_if(m_services.network->vpnConnections(), [&path](const VpnConnectionInfo& vpn) {
      return vpn.path == path;
    });
    if (found != m_services.network->vpnConnections().end()) {
      const VpnConnectionInfo vpn = *found;
      Button* action = nullptr;
      row->addChild(ui::button({
          .out = &action,
          .text = vpn.active ? "Disconnect" : "Connect",
          .variant = vpn.active ? ButtonVariant::Destructive : ButtonVariant::Ghost,
          .minHeight = Style::controlHeightSm * m_scale,
          .paddingH = Style::spaceSm * m_scale,
          .radius = Style::scaledRadiusMd(m_scale),
          .onClick = [network = m_services.network, vpn]() {
            if (vpn.active) {
              (void)network->deactivateVpnConnection(vpn);
            } else {
              (void)network->activateVpnConnection(vpn);
            }
          },
      }));
      m_liveRows[controlId] = {.detail = detailLabel, .action = action};
      hasTrailingControl = true;
    }
  } else if (controlId.starts_with("audio-sink:") && m_services.audio != nullptr) {
    const std::string idText = controlId.substr(std::string_view("audio-sink:").size());
    const auto found = std::ranges::find_if(m_services.audio->state().sinks, [&idText](const AudioNode& node) {
      return std::to_string(node.id) == idText;
    });
    if (found != m_services.audio->state().sinks.end()) {
      const AudioNode device = *found;
      Slider* slider = nullptr;
      Button* mute = nullptr;
      Button* use = nullptr;
      row->addChild(ui::slider({
          .out = &slider,
          .minValue = 0.0,
          .maxValue = 1.5,
          .step = 0.01,
          .value = device.volume,
          .enabled = device.available,
          .presentation = SliderPresentation::LevelCompact,
          .trackHeight = 18.0f * m_scale,
          .thumbSize = 33.0f * m_scale,
          .controlHeight = 33.0f * m_scale,
          .width = 150.0f * m_scale,
          .onValueChanged = [audio = m_services.audio, id = device.id](double volume) {
            audio->setSinkVolume(id, static_cast<float>(volume));
          },
      }));
      row->addChild(ui::button({
          .out = &mute,
          .glyph = device.muted ? "volume_off" : "volume_up",
          .variant = ButtonVariant::Ghost,
          .onClick = [audio = m_services.audio, id = device.id, muted = device.muted]() {
            audio->setSinkMuted(id, !muted);
          },
      }));
      row->addChild(ui::button({
          .out = &use,
          .text = device.isDefault ? "Default" : "Use",
          .enabled = !device.isDefault && device.available,
          .variant = ButtonVariant::Ghost,
          .onClick = [audio = m_services.audio, id = device.id]() { audio->setDefaultSink(id); },
      }));
      m_liveRows[controlId] = {.detail = detailLabel, .action = mute, .secondaryAction = use, .slider = slider};
      hasTrailingControl = true;
    }
  } else if (controlId.starts_with("audio-source:") && m_services.audio != nullptr) {
    const std::string idText = controlId.substr(std::string_view("audio-source:").size());
    const auto found = std::ranges::find_if(m_services.audio->state().sources, [&idText](const AudioNode& node) {
      return std::to_string(node.id) == idText;
    });
    if (found != m_services.audio->state().sources.end()) {
      const AudioNode device = *found;
      Slider* slider = nullptr;
      Button* mute = nullptr;
      Button* use = nullptr;
      row->addChild(ui::slider({
          .out = &slider,
          .minValue = 0.0,
          .maxValue = 1.5,
          .step = 0.01,
          .value = device.volume,
          .enabled = device.available,
          .presentation = SliderPresentation::LevelCompact,
          .trackHeight = 18.0f * m_scale,
          .thumbSize = 33.0f * m_scale,
          .controlHeight = 33.0f * m_scale,
          .width = 150.0f * m_scale,
          .onValueChanged = [audio = m_services.audio, id = device.id](double volume) {
            audio->setSourceVolume(id, static_cast<float>(volume));
          },
      }));
      row->addChild(ui::button({
          .out = &mute,
          .glyph = device.muted ? "mic_off" : "mic",
          .variant = ButtonVariant::Ghost,
          .onClick = [audio = m_services.audio, id = device.id, muted = device.muted]() {
            audio->setSourceMuted(id, !muted);
          },
      }));
      row->addChild(ui::button({
          .out = &use,
          .text = device.isDefault ? "Default" : "Use",
          .enabled = !device.isDefault && device.available,
          .variant = ButtonVariant::Ghost,
          .onClick = [audio = m_services.audio, id = device.id]() { audio->setDefaultSource(id); },
      }));
      m_liveRows[controlId] = {.detail = detailLabel, .action = mute, .secondaryAction = use, .slider = slider};
      hasTrailingControl = true;
    }
  } else if (controlId.starts_with("audio-app:") && m_services.audio != nullptr) {
    const std::string idText = controlId.substr(std::string_view("audio-app:").size());
    const auto found = std::ranges::find_if(m_services.audio->state().programOutputs, [&idText](const AudioNode& node) {
      return std::to_string(node.id) == idText;
    });
    if (found != m_services.audio->state().programOutputs.end()) {
      const AudioNode stream = *found;
      Slider* slider = nullptr;
      Button* mute = nullptr;
      row->addChild(
          ui::slider({
              .out = &slider,
              .minValue = 0.0,
              .maxValue = 1.5,
              .step = 0.01,
              .value = static_cast<double>(stream.volume),
              .enabled = stream.available,
              .presentation = SliderPresentation::LevelCompact,
              .trackHeight = 18.0f * m_scale,
              .thumbSize = 33.0f * m_scale,
              .controlHeight = 33.0f * m_scale,
              .width = 180.0f * m_scale,
              .onValueChanged = [audio = m_services.audio, id = stream.id](double volume) {
                audio->setProgramOutputVolume(id, static_cast<float>(volume));
              },
          })
      );
      row->addChild(ui::button({
          .out = &mute,
          .glyph = stream.muted ? "volume_off" : "volume_up",
          .variant = ButtonVariant::Ghost,
          .onClick = [audio = m_services.audio, id = stream.id, muted = stream.muted]() {
            audio->setProgramOutputMuted(id, !muted);
          },
      }));
      m_liveRows[controlId] = {.detail = detailLabel, .action = mute, .slider = slider};
      hasTrailingControl = true;
    }
  }
  if (!hasTrailingControl) {
    row->addChild(
        ui::label({
            .text = available ? "Available" : "Unavailable",
            .fontSize = Style::fontSizeCaption * m_scale,
            .fontWeight = FontWeight::SemiBold,
            .color = colorSpecFromRole(
                available ? ColorRole::Primary : ColorRole::OnSurfaceVariant, available ? 1.0f : 0.5f
            ),
        })
    );
  }
  m_content->addChild(std::move(row));
}

void NexusView::addSearchResults(std::string_view query) {
  if (m_content == nullptr) {
    return;
  }
  struct Result {
    NexusPage page;
    std::string id;
    std::string title;
    std::string description;
  };
  const std::string needle = StringUtils::toLower(StringUtils::trim(query));
  std::vector<Result> results;
  std::set<std::pair<NexusPage, std::string>> seen;
  const auto consider = [&](NexusPage page, std::string id, std::string title, std::string description,
                            std::string extra = {}) {
    const std::string haystack = StringUtils::toLower(
        pageTitle(nexusPageDescriptor(page)) + " " + title + " " + description + " " + extra
    );
    if (!haystack.contains(needle) || !seen.emplace(page, id).second) {
      return;
    }
    results.push_back({page, std::move(id), std::move(title), std::move(description)});
  };

  for (const auto& descriptor : kControls) {
    consider(
        descriptor.page, std::string(descriptor.id), std::string(descriptor.title),
        std::string(descriptor.description)
    );
  }
  if (m_services.config != nullptr) {
    for (const auto& page : nexusPages()) {
      for (const auto& entry : nexusEntries(m_services.config->config(), page.page)) {
        consider(page.page, pathKey(entry.path), entry.title, entry.subtitle, entry.searchText);
      }
    }
  }

  if (results.empty()) {
    m_content->addChild(
        ui::column(
            {.align = FlexAlign::Center,
             .gap = Style::spaceSm * m_scale,
             .padding = Style::spaceLg * 2.0f * m_scale,
             .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.36f),
             .radius = Style::scaledRadiusLg(m_scale),
             .fillWidth = true},
            ui::glyph({
                .glyph = "search_off",
                .glyphSize = 36.0f * m_scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            }),
            ui::label({
                .text = "No settings found",
                .fontSize = Style::fontSizeHeader * m_scale,
                .fontWeight = FontWeight::Bold,
                .color = colorSpecFromRole(ColorRole::OnSurface),
            }),
            ui::label({
                .text = "Try a page name, control, or configuration term.",
                .fontSize = Style::fontSizeBody * m_scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            })
        )
    );
    return;
  }

  for (const auto& page : nexusPages()) {
    const bool hasPage = std::ranges::find(results, page.page, &Result::page) != results.end();
    if (!hasPage) {
      continue;
    }
    m_content->addChild(
        ui::label({
            .text = pageTitle(page),
            .fontSize = Style::fontSizeBody * m_scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::Secondary),
        })
    );
    for (const auto& result : results) {
      if (result.page != page.page) {
        continue;
      }
      Button* resultButton = nullptr;
      auto button = ui::button({
          .out = &resultButton,
          .contentAlign = ButtonContentAlign::Start,
          .variant = ButtonVariant::Tab,
          .minHeight = Style::controlHeight * m_scale,
          .paddingH = Style::spaceMd * m_scale,
          .radius = Style::scaledRadiusMd(m_scale),
          .onClick = [this, resultPage = result.page, id = result.id]() mutable {
            deferContentMutation([this, resultPage, id = std::move(id)]() mutable {
              selectSearchResult(resultPage, std::move(id));
            });
          },
      });
      resultButton->addChild(ui::glyph({
          .glyph = std::string(page.glyph),
          .glyphSize = Style::fontSizeBody * m_scale,
          .color = colorSpecFromRole(ColorRole::Primary),
      }));
      resultButton->addChild(
          ui::column(
              {.align = FlexAlign::Start, .gap = 2.0f * m_scale, .flexGrow = 1.0f},
              ui::label({
                  .text = result.title,
                  .fontSize = Style::fontSizeBody * m_scale,
                  .fontWeight = FontWeight::SemiBold,
                  .color = colorSpecFromRole(ColorRole::OnSurface),
              }),
              ui::label({
                  .text = result.description,
                  .fontSize = Style::fontSizeCaption * m_scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .maxLines = 1,
              })
          )
      );
      resultButton->addChild(ui::glyph({
          .glyph = "arrow_forward",
          .glyphSize = Style::fontSizeBody * m_scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      }));
      m_content->addChild(std::move(button));
    }
  }
}

void NexusView::addPageActions() {
  if (m_resetButton == nullptr) {
    return;
  }
  const bool searchActive = !m_route.query().empty();
  m_resetButton->setVisible(!searchActive);
  m_resetButton->setParticipatesInLayout(!searchActive);
  if (searchActive) {
    return;
  }

  bool hasOverride = false;
  if (m_services.config != nullptr) {
    const auto paths = currentPageResetPaths();
    hasOverride = std::ranges::any_of(paths, [this](const auto& path) {
      return m_services.config->hasOverride(path);
    });
  }
  if (!hasOverride) {
    m_resetConfirmationPending = false;
  }
  m_resetButton->setEnabled(hasOverride);
  m_resetButton->setText(
      m_resetConfirmationPending ? i18n::tr("settings.nexus.confirm-reset")
                                 : i18n::tr("settings.nexus.reset-page")
  );
  m_resetButton->setGlyph(m_resetConfirmationPending ? "warning" : "restart_alt");
  m_resetButton->setVariant(
      m_resetConfirmationPending ? ButtonVariant::Destructive : ButtonVariant::Ghost
  );
}

void NexusView::addNetworkPasswordPrompt() {
  static constexpr std::string_view kPrefix = "network-password:";
  if (m_content == nullptr || m_services.network == nullptr
      || !m_route.selectedEntity().starts_with(kPrefix)) {
    return;
  }
  const std::string path = m_route.selectedEntity().substr(kPrefix.size());
  const auto found = std::ranges::find_if(m_services.network->accessPoints(), [&path](const AccessPointInfo& ap) {
    return ap.path == path;
  });
  if (found == m_services.network->accessPoints().end()) {
    m_route.setSelectedEntity({});
    return;
  }
  const AccessPointInfo accessPoint = *found;
  auto card = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * m_scale,
      .padding = Style::spaceLg * m_scale,
      .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.85f),
      .radius = Style::scaledRadiusXl(m_scale),
      .fillWidth = true,
  });
  card->addChild(
      ui::label({
          .text = "Connect to " + accessPoint.ssid,
          .fontSize = Style::fontSizeHeader * m_scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );
  auto inputRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * m_scale, .fillWidth = true});
  inputRow->addChild(
      ui::input({
          .out = &m_credentialInput,
          .placeholder = "Wi-Fi password",
          .controlHeight = Style::controlHeight * m_scale,
          .horizontalPadding = Style::spaceMd * m_scale,
          .passwordMode = true,
          .flexGrow = 1.0f,
          .onSubmit = [this, accessPoint](const std::string& password) {
            if (password.empty()) {
              showError("Enter the network password.");
              return;
            }
            (void)m_services.network->activateAccessPoint(accessPoint, password);
            m_route.setSelectedEntity({});
            requestContentRefresh();
          },
      })
  );
  inputRow->addChild(
      ui::button({
          .text = "Cancel",
          .variant = ButtonVariant::Ghost,
          .onClick = [this]() {
            m_route.setSelectedEntity({});
            requestContentRefresh();
          },
      })
  );
  inputRow->addChild(
      ui::button({
          .text = "Connect",
          .variant = ButtonVariant::Primary,
          .onClick = [this, accessPoint]() {
            if (m_credentialInput == nullptr || m_credentialInput->value().empty()) {
              showError("Enter the network password.");
              return;
            }
            (void)m_services.network->activateAccessPoint(accessPoint, m_credentialInput->value());
            m_route.setSelectedEntity({});
            requestContentRefresh();
          },
      })
  );
  card->addChild(std::move(inputRow));
  m_content->addChild(std::move(card));
}

void NexusView::addBluetoothPairingPrompt() {
  if (m_content == nullptr || m_services.bluetoothAgent == nullptr
      || !m_services.bluetoothAgent->hasPendingRequest()) {
    return;
  }
  const BluetoothPairingRequest request = m_services.bluetoothAgent->pendingRequest();
  const std::string device = request.deviceAlias.empty() ? "Bluetooth device" : request.deviceAlias;
  std::string message;
  switch (request.kind) {
  case BluetoothPairingKind::PinCode:
    message = "Enter the PIN requested by " + device + ".";
    break;
  case BluetoothPairingKind::Passkey:
    message = "Enter the numeric passkey requested by " + device + ".";
    break;
  case BluetoothPairingKind::DisplayPinCode:
    message = "Enter PIN " + request.pin + " on " + device + ".";
    break;
  case BluetoothPairingKind::DisplayPasskey:
  case BluetoothPairingKind::Confirm:
    message = "Confirm passkey " + std::to_string(request.passkey) + " for " + device + ".";
    break;
  case BluetoothPairingKind::Authorize:
    message = "Allow " + device + " to pair?";
    break;
  case BluetoothPairingKind::AuthorizeService:
    message = "Allow " + device + " to use service " + request.uuid + "?";
    break;
  case BluetoothPairingKind::None:
    return;
  }
  auto card = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * m_scale,
      .padding = Style::spaceLg * m_scale,
      .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.85f),
      .radius = Style::scaledRadiusXl(m_scale),
      .fillWidth = true,
  });
  card->addChild(ui::label({
      .text = "Bluetooth pairing",
      .fontSize = Style::fontSizeHeader * m_scale,
      .fontWeight = FontWeight::Bold,
      .color = colorSpecFromRole(ColorRole::OnSurface),
  }));
  card->addChild(ui::label({
      .text = std::move(message),
      .fontSize = Style::fontSizeBody * m_scale,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
  }));
  const bool needsEntry = request.kind == BluetoothPairingKind::PinCode
      || request.kind == BluetoothPairingKind::Passkey;
  if (needsEntry) {
    card->addChild(ui::input({
        .out = &m_pairingInput,
        .placeholder = request.kind == BluetoothPairingKind::PinCode ? "PIN" : "Numeric passkey",
        .controlHeight = Style::controlHeight * m_scale,
        .horizontalPadding = Style::spaceMd * m_scale,
        .flexGrow = 1.0f,
    }));
  }
  auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * m_scale, .fillWidth = true});
  actions->addChild(ui::row({.flexGrow = 1.0f}));
  actions->addChild(ui::button({
      .text = "Cancel",
      .variant = ButtonVariant::Ghost,
      .onClick = [this]() {
        m_services.bluetoothAgent->cancelPending();
        requestContentRefresh();
      },
  }));
  actions->addChild(ui::button({
      .text = needsEntry ? "Submit" : "Confirm",
      .variant = ButtonVariant::Primary,
      .onClick = [this, kind = request.kind]() {
        if (kind == BluetoothPairingKind::PinCode) {
          if (m_pairingInput == nullptr || m_pairingInput->value().empty()) {
            showError("Enter the pairing PIN.");
            return;
          }
          m_services.bluetoothAgent->submitPin(m_pairingInput->value());
        } else if (kind == BluetoothPairingKind::Passkey) {
          std::uint32_t passkey = 0;
          const std::string value = m_pairingInput != nullptr ? m_pairingInput->value() : std::string{};
          const auto parsed = std::from_chars(value.data(), value.data() + value.size(), passkey);
          if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
            showError("Enter a numeric passkey.");
            return;
          }
          m_services.bluetoothAgent->submitPasskey(passkey);
        } else {
          m_services.bluetoothAgent->acceptConfirm();
        }
        showError("");
        requestContentRefresh();
      },
  }));
  card->addChild(std::move(actions));
  m_content->addChild(std::move(card));
}

void NexusView::addDefaultAppRows() {
  if (m_content == nullptr || m_services.config == nullptr) {
    return;
  }
  const auto& defaults = m_services.config->config().defaultApps;
  struct DefaultAppRow {
    std::string_view kind;
    std::string_view title;
    std::string_view description;
    const std::string* value;
  };
  const std::array rows = {
      DefaultAppRow{"terminal", "Default terminal", "Used for terminal actions", &defaults.terminal},
      DefaultAppRow{"audio", "Default audio app", "Used for audio controls", &defaults.audio},
      DefaultAppRow{"media_playback", "Default media player", "Used for media actions", &defaults.mediaPlayback},
      DefaultAppRow{"file_manager", "Default file manager", "Used to open folders", &defaults.fileManager},
  };
  m_content->addChild(ui::label({
      .text = "Default apps",
      .fontSize = Style::fontSizeBody * m_scale,
      .fontWeight = FontWeight::Bold,
      .color = colorSpecFromRole(ColorRole::Secondary),
  }));
  for (const auto& app : rows) {
    auto row = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceMd * m_scale,
        .padding = Style::spaceMd * m_scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.58f),
        .radius = Style::scaledRadiusLg(m_scale),
        .fillWidth = true,
    });
    row->addChild(
        ui::column(
            {.align = FlexAlign::Start, .gap = 2.0f * m_scale, .flexGrow = 1.0f},
            ui::label({
                .text = std::string(app.title),
                .fontSize = Style::fontSizeBody * m_scale,
                .fontWeight = FontWeight::SemiBold,
                .color = colorSpecFromRole(ColorRole::OnSurface),
            }),
            ui::label({
                .text = std::string(app.description) + " · " + (app.value->empty() ? "Auto" : *app.value),
                .fontSize = Style::fontSizeCaption * m_scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            })
        )
    );
    row->addChild(ui::button({
        .text = "Choose",
        .variant = ButtonVariant::Ghost,
        .onClick = [this, kind = std::string(app.kind)]() {
          m_route.setSelectedEntity("default-app:" + kind);
          requestContentRefresh();
        },
    }));
    m_content->addChild(std::move(row));
  }
}

void NexusView::addDefaultAppPicker() {
  static constexpr std::string_view kPrefix = "default-app:";
  if (m_content == nullptr || m_services.config == nullptr || !m_route.selectedEntity().starts_with(kPrefix)) {
    return;
  }
  const std::string kind = m_route.selectedEntity().substr(kPrefix.size());
  std::string title;
  std::string selected;
  if (kind == "terminal") {
    title = "Choose a terminal";
    selected = m_services.config->config().defaultApps.terminal;
  } else if (kind == "audio") {
    title = "Choose an audio app";
    selected = m_services.config->config().defaultApps.audio;
  } else if (kind == "media_playback") {
    title = "Choose a media player";
    selected = m_services.config->config().defaultApps.mediaPlayback;
  } else if (kind == "file_manager") {
    title = "Choose a file manager";
    selected = m_services.config->config().defaultApps.fileManager;
  } else {
    m_route.setSelectedEntity({});
    return;
  }
  std::vector<SearchPickerOption> options;
  options.push_back({.value = "", .label = "Automatic", .description = "Use the system default"});
  for (const auto& entry : desktopEntries()) {
    if (entry.hidden || entry.noDisplay || entry.name.empty()) {
      continue;
    }
    options.push_back({
        .value = entry.id,
        .label = entry.name,
        .description = !entry.genericName.empty() ? entry.genericName : entry.comment,
        .enabled = true,
        .icon = entry.icon,
    });
  }
  m_content->addChild(
      ui::column(
          {.align = FlexAlign::Stretch,
           .gap = Style::spaceSm * m_scale,
           .padding = Style::spaceLg * m_scale,
           .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.85f),
           .radius = Style::scaledRadiusXl(m_scale),
           .fillWidth = true},
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm * m_scale},
              ui::label({
                  .text = std::move(title),
                  .fontSize = Style::fontSizeHeader * m_scale,
                  .fontWeight = FontWeight::Bold,
                  .color = colorSpecFromRole(ColorRole::OnSurface),
                  .flexGrow = 1.0f,
              }),
              ui::button({
                  .glyph = "close",
                  .variant = ButtonVariant::Ghost,
                  .onClick = [this]() {
                    m_route.setSelectedEntity({});
                    requestContentRefresh();
                  },
              })
          ),
          ui::searchPicker({
              .placeholder = "Search installed apps",
              .emptyText = "No matching desktop apps",
              .selectedValue = selected,
              .options = std::move(options),
              .height = 300.0f * m_scale,
              .onActivated = [this, kind](const SearchPickerOption& option) {
                if (!m_services.config->setOverride({"default_apps", kind}, option.value)) {
                  showError(
                      m_services.config->lastMutationError().empty() ? "The default app could not be saved."
                                                                      : m_services.config->lastMutationError()
                  );
                  return;
                }
                m_route.setSelectedEntity({});
                showError("");
                requestContentRefresh();
              },
              .onCancel = [this]() {
                m_route.setSelectedEntity({});
                requestContentRefresh();
              },
          })
      )
  );
}

void NexusView::addAboutInfo() {
  addControlRow("GNIL", gnil::build_info::displayVersion(), true, "shell");
  addControlRow("Distribution", distroLabel(), true, "system.distro");
  addControlRow("Kernel", kernelLabel(), true, "system.kernel");
  addControlRow("Compositor", compositorLabel(), true, "system.compositor");
  addControlRow("Processor", cpuModelName(), true, "system.cpu");
  addControlRow("Graphics", gpuLabel(), true, "system.gpu");
  addControlRow("Memory", memoryTotalLabel(), true, "system.memory");
  addControlRow("Disk", diskUsageLabel("/"), true, "system.disk");
  if (m_services.platform != nullptr) {
    std::string outputs;
    for (const auto& output : m_services.platform->outputs()) {
      if (!outputs.empty()) {
        outputs += ", ";
      }
      outputs += !output.connectorName.empty() ? output.connectorName : output.interfaceName;
    }
    addControlRow("Outputs", outputs.empty() ? "No outputs reported" : outputs, true, "system.outputs");
  }
  const char* session = std::getenv("XDG_SESSION_TYPE");
  addControlRow("Session", session != nullptr && session[0] != '\0' ? session : "Wayland", true, "system.session");
  addControlRow("Config path", FileUtils::configDir(), true, "support.config-path");
  addControlRow("State path", FileUtils::stateDir(), true, "support.state-path");
  addControlRow("Data path", FileUtils::dataDir(), true, "support.data-path");
  if (m_services.config != nullptr) {
    addControlRow("Diagnostics", "Copy a sanitized system and configuration report", true, "support.diagnostics");
    auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * m_scale, .fillWidth = true});
    actions->addChild(ui::row({.flexGrow = 1.0f}));
    actions->addChild(ui::button({
        .text = "Save support report",
        .glyph = "save",
        .variant = ButtonVariant::Primary,
        .onClick = [this]() { saveSupportReport(); },
    }));
    m_content->addChild(std::move(actions));
  }
}

void NexusView::saveSupportReport() {
  if (m_services.config == nullptr) {
    return;
  }
  FileDialogOptions options;
  options.mode = FileDialogMode::Save;
  options.defaultFilename = "gnil-support-report.toml";
  options.title = "Save support report";
  options.extensions = {".toml"};
  const bool opened = FileDialog::open(std::move(options), [this](std::optional<std::filesystem::path> result) {
    if (!result.has_value() || m_services.config == nullptr) {
      return;
    }
    auto path = *result;
    if (path.extension().empty()) {
      path += ".toml";
    }
    if (!writeTextFileAtomic(path, m_services.config->buildSupportReport())) {
      showError("The support report could not be saved.");
      return;
    }
    showError("");
  });
  if (!opened) {
    showError("The support report dialog could not be opened.");
  }
}

std::vector<std::vector<std::string>> NexusView::currentPageResetPaths() const {
  if (m_services.config == nullptr) {
    return {};
  }
  auto entries = nexusEntries(m_services.config->config(), m_route.page());
  if (const NexusSubpage* subpage = m_route.currentSubpage(); subpage != nullptr) {
    std::erase_if(entries, [subpage](const settings::SettingEntry& entry) { return entry.group != subpage->id; });
  }
  std::set<std::vector<std::string>> uniquePaths;
  for (const auto& entry : entries) {
    if (!entry.path.empty()) {
      uniquePaths.insert(entry.path);
    }
    if (const auto* range = std::get_if<settings::RangeSliderSetting>(&entry.control); range != nullptr) {
      uniquePaths.insert(range->highPath);
    }
    if (const auto* select = std::get_if<settings::SelectSetting>(&entry.control);
        select != nullptr && !select->linkedPath.empty()) {
      uniquePaths.insert(select->linkedPath);
    }
  }
  if (m_route.page() == NexusPage::Apps) {
    uniquePaths.insert({"default_apps", "terminal"});
    uniquePaths.insert({"default_apps", "audio"});
    uniquePaths.insert({"default_apps", "media_playback"});
    uniquePaths.insert({"default_apps", "file_manager"});
  }
  return {uniquePaths.begin(), uniquePaths.end()};
}

void NexusView::resetCurrentPage() {
  if (m_services.config == nullptr) {
    return;
  }
  const auto paths = currentPageResetPaths();
  bool changed = false;
  if (!m_services.config->clearOverrides(paths, &changed)) {
    showError(
        m_services.config->lastMutationError().empty() ? "The page could not be reset."
                                                       : m_services.config->lastMutationError()
    );
    return;
  }
  m_resetConfirmationPending = false;
  showError("");
  if (changed) {
    requestContentRefresh();
  }
}

void NexusView::addConfigPage() {
  if (m_content == nullptr || m_services.config == nullptr) {
    return;
  }
  const Config& config = m_services.config->config();
  auto entries = nexusEntries(config, m_route.page());
  if (const NexusSubpage* subpage = m_route.currentSubpage(); subpage != nullptr) {
    std::erase_if(entries, [subpage](const settings::SettingEntry& entry) { return entry.group != subpage->id; });
  }
  if (entries.empty()) {
    return;
  }
  const auto section = representativeSection(m_route.page());
  settings::SettingsContentContext context{
      .config = config,
      .configService = m_services.config,
      .scale = m_scale,
      .searchQuery = {},
      .selectedSection = settings::settingsSectionId(section),
      .selectedBar = nullptr,
      .selectedMonitorOverride = nullptr,
      .showAdvanced = true,
      .showOverriddenOnly = false,
      .targetPath = m_route.pendingControl(),
      .editingWidgetName = m_editingWidgetName,
      .editingCapsuleGroupId = m_editingCapsuleGroupId,
      .selectedLaneWidgets = m_selectedLaneWidgets,
      .pendingDeleteWidgetName = m_pendingDeleteWidgetName,
      .pendingDeleteWidgetSettingPath = m_pendingDeleteWidgetSettingPath,
      .renamingWidgetName = m_renamingWidgetName,
      .requestRebuild = [this]() { requestContentRefresh(); },
      .requestContentRebuild = [this]() { requestContentRefresh(); },
      .resetContentScroll = [this]() {
        m_route.setScrollOffset(0.0f);
        if (m_scroll != nullptr) {
          m_scroll->setScrollOffset(0.0f);
        }
      },
      .setScrollTarget = [this](Node* target) { m_pendingScrollTarget = target; },
      .focusArea = {},
      // Widget add/inspector sheets are xdg popups owned by SettingsWindow.
      // Keep the Nexus context stable, then travel to that host instead of
      // silently dropping the action as the old empty callbacks did.
      .openBarWidgetAddPopup = [this](const std::vector<std::string>&) {
        m_route.setPage(NexusPage::Panels);
        m_route.setSelectedControl("bar");
        if (m_requestPip) {
          m_requestPip();
        }
      },
      .openSearchPickerPopup = {},
      .setOverride = [this](std::vector<std::string> path, ConfigOverrideValue value) {
        static const std::vector<std::string> kFavouritePath = {"shell", "launcher", "favourite_apps"};
        static const std::vector<std::string> kHiddenPath = {"shell", "launcher", "hidden_apps"};
        if (path == kHiddenPath || path == kFavouritePath) {
          const auto* changedApps = std::get_if<std::vector<std::string>>(&value);
          if (changedApps != nullptr) {
            std::vector<std::string> hidden = path == kHiddenPath
                ? *changedApps
                : m_services.config->config().shell.launcher.hiddenApps;
            std::vector<std::string> favourites = path == kFavouritePath
                ? *changedApps
                : m_services.config->config().shell.launcher.favouriteApps;
            std::erase_if(favourites, [&hidden](const std::string& app) {
              return std::ranges::contains(hidden, app);
            });
            if (!m_services.config->setOverrides({
                    {kHiddenPath, ConfigOverrideValue{std::move(hidden)}},
                    {kFavouritePath, ConfigOverrideValue{std::move(favourites)}},
                })) {
              showError(
                  m_services.config->lastMutationError().empty() ? "The app lists could not be saved."
                                                                  : m_services.config->lastMutationError()
              );
            } else {
              showError("");
            }
            return;
          }
        }
        std::string error;
        if (!m_services.config->validateOverride(path, value, &error)
            || !m_services.config->setOverride(path, std::move(value))) {
          showError(
              !error.empty() ? std::move(error)
                             : (m_services.config->lastMutationError().empty()
                                    ? std::string("This value is not valid.")
                                    : m_services.config->lastMutationError())
          );
          return;
        }
        showError("");
      },
      .setOverrides = [this](
                          std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides
                      ) {
        if (!m_services.config->setOverrides(std::move(overrides))) {
          showError(
              m_services.config->lastMutationError().empty() ? "These values could not be saved."
                                                              : m_services.config->lastMutationError()
          );
          return;
        }
        showError("");
      },
      .clearOverride = [this](std::vector<std::string> path) {
        if (!m_services.config->clearOverride(path)) {
          showError(
              m_services.config->lastMutationError().empty() ? "This setting could not be reset."
                                                              : m_services.config->lastMutationError()
          );
          return;
        }
        showError("");
        requestContentRefresh();
      },
      .openCapsuleGroupEditor = [this](std::vector<std::string>, std::string) {
        m_route.setPage(NexusPage::Panels);
        m_route.setSelectedControl("bar");
        if (m_requestPip) {
          m_requestPip();
        }
      },
  };
  (void)settings::addSettingsContentSections(*m_content, entries, std::move(context));
}

void NexusView::addWallpaperBrowserCard() {
  if (m_content == nullptr) {
    return;
  }
  const bool available = m_services.wallpaper != nullptr && static_cast<bool>(m_services.openWallpaperPanel);
  auto card = ui::button({
      .enabled = available,
      .variant = ButtonVariant::Outline,
      .surfaceOpacity = 0.34f,
      .minHeight = 92.0f * m_scale,
      .paddingV = Style::spaceMd * m_scale,
      .paddingH = Style::spaceLg * m_scale,
      .gap = Style::spaceMd * m_scale,
      .radius = Style::scaledRadiusXl(m_scale),
      .onClick = [this]() {
        if (m_services.openWallpaperPanel) {
          m_services.openWallpaperPanel();
        }
      },
  });
  card->addChild(ui::column(
      {.align = FlexAlign::Center,
       .justify = FlexJustify::Center,
       .fill = colorSpecFromRole(ColorRole::Primary, 0.09f),
       .radius = 26.0f * m_scale,
       .width = 52.0f * m_scale,
       .height = 52.0f * m_scale},
      ui::glyph({
          .glyph = "wallpaper-selector",
          .glyphSize = 27.0f * m_scale,
          .color = colorSpecFromRole(ColorRole::Primary),
      })
  ));
  card->addChild(ui::column(
      {.align = FlexAlign::Start, .gap = 3.0f * m_scale, .flexGrow = 1.0f},
      ui::label({
          .text = i18n::tr("settings.nexus.wallpaper.title"),
          .fontSize = Style::fontSizeBody * m_scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      }),
      ui::label({
          .text = i18n::tr("settings.nexus.wallpaper.description"),
          .fontSize = Style::fontSizeCaption * m_scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
      })
  ));
  card->addChild(ui::row(
      {.align = FlexAlign::Center,
       .paddingV = Style::spaceXs * m_scale,
       .paddingH = Style::spaceSm * m_scale,
       .fill = colorSpecFromRole(available ? ColorRole::Primary : ColorRole::SurfaceVariant, 0.10f),
       .radius = Style::scaledRadiusMd(m_scale)},
      ui::label({
          .text = i18n::tr(available ? "settings.nexus.available" : "settings.nexus.unavailable"),
          .fontSize = Style::fontSizeCaption * m_scale,
          .fontWeight = FontWeight::SemiBold,
          .color = colorSpecFromRole(available ? ColorRole::Primary : ColorRole::OnSurfaceVariant),
      })
  ));
  card->addChild(ui::glyph({
      .glyph = "chevron-right",
      .glyphSize = Style::baseGlyphSize * m_scale,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
  }));
  m_content->addChild(std::move(card));
}

void NexusView::addConfigGroupIndex() {
  if (m_content == nullptr || m_services.config == nullptr) {
    return;
  }
  auto entries = nexusEntries(m_services.config->config(), m_route.page());
  std::vector<std::pair<std::string, std::size_t>> groups;
  for (const auto& entry : entries) {
    if ((entry.visibleWhen && !entry.visibleWhen(m_services.config->config()))
        || (m_route.page() == NexusPage::WallpaperStyle && entry.group == "wallpaper")) {
      continue;
    }
    auto found = std::ranges::find(groups, entry.group, &std::pair<std::string, std::size_t>::first);
    if (found == groups.end()) {
      groups.emplace_back(entry.group, 1);
    } else {
      ++found->second;
    }
  }
  if (groups.empty()) {
    return;
  }

  auto groupCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = 0.0f,
      .fill = colorSpecFromRole(ColorRole::Surface, 0.72f),
      .radius = Style::scaledRadiusXl(m_scale),
      .border = colorSpecFromRole(ColorRole::Outline, 0.24f),
      .borderWidth = Style::borderWidth,
      .fillWidth = true,
      .clipChildren = true,
  });
  for (std::size_t index = 0; index < groups.size(); ++index) {
    const auto& [id, count] = groups[index];
    auto [title, glyph] = groupPresentation(m_route.page(), id);
    auto row = ui::button({
        .variant = ButtonVariant::Ghost,
        .surfaceOpacity = 0.20f,
        .minHeight = 62.0f * m_scale,
        .paddingV = Style::spaceSm * m_scale,
        .paddingH = Style::spaceMd * m_scale,
        .gap = Style::spaceMd * m_scale,
        .radius = 0.0f,
        .onClick = [this, id, title]() {
          deferContentMutation([this, id, title]() {
            m_route.pushSubpage(NexusSubpage{.id = id, .title = title});
            m_route.setScrollOffset(0.0f);
            rebuildContent();
            animateContentReveal();
          });
        },
    });
    row->addChild(ui::column(
        {.align = FlexAlign::Center,
         .justify = FlexJustify::Center,
         .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.58f),
         .radius = 20.0f * m_scale,
         .width = 40.0f * m_scale,
         .height = 40.0f * m_scale},
        ui::glyph({
            .glyph = glyph,
            .glyphSize = Style::baseGlyphSize * m_scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    ));
    row->addChild(ui::label({
        .text = title,
        .fontSize = Style::fontSizeBody * m_scale,
        .fontWeight = FontWeight::SemiBold,
        .color = colorSpecFromRole(ColorRole::OnSurface),
        .flexGrow = 1.0f,
    }));
    row->addChild(ui::row(
        {.align = FlexAlign::Center,
         .justify = FlexJustify::Center,
         .paddingV = 2.0f * m_scale,
         .paddingH = Style::spaceXs * m_scale,
         .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.62f),
         .radius = Style::scaledRadiusSm(m_scale),
         .minWidth = 28.0f * m_scale},
        ui::label({
            .text = std::to_string(count),
            .fontSize = Style::fontSizeCaption * m_scale,
            .fontWeight = FontWeight::SemiBold,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    ));
    row->addChild(ui::glyph({
        .glyph = "chevron-right",
        .glyphSize = Style::baseGlyphSize * m_scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
    }));
    groupCard->addChild(std::move(row));
    if (index + 1 < groups.size()) {
      groupCard->addChild(ui::box({
          .fill = colorSpecFromRole(ColorRole::Outline, 0.14f),
          .height = Style::borderWidth,
      }));
    }
  }
  m_content->addChild(std::move(groupCard));
}

std::string NexusView::liveStructureKey() const {
  std::string key = std::string(nexusPageDescriptor(m_route.page()).id);
  if (m_route.page() == NexusPage::Network && m_services.network != nullptr) {
    auto accessPoints = m_services.network->accessPoints();
    std::ranges::stable_sort(accessPoints, [network = m_services.network](const AccessPointInfo& lhs,
                                                                         const AccessPointInfo& rhs) {
      const auto rank = [network](const AccessPointInfo& ap) {
        return std::pair{ap.active ? 0 : network->hasSavedConnection(ap.ssid) ? 1 : 2, -ap.strength};
      };
      return rank(lhs) < rank(rhs);
    });
    for (const auto& ap : accessPoints) {
      key += '|';
      key += ap.path;
    }
    for (const auto& vpn : m_services.network->vpnConnections()) {
      key += "|vpn:";
      key += vpn.path;
    }
  } else if (m_route.page() == NexusPage::ConnectedDevices && m_services.bluetooth != nullptr) {
    if (m_services.bluetoothAgent != nullptr && m_services.bluetoothAgent->hasPendingRequest()) {
      const auto request = m_services.bluetoothAgent->pendingRequest();
      key += "|pairing:" + std::to_string(static_cast<int>(request.kind)) + ':' + request.devicePath;
    }
    for (const auto& device : m_services.bluetooth->devices()) {
      key += '|';
      key += device.path;
    }
  } else if (m_route.page() == NexusPage::Audio && m_services.audio != nullptr) {
    for (const auto& sink : m_services.audio->state().sinks) {
      key += "|sink:" + std::to_string(sink.id);
    }
    for (const auto& source : m_services.audio->state().sources) {
      key += "|source:" + std::to_string(source.id);
    }
    for (const auto& stream : m_services.audio->state().programOutputs) {
      key += '|';
      key += std::to_string(stream.id);
    }
  }
  return key;
}

void NexusView::update() {
  if (m_content == nullptr || !m_route.query().empty()) {
    return;
  }
  const std::string structure = liveStructureKey();
  if (structure != m_liveStructure) {
    refresh();
    return;
  }
  if (m_route.page() == NexusPage::Network && m_services.network != nullptr) {
    const auto& state = m_services.network->state();
    if (m_wifiToggle != nullptr) {
      m_wifiToggle->setChecked(state.wirelessEnabled);
      m_wifiToggle->setEnabled(m_services.network->hasStateSnapshot());
    }
    for (const auto& ap : m_services.network->accessPoints()) {
      const auto row = m_liveRows.find("network-ap:" + ap.path);
      if (row == m_liveRows.end()) {
        continue;
      }
      if (row->second.detail != nullptr) {
        const std::string status = ap.active ? "Connected"
            : m_services.network->hasSavedConnection(ap.ssid) ? "Saved · " + std::to_string(ap.strength) + "%"
            : ap.secured ? "Secured · " + std::to_string(ap.strength) + "%"
                         : "Open · " + std::to_string(ap.strength) + "%";
        row->second.detail->setText(status);
      }
      if (row->second.action != nullptr) {
        const bool needsPassword = ap.secured && !m_services.network->hasSavedConnection(ap.ssid);
        row->second.action->setText(ap.active ? "Connected" : needsPassword ? "Enter password" : "Connect");
        row->second.action->setEnabled(!ap.active);
      }
    }
    for (const auto& vpn : m_services.network->vpnConnections()) {
      const auto row = m_liveRows.find("network-vpn:" + vpn.path);
      if (row == m_liveRows.end()) {
        continue;
      }
      if (row->second.detail != nullptr) {
        row->second.detail->setText(vpn.active ? "Active VPN" : "Saved VPN");
      }
      if (row->second.action != nullptr) {
        row->second.action->setText(vpn.active ? "Disconnect" : "Connect");
        row->second.action->setVariant(vpn.active ? ButtonVariant::Destructive : ButtonVariant::Ghost);
      }
    }
  } else if (m_route.page() == NexusPage::ConnectedDevices && m_services.bluetooth != nullptr) {
    const auto& state = m_services.bluetooth->state();
    if (m_bluetoothToggle != nullptr) {
      m_bluetoothToggle->setChecked(state.powered);
      m_bluetoothToggle->setEnabled(state.adapterPresent && !state.rfkillHardBlocked);
    }
    if (m_discoverableToggle != nullptr) {
      m_discoverableToggle->setChecked(state.discoverable);
      m_discoverableToggle->setEnabled(state.adapterPresent && state.powered);
    }
    for (const auto& device : m_services.bluetooth->devices()) {
      const auto row = m_liveRows.find("bluetooth-device:" + device.path);
      if (row == m_liveRows.end()) {
        continue;
      }
      if (row->second.detail != nullptr) {
        std::string status = device.connected ? "Connected" : device.paired ? "Paired" : "Available";
        if (device.hasBattery) {
          status += " · " + std::to_string(device.batteryPercent) + "% battery";
        }
        if (device.hasRssi) {
          status += " · " + std::to_string(device.rssi) + " dBm";
        }
        row->second.detail->setText(status);
      }
      if (row->second.action != nullptr) {
        row->second.action->setText(
            device.connected ? "Disconnect" : device.paired ? "Connect" : "Pair"
        );
        row->second.action->setEnabled(!device.connecting);
      }
    }
  } else if (m_route.page() == NexusPage::Audio && m_services.audio != nullptr) {
    if (const AudioNode* sink = m_services.audio->defaultSink(); sink != nullptr && m_outputSlider != nullptr
        && !m_outputSlider->dragging()) {
      m_outputSlider->setValue(sink->volume);
      m_outputSlider->setEnabled(sink->available);
    }
    if (const AudioNode* source = m_services.audio->defaultSource(); source != nullptr && m_inputSlider != nullptr
        && !m_inputSlider->dragging()) {
      m_inputSlider->setValue(source->volume);
      m_inputSlider->setEnabled(source->available);
    }
    for (const auto& stream : m_services.audio->state().programOutputs) {
      const auto row = m_liveRows.find("audio-app:" + std::to_string(stream.id));
      if (row != m_liveRows.end() && row->second.slider != nullptr && !row->second.slider->dragging()) {
        row->second.slider->setValue(stream.volume);
        row->second.slider->setEnabled(stream.available);
      }
      if (row != m_liveRows.end() && row->second.detail != nullptr) {
        row->second.detail->setText(stream.muted ? "Muted" : "Application volume");
      }
      if (row != m_liveRows.end() && row->second.action != nullptr) {
        row->second.action->setGlyph(stream.muted ? "volume_off" : "volume_up");
        row->second.action->setOnClick([audio = m_services.audio, id = stream.id, muted = stream.muted]() {
          audio->setProgramOutputMuted(id, !muted);
        });
      }
    }
    for (const auto& sink : m_services.audio->state().sinks) {
      const auto row = m_liveRows.find("audio-sink:" + std::to_string(sink.id));
      if (row != m_liveRows.end() && row->second.slider != nullptr && !row->second.slider->dragging()) {
        row->second.slider->setValue(sink.volume);
        row->second.slider->setEnabled(sink.available);
      }
      if (row != m_liveRows.end() && row->second.action != nullptr) {
        row->second.action->setGlyph(sink.muted ? "volume_off" : "volume_up");
        row->second.action->setOnClick([audio = m_services.audio, id = sink.id, muted = sink.muted]() {
          audio->setSinkMuted(id, !muted);
        });
      }
      if (row != m_liveRows.end() && row->second.detail != nullptr) {
        row->second.detail->setText(sink.isDefault ? "Default output" : "Output device");
      }
      if (row != m_liveRows.end() && row->second.secondaryAction != nullptr) {
        row->second.secondaryAction->setText(sink.isDefault ? "Default" : "Use");
        row->second.secondaryAction->setEnabled(!sink.isDefault && sink.available);
      }
    }
    for (const auto& source : m_services.audio->state().sources) {
      const auto row = m_liveRows.find("audio-source:" + std::to_string(source.id));
      if (row != m_liveRows.end() && row->second.slider != nullptr && !row->second.slider->dragging()) {
        row->second.slider->setValue(source.volume);
        row->second.slider->setEnabled(source.available);
      }
      if (row != m_liveRows.end() && row->second.action != nullptr) {
        row->second.action->setGlyph(source.muted ? "mic_off" : "mic");
        row->second.action->setOnClick([audio = m_services.audio, id = source.id, muted = source.muted]() {
          audio->setSourceMuted(id, !muted);
        });
      }
      if (row != m_liveRows.end() && row->second.detail != nullptr) {
        row->second.detail->setText(source.isDefault ? "Default input" : "Input device");
      }
      if (row != m_liveRows.end() && row->second.secondaryAction != nullptr) {
        row->second.secondaryAction->setText(source.isDefault ? "Default" : "Use");
        row->second.secondaryAction->setEnabled(!source.isDefault && source.available);
      }
    }
  }
}

void NexusView::rebuildContent() {
  if (m_content == nullptr || m_title == nullptr) {
    return;
  }
  while (!m_content->children().empty()) {
    (void)m_content->removeChild(m_content->children().back().get());
  }
  m_pendingScrollTarget = nullptr;
  m_liveRows.clear();
  m_wifiToggle = nullptr;
  m_bluetoothToggle = nullptr;
  m_discoverableToggle = nullptr;
  m_outputSlider = nullptr;
  m_inputSlider = nullptr;
  m_errorLabel = nullptr;
  m_credentialInput = nullptr;
  m_pairingInput = nullptr;
  m_contentRefreshPending = false;
  const bool hasSubpage = m_route.currentSubpage() != nullptr;
  if (m_backButton != nullptr) {
    m_backButton->setVisible(hasSubpage);
    m_backButton->setParticipatesInLayout(hasSubpage);
  }
  addPageActions();
  if (!m_route.query().empty()) {
    if (m_backButton != nullptr) {
      m_backButton->setVisible(false);
      m_backButton->setParticipatesInLayout(false);
    }
    m_title->setText(i18n::tr("settings.nexus.search-results"));
    addSearchResults(m_route.query());
    if (m_scroll != nullptr) {
      m_scroll->setScrollOffset(0.0f);
    }
    return;
  }
  const auto& page = nexusPageDescriptor(m_route.page());
  const std::string currentPageTitle = pageTitle(page);
  if (const NexusSubpage* subpage = m_route.currentSubpage(); subpage != nullptr) {
    m_title->setText(currentPageTitle + "  /  " + subpage->title);
  } else {
    m_title->setText(currentPageTitle);
  }
  const bool available = pageAvailable(m_route.page());
  if (!m_errorMessage.empty()) {
    m_content->addChild(
        ui::label({
            .out = &m_errorLabel,
            .text = m_errorMessage,
            .fontSize = Style::fontSizeBody * m_scale,
            .fontWeight = FontWeight::SemiBold,
            .color = colorSpecFromRole(ColorRole::Error),
        })
    );
  }
  const auto addDescriptor = [this, available](std::string_view id) {
    const auto found = std::ranges::find(kControls, id, &ControlDescriptor::id);
    if (found != kControls.end() && found->page == m_route.page()) {
      addControlRow(std::string(found->title), std::string(found->description), available, std::string(found->id));
    }
  };

  switch (m_route.page()) {
  case NexusPage::WallpaperStyle:
    if (m_route.currentSubpage() != nullptr) {
      addConfigPage();
    } else {
      addWallpaperBrowserCard();
      addConfigGroupIndex();
    }
    break;
  case NexusPage::Network:
    addDescriptor("wifi");
    addDescriptor("ethernet");
    addDescriptor("rescan");
    addConfigPage();
    addNetworkPasswordPrompt();
    break;
  case NexusPage::ConnectedDevices:
    addDescriptor("bluetooth");
    addDescriptor("discoverable");
    addDescriptor("pair");
    addBluetoothPairingPrompt();
    break;
  case NexusPage::Audio:
    addDescriptor("output");
    addDescriptor("input");
    addConfigPage();
    break;
  case NexusPage::Panels:
    if (m_route.currentSubpage() != nullptr) {
      addConfigPage();
    } else {
      addDescriptor("rail");
      addDescriptor("workspace");
      addConfigGroupIndex();
    }
    break;
  case NexusPage::Apps:
    if (m_route.currentSubpage() != nullptr) {
      addConfigPage();
    } else {
      addDefaultAppRows();
      addDefaultAppPicker();
      addConfigGroupIndex();
      addDescriptor("reset-usage");
    }
    break;
  case NexusPage::Services:
    if (m_route.currentSubpage() != nullptr) {
      addConfigPage();
    } else {
      addDescriptor("clear-clipboard");
      addConfigGroupIndex();
    }
    break;
  case NexusPage::LanguageRegion:
    if (m_route.currentSubpage() != nullptr) {
      addConfigPage();
    } else {
      addControlRow("Language", "English", true, "language");
      addConfigGroupIndex();
    }
    break;
  case NexusPage::About:
    if (m_route.currentSubpage() != nullptr) {
      addConfigPage();
    } else {
      addConfigGroupIndex();
      addAboutInfo();
    }
    break;
  }

  if (m_route.page() == NexusPage::Network && m_services.network != nullptr) {
    auto accessPoints = m_services.network->accessPoints();
    std::ranges::stable_sort(accessPoints, [network = m_services.network](const AccessPointInfo& lhs,
                                                                         const AccessPointInfo& rhs) {
      const auto rank = [network](const AccessPointInfo& ap) {
        return std::pair{ap.active ? 0 : network->hasSavedConnection(ap.ssid) ? 1 : 2, -ap.strength};
      };
      return rank(lhs) < rank(rhs);
    });
    for (const auto& ap : accessPoints) {
      const std::string status = ap.active ? "Connected"
          : m_services.network->hasSavedConnection(ap.ssid) ? "Saved · " + std::to_string(ap.strength) + "%"
          : ap.secured ? "Secured · " + std::to_string(ap.strength) + "%"
                       : "Open · " + std::to_string(ap.strength) + "%";
      addControlRow(ap.ssid, status, true, "network-ap:" + ap.path);
    }
    for (const auto& vpn : m_services.network->vpnConnections()) {
      addControlRow(vpn.name, vpn.active ? "Active VPN" : "Saved VPN", true, "network-vpn:" + vpn.path);
    }
  } else if (m_route.page() == NexusPage::ConnectedDevices && m_services.bluetooth != nullptr) {
    for (const auto& device : m_services.bluetooth->devices()) {
      std::string status = device.connected ? "Connected" : device.paired ? "Paired" : "Available";
      if (device.hasBattery) {
        status += " · " + std::to_string(device.batteryPercent) + "% battery";
      }
      if (device.hasRssi) {
        status += " · " + std::to_string(device.rssi) + " dBm";
      }
      addControlRow(
          device.alias.empty() ? device.address : device.alias, status, true, "bluetooth-device:" + device.path
      );
    }
  } else if (m_route.page() == NexusPage::Audio && m_services.audio != nullptr) {
    for (const auto& sink : m_services.audio->state().sinks) {
      addControlRow(
          audioDeviceLabel(sink), sink.isDefault ? "Default output" : "Output device", sink.available,
          "audio-sink:" + std::to_string(sink.id)
      );
    }
    for (const auto& source : m_services.audio->state().sources) {
      addControlRow(
          audioDeviceLabel(source), source.isDefault ? "Default input" : "Input device", source.available,
          "audio-source:" + std::to_string(source.id)
      );
    }
    for (const auto& stream : m_services.audio->state().programOutputs) {
      const std::string name = !stream.applicationName.empty() ? stream.applicationName
          : !stream.description.empty()                        ? stream.description
                                                               : stream.name;
      addControlRow(
          name, stream.muted ? "Muted" : "Application volume", stream.available,
          "audio-app:" + std::to_string(stream.id)
      );
    }
  }
  if (m_scroll != nullptr) {
    m_scroll->setScrollOffset(m_route.scrollOffset());
  }
  m_liveStructure = liveStructureKey();
}

void NexusView::search(std::string_view query) {
  if (m_scroll != nullptr && m_route.query().empty()) {
    m_route.setScrollOffset(m_scroll->scrollOffset());
  }
  m_route.setQuery(std::string(query));
  rebuildContent();
}

bool NexusView::escape() {
  if (m_services.bluetoothAgent != nullptr && m_services.bluetoothAgent->hasPendingRequest()) {
    m_services.bluetoothAgent->cancelPending();
    requestContentRefresh();
    return true;
  }
  if (!m_pendingForgetEntity.empty()) {
    m_pendingForgetEntity.clear();
    requestContentRefresh();
    return true;
  }
  if (!m_route.selectedEntity().empty()) {
    m_route.setSelectedEntity({});
    requestContentRefresh();
    return true;
  }
  if (m_resetConfirmationPending) {
    m_resetConfirmationPending = false;
    requestContentRefresh();
    return true;
  }
  if (m_clipboardClearConfirmationPending) {
    m_clipboardClearConfirmationPending = false;
    requestContentRefresh();
    return true;
  }
  if (m_launcherResetConfirmationPending) {
    m_launcherResetConfirmationPending = false;
    requestContentRefresh();
    return true;
  }
  if (!m_route.query().empty()) {
    m_route.clearQuery();
    if (m_search != nullptr) {
      m_search->setValue("");
    }
    rebuildContent();
    return true;
  }
  if (m_route.popSubpage()) {
    rebuildContent();
    animateContentReveal();
    return true;
  }
  return false;
}

void NexusView::layout(Renderer& renderer, float width, float height) {
  if (m_root == nullptr) {
    return;
  }
  if (m_contentRefreshPending) {
    rebuildContent();
  }
  if (m_search != nullptr) {
    const bool labeledNavigation = m_navigationMode == NexusNavigationMode::LabeledSidebar;
    const float workspaceWidth = labeledNavigation ? std::max(0.0f, width - 224.0f * m_scale) : width;
    const float responsiveFraction = labeledNavigation ? 0.46f : 0.34f;
    const float responsiveWidth =
        std::clamp(workspaceWidth * responsiveFraction, 180.0f * m_scale, 300.0f * m_scale);
    m_search->setSize(responsiveWidth, Style::controlHeight * m_scale);
  }
  m_root->setSize(width, height);
  m_root->layout(renderer);
  if (m_pendingScrollTarget != nullptr && m_scroll != nullptr) {
    float targetLeft = 0.0f;
    float targetTop = 0.0f;
    float targetRight = 0.0f;
    float targetBottom = 0.0f;
    float scrollLeft = 0.0f;
    float scrollTop = 0.0f;
    float scrollRight = 0.0f;
    float scrollBottom = 0.0f;
    Node::transformedBounds(m_pendingScrollTarget, targetLeft, targetTop, targetRight, targetBottom);
    Node::transformedBounds(m_scroll, scrollLeft, scrollTop, scrollRight, scrollBottom);
    const float margin = Style::spaceMd * m_scale;
    if (targetTop < scrollTop + margin) {
      m_scroll->setScrollOffset(m_scroll->scrollOffset() + targetTop - scrollTop - margin);
    } else if (targetBottom > scrollBottom - margin) {
      m_scroll->setScrollOffset(m_scroll->scrollOffset() + targetBottom - scrollBottom + margin);
    }
    m_route.clearPendingControl();
    m_pendingScrollTarget = nullptr;
    m_root->layout(renderer);
  }
}
