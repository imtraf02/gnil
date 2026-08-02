#include "shell/control_center/tabs/home_tab.h"

#include "config/config_service.h"
#include "compositors/compositor_platform.h"
#include "core/build_info.h"
#include "core/deferred_call.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "cursor-shape-v1-client-protocol.h"
#include "dbus/accounts/accounts_service.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "notification/notifications.h"
#include "render/animation/animation_manager.h"
#include "pipewire/pipewire_service.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "shell/profile/avatar_path.h"
#include "shell/wallpaper/wallpaper.h"
#include "system/brightness_service.h"
#include "system/distro_info.h"
#include "system/gamma_service.h"
#include "system/hardware_info.h"
#include "system/format_units.h"
#include "system/system_monitor_service.h"
#include "system/weather_service.h"
#include "time/time_format.h"
#include "ui/builders/actions.h"
#include "ui/builders/display.h"
#include "ui/builders/input.h"
#include "ui/builders/layout.h"
#include "ui/controls/grid_view.h"
#include "ui/controls/progress_bar.h"
#include "ui/controls/select.h"
#include "ui/controls/slider.h"
#include "ui/controls/toggle.h"
#include "ui/dialogs/file_dialog.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/statvfs.h>

using namespace control_center;

namespace {

  constexpr Logger kLog("control-center");

  constexpr float kHomeAvatarScale = 2.0f;
  constexpr float kHomeWideMinWidth = 960.0f;
  constexpr float kHomeWideMinHeight = 500.0f;
  constexpr float kHomeCompactMaxWidth = 760.0f;
  constexpr auto kHomeTransientPositionRegressionWindow = std::chrono::milliseconds(1500);
  constexpr std::int64_t kHomeTransientPositionRegressionFloorUs = 5'000'000;
  constexpr std::int64_t kHomeTransientPositionRegressionCeilingUs = 1'500'000;
  constexpr std::int64_t kHomeTransientPositionRegressionDeltaUs = 5'000'000;
  constexpr int kHomeMediaArtLayoutPassLimit = 8;

  float homeAvatarSize(float scale) { return Style::controlHeightLg * kHomeAvatarScale * scale; }

  std::filesystem::path avatarStartDirectory(const AccountsService* accounts, const ConfigService* config) {
    const std::string currentPath =
        config != nullptr ? shell::resolvedAvatarPath(accounts, config->config()) : std::string{};
    const std::filesystem::path current(currentPath);
    std::error_code ec;
    if (!current.empty() && std::filesystem::exists(current, ec) && current.has_parent_path()) {
      return current.parent_path();
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
      return std::filesystem::path(home) / "Pictures";
    }
    return {};
  }

  void openControlCenterTab(std::string_view tab) {
    PanelManager::instance().togglePanel("dashboard", PanelOpenRequest{.context = tab});
  }

  std::string formatShellTime(const ConfigService* config) {
    const char* format = config != nullptr ? config->config().shell.timeFormat.c_str() : "{:%H:%M}";
    return formatLocalTime(format);
  }

  std::string formatShellDate(const ConfigService* config) {
    const char* format = "%A, %B %-d, %Y";
    if (config != nullptr && config->config().shell.dateFormat != "%A, %x"
        && !config->config().shell.dateFormat.empty()) {
      format = config->config().shell.dateFormat.c_str();
    }
    return formatLocalTime(format);
  }

  std::string userHostLine() { return std::format("{}@{}", sessionDisplayName(), hostName()); }

  std::string gnilVersionLine() { return std::format("GNIL {}", gnil::build_info::displayVersion()); }

  struct StorageUsage {
    double usedBytes = 0.0;
    double totalBytes = 0.0;
    float percent = 0.0f;
  };

  std::optional<StorageUsage> readRootStorageUsage() {
    struct statvfs stats {};
    if (statvfs("/", &stats) != 0 || stats.f_blocks == 0) {
      return std::nullopt;
    }
    const auto blockSize = static_cast<double>(stats.f_frsize != 0 ? stats.f_frsize : stats.f_bsize);
    if (blockSize <= 0.0) {
      return std::nullopt;
    }
    const double totalBytes = static_cast<double>(stats.f_blocks) * blockSize;
    const double availableBytes = static_cast<double>(stats.f_bavail) * blockSize;
    const double usedBytes = std::max(0.0, totalBytes - availableBytes);
    return StorageUsage{
        .usedBytes = usedBytes,
        .totalBytes = totalBytes,
        .percent = static_cast<float>(std::clamp(usedBytes / totalBytes * 100.0, 0.0, 100.0)),
    };
  }

  void applyHomeCardStyle(Flex& card, float scale, float fillOpacity, bool showBorder) {
    applySectionCardStyle(card, scale, fillOpacity, showBorder);
    card.setGap(Style::spaceSm * scale);
    card.setPadding(Style::cardPadding * scale);
  }

  void applyShortcutButtonStyle(Button& button, bool enabled, bool active, float fillOpacity) {
    const bool on = enabled && active;
    button.setVariant(on ? ButtonVariant::Primary : ButtonVariant::Default);
    button.setSurfaceOpacity(on ? 1.0f : fillOpacity);
    button.setEnabled(enabled);
  }

  // The whole home cards are clickable; on hover swap the card outline to a subtle highlight.
  void applyHomeCardHover(Flex& card, bool hovered, bool baseBorders) {
    if (hovered) {
      card.setBorder(colorSpecFromRole(ColorRole::Hover, 0.6f), Style::borderWidth);
    } else if (baseBorders) {
      card.setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
    } else {
      card.clearBorder();
    }
  }

  void applyAvatarChrome(Image* avatar, bool highlighted) {
    if (avatar == nullptr) {
      return;
    }
    const float borderWidth = Style::borderWidth * 3.0f;
    if (highlighted) {
      avatar->setBorder(colorSpecFromRole(ColorRole::Hover), borderWidth);
      return;
    }
    avatar->setBorder(colorSpecFromRole(ColorRole::Primary), borderWidth);
  }

} // namespace

HomeTab::HomeTab(const ControlCenterServices& services)
    : m_mpris(services.mpris), m_httpClient(services.httpClient), m_weather(services.weather),
      m_config(services.config), m_accounts(services.accounts), m_upower(services.upower),
      m_wallpaper(services.wallpaper),
      m_thumbnails(services.thumbnails), m_sysmon(services.sysmon), m_services(services.shortcutServices()),
      m_audio(services.audio), m_brightness(services.brightness), m_nightLight(services.nightLight),
      m_platform(services.platform) {
  if (m_config != nullptr) {
    m_pendingTemperature = m_config->config().nightlight.nightTemperature;
  }
  if (m_thumbnails != nullptr) {
    m_thumbnailPendingSub = m_thumbnails->subscribePendingUpload([this]() {
      if (m_wallpaperBg == nullptr) {
        return;
      }
      PanelManager::instance().requestUpdateOnly();
    });
  }

  // Pre-warm the wallpaper preview thumbnail as soon as the wallpaper changes,
  // even while the control center is closed, so it is already decoded by the
  // time the panel opens. Uses the last preview size the home card requested.
  if (m_wallpaper != nullptr) {
    m_wallpaperChangedConn = m_wallpaper->changed().connect([this]() {
      if (m_thumbnails != nullptr && m_loadedWallpaperSize > 0) {
        ensureWallpaperThumbnail(m_wallpaper->currentPath(), m_loadedWallpaperSize);
      }
      if (m_wallpaperBg != nullptr) {
        PanelManager::instance().requestUpdateOnly();
      }
    });
  }
}

HomeTab::~HomeTab() {
  if (m_thumbnails != nullptr && !m_loadedWallpaperPath.empty()) {
    m_thumbnails->release(m_loadedWallpaperPath, m_loadedWallpaperSize);
  }
}

std::unique_ptr<Flex> HomeTab::create() {
  const float scale = contentScale();
  const std::string displayName = sessionDisplayName();

  // Keep the dashboard dense: the main area is split into three stacked rows while the
  // hardware column runs alongside it from top to bottom.
  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .fillHeight = true,
  });

  auto contentRow = ui::row({
      .out = &m_contentRow,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
      .fillHeight = true,
      .flexGrow = 1.0f,
  });

  // ================= Main area: time/profile, media/shortcuts, system stats =================
  auto mainColumn = ui::column({
      .out = &m_mainColumn,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
      .fillHeight = true,
      .flexGrow = 2.85f,
  });

  // --- Date/Time + Weather ---
  auto dateTimeCard = ui::column({
      .out = &m_dateTimeCard,
      .align = FlexAlign::Stretch,
      .justify = FlexJustify::Center,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
      .fillHeight = true,
      .flexGrow = 0.8f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applyHomeCardStyle(card, scale, opacity, borders);
      },
  });
  dateTimeCard->addChild(ui::label({
      .out = &m_timeLabel,
      .text = formatShellTime(m_config),
      .fontSize = Style::fontSizeTitle * 2.0f * scale,
      .fontWeight = FontWeight::Bold,
      .color = colorSpecFromRole(ColorRole::Primary),
      .textAlign = TextAlign::Center,
  }));
  dateTimeCard->addChild(ui::label({
      .out = &m_dateLabel,
      .text = formatShellDate(m_config),
      .fontSize = Style::fontSizeBody * scale,
      .color = colorSpecFromRole(ColorRole::OnSurface),
      .textAlign = TextAlign::Center,
  }));
  dateTimeCard->addChild(ui::row(
      {.out = &m_weatherLocationRow,
       .align = FlexAlign::Center,
       .justify = FlexJustify::Center,
       .gap = Style::spaceXs * scale,
       .visible = false},
      ui::glyph({
          .glyph = "map-pin",
          .glyphSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      }),
      ui::label({
          .out = &m_locationLabel,
          .text = i18n::tr("control-center.weather.no-location-title"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
      })
  ));
  addDivider(*dateTimeCard, scale);
  dateTimeCard->addChild(ui::row(
      {.align = FlexAlign::Center, .justify = FlexJustify::SpaceBetween, .gap = Style::spaceMd * scale},
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
          ui::glyph({
              .out = &m_weatherGlyph,
              .glyph = "weather-cloud-sun",
              .glyphSize = Style::fontSizeTitle * scale,
              .color = colorSpecFromRole(ColorRole::Primary),
          }),
          ui::label({
              .out = &m_weatherLine,
              .text = "—",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 2,
          })
      ),
      ui::row(
          {.out = &m_humidityRow, .align = FlexAlign::Center, .gap = Style::spaceXs * scale, .visible = false},
          ui::glyph({
              .glyph = "droplet",
              .glyphSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::Secondary),
          }),
          ui::label({
              .out = &m_humidityLabel,
              .text = "—",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      )
  ));
  addDivider(*dateTimeCard, scale);
  dateTimeCard->addChild(ui::row(
      {.out = &m_sunsetRow,
       .align = FlexAlign::Center,
       .justify = FlexJustify::Center,
       .gap = Style::spaceXs * scale,
       .visible = false},
      ui::glyph({
          .glyph = "sunset",
          .glyphSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Tertiary),
      }),
      ui::label({
          .out = &m_sunsetLabel,
          .text = "—",
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  ));

  // Clicking anywhere on the clock/weather card opens the weather tab.
  m_dateTimeCardArea = addCardOverlay(*m_dateTimeCard, []() { openControlCenterTab("weather"); });

  // --- Media ---
  auto mediaCard = ui::column({
      .out = &m_mediaCard,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceXs * scale,
      .fillWidth = true,
      .fillHeight = true,
      .flexGrow = 0.95f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applyHomeCardStyle(card, scale, opacity, borders);
      },
  });
  mediaCard->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceXs * scale, .fillWidth = true},
      ui::label({
          .text = i18n::tr("dashboard.home.media.now-playing"),
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0f,
      }),
      ui::button({
          .glyph = "arrow-right",
          .glyphSize = Style::fontSizeCaption * scale,
          .controlHeight = Style::controlHeightSm * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("dashboard.home.media.open"),
          .minWidth = Style::controlHeightSm * scale,
          .onClick = []() { openControlCenterTab("media"); },
      })
  ));

  const float artSize = Style::controlHeightLg * 1.22f * scale;
  auto mediaContent = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::column(
          {.out = &m_mediaArtSlot,
           .align = FlexAlign::Center,
           .justify = FlexJustify::Center,
           .width = artSize,
           .height = artSize},
          ui::glyph({
              .out = &m_mediaArtFallback,
              .glyph = "disc-filled",
              .glyphSize = artSize * 0.55f,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          }),
          ui::image({
              .out = &m_mediaArt,
              .fit = ImageFit::Cover,
              .radius = Style::scaledRadiusLg(scale),
              .width = artSize,
              .height = artSize,
              .participatesInLayout = false,
              .configure = [](Image& image) { image.setZIndex(1); },
          })
      ),
      ui::column(
          {.out = &m_mediaText, .align = FlexAlign::Stretch, .gap = Style::spaceXs * 0.5f * scale, .flexGrow = 1.0f},
          ui::label({
              .out = &m_mediaTrack,
              .text = "...",
              .fontSize = Style::fontSizeBody * 0.95f * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
          }),
          ui::label({
              .out = &m_mediaArtist,
              .text = i18n::tr("control-center.home.media.no-active-player"),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          }),
          ui::label({
              .out = &m_mediaStatus,
              .text = i18n::tr("control-center.home.media.idle"),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          }),
          ui::label({
              .out = &m_mediaProgress,
              .text = " ",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::Secondary),
              .visible = false,
          })
      )
  );
  mediaCard->addChild(std::move(mediaContent));

  mediaCard->addChild(ui::slider({
      .out = &m_mediaSeekSlider,
      .minValue = 0.0,
      .maxValue = 1.0,
      .step = 0.001,
      .value = 0.0,
      .enabled = false,
      .trackHeight = Style::sliderTrackHeight * scale,
      .thumbSize = Style::sliderThumbSize * 0.8f * scale,
      .controlHeight = Style::controlHeightSm * scale,
      .tooltip = i18n::tr("dashboard.home.media.seek"),
      .onValueChanged = [this](double value) {
        if (m_syncingMediaSeek || m_mpris == nullptr) {
          return;
        }
        const auto active = m_mpris->activePlayer();
        if (active.has_value() && active->canSeek && active->lengthUs > 0) {
          (void)m_mpris->setPositionActive(static_cast<std::int64_t>(value * static_cast<double>(active->lengthUs)));
        }
      },
  }));
  mediaCard->addChild(ui::row(
      {.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .out = &m_mediaShuffleButton,
          .glyph = "shuffle",
          .controlHeight = Style::controlHeight * scale,
          .enabled = false,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("dashboard.home.media.shuffle"),
          .minWidth = Style::controlHeight * scale,
          .onClick = [this]() {
            if (m_mpris != nullptr) {
              const auto shuffle = m_mpris->shuffleActive();
              (void)m_mpris->setShuffleActive(!shuffle.value_or(false));
            }
          },
      }),
      ui::button({
          .out = &m_mediaPreviousButton,
          .glyph = "player-skip-back-filled",
          .controlHeight = Style::controlHeight * scale,
          .enabled = false,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("dashboard.home.media.previous"),
          .minWidth = Style::controlHeight * scale,
          .onClick = [this]() {
            if (m_mpris != nullptr) {
              (void)m_mpris->previousActive();
            }
          },
      }),
      ui::button({
          .out = &m_mediaPlayButton,
          .glyph = "player-play-filled",
          .controlHeight = Style::controlHeight * scale,
          .enabled = false,
          .variant = ButtonVariant::Primary,
          .tooltip = i18n::tr("dashboard.home.media.play"),
          .minWidth = Style::controlHeight * scale,
          .onClick = [this]() {
            if (m_mpris != nullptr) {
              (void)m_mpris->playPauseActive();
            }
          },
      }),
      ui::button({
          .out = &m_mediaNextButton,
          .glyph = "player-skip-forward-filled",
          .controlHeight = Style::controlHeight * scale,
          .enabled = false,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("dashboard.home.media.next"),
          .minWidth = Style::controlHeight * scale,
          .onClick = [this]() {
            if (m_mpris != nullptr) {
              (void)m_mpris->nextActive();
            }
          },
      })
  ));
  // Preserve a keyboard-level card affordance without covering the transport controls.
  m_mediaCardArea = addCardOverlay(
      *m_mediaCard, []() { openControlCenterTab("media"); }, {.keyboardFocus = true, .pointerHitTest = false}
  );

  // --- User card ---
  auto userCard = ui::column({
      .out = &m_userCard,
      .justify = FlexJustify::Center,
      .fillHeight = true,
      .flexGrow = 1.2f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applyHomeCardStyle(card, scale, opacity, borders);
      },
  });

  {
    const float wallpaperRadius = std::max(0.0f, Style::scaledRadiusXl(scale) - Style::borderWidth);
    userCard->addChild(
        ui::image({
            .out = &m_wallpaperPlaceholder,
            .fit = ImageFit::Cover,
            .radius = wallpaperRadius,
            .participatesInLayout = false,
            .configure = [](Image& image) { image.setZIndex(-2); },
        })
    );

    userCard->addChild(
        ui::image({
            .out = &m_wallpaperBg,
            .fit = ImageFit::Cover,
            .radius = wallpaperRadius,
            .participatesInLayout = false,
            .configure = [](Image& image) {
              image.setZIndex(-1);
              image.setOpacity(0.0f);
            },
        })
    );

    userCard->addChild(
        ui::box({
            .out = &m_wallpaperGradient,
            .participatesInLayout = false,
            .configure = [](Box& box) { box.setZIndex(-1); },
        })
    );

    userCard->addChild(
        ui::box({
            .out = &m_userDetailsSurface,
            .participatesInLayout = false,
            .configure = [scale](Box& box) {
              box.setFill(colorSpecFromRole(ColorRole::Surface, 0.78f));
              box.setBorder(colorSpecFromRole(ColorRole::Outline, 0.55f), Style::borderWidth);
              box.setRadius(Style::scaledRadiusMd(scale));
              box.setZIndex(0);
            },
        })
    );
  }

  const float avatarSize = homeAvatarSize(scale);
  const auto openAvatarPicker = [this]() {
    if (m_config == nullptr) {
      return;
    }

    FileDialogOptions options;
    options.mode = FileDialogMode::Open;
    options.defaultViewMode = FileDialogViewMode::Grid;
    options.title = i18n::tr("control-center.home.select-avatar");
    options.extensions = {".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif"};
    options.startDirectory = avatarStartDirectory(m_accounts, m_config);

    (void)FileDialog::open(std::move(options), [this](std::optional<std::filesystem::path> pickedPath) {
      if (!pickedPath.has_value() || m_config == nullptr) {
        return;
      }
      const auto applyResult = shell::applyAvatarPath(m_accounts, m_config, pickedPath->string());
      if (applyResult.success()) {
        m_loadedAvatarPath.clear();
        DeferredCall::callLater([]() {
          PanelManager::instance().refresh();
          PanelManager::instance().requestRedraw();
        });
        return;
      }
      notify::error(
          "GNIL", i18n::tr("control-center.home.avatar-error-title"),
          i18n::tr(shell::avatarApplyErrorTranslationKey(applyResult.error))
      );
    });
  };

  auto avatarArea = std::make_unique<InputArea>();
  avatarArea->setSize(avatarSize, avatarSize);
  avatarArea->setHitShape(InputArea::HitShape::Circle);
  avatarArea->setFocusable(true);
  avatarArea->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
  avatarArea->setOnClick([openAvatarPicker](const InputArea::PointerData&) { openAvatarPicker(); });
  avatarArea->setOnKeyDown([openAvatarPicker](const InputArea::KeyData& key) {
    if (key.pressed && KeybindMatcher::matches(KeybindAction::Validate, key.sym, key.modifiers)) {
      openAvatarPicker();
    }
  });
  m_userAvatarArea = avatarArea.get();
  avatarArea->addChild(
      ui::image({
          .out = &m_userAvatar,
          .fit = ImageFit::Cover,
          .radius = avatarSize * 0.5f,
          .padding = 1.0f * scale,
          .width = avatarSize,
          .height = avatarSize,
          .configure = [](Image& image) {
            image.setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth * 3.0f);
            image.setHitTestVisible(false);
          },
      })
  );
  const auto syncAvatarChrome = [this]() {
    const bool highlighted =
        m_userAvatarArea != nullptr && (m_userAvatarArea->focused() || m_userAvatarArea->hovered());
    applyAvatarChrome(m_userAvatar, highlighted);
    PanelManager::instance().requestRedraw();
  };
  avatarArea->setOnEnter([syncAvatarChrome](const InputArea::PointerData&) { syncAvatarChrome(); });
  avatarArea->setOnLeave([syncAvatarChrome]() { syncAvatarChrome(); });
  avatarArea->setOnFocusGain(syncAvatarChrome);
  avatarArea->setOnFocusLoss(syncAvatarChrome);
  auto userRow = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceMd * scale}, std::move(avatarArea),
      ui::column(
          {.out = &m_userMain,
           .align = FlexAlign::Stretch,
           .justify = FlexJustify::Center,
           .gap = Style::spaceXs * 0.5f * scale,
           .minHeight = avatarSize,
           .width = 0.0f,
           .height = avatarSize,
           .flexGrow = 1.0f},
          ui::label({
              .text = displayName,
              .fontSize = Style::fontSizeTitle * 1.12f * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::OnSurface),
          }),
          ui::label({
              .out = &m_userHost,
              .text = userHostLine(),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          }),
          ui::label({
              .out = &m_userUptime,
              .text = "…",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          }),
          ui::label({
              .out = &m_userVersion,
              .text = gnilVersionLine(),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      )
  );
  userCard->addChild(std::move(userRow));

  // Wallpaper panel: full-card keyboard target; carved pointer target leaves the avatar clickable.
  const auto openWallpaperPanel = []() { PanelManager::instance().togglePanel("wallpaper"); };
  m_userCardKeyboardArea =
      addCardOverlay(*m_userCard, openWallpaperPanel, {.keyboardFocus = true, .pointerHitTest = false});
  m_userCardArea = addCardOverlay(*m_userCard, openWallpaperPanel, {.keyboardFocus = false, .pointerHitTest = true});

  // --- Shortcuts ---
  std::vector<ShortcutConfig> shortcuts;
  shortcuts.push_back({"wifi"});
  shortcuts.push_back({"bluetooth"});
  shortcuts.push_back({"dark_mode"});
  shortcuts.push_back({"caffeine"});
  shortcuts.push_back({"audio"});
  shortcuts.push_back({"nightlight"});

  const std::size_t count = shortcuts.size();

  auto grid = std::make_unique<GridView>();
  grid->setColumns(3);
  grid->setColumnGap(Style::spaceSm * scale);
  grid->setRowGap(Style::spaceSm * scale);
  grid->setPadding(0.0f);
  grid->setUniformCellSize(true);
  grid->setStretchItems(true);
  grid->setSquareCells(false);
  grid->setMinCellHeight(0.0f);
  grid->setFlexGrow(1.35f);
  m_shortcutsGrid = grid.get();
  m_shortcutPads.clear();

  for (std::size_t i = 0; i < count; ++i) {
    const auto& sc = shortcuts[i];
    std::unique_ptr<Shortcut> shortcut = ShortcutRegistry::create(sc.type, m_services);
    if (shortcut == nullptr) {
      continue;
    }

    const std::string label = shortcut->displayLabel();
    const bool enabled = shortcut->enabled();
    const bool isActive = shortcut->isToggle() && shortcut->active();

    const std::size_t padIdx = m_shortcutPads.size();
    auto btn = ui::button({
        .text = label,
        .glyph = shortcut->displayIcon(),
        .glyphSize = Style::fontSizeTitle * 1.1f * scale,
        .tooltip = label,
        .minHeight = 64.0f * scale,
        .padding = (Style::spaceSm - 2.0f) * scale,
        .gap = Style::spaceXs * scale,
        .radius = Style::scaledRadiusLg(scale),
        .onClick =
            [this, padIdx]() {
              if (padIdx < m_shortcutPads.size()) {
                m_shortcutPads[padIdx].shortcut->onClick();
              }
            },
        .onRightClick =
            [this, padIdx]() {
              if (padIdx < m_shortcutPads.size()) {
                m_shortcutPads[padIdx].shortcut->onRightClick();
              }
            },
        .configure =
            [enabled, isActive, fillOpacity = panelCardOpacity(), scale](Button& button) {
              button.setAlign(FlexAlign::Stretch);
              button.label()->setFontSize(Style::fontSizeMini * scale);
              button.label()->setMaxLines(2);
              button.label()->setTextAlign(TextAlign::Center);
              button.setDirection(FlexDirection::Vertical);
              applyShortcutButtonStyle(button, enabled, isActive, fillOpacity);
            },
    });

    Button* btnPtr = btn.get();
    if (auto* ia = btnPtr->inputArea(); ia != nullptr) {
      ia->setOnAxisHandler([this, padIdx](const InputArea::PointerData& data) -> bool {
        if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL || padIdx >= m_shortcutPads.size()) {
          return false;
        }
        const float steps = data.scrollSteps();
        if (steps == 0.0f) {
          return false;
        }
        m_shortcutPads[padIdx].shortcut->onScroll(steps > 0.0f ? -1 : 1);
        return true;
      });
    }
    ShortcutPad pad;
    pad.shortcut = std::move(shortcut);
    pad.button = btnPtr;
    pad.glyph = btnPtr->glyph();
    pad.label = btnPtr->label();
    m_shortcutPads.push_back(std::move(pad));
    grid->addChild(std::move(btn));
  }

  auto shortcutCard = ui::column({
      .out = &m_shortcutCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
      .fillHeight = true,
      .flexGrow = 1.15f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applyHomeCardStyle(card, scale, opacity, borders);
      },
  });
  shortcutCard->addChild(ui::label({
      .text = i18n::tr("dashboard.home.shortcuts.title"),
      .fontSize = Style::fontSizeBody * scale,
      .fontWeight = FontWeight::Bold,
      .color = colorSpecFromRole(ColorRole::OnSurface),
  }));
  shortcutCard->addChild(std::move(grid));

  auto topRow = ui::row({
      .out = &m_topRow,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
      .fillHeight = true,
      .flexGrow = 0.82f,
  });
  topRow->addChild(std::move(dateTimeCard));
  topRow->addChild(std::move(userCard));

  auto lowerRow = ui::row({
      .out = &m_lowerRow,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
      .fillHeight = true,
      .flexGrow = 1.35f,
  });
  auto centerColumn = ui::column({
      .out = &m_centerColumn,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
      .fillHeight = true,
      .flexGrow = 1.38f,
  });
  centerColumn->addChild(std::move(shortcutCard));
  lowerRow->addChild(std::move(mediaCard));
  lowerRow->addChild(std::move(centerColumn));
  mainColumn->addChild(std::move(topRow));
  mainColumn->addChild(std::move(lowerRow));

  // ================= Right column: hardware sliders + anniversary =================
  auto rightColumn = ui::column({
      .out = &m_rightColumn,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .minWidth = 244.0f * scale,
      .fillWidth = true,
      .fillHeight = true,
      .flexGrow = 1.0f,
  });

  // --- Volume Card ---
  auto volumeCard = ui::column({
      .out = &m_volumeCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .minHeight = 204.0f * scale,
      .fillWidth = true,
      .flexGrow = 1.35f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applyHomeCardStyle(card, scale, opacity, borders);
      },
  });

  auto volumeHeader = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::label({
          .text = i18n::tr("dashboard.home.audio.title"),
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0f,
      }),
      ui::label({
          .out = &m_volumeValueLabel,
          .text = "—",
          .fontSize = Style::fontSizeMini * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  volumeCard->addChild(std::move(volumeHeader));

  auto volumeSlider = ui::slider({
      .out = &m_volumeSlider,
      .minValue = 0.0f,
      .maxValue = 1.0f,
      .step = 0.01f,
      .value = 1.0f,
      .presentation = SliderPresentation::LevelCompact,
      .glyph = "volume-high",
      .glyphSize = Style::fontSizeBody * scale,
      .trackHeight = 18.0f * scale,
      .thumbSize = 28.0f * scale,
      .controlHeight = 40.0f * scale,
      .tooltip = i18n::tr("dashboard.home.audio.volume"),
      .wheelAdjustEnabled = true,
      .onValueChanged = [this](double value) {
        if (m_syncingVolumeSlider || m_audio == nullptr) {
          return;
        }
        if (m_volumeValueLabel != nullptr) {
          m_volumeValueLabel->setText(std::format("{:.0f}%", value * 100.0));
        }
        m_audio->setVolume(static_cast<float>(value));
      },
  });
  volumeCard->addChild(std::move(volumeSlider));
  volumeCard->addChild(ui::select({
      .out = &m_outputSelect,
      .options = std::vector<std::string>{},
      .placeholder = i18n::tr("dashboard.home.audio.no-output"),
      .fontSize = Style::fontSizeMini * scale,
      .controlHeight = Style::controlHeightSm * scale,
      .enabled = false,
      .onSelectionChanged = [this](std::size_t index, std::string_view) {
        if (m_audio == nullptr || index >= m_audio->state().sinks.size()) {
          return;
        }
        m_audio->setDefaultSink(m_audio->state().sinks[index].id);
      },
  }));
  volumeCard->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::label({
          .text = i18n::tr("dashboard.home.audio.microphone"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0f,
      }),
      ui::label({
          .out = &m_microphoneValueLabel,
          .text = "—",
          .fontSize = Style::fontSizeMini * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  ));
  volumeCard->addChild(ui::slider({
      .out = &m_microphoneSlider,
      .minValue = 0.0f,
      .maxValue = 1.0f,
      .step = 0.01f,
      .value = 1.0f,
      .presentation = SliderPresentation::LevelCompact,
      .glyph = "microphone",
      .glyphSize = Style::fontSizeBody * scale,
      .trackHeight = 18.0f * scale,
      .thumbSize = 28.0f * scale,
      .controlHeight = 40.0f * scale,
      .tooltip = i18n::tr("dashboard.home.audio.microphone-value"),
      .wheelAdjustEnabled = true,
      .onValueChanged = [this](double value) {
        if (m_syncingMicrophoneSlider || m_audio == nullptr) {
          return;
        }
        if (m_microphoneValueLabel != nullptr) {
          m_microphoneValueLabel->setText(std::format("{:.0f}%", value * 100.0));
        }
        m_audio->setMicVolume(static_cast<float>(value));
      },
  }));
  volumeCard->addChild(ui::button({
      .out = &m_muteAllButton,
      .text = i18n::tr("dashboard.home.audio.mute-all"),
      .glyph = "volume-off",
      .controlHeight = Style::controlHeight * scale,
      .variant = ButtonVariant::Outline,
      .onClick = [this]() {
        if (m_audio == nullptr) {
          return;
        }
        const AudioNode* sink = m_audio->defaultSink();
        const AudioNode* source = m_audio->defaultSource();
        const bool bothMuted = sink != nullptr && source != nullptr && sink->muted && source->muted;
        if (sink != nullptr) {
          m_audio->setMuted(!bothMuted);
        }
        if (source != nullptr) {
          m_audio->setMicMuted(!bothMuted);
        }
      },
  }));

  // --- Brightness Card ---
  auto brightnessCard = ui::column({
      .out = &m_brightnessCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .minHeight = 148.0f * scale,
      .fillWidth = true,
      .flexGrow = 1.0f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applyHomeCardStyle(card, scale, opacity, borders);
      },
  });

  auto brightnessHeader = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::label({
          .text = i18n::tr("settings.widgets.types.brightness"),
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0f,
      }),
      ui::label({
          .out = &m_brightnessValueLabel,
          .text = "—",
          .fontSize = Style::fontSizeMini * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  brightnessCard->addChild(std::move(brightnessHeader));

  float minBrightness = 0.0f;
  if (m_config != nullptr) {
    minBrightness = m_config->config().brightness.minimumBrightness;
  }
  auto brightnessSlider = ui::slider({
      .out = &m_brightnessSlider,
      .minValue = minBrightness,
      .maxValue = 1.0f,
      .step = 0.01f,
      .value = 1.0f,
      .presentation = SliderPresentation::LevelCompact,
      .glyph = "brightness-high",
      .glyphSize = Style::fontSizeBody * scale,
      .trackHeight = 18.0f * scale,
      .thumbSize = 28.0f * scale,
      .controlHeight = 40.0f * scale,
      .tooltip = i18n::tr("settings.widgets.types.brightness"),
      .wheelAdjustEnabled = true,
      .onValueChanged = [this](double value) {
        if (m_syncingBrightnessSlider || m_brightness == nullptr) {
          return;
        }
        const auto& displays = m_brightness->displays();
        for (const auto& display : displays) {
          if (display.controllable) {
            m_brightness->setBrightness(display.id, static_cast<float>(value));
          }
        }
        if (m_brightnessValueLabel != nullptr) {
          m_brightnessValueLabel->setText(std::format("{:.0f}%", value * 100.0));
        }
      },
  });
  brightnessCard->addChild(std::move(brightnessSlider));
  const bool gammaAvailable = m_platform != nullptr && m_platform->hasGammaControl();
  brightnessCard->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .visible = gammaAvailable,
       .participatesInLayout = gammaAvailable},
      ui::glyph({
          .glyph = "weather-moon-stars",
          .glyphSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::Primary),
      }),
      ui::column(
          {.align = FlexAlign::Stretch, .gap = 0.0f, .flexGrow = 1.0f},
          ui::label({
              .text = i18n::tr("control-center.shortcuts.nightlight"),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
          }),
          ui::label({
              .text = i18n::tr("dashboard.home.brightness.nightlight-description"),
              .fontSize = Style::fontSizeMini * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      ),
      ui::toggle({
          .out = &m_nightLightToggle,
          .checked = m_nightLight != nullptr && m_nightLight->forceEnabled(),
          .enabled = m_nightLight != nullptr,
          .scale = scale,
          .onChange = [this](bool force) {
            if (!m_syncingNightLight && m_nightLight != nullptr) {
              if (m_config != nullptr) {
                (void)m_config->setOverrides({
                    {{"nightlight", "enabled"}, force},
                    {{"nightlight", "force"}, force},
                });
              } else {
                m_nightLight->setEnabled(force);
                m_nightLight->setForceEnabled(force);
              }
            }
          },
      })
  ));
  brightnessCard->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .visible = gammaAvailable,
       .participatesInLayout = gammaAvailable},
      ui::glyph({
          .glyph = "temperature-sun",
          .glyphSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Tertiary),
      }),
      ui::slider({
          .out = &m_temperatureSlider,
          .minValue = NightLightConfig::kTemperatureMin,
          .maxValue = NightLightConfig::kTemperatureMax,
          .step = 100.0,
          .value = m_config != nullptr ? m_config->config().nightlight.nightTemperature : 4000,
          .controlHeight = 40.0f * scale,
          .tooltip = i18n::tr("dashboard.home.brightness.nightlight-description"),
          .flexGrow = 1.0f,
          .onValueChanged = [this](double value) {
            if (m_syncingTemperature) {
              return;
            }
            m_pendingTemperature = static_cast<std::int32_t>(std::lround(value / 100.0) * 100);
            if (m_temperatureValueLabel != nullptr) {
              m_temperatureValueLabel->setText(std::format("{}K", m_pendingTemperature));
            }
          },
          .onDragEnd = [this]() {
            if (m_config == nullptr) {
              return;
            }
            const auto night = static_cast<std::int64_t>(m_pendingTemperature);
            const auto day = std::max<std::int64_t>(
                m_config->config().nightlight.dayTemperature, night + NightLightConfig::kTemperatureGap
            );
            (void)m_config->setOverrides({
                {{"nightlight", "temperature_night"}, night},
                {{"nightlight", "temperature_day"}, day},
            });
          },
      }),
      ui::label({
          .out = &m_temperatureValueLabel,
          .text = "4000K",
          .fontSize = Style::fontSizeMini * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  ));

  // ================= System stats =================
  auto metricsSlot = ui::row({
      .out = &m_bottomRow,
      .align = FlexAlign::Stretch,
      .justify = FlexJustify::Center,
      .fillWidth = true,
      .fillHeight = true,
      .flexGrow = 0.78f,
  });

  m_metricPads.clear();
  auto metricsCard = ui::column({
      .out = &m_metricsCard,
      .align = FlexAlign::Stretch,
      .justify = FlexJustify::Start,
      .gap = Style::spaceXs * scale,
      .fillWidth = true,
      .fillHeight = true,
      .flexGrow = 1.0f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applyHomeCardStyle(card, scale, opacity, borders);
      },
  });
  metricsCard->addChild(ui::label({
      .text = i18n::tr("dashboard.home.metrics.title"),
      .fontSize = Style::fontSizeBody * scale,
      .fontWeight = FontWeight::Bold,
      .color = colorSpecFromRole(ColorRole::OnSurface),
  }));

  auto metricsRail = ui::row({
      .out = &m_metricsRail,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
      .fillHeight = true,
      .flexGrow = 1.0f,
  });

  const auto addFooterMetric = [this, scale](Flex& parent, std::string glyph, ProgressBar** bar, Glyph** iconOut,
                                             std::string name, std::string tooltip) {
    auto metric = ui::column({
        .align = FlexAlign::Center,
        .justify = FlexJustify::SpaceBetween,
        .minWidth = 28.0f * scale,
        .fillWidth = true,
        .fillHeight = true,
        .flexGrow = 1.0f,
    });
    Flex* metricPtr = metric.get();
    metric->addChild(ui::progressBar({
        .out = bar,
        .fill = colorSpecFromRole(ColorRole::Primary),
        .track = colorSpecFromRole(ColorRole::Outline, 0.2f),
        .radius = 3.5f * scale,
        .orientation = ProgressBarOrientation::Vertical,
        .progress = 0.0f,
        .width = 8.0f * scale,
        .height = 56.0f * scale,
    }));
    metric->addChild(ui::glyph({
        .out = iconOut,
        .glyph = std::move(glyph),
        .glyphSize = Style::fontSizeMini * 1.1f * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .width = Style::fontSizeMini * 1.2f * scale,
        .height = Style::fontSizeMini * 1.2f * scale,
    }));
    auto hitArea = std::make_unique<InputArea>();
    hitArea->setParticipatesInLayout(false);
    hitArea->setFocusable(false);
    hitArea->setTabStop(false);
    hitArea->setZIndex(3);
    hitArea->setTooltip(std::move(tooltip));
    InputArea* hitAreaPtr = hitArea.get();
    metric->addChild(std::move(hitArea));
    m_metricPads.push_back(HomeMetricPad{metricPtr, hitAreaPtr, std::move(name)});
    parent.addChild(std::move(metric));
  };

  addFooterMetric(*metricsRail, "cpu-usage", &m_cpuBar, nullptr, "CPU", i18n::tr("dashboard.home.metrics.cpu"));
  addFooterMetric(
      *metricsRail, "battery-4", &m_batteryBar, &m_batteryGlyph, "Battery", i18n::tr("dashboard.home.metrics.battery")
  );
  addFooterMetric(
      *metricsRail, "memory", &m_footerMemoryBar, nullptr, "Memory", i18n::tr("dashboard.home.metrics.memory")
  );
  addFooterMetric(
      *metricsRail, "storage", &m_bottomStorageBar, nullptr, "Storage", i18n::tr("dashboard.home.metrics.storage")
  );
  metricsCard->addChild(std::move(metricsRail));
  metricsSlot->addChild(std::move(metricsCard));
  m_centerColumn->addChild(std::move(metricsSlot));

  auto anniversaryCard = ui::column({
      .out = &m_anniversaryCard,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .gap = Style::spaceXs * scale,
      .minHeight = 96.0f * scale,
      .fillWidth = true,
      .flexGrow = 0.62f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applyHomeCardStyle(card, scale, opacity, borders);
      },
  });
  anniversaryCard->addChild(ui::row(
      {.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceXs * scale},
      ui::glyph({
          .glyph = "heart-filled",
          .glyphSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      }),
      ui::label({
          .text = i18n::trOr("dashboard.home.anniversary.title", "Anniversary"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  ));
  anniversaryCard->addChild(ui::label({
      .text = i18n::trOr("dashboard.home.anniversary.empty", "No anniversary configured"),
      .fontSize = Style::fontSizeMini * scale,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .maxLines = 2,
      .textAlign = TextAlign::Center,
  }));
  anniversaryCard->addChild(ui::button({
      .text = i18n::trOr("dashboard.home.configure-now", "Configure now"),
      .glyph = "settings",
      .fontSize = Style::fontSizeMini * scale,
      .glyphSize = Style::fontSizeMini * scale,
      .controlHeight = Style::controlHeightSm * scale,
      .variant = ButtonVariant::Outline,
      .onClick = []() { PanelManager::instance().openPanel("settings"); },
  }));
  rightColumn->addChild(std::move(volumeCard));
  rightColumn->addChild(std::move(brightnessCard));
  rightColumn->addChild(std::move(anniversaryCard));

  contentRow->addChild(std::move(mainColumn));
  contentRow->addChild(std::move(rightColumn));
  tab->addChild(std::move(contentRow));

  return tab;
}

std::unique_ptr<Flex> HomeTab::createHeaderActions() {
  const float scale = contentScale();
  return ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .out = &m_settingsButton,
          .glyph = "settings",
          .tooltip = i18n::tr("control-center.header.settings"),
          .onClick = []() { PanelManager::instance().openPanel("settings"); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      }),
      ui::button({
          .out = &m_sessionButton,
          .glyph = "shutdown",
          .tooltip = i18n::tr("control-center.header.session"),
          .onClick = []() { PanelManager::instance().togglePanel("session"); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
}

void HomeTab::applyResponsiveLayout(float contentWidth, float bodyHeight) {
  if (m_contentRow == nullptr || m_mainColumn == nullptr || m_topRow == nullptr || m_lowerRow == nullptr
      || m_centerColumn == nullptr || m_rightColumn == nullptr) {
    return;
  }

  const float scale = std::max(0.01f, contentScale());
  const float logicalWidth = contentWidth / scale;
  const float logicalHeight = bodyHeight / scale;
  const LayoutMode nextMode = logicalWidth < kHomeCompactMaxWidth
      ? LayoutMode::Compact
      : (logicalWidth >= kHomeWideMinWidth && logicalHeight >= kHomeWideMinHeight ? LayoutMode::Wide
                                                                                  : LayoutMode::Medium);
  const bool modeChanged = m_layoutModeInitialized && nextMode != m_layoutMode;
  m_layoutMode = nextMode;
  m_layoutModeInitialized = true;

  const bool compact = nextMode == LayoutMode::Compact;
  const bool wide = nextMode == LayoutMode::Wide;

  m_contentRow->setDirection(compact ? FlexDirection::Vertical : FlexDirection::Horizontal);
  m_mainColumn->setFlexGrow(compact ? 2.3f : (wide ? 2.85f : 2.65f));
  m_mainColumn->setMinWidth(0.0f);
  m_topRow->setFlexGrow(wide ? 0.82f : 0.92f);
  m_lowerRow->setFlexGrow(wide ? 1.35f : 1.28f);

  if (m_dateTimeCard != nullptr) {
    m_dateTimeCard->setFlexGrow(wide ? 0.8f : 0.9f);
  }
  if (m_userCard != nullptr) {
    m_userCard->setFlexGrow(wide ? 1.62f : 1.45f);
  }
  if (m_mediaCard != nullptr) {
    m_mediaCard->setFlexGrow(compact ? 0.95f : 1.0f);
  }
  m_centerColumn->setFlexGrow(wide ? 1.38f : 1.24f);
  if (m_shortcutCard != nullptr) {
    m_shortcutCard->setFlexGrow(wide ? 1.15f : 1.0f);
  }
  if (m_bottomRow != nullptr) {
    m_bottomRow->setFlexGrow(wide ? 0.78f : 0.72f);
  }

  m_rightColumn->setDirection(compact ? FlexDirection::Horizontal : FlexDirection::Vertical);
  m_rightColumn->setMinWidth(compact ? 0.0f : 244.0f * scale);
  m_rightColumn->setMinHeight(compact ? 220.0f * scale : 0.0f);
  m_rightColumn->setFlexGrow(compact ? 1.1f : 1.0f);

  if (m_volumeCard != nullptr) {
    m_volumeCard->setMinHeight(204.0f * scale);
    m_volumeCard->setFlexGrow(compact ? 1.1f : 1.35f);
  }
  if (m_brightnessCard != nullptr) {
    m_brightnessCard->setMinHeight(compact ? 204.0f * scale : 148.0f * scale);
    m_brightnessCard->setFlexGrow(1.0f);
  }
  if (m_anniversaryCard != nullptr) {
    m_anniversaryCard->setVisible(!compact);
    m_anniversaryCard->setParticipatesInLayout(!compact);
    m_anniversaryCard->setFlexGrow(0.62f);
  }
  if (m_userVersion != nullptr) {
    m_userVersion->setVisible(!compact);
    m_userVersion->setParticipatesInLayout(!compact);
  }
  if (m_shortcutsGrid != nullptr) {
    m_shortcutsGrid->setColumns(compact ? 2 : 3);
  }

  if (!modeChanged || m_rootLayout == nullptr) {
    return;
  }
  if (AnimationManager* animations = m_rootLayout->animationManager(); animations != nullptr) {
    if (m_layoutModeAnimId != 0) {
      animations->cancel(m_layoutModeAnimId);
    }
    Flex* root = m_rootLayout;
    root->setOpacity(0.92f);
    m_layoutModeAnimId = animations->animate(
        0.92f, 1.0f, static_cast<float>(Style::animNormal), Easing::FluidSpatial,
        [root](float value) { root->setOpacity(value); }, [this]() { m_layoutModeAnimId = 0; }, root
    );
  } else {
    m_rootLayout->setOpacity(1.0f);
  }
}

void HomeTab::resizeMetricBars() {
  if (m_metricsRail == nullptr) {
    return;
  }
  const float scale = contentScale();
  const float iconAndGap = Style::fontSizeMini * 1.2f * scale + Style::spaceXs * scale;
  const float available = std::max(10.0f * scale, m_metricsRail->height() - iconAndGap);
  const float barHeight = std::min(72.0f * scale, available);
  for (ProgressBar* bar : {m_cpuBar, m_batteryBar, m_footerMemoryBar, m_bottomStorageBar}) {
    if (bar != nullptr) {
      bar->setSize(8.0f * scale, barHeight);
    }
  }
}

void HomeTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr) {
    return;
  }

  applyResponsiveLayout(contentWidth, bodyHeight);

  if (m_userAvatar != nullptr && m_userMain != nullptr) {
    const float userMainHeight = std::max(1.0f, m_userAvatar->height());
    m_userMain->setMinHeight(userMainHeight);
    m_userMain->setSize(m_userMain->width(), userMainHeight);
  }

  if (m_shortcutsGrid != nullptr && !m_shortcutPads.empty()) {
    const float scale = contentScale();
    for (auto& pad : m_shortcutPads) {
      if (pad.label == nullptr) {
        continue;
      }
      float inner = 1.0f;
      if (pad.button != nullptr && pad.button->width() > 1.0f) {
        inner = std::max(1.0f, pad.button->width() - pad.button->paddingLeft() - pad.button->paddingRight());
      } else {
        const float gridW = m_shortcutsGrid->width();
        const float innerGrid =
            std::max(1.0f, gridW - m_shortcutsGrid->paddingLeft() - m_shortcutsGrid->paddingRight());
        const std::size_t cols = std::max<std::size_t>(1, std::min(m_shortcutsGrid->columns(), m_shortcutPads.size()));
        const float cellWidth =
            (innerGrid - static_cast<float>(cols - 1) * m_shortcutsGrid->columnGap()) / static_cast<float>(cols);
        inner = std::max(1.0f, cellWidth - 2.0f * Style::spaceSm * scale);
      }
      pad.label->setMaxWidth(inner);
    }
  }

  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
  resizeMetricBars();
  m_rootLayout->layout(renderer);

  const auto innerWidth = [](Flex* card) {
    if (card == nullptr) {
      return 1.0f;
    }
    return std::max(1.0f, card->width() - (card->paddingLeft() + card->paddingRight()));
  };

  const float dateTimeWrap = innerWidth(m_dateTimeCard);
  if (m_timeLabel != nullptr) {
    m_timeLabel->setMaxWidth(dateTimeWrap);
    m_timeLabel->setMaxLines(1);
  }

  const float dateTimeRightWrap = dateTimeWrap;
  if (m_dateLabel != nullptr) {
    m_dateLabel->setMaxWidth(dateTimeRightWrap);
    m_dateLabel->setMaxLines(1);
  }
  if (m_weatherLine != nullptr) {
    const float weatherTextWrap = std::max(
        1.0f,
        dateTimeRightWrap
            - (m_weatherGlyph != nullptr ? m_weatherGlyph->width() : 0.0f)
            - Style::spaceXs * contentScale()
    );
    m_weatherLine->setMaxWidth(weatherTextWrap);
    m_weatherLine->setMaxLines(2);
  }

  resizeMediaArtToCard();

  for (Label* label : {m_mediaArtist, m_mediaStatus, m_mediaProgress}) {
    if (label != nullptr) {
      label->setMaxLines(1);
    }
  }
  if (m_mediaTrack != nullptr) {
    m_mediaTrack->setMaxLines(2);
  }

  if (m_userCard != nullptr) {
    const float userWrap = innerWidth(m_userCard);
    for (Label* label : {m_userHost, m_userUptime, m_userVersion}) {
      if (label != nullptr) {
        label->setMaxWidth(userWrap);
        label->setMaxLines(1);
      }
    }
  }

  if (m_userAvatar != nullptr && m_userMain != nullptr) {
    const float scale = contentScale();
    const float minAvatar = homeAvatarSize(scale);
    const float desiredAvatar = std::max(minAvatar, m_userMain->height());
    if (std::abs(m_userAvatar->width() - desiredAvatar) > 0.5f) {
      m_userAvatar->setSize(desiredAvatar, desiredAvatar);
      m_userAvatar->setRadius(desiredAvatar * 0.5f);
      m_userAvatar->setPadding(1.0f * scale);
    }
    m_userMain->setMinHeight(desiredAvatar);
    m_userMain->setSize(m_userMain->width(), desiredAvatar);
  }

  bool artSizeChanged = false;
  for (int pass = 0; pass < kHomeMediaArtLayoutPassLimit; ++pass) {
    m_rootLayout->layout(renderer);
    artSizeChanged = resizeMediaArtToCard();
    if (!artSizeChanged) {
      break;
    }
  }
  if (artSizeChanged) {
    m_rootLayout->layout(renderer);
  }

  const float scale = contentScale();
  for (const auto& pad : m_metricPads) {
    if (pad.metric == nullptr || pad.hitArea == nullptr) {
      continue;
    }
    pad.hitArea->setPosition(0.0f, 0.0f);
    pad.hitArea->setFrameSize(pad.metric->width(), pad.metric->height());
  }

  if (m_userDetailsSurface != nullptr && m_userCard != nullptr && m_userMain != nullptr) {
    float mainX = 0.0f;
    float mainY = 0.0f;
    float cardX = 0.0f;
    float cardY = 0.0f;
    Node::absolutePosition(m_userMain, mainX, mainY);
    Node::absolutePosition(m_userCard, cardX, cardY);
    const float inset = Style::spaceSm * scale;
    m_userDetailsSurface->setRadius(Style::scaledRadiusMd(scale));
    m_userDetailsSurface->setPosition(std::max(0.0f, mainX - cardX - inset), std::max(0.0f, mainY - cardY - inset));
    m_userDetailsSurface->setFrameSize(m_userMain->width() + inset * 2.0f, m_userMain->height() + inset * 2.0f);
  }

  layoutWallpaperBackground(renderer);
  layoutCardOverlays();
  if (m_weatherGlyph != nullptr) {
    m_weatherGlyph->measure(renderer);
  }
}

InputArea* HomeTab::addCardOverlay(Flex& card, std::function<void()> onActivate) {
  return addCardOverlay(card, std::move(onActivate), CardOverlayOptions{});
}

InputArea* HomeTab::addCardOverlay(Flex& card, std::function<void()> onActivate, CardOverlayOptions options) {
  auto area = std::make_unique<InputArea>();
  area->setParticipatesInLayout(false);
  area->setZIndex(3);
  area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
  if (!options.pointerHitTest) {
    area->setHitTestVisible(false);
  }
  if (options.keyboardFocus) {
    area->setFocusable(true);
  } else {
    area->setFocusable(false);
    area->setTabStop(false);
  }

  Flex* cardPtr = &card;
  const bool borders = panelBordersEnabled();
  InputArea* areaPtr = area.get();
  std::function<void()> activate = std::move(onActivate);

  const auto setHovered = [cardPtr, borders](bool hovered) {
    applyHomeCardHover(*cardPtr, hovered, borders);
    PanelManager::instance().requestRedraw();
  };

  if (options.pointerHitTest) {
    area->setOnEnter([setHovered](const InputArea::PointerData&) { setHovered(true); });
    area->setOnLeave([setHovered, areaPtr]() {
      if (areaPtr->focused()) {
        return;
      }
      setHovered(false);
    });
    area->setOnClick([activate](const InputArea::PointerData&) { activate(); });
  }
  if (options.keyboardFocus) {
    area->setOnFocusGain([setHovered]() { setHovered(true); });
    area->setOnFocusLoss([setHovered, areaPtr]() {
      if (areaPtr->hovered()) {
        return;
      }
      setHovered(false);
    });
    area->setOnKeyDown([activate](const InputArea::KeyData& key) {
      if (key.pressed && KeybindMatcher::matches(KeybindAction::Validate, key.sym, key.modifiers)) {
        activate();
      }
    });
  }

  return static_cast<InputArea*>(card.addChild(std::move(area)));
}

void HomeTab::layoutCardOverlays() {
  const auto cover = [](Flex* card, InputArea* area) {
    if (card == nullptr || area == nullptr) {
      return;
    }
    area->setPosition(0.0f, 0.0f);
    area->setSize(card->width(), card->height());
  };
  cover(m_mediaCard, m_mediaCardArea);
  cover(m_dateTimeCard, m_dateTimeCardArea);
  cover(m_performanceCard, m_performanceCardArea);
  cover(m_userCard, m_userCardKeyboardArea);

  // The pointer overlay must not swallow the avatar's own click, so start it just past the
  // avatar's right edge; the avatar (a nested InputArea) keeps the carved-out left region.
  if (m_userCard != nullptr && m_userCardArea != nullptr) {
    float left = 0.0f;
    if (m_userAvatar != nullptr) {
      float ax = 0.0f, ay = 0.0f, cx = 0.0f, cy = 0.0f;
      Node::absolutePosition(m_userAvatar, ax, ay);
      Node::absolutePosition(m_userCard, cx, cy);
      left = std::max(0.0f, (ax - cx) + m_userAvatar->width() + Style::spaceMd * contentScale());
    }
    m_userCardArea->setPosition(left, 0.0f);
    m_userCardArea->setSize(std::max(0.0f, m_userCard->width() - left), m_userCard->height());
  }
}

bool HomeTab::resizeMediaArtToCard() {
  if (m_mediaCard == nullptr || m_mediaArt == nullptr || m_mediaArtSlot == nullptr) {
    return false;
  }

  const float scale = contentScale();
  const float minArt = Style::controlHeightLg * 1.22f * scale;
  const float maxArt = Style::controlHeightLg * 1.8f * scale;
  const float available =
      std::max(0.0f, m_mediaCard->height() - m_mediaCard->paddingTop() - m_mediaCard->paddingBottom());
  const float desired = std::clamp(available * 0.36f, minArt, maxArt);
  if (std::abs(m_mediaArtSlot->width() - desired) <= 0.5f) {
    return false;
  }

  m_mediaArtSlot->setSize(desired, desired);
  m_mediaArt->setSize(desired, desired);
  m_mediaArt->setRadius(Style::scaledRadiusLg(scale));
  if (m_mediaArtFallback != nullptr) {
    m_mediaArtFallback->setGlyphSize(desired * 0.55f);
  }
  return true;
}

void HomeTab::layoutWallpaperBackground(Renderer& renderer) {
  if (m_userCard == nullptr || m_wallpaperBg == nullptr) {
    return;
  }

  const float bw = Style::borderWidth;
  const float cw = std::max(0.0f, m_userCard->width() - bw * 2.0f);
  const float ch = std::max(0.0f, m_userCard->height() - bw * 2.0f);
  m_wallpaperBg->setPosition(bw, bw);
  m_wallpaperBg->setSize(cw, ch);
  if (m_wallpaperPlaceholder != nullptr) {
    m_wallpaperPlaceholder->setPosition(bw, bw);
    m_wallpaperPlaceholder->setSize(cw, ch);
  }

  if (m_wallpaperGradient != nullptr) {
    const float radius = std::max(0.0f, Style::scaledRadiusXl(contentScale()) - bw);
    m_wallpaperGradient->setPosition(bw, bw);
    m_wallpaperGradient->setFrameSize(cw, ch);
    const Color surface = colorForRole(ColorRole::Surface);
    const Color translucentSurface = rgba(surface.r, surface.g, surface.b, surface.a * 0.9f);
    const Color transparentSurface = rgba(surface.r, surface.g, surface.b, 0.0f);
    m_wallpaperGradient->setStyle(
        RoundedRectStyle{
            .fill = surface,
            .fillMode = FillMode::LinearGradient,
            .gradientDirection = GradientDirection::Vertical,
            .gradientStops =
                {GradientStop{0.0f, transparentSurface}, GradientStop{0.42f, transparentSurface},
                 GradientStop{0.82f, translucentSurface}, GradientStop{1.0f, translucentSurface}},
            .radius = radius,
        }
    );
  }

  syncWallpaperBackground(renderer);
}

void HomeTab::ensureWallpaperThumbnail(const std::string& path, int targetPx) {
  if (m_thumbnails == nullptr) {
    return;
  }
  if (path == m_loadedWallpaperPath && targetPx == m_loadedWallpaperSize) {
    return;
  }
  if (!m_loadedWallpaperPath.empty() && m_loadedWallpaperSize > 0) {
    m_thumbnails->release(m_loadedWallpaperPath, m_loadedWallpaperSize);
  }
  if (!path.empty() && targetPx > 0) {
    (void)m_thumbnails->acquire(path, targetPx);
  }
  m_loadedWallpaperPath = path;
  m_loadedWallpaperSize = targetPx;
}

void HomeTab::syncWallpaperBackground(Renderer& renderer) {
  if (m_wallpaperBg == nullptr || m_wallpaperPlaceholder == nullptr) {
    return;
  }

  const std::string path = m_wallpaper != nullptr ? m_wallpaper->currentPath() : std::string{};
  const float renderScale = std::max(1.0f, renderer.renderScale());
  const int targetPx =
      static_cast<int>(std::lround(std::max(m_wallpaperBg->width(), m_wallpaperBg->height()) * renderScale));

  ensureWallpaperThumbnail(path, targetPx);

  if (path.empty()) {
    m_wallpaperPlaceholder->setVisible(false);
    m_wallpaperBg->setVisible(false);
    cancelCrispFade();
    m_wallpaperBg->setOpacity(0.0f);
    m_crispWorkingPath.clear();
    m_crispWorkingSize = 0;
    m_crispShown = false;
    m_crispNeedsFade = false;
    return;
  }

  // Instant placeholder: show the resident full-screen wallpaper texture (already
  // in VRAM, mipmapped) so the correct wallpaper appears with no decode wait.
  const TextureHandle resident = m_wallpaper != nullptr ? m_wallpaper->currentTexture() : TextureHandle{};
  if (resident.valid()) {
    m_wallpaperPlaceholder->setExternalTexture(renderer, resident);
    m_wallpaperPlaceholder->setVisible(true);
  } else {
    m_wallpaperPlaceholder->setVisible(false);
  }

  // Reset the crisp layer when the wallpaper identity or target size changes; the
  // placeholder carries the view until the new card-sized thumbnail is decoded.
  if (path != m_crispWorkingPath || targetPx != m_crispWorkingSize) {
    m_crispWorkingPath = path;
    m_crispWorkingSize = targetPx;
    m_crispShown = false;
    m_crispNeedsFade = false;
    cancelCrispFade();
    m_wallpaperBg->setOpacity(0.0f);
    m_wallpaperBg->setVisible(false);
  }

  if (m_thumbnails == nullptr || targetPx <= 0 || m_crispShown) {
    return;
  }

  (void)m_thumbnails->uploadPending(renderer.textureManager());
  const TextureHandle crisp = m_thumbnails->peek(path, targetPx);
  if (!crisp.valid()) {
    // Still decoding: keep the placeholder; fade the crisp layer in once it lands.
    m_crispNeedsFade = true;
    return;
  }

  m_wallpaperBg->setExternalTexture(renderer, crisp);
  m_wallpaperBg->setVisible(true);
  m_crispShown = true;
  if (m_crispNeedsFade) {
    startCrispFade();
  } else {
    // Ready on the first look (cached) — snap in without a crossfade.
    cancelCrispFade();
    m_wallpaperBg->setOpacity(1.0f);
  }
}

void HomeTab::startCrispFade() {
  if (m_wallpaperBg == nullptr) {
    return;
  }
  AnimationManager* animations = m_wallpaperBg->animationManager();
  if (animations == nullptr) {
    m_wallpaperBg->setOpacity(1.0f);
    return;
  }
  cancelCrispFade();
  Image* crisp = m_wallpaperBg;
  m_wallpaperCrispAnimId = animations->animate(
      0.0f, 1.0f, static_cast<float>(Style::animNormal), Easing::EaseOutCubic,
      [crisp](float v) { crisp->setOpacity(v); }, [this]() { m_wallpaperCrispAnimId = 0; }, crisp
  );
}

void HomeTab::cancelCrispFade() {
  if (m_wallpaperCrispAnimId != 0 && m_wallpaperBg != nullptr) {
    if (AnimationManager* animations = m_wallpaperBg->animationManager()) {
      animations->cancel(m_wallpaperCrispAnimId);
    }
  }
  m_wallpaperCrispAnimId = 0;
}

void HomeTab::doUpdate(Renderer& renderer) {
  if (!m_active) {
    m_progressTimer.stop();
    return;
  }

  if (!m_progressTimer.active()) {
    m_progressTimer.startRepeating(std::chrono::milliseconds(1000), [this]() {
      if (!m_active) {
        return;
      }
      // The clock, resource summary and MPRIS progress share this low-rate
      // tick. refresh() includes layout so newly decoded artwork gets its
      // final card dimensions without a second resize flash.
      PanelManager::instance().refresh();
      PanelManager::instance().requestRedraw();
    });
  }
  sync(renderer);
}

void HomeTab::onFrameTick(float /*deltaMs*/) {}

void HomeTab::setActive(bool active) {
  const bool becameActive = active && !m_active;
  m_active = active;
  if (!active) {
    m_progressTimer.stop();
    m_nextRealtimeUpdateAt = {};
    m_lastRealtimeMprisPollAt = {};
    m_mediaPositionBusName.clear();
    m_mediaPositionTrackId.clear();
    m_mediaPositionTrackSignature.clear();
    m_mediaLastPlaybackStatus.clear();
    m_mediaPositionUs = 0;
    m_mediaPositionSampleAt = {};
    return;
  }

  if (becameActive) {
    // Other tabs were laid out while this body was hidden; flex sizes for the media row can be stale.
    // Defer so the tab container receives its configure size before HomeTab::doLayout runs.
    DeferredCall::callLater([]() {
      PanelManager::instance().requestLayout();
      PanelManager::instance().requestUpdateOnly();
    });
  }
}

void HomeTab::onClose() {
  m_progressTimer.stop();
  m_rootLayout = nullptr;
  m_contentRow = nullptr;
  m_mainColumn = nullptr;
  m_topRow = nullptr;
  m_lowerRow = nullptr;
  m_centerColumn = nullptr;
  m_rightColumn = nullptr;
  m_shortcutCard = nullptr;
  m_metricsCard = nullptr;
  m_metricsRail = nullptr;
  m_bottomRow = nullptr;
  m_anniversaryCard = nullptr;
  m_dateTimeCard = nullptr;
  m_mediaCard = nullptr;
  m_mediaText = nullptr;
  m_userCard = nullptr;
  m_userMain = nullptr;
  m_performanceCard = nullptr;
  m_userAvatar = nullptr;
  m_timeLabel = nullptr;
  m_dateLabel = nullptr;
  m_weatherGlyph = nullptr;
  m_weatherLine = nullptr;
  m_locationLabel = nullptr;
  m_weatherLocationRow = nullptr;
  m_humidityRow = nullptr;
  m_sunsetRow = nullptr;
  m_humidityLabel = nullptr;
  m_sunsetLabel = nullptr;
  m_userHost = nullptr;
  m_userUptime = nullptr;
  m_userVersion = nullptr;
  m_settingsButton = nullptr;
  m_sessionButton = nullptr;
  m_userCardKeyboardArea = nullptr;
  m_userCardArea = nullptr;
  m_mediaCardArea = nullptr;
  m_dateTimeCardArea = nullptr;
  m_performanceCardArea = nullptr;
  m_cpuSummary = nullptr;
  m_cpuBar = nullptr;
  m_batteryValue = nullptr;
  m_batteryGlyph = nullptr;
  m_memoryValue = nullptr;
  m_storageValue = nullptr;
  m_batteryBar = nullptr;
  m_footerMemoryBar = nullptr;
  m_bottomStorageBar = nullptr;
  m_loadedAvatarPath.clear();
  m_loadedAvatarSize = 0;
  // The crisp fade animation is tagged with the m_wallpaperBg node as owner, so
  // it is cancelled automatically when the node tree is destroyed on close.
  m_wallpaperCrispAnimId = 0;
  m_layoutModeAnimId = 0;
  m_layoutMode = LayoutMode::Wide;
  m_layoutModeInitialized = false;
  m_crispWorkingPath.clear();
  m_crispWorkingSize = 0;
  m_crispShown = false;
  m_crispNeedsFade = false;
  m_wallpaperPlaceholder = nullptr;
  m_wallpaperBg = nullptr;
  m_wallpaperGradient = nullptr;
  m_userDetailsSurface = nullptr;
  m_mediaTrack = nullptr;
  m_mediaArtist = nullptr;
  m_mediaStatus = nullptr;
  m_mediaProgress = nullptr;
  m_mediaSeekSlider = nullptr;
  m_mediaShuffleButton = nullptr;
  m_mediaPreviousButton = nullptr;
  m_mediaPlayButton = nullptr;
  m_mediaNextButton = nullptr;
  m_mediaArt = nullptr;
  m_mediaArtSlot = nullptr;
  m_mediaArtFallback = nullptr;
  m_loadedMediaArtUrl.clear();
  m_mediaPositionBusName.clear();
  m_mediaPositionTrackId.clear();
  m_mediaPositionTrackSignature.clear();
  m_mediaLastPlaybackStatus.clear();
  m_mediaPositionUs = 0;
  m_mediaPositionSampleAt = {};
  m_nextRealtimeUpdateAt = {};
  m_lastRealtimeMprisPollAt = {};
  m_shortcutsGrid = nullptr;
  m_shortcutPads.clear();
  m_metricPads.clear();
  m_volumeCard = nullptr;
  m_volumeSlider = nullptr;
  m_volumeValueLabel = nullptr;
  m_outputSelect = nullptr;
  m_microphoneSlider = nullptr;
  m_microphoneValueLabel = nullptr;
  m_muteAllButton = nullptr;
  m_brightnessCard = nullptr;
  m_brightnessSlider = nullptr;
  m_brightnessValueLabel = nullptr;
  m_nightLightToggle = nullptr;
  m_temperatureSlider = nullptr;
  m_temperatureValueLabel = nullptr;
  m_audioSerial = 0;
}

void HomeTab::onPanelCardOpacityChanged(float opacity) {
  (void)opacity;
  syncShortcuts();
}

void HomeTab::setMetricTooltip(std::string_view name, std::string detail) {
  for (auto& pad : m_metricPads) {
    if (pad.name == name && pad.hitArea != nullptr) {
      pad.hitArea->setTooltip(std::format("{}: {}", name, detail));
      return;
    }
  }
}

void HomeTab::syncScaledFonts() {
  const float s = contentScale();
  if (m_timeLabel != nullptr) {
    m_timeLabel->setFontSize(Style::fontSizeTitle * 2.0f * s);
  }
  if (m_dateLabel != nullptr) {
    m_dateLabel->setFontSize(Style::fontSizeBody * 0.9f * s);
  }
  if (m_weatherGlyph != nullptr) {
    m_weatherGlyph->setGlyphSize(Style::fontSizeCaption * 1.12f * s);
  }
  if (m_weatherLine != nullptr) {
    m_weatherLine->setFontSize(Style::fontSizeCaption * s);
  }
  for (Label* label : {m_userHost, m_userUptime, m_userVersion}) {
    if (label != nullptr) {
      label->setFontSize(Style::fontSizeCaption * s);
    }
  }
  if (m_mediaTrack != nullptr) {
    m_mediaTrack->setFontSize(Style::fontSizeBody * 0.95f * s);
  }
  if (m_mediaArtist != nullptr) {
    m_mediaArtist->setFontSize(Style::fontSizeCaption * s);
  }
  if (m_mediaStatus != nullptr) {
    m_mediaStatus->setFontSize(Style::fontSizeCaption * s);
  }
  if (m_mediaProgress != nullptr) {
    m_mediaProgress->setFontSize(Style::fontSizeCaption * s);
  }
  for (Label* label : {m_volumeValueLabel, m_microphoneValueLabel, m_brightnessValueLabel, m_temperatureValueLabel}) {
    if (label != nullptr) {
      label->setFontSize(Style::fontSizeMini * s);
    }
  }
  for (auto& pad : m_shortcutPads) {
    if (pad.label != nullptr) {
      pad.label->setFontSize(Style::fontSizeMini * s);
    }
    if (pad.glyph != nullptr) {
      pad.glyph->setGlyphSize(Style::fontSizeTitle * 1.35f * s);
    }
  }
}

void HomeTab::sync(Renderer& renderer) {
  syncScaledFonts();
  syncShortcuts();

  const auto storageUsage = readRootStorageUsage();

  if (m_timeLabel != nullptr) {
    m_timeLabel->setText(formatShellTime(m_config));
  }
  if (m_dateLabel != nullptr) {
    m_dateLabel->setText(formatShellDate(m_config));
  }

  syncWallpaperBackground(renderer);

  if (m_userAvatar != nullptr && m_config != nullptr) {
    const std::string displayPath = shell::avatarDisplayPath(m_accounts, m_config->config());
    const int avatarSize = static_cast<int>(std::round(m_userAvatar->width()));
    if (displayPath != m_loadedAvatarPath || avatarSize != m_loadedAvatarSize) {
      if (displayPath.empty()) {
        m_userAvatar->clear(renderer);
      } else {
        // Decode at the avatar's final on-screen size with no mipmaps: layout grows the
        // avatar to match the user text block, and trilinear mipmap sampling softens an
        // image displayed near 1:1. Both made the avatar look blurry.
        (void)m_userAvatar->setSourceFile(renderer, displayPath, avatarSize, false);
      }
      m_loadedAvatarPath = displayPath;
      m_loadedAvatarSize = avatarSize;
    }
  }

  if (m_userHost != nullptr) {
    m_userHost->setText(userHostLine());
  }
  if (m_userUptime != nullptr) {
    const auto uptime = systemUptime();
    const std::string uptimeText =
        uptime.has_value() ? formatDuration(*uptime) : i18n::tr("control-center.home.unknown");
    m_userUptime->setText(i18n::tr("control-center.home.uptime", "uptime", uptimeText));
  }
  if (m_userVersion != nullptr) {
    m_userVersion->setText(gnilVersionLine());
  }

  if (m_cpuSummary != nullptr || m_cpuBar != nullptr) {
    if (m_sysmon != nullptr && m_sysmon->isRunning()) {
      const auto& stats = m_sysmon->latest();
      if (m_cpuSummary != nullptr) {
        m_cpuSummary->setText(std::format("{:.0f}%", stats.cpuUsagePercent));
      }
      if (m_cpuBar != nullptr) {
        m_cpuBar->setProgress(static_cast<float>(std::clamp(stats.cpuUsagePercent / 100.0, 0.0, 1.0)));
      }
      setMetricTooltip("CPU", std::format("{:.0f}%", stats.cpuUsagePercent));
    } else {
      if (m_cpuSummary != nullptr) {
        m_cpuSummary->setText("—");
      }
      if (m_cpuBar != nullptr) {
        m_cpuBar->setProgress(0.0f);
      }
      setMetricTooltip("CPU", i18n::tr("dashboard.home.metrics.unavailable"));
    }
  }

  if (m_batteryValue != nullptr || m_batteryBar != nullptr || m_batteryGlyph != nullptr) {
    if (m_upower != nullptr && m_upower->state().isPresent) {
      const auto& battery = m_upower->state();
      const int percent = static_cast<int>(std::lround(std::clamp(battery.percentage, 0.0, 100.0)));
      if (m_batteryValue != nullptr) {
        m_batteryValue->setText(std::format("{}%", percent));
      }
      if (m_batteryBar != nullptr) {
        m_batteryBar->setProgress(static_cast<float>(percent) / 100.0f);
      }
      if (m_batteryGlyph != nullptr) {
        m_batteryGlyph->setGlyph(batteryGlyphName(battery.percentage, battery.state));
      }
      setMetricTooltip("Battery", std::format("{:.0f}%", battery.percentage));
    } else {
      if (m_batteryValue != nullptr) {
        m_batteryValue->setText(i18n::tr("lockscreen.dashboard.no-battery"));
      }
      if (m_batteryBar != nullptr) {
        m_batteryBar->setProgress(0.0f);
      }
      setMetricTooltip("Battery", i18n::tr("dashboard.home.metrics.unavailable"));
    }
  }

  if (m_memoryValue != nullptr || m_storageValue != nullptr || m_footerMemoryBar != nullptr
      || m_bottomStorageBar != nullptr) {
    if (m_sysmon != nullptr && m_sysmon->isRunning()) {
      const auto& stats = m_sysmon->latest();
      if (m_memoryValue != nullptr) {
        m_memoryValue->setText(FormatUnits::formatBinaryMibUsageAsGib(stats.ramUsedMb, stats.ramTotalMb));
      }
      if (m_footerMemoryBar != nullptr) {
        m_footerMemoryBar->setProgress(static_cast<float>(std::clamp(stats.ramUsagePercent / 100.0, 0.0, 1.0)));
      }
      setMetricTooltip(
          "Memory",
          std::format(
              "{} ({:.0f}%)", FormatUnits::formatBinaryMibUsageAsGib(stats.ramUsedMb, stats.ramTotalMb),
              stats.ramUsagePercent
          )
      );
    } else {
      if (m_memoryValue != nullptr) {
        m_memoryValue->setText("—");
      }
      if (m_footerMemoryBar != nullptr) {
        m_footerMemoryBar->setProgress(0.0f);
      }
      setMetricTooltip("Memory", i18n::tr("dashboard.home.metrics.unavailable"));
    }
    if (storageUsage.has_value()) {
      if (m_storageValue != nullptr) {
        m_storageValue->setText(
            FormatUnits::formatDecimalBytesUsage(storageUsage->usedBytes, storageUsage->totalBytes)
        );
      }
      if (m_bottomStorageBar != nullptr) {
        m_bottomStorageBar->setProgress(storageUsage->percent / 100.0f);
      }
      setMetricTooltip(
          "Storage",
          std::format(
              "{} ({:.0f}%)",
              FormatUnits::formatDecimalBytesUsage(storageUsage->usedBytes, storageUsage->totalBytes),
              storageUsage->percent
          )
      );
    } else {
      if (m_storageValue != nullptr) {
        m_storageValue->setText("—");
      }
      if (m_bottomStorageBar != nullptr) {
        m_bottomStorageBar->setProgress(0.0f);
      }
      setMetricTooltip("Storage", i18n::tr("dashboard.home.metrics.unavailable"));
    }
  }

  if (m_weatherGlyph != nullptr && m_weatherLine != nullptr) {
    const auto setWeatherDetailsVisible = [this](bool location, bool humidity, bool sunset) {
      if (m_weatherLocationRow != nullptr) {
        m_weatherLocationRow->setVisible(location);
      }
      if (m_humidityRow != nullptr) {
        m_humidityRow->setVisible(humidity);
      }
      if (m_sunsetRow != nullptr) {
        m_sunsetRow->setVisible(sunset);
      }
    };
    if (m_weather == nullptr || !m_weather->enabled()) {
      m_weatherGlyph->setGlyph("weather-cloud-off");
      m_weatherGlyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      m_weatherLine->setText(i18n::tr("control-center.home.weather.disabled"));
      setWeatherDetailsVisible(false, false, false);
      if (m_humidityLabel != nullptr) m_humidityLabel->setText("—");
      if (m_sunsetLabel != nullptr) m_sunsetLabel->setText("—");
    } else if (!m_weather->locationConfigured()) {
      m_weatherGlyph->setGlyph("weather-cloud");
      m_weatherGlyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      m_weatherLine->setText(i18n::tr("control-center.weather.no-location-body"));
      setWeatherDetailsVisible(false, false, false);
      if (m_humidityLabel != nullptr) m_humidityLabel->setText("—");
      if (m_sunsetLabel != nullptr) m_sunsetLabel->setText("—");

    } else {
      const auto& snapshot = m_weather->snapshot();
      if (!snapshot.valid) {
        m_weatherGlyph->setGlyph("weather-cloud");
        m_weatherGlyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
        m_weatherLine->setText(
            m_weather->loading() ? i18n::tr("control-center.home.weather.fetching")
                                 : i18n::tr("control-center.home.weather.data-unavailable")
        );
        setWeatherDetailsVisible(false, false, false);
        if (m_humidityLabel != nullptr) m_humidityLabel->setText("—");
        if (m_sunsetLabel != nullptr) m_sunsetLabel->setText("—");
      } else {
        m_weatherGlyph->setGlyph(WeatherService::glyphForCode(snapshot.current.weatherCode, snapshot.current.isDay));
        m_weatherGlyph->setColor(colorSpecFromRole(ColorRole::Primary));
        const int t = static_cast<int>(std::lround(m_weather->displayTemperature(snapshot.current.temperatureC)));
        m_weatherLine->setText(
            std::format(
                "{}{} · {}", t, m_weather->displayTemperatureUnit(),
                WeatherService::descriptionForCode(snapshot.current.weatherCode)
            )
        );
        if (m_locationLabel != nullptr) {
          m_locationLabel->setText(snapshot.locationName.empty() ? i18n::tr("dashboard.home.weather.current-location")
                                                                 : snapshot.locationName);
        }
        const int humidity = snapshot.forecastHours.empty() ? 0 : snapshot.forecastHours.front().relativeHumidityPercent;
        if (m_humidityLabel != nullptr) {
          m_humidityLabel->setText(humidity > 0 ? std::format("{}%", humidity) : "—");
        }
        std::string sunset = snapshot.forecastDays.empty() ? std::string{} : snapshot.forecastDays.front().sunsetIso;
        const auto separator = sunset.find('T');
        if (separator != std::string::npos && sunset.size() >= separator + 6) {
          sunset = sunset.substr(separator + 1, 5);
        }
        if (m_sunsetLabel != nullptr) {
          m_sunsetLabel->setText(
              sunset.empty() ? "—" : i18n::tr("dashboard.home.weather.sunset", "time", sunset)
          );
        }
        setWeatherDetailsVisible(true, humidity > 0, !sunset.empty());
      }
    }
  }

  if (m_mediaTrack != nullptr && m_mediaArtist != nullptr && m_mediaStatus != nullptr && m_mediaProgress != nullptr) {
    if (m_mpris == nullptr) {
      m_mediaTrack->setText(i18n::tr("control-center.home.media.playback-unavailable"));
      m_mediaTrack->setTooltip(i18n::tr("control-center.home.media.playback-unavailable"));
      m_mediaArtist->setText("");
      m_mediaArtist->setVisible(false);
      m_mediaStatus->setText(i18n::tr("control-center.home.media.unavailable"));
      m_mediaProgress->setText(" ");
      m_mediaProgress->setVisible(false);
      m_mediaStatus->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      if (m_mediaArt != nullptr) {
        m_mediaArt->clear(renderer);
        m_mediaArt->setVisible(false);
      }
      m_loadedMediaArtUrl.clear();
      PanelManager::instance().requestLayout();
    } else {
      const auto active = m_mpris->activePlayer();
      if (!active.has_value()) {
        m_mediaPositionBusName.clear();
        m_mediaPositionTrackId.clear();
        m_mediaPositionTrackSignature.clear();
        m_mediaLastPlaybackStatus.clear();
        m_mediaPositionUs = 0;
        m_mediaPositionSampleAt = {};
        m_mediaTrack->setText(i18n::tr("control-center.home.media.nothing-playing"));
        m_mediaTrack->setTooltip(i18n::tr("control-center.home.media.nothing-playing"));
        m_mediaArtist->setText("");
        m_mediaArtist->setVisible(false);
        m_mediaStatus->setText(i18n::tr("control-center.home.media.idle"));
        m_mediaProgress->setText(" ");
        m_mediaProgress->setVisible(false);
        m_mediaStatus->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
        if (m_mediaArt != nullptr) {
          m_mediaArt->clear(renderer);
          m_mediaArt->setVisible(false);
        }
        m_loadedMediaArtUrl.clear();
        PanelManager::instance().requestLayout();
      } else {
        const std::string trackText =
            active->title.empty() ? i18n::tr("control-center.home.media.unknown-track") : active->title;
        const std::string artists = mpris::joinArtists(active->artists);
        const std::string artistText = artists.empty() ? i18n::tr("control-center.home.media.unknown-artist") : artists;
        if (m_mediaTrack->text() != trackText || m_mediaArtist->text() != artistText) {
          m_mediaTrack->setText(trackText);
          m_mediaArtist->setText(artistText);
          PanelManager::instance().requestLayout();
        }
        m_mediaTrack->setTooltip(trackText);
        m_mediaArtist->setVisible(true);
        const std::string trackSignature = std::format(
            "{}\n{}\n{}\n{}\n{}", active->trackId, active->title, artists, active->album, active->sourceUrl
        );
        std::string progressText;
        if (active->lengthUs > 0) {
          const auto now = std::chrono::steady_clock::now();
          std::int64_t livePositionUs = std::max<std::int64_t>(0, active->positionUs);
          livePositionUs = std::clamp<std::int64_t>(livePositionUs, 0, active->lengthUs);
          const bool sameDisplayedTrack =
              m_mediaPositionBusName == active->busName && m_mediaPositionTrackSignature == trackSignature;
          const bool withinTransientRegressionWindow =
              m_mediaPositionSampleAt != std::chrono::steady_clock::time_point{}
              && now - m_mediaPositionSampleAt <= kHomeTransientPositionRegressionWindow;
          const bool preserveDisplayedPosition = sameDisplayedTrack
              && m_mediaLastPlaybackStatus == "Playing"
              && active->playbackStatus == "Playing"
              && m_mediaPositionUs >= kHomeTransientPositionRegressionFloorUs
              && livePositionUs <= kHomeTransientPositionRegressionCeilingUs
              && livePositionUs + kHomeTransientPositionRegressionDeltaUs < m_mediaPositionUs
              && withinTransientRegressionWindow;
          if (preserveDisplayedPosition) {
            livePositionUs = m_mediaPositionUs;
          }

          m_mediaPositionBusName = active->busName;
          m_mediaPositionTrackId = active->trackId;
          m_mediaPositionTrackSignature = trackSignature;
          m_mediaLastPlaybackStatus = active->playbackStatus;
          if (!preserveDisplayedPosition) {
            m_mediaPositionUs = livePositionUs;
            m_mediaPositionSampleAt = now;
          }

          const std::int64_t positionSec = std::max<std::int64_t>(0, livePositionUs / 1000000);
          const std::int64_t lengthSec = std::max<std::int64_t>(1, active->lengthUs / 1000000);
          progressText = std::format("{} / {}", formatClockTime(positionSec), formatClockTime(lengthSec));
        } else {
          m_mediaPositionBusName.clear();
          m_mediaPositionTrackId.clear();
          m_mediaPositionTrackSignature.clear();
          m_mediaLastPlaybackStatus.clear();
          m_mediaPositionUs = 0;
          m_mediaPositionSampleAt = {};
        }
        m_mediaProgress->setText(" ");
        m_mediaProgress->setVisible(false);
        if (m_mediaArt != nullptr) {
          const std::string artUrl = mpris::effectiveArtUrl(*active);
          const bool artRetry = !artUrl.empty() && !m_mediaArt->hasImage();
          if (artUrl != m_loadedMediaArtUrl || artRetry) {
            const std::string artPath = mpris::resolveArtworkSource(
                m_httpClient, m_pendingArtDownloads, artUrl,
                [this] {
                  m_loadedMediaArtUrl.clear();
                  PanelManager::instance().refresh();
                },
                m_aliveGuard
            );
            bool loaded = false;
            if (!artPath.empty()) {
              const int decodeSize = static_cast<int>(std::round(Style::controlHeightLg * 2.6f * contentScale()));
              loaded = m_mediaArt->setSourceFile(renderer, artPath, decodeSize, true, true);
              if (!loaded) {
                m_mediaArt->clear(renderer);
              }
            } else {
              m_mediaArt->clear(renderer);
            }
            m_mediaArt->setVisible(loaded);
            m_mediaArtFallback->setVisible(!loaded);
            m_loadedMediaArtUrl = loaded ? artUrl : std::string{};
            PanelManager::instance().requestLayout();
          }
        }
        std::string statusText;
        if (active->playbackStatus == "Playing") {
          statusText = i18n::tr("control-center.home.media.playing");
          m_mediaStatus->setColor(colorSpecFromRole(ColorRole::Primary));
        } else if (active->playbackStatus == "Paused") {
          statusText = i18n::tr("control-center.home.media.paused");
          m_mediaStatus->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
        } else {
          statusText = active->playbackStatus;
          m_mediaStatus->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
        }
        if (!progressText.empty()) {
          statusText = std::format("{} · {}", statusText, progressText);
        }
        if (m_mediaStatus->text() != statusText) {
          m_mediaStatus->setText(statusText);
          PanelManager::instance().requestLayout();
        }
      }
    }
  }

  if (m_mediaSeekSlider != nullptr) {
    const auto active = m_mpris != nullptr ? m_mpris->activePlayer() : std::optional<MprisPlayerInfo>{};
    const bool hasPlayer = active.has_value();
    const bool canSeek = hasPlayer && active->canSeek && active->lengthUs > 0;
    m_mediaSeekSlider->setEnabled(canSeek);
    if (!m_mediaSeekSlider->dragging()) {
      m_syncingMediaSeek = true;
      const double fraction = canSeek
          ? std::clamp(static_cast<double>(active->positionUs) / static_cast<double>(active->lengthUs), 0.0, 1.0)
          : 0.0;
      m_mediaSeekSlider->setValue(fraction);
      m_syncingMediaSeek = false;
    }
    if (m_mediaShuffleButton != nullptr) {
      m_mediaShuffleButton->setEnabled(hasPlayer);
      m_mediaShuffleButton->setSelected(hasPlayer && active->shuffle);
    }
    if (m_mediaPreviousButton != nullptr) m_mediaPreviousButton->setEnabled(hasPlayer && active->canGoPrevious);
    if (m_mediaNextButton != nullptr) m_mediaNextButton->setEnabled(hasPlayer && active->canGoNext);
    if (m_mediaPlayButton != nullptr) {
      m_mediaPlayButton->setEnabled(hasPlayer && (active->canPlay || active->canPause));
      const bool playing = hasPlayer && active->playbackStatus == "Playing";
      m_mediaPlayButton->setGlyph(playing ? "player-pause-filled" : "player-play-filled");
      m_mediaPlayButton->setTooltip(
          i18n::tr(playing ? "dashboard.home.media.pause" : "dashboard.home.media.play")
      );
    }
  }

  if (m_volumeSlider != nullptr && m_audio != nullptr && !m_volumeSlider->dragging()) {
    const AudioNode* sink = m_audio->defaultSink();
    if (sink != nullptr) {
      m_syncingVolumeSlider = true;
      m_volumeSlider->setValue(sink->volume);
      m_syncingVolumeSlider = false;
      if (m_volumeValueLabel != nullptr) {
        m_volumeValueLabel->setText(std::format("{:.0f}%", sink->volume * 100.0f));
      }
    } else if (m_volumeValueLabel != nullptr) {
      m_volumeValueLabel->setText("—");
    }
  }

  if (m_audio != nullptr) {
    if (m_outputSelect != nullptr && m_audioSerial != m_audio->changeSerial()) {
      std::vector<std::string> outputs;
      std::size_t selected = 0;
      const auto& sinks = m_audio->state().sinks;
      outputs.reserve(sinks.size());
      for (std::size_t i = 0; i < sinks.size(); ++i) {
        outputs.push_back(audioDeviceLabel(sinks[i]));
        if (sinks[i].isDefault || sinks[i].id == m_audio->state().defaultSinkId) {
          selected = i;
        }
      }
      m_outputSelect->setOptions(std::move(outputs));
      m_outputSelect->setEnabled(!sinks.empty());
      if (!sinks.empty()) {
        m_outputSelect->setSelectedIndexSilently(selected);
      }
      m_audioSerial = m_audio->changeSerial();
    }
    if (m_microphoneSlider != nullptr && !m_microphoneSlider->dragging()) {
      if (const AudioNode* source = m_audio->defaultSource(); source != nullptr) {
        m_syncingMicrophoneSlider = true;
        m_microphoneSlider->setValue(source->volume);
        m_syncingMicrophoneSlider = false;
        m_microphoneSlider->setEnabled(true);
        if (m_microphoneValueLabel != nullptr) {
          m_microphoneValueLabel->setText(std::format("{:.0f}%", source->volume * 100.0f));
        }
      } else {
        m_microphoneSlider->setEnabled(false);
        if (m_microphoneValueLabel != nullptr) {
          m_microphoneValueLabel->setText("—");
        }
      }
    }
    if (m_muteAllButton != nullptr) {
      const AudioNode* sink = m_audio->defaultSink();
      const AudioNode* source = m_audio->defaultSource();
      const bool bothMuted = sink != nullptr && source != nullptr && sink->muted && source->muted;
      m_muteAllButton->setText(
          bothMuted ? i18n::tr("dashboard.home.audio.unmute-all") : i18n::tr("dashboard.home.audio.mute-all")
      );
      m_muteAllButton->setGlyph(bothMuted ? "volume" : "volume-off");
      m_muteAllButton->setVariant(bothMuted ? ButtonVariant::Primary : ButtonVariant::Outline);
      m_muteAllButton->setEnabled(sink != nullptr || source != nullptr);
    }
  }

  if (m_brightnessSlider != nullptr && m_brightness != nullptr && !m_brightnessSlider->dragging()) {
    const auto& displays = m_brightness->displays();
    if (!displays.empty()) {
      const BrightnessDisplay* defaultDisplay = nullptr;
      for (const auto& display : displays) {
        if (display.controllable) {
          defaultDisplay = &display;
          break;
        }
      }
      if (defaultDisplay == nullptr) {
        defaultDisplay = &displays.front();
      }

      m_syncingBrightnessSlider = true;
      m_brightnessSlider->setValue(defaultDisplay->brightness);
      m_syncingBrightnessSlider = false;
      m_brightnessSlider->setEnabled(defaultDisplay->controllable);
      if (m_brightnessValueLabel != nullptr) {
        m_brightnessValueLabel->setText(std::format("{:.0f}%", defaultDisplay->brightness * 100.0f));
      }
    } else {
      m_brightnessSlider->setEnabled(false);
      if (m_brightnessValueLabel != nullptr) {
        m_brightnessValueLabel->setText("—");
      }
    }
  }

  if (m_nightLightToggle != nullptr && m_nightLight != nullptr) {
    m_syncingNightLight = true;
    m_nightLightToggle->setChecked(m_nightLight->forceEnabled());
    m_syncingNightLight = false;
  }
  if (m_temperatureSlider != nullptr && m_config != nullptr && !m_temperatureSlider->dragging()) {
    const auto temperature = m_config->config().nightlight.nightTemperature;
    m_pendingTemperature = temperature;
    m_syncingTemperature = true;
    m_temperatureSlider->setValue(temperature);
    m_syncingTemperature = false;
    if (m_temperatureValueLabel != nullptr) {
      m_temperatureValueLabel->setText(std::format("{}K", temperature));
    }
  }
}

void HomeTab::syncShortcuts() {
  for (auto& pad : m_shortcutPads) {
    auto& sc = *pad.shortcut;
    const bool enabled = sc.enabled();
    const bool on = sc.isToggle() && sc.active();

    if (pad.button != nullptr) {
      applyShortcutButtonStyle(*pad.button, enabled, on, panelCardOpacity());
    }
    if (pad.glyph != nullptr) {
      pad.glyph->setGlyph(sc.displayIcon());
    }
    if (pad.button != nullptr && pad.label != nullptr) {
      const std::string label = sc.displayLabel();
      if (pad.label->text() != label) {
        pad.button->setText(label);
      }
      pad.button->setTooltip(label);
    }
  }
}
