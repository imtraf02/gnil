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
#include "system/system_monitor_service.h"
#include "system/weather_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
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
#include <string>
#include <string_view>
#include <system_error>

using namespace control_center;

namespace {

  constexpr Logger kLog("control-center");

  constexpr float kHomeAvatarScale = 2.6f;
  constexpr std::size_t kHomeShortcutGridColumns = 4;
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
    const char* format = config != nullptr ? config->config().shell.dateFormat.c_str() : "%A, %x";
    return formatLocalTime(format);
  }

  std::string userHostLine() { return std::format("{}@{}", sessionDisplayName(), hostName()); }

  std::string gnilVersionLine() { return std::format("GNIL {}", gnil::build_info::displayVersion()); }

  void applyHomeCardStyle(Flex& card, float scale, float fillOpacity, bool /*showBorder*/) {
    applySectionCardStyle(card, scale, fillOpacity, /*showBorder=*/false);
    card.setGap(Style::spaceSm * scale);
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

  class VpnShortcut final : public Shortcut {
  public:
    explicit VpnShortcut(INetworkService* service) : m_service(service) {}
    std::string_view id() const override { return "vpn"; }
    std::string defaultLabel() const override { return i18n::tr("dashboard.home.shortcuts.vpn"); }
    std::string_view iconOn() const override { return "shield-check"; }
    std::string_view iconOff() const override { return "shield"; }
    bool isToggle() const override { return true; }
    bool enabled() const override { return m_service != nullptr; }
    bool active() const override { return m_service != nullptr && m_service->state().vpnActive; }
    void onClick() override {
      if (m_service == nullptr) {
        return;
      }
      const auto& vpns = m_service->vpnConnections();
      bool deactivated = false;
      for (const auto& vpn : vpns) {
        if (vpn.active) {
          deactivated = m_service->deactivateVpnConnection(vpn) || deactivated;
        }
      }
      if (deactivated) {
        return;
      }
      if (vpns.size() == 1) {
        (void)m_service->activateVpnConnection(vpns.front());
        return;
      }
      PanelManager::instance().togglePanel("network");
    }
    void onRightClick() override { PanelManager::instance().togglePanel("network"); }

  private:
    INetworkService* m_service = nullptr;
  };

} // namespace

HomeTab::HomeTab(const ControlCenterServices& services)
    : m_mpris(services.mpris), m_httpClient(services.httpClient), m_weather(services.weather),
      m_config(services.config), m_accounts(services.accounts), m_wallpaper(services.wallpaper),
      m_thumbnails(services.thumbnails), m_sysmon(services.sysmon), m_services(services.shortcutServices()),
      m_audio(services.audio), m_brightness(services.brightness), m_nightLight(services.nightLight),
      m_platform(services.platform) {
  if (m_config != nullptr) {
    m_pendingTemperature = m_config->config().nightlight.nightTemperature;
  }
  if (m_sysmon != nullptr) {
    m_sysmon->retainDiskPath("/");
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
  if (m_sysmon != nullptr) {
    m_sysmon->releaseDiskPath("/");
  }
  if (m_thumbnails != nullptr && !m_loadedWallpaperPath.empty()) {
    m_thumbnails->release(m_loadedWallpaperPath, m_loadedWallpaperSize);
  }
}

std::unique_ptr<Flex> HomeTab::create() {
  const float scale = contentScale();
  const std::string displayName = sessionDisplayName();

  // Root layout is a horizontal row (3 columns)
  auto tab = ui::row({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::panelPadding * scale,
  });

  // ================= Column 1: Left (Weather & Media) =================
  auto leftColumn = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::panelPadding * scale,
      .fillHeight = true,
      .flexGrow = 1.05f,
  });

  // --- Date/Time + Weather ---
  auto dateTimeCard = ui::column({
      .out = &m_dateTimeCard,
      .align = FlexAlign::Stretch,
      .justify = FlexJustify::Center,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
      .fillHeight = true,
      .flexGrow = 0.9f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applyHomeCardStyle(card, scale, opacity, borders);
      },
  });
  dateTimeCard->addChild(ui::label({
      .out = &m_timeLabel,
      .text = formatShellTime(m_config),
      .fontSize = Style::fontSizeTitle * 2.35f * scale,
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
      {.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceXs * scale},
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
          {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
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
      {.align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = Style::spaceXs * scale},
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
      .justify = FlexJustify::Center,
      .gap = Style::spaceXs * scale,
      .fillWidth = true,
      .fillHeight = true,
      .flexGrow = 1.0f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applyHomeCardStyle(card, scale, opacity, borders);
      },
  });
  mediaCard->addChild(ui::label({
      .text = i18n::tr("dashboard.home.media.now-playing"),
      .fontSize = Style::fontSizeBody * scale,
      .fontWeight = FontWeight::Bold,
      .color = colorSpecFromRole(ColorRole::OnSurface),
  }));

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
          .controlHeight = Style::controlHeightSm * scale,
          .enabled = false,
          .variant = ButtonVariant::Ghost,
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
          .controlHeight = Style::controlHeightSm * scale,
          .enabled = false,
          .variant = ButtonVariant::Ghost,
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
          .onClick = [this]() {
            if (m_mpris != nullptr) {
              (void)m_mpris->playPauseActive();
            }
          },
      }),
      ui::button({
          .out = &m_mediaNextButton,
          .glyph = "player-skip-forward-filled",
          .controlHeight = Style::controlHeightSm * scale,
          .enabled = false,
          .variant = ButtonVariant::Ghost,
          .onClick = [this]() {
            if (m_mpris != nullptr) {
              (void)m_mpris->nextActive();
            }
          },
      }),
      ui::button({
          .text = i18n::tr("dashboard.home.media.open"),
          .glyph = "arrow-right",
          .controlHeight = Style::controlHeightSm * scale,
          .variant = ButtonVariant::Ghost,
          .onClick = []() { openControlCenterTab("media"); },
      })
  ));

  // Preserve a keyboard-level card affordance without covering the transport controls.
  m_mediaCardArea = addCardOverlay(
      *m_mediaCard, []() { openControlCenterTab("media"); }, {.keyboardFocus = true, .pointerHitTest = false}
  );

  leftColumn->addChild(std::move(dateTimeCard));
  leftColumn->addChild(std::move(mediaCard));
  tab->addChild(std::move(leftColumn));

  // ================= Column 2: Middle (User Profile & Shortcuts) =================
  auto middleColumn = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::panelPadding * scale,
      .fillHeight = true,
      .flexGrow = 1.7f,
  });

  // --- User card ---
  auto userCard = ui::column({
      .out = &m_userCard,
      .justify = FlexJustify::Center,
      .fillHeight = true,
      .flexGrow = 1.0f,
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
  const auto configureUserDetailLabel = [scale](Label& label) {
    label.setShadow(Color{0.0f, 0.0f, 0.0f, 0.36f}, 0.0f, 1.0f * scale);
  };
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
              .configure =
                  [scale](Label& label) { label.setShadow(Color{0.0f, 0.0f, 0.0f, 0.42f}, 0.0f, 1.0f * scale); },
          }),
          ui::label({
              .out = &m_userHost,
              .text = userHostLine(),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .configure = configureUserDetailLabel,
          }),
          ui::label({
              .out = &m_userUptime,
              .text = "…",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .configure = configureUserDetailLabel,
          }),
          ui::label({
              .out = &m_userVersion,
              .text = gnilVersionLine(),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .configure = configureUserDetailLabel,
          })
      )
  );
  userCard->addChild(std::move(userRow));
  addDivider(*userCard, scale);
  const auto addResourceMeter = [scale](
                                    Flex& parent, std::string glyph, std::string label, Label** value,
                                    ProgressBar** progress
                                ) {
    parent.addChild(ui::column(
        {.align = FlexAlign::Stretch, .gap = Style::spaceXs * 0.35f * scale, .flexGrow = 1.0f},
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
            ui::glyph({
                .glyph = std::move(glyph),
                .glyphSize = Style::fontSizeMini * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            }),
            ui::label({
                .text = std::move(label),
                .fontSize = Style::fontSizeMini * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .flexGrow = 1.0f,
            }),
            ui::label({
                .out = value,
                .text = "—",
                .fontSize = Style::fontSizeMini * scale,
                .fontWeight = FontWeight::Bold,
                .color = colorSpecFromRole(ColorRole::OnSurface),
            })
        ),
        ui::progressBar({
            .out = progress,
            .fill = colorSpecFromRole(ColorRole::Primary),
            .track = colorSpecFromRole(ColorRole::Outline, 0.2f),
            .progress = 0.0f,
            .height = 4.0f * scale,
        })
    ));
  };
  auto resourceRow = ui::row({
      .align = FlexAlign::Stretch, .gap = Style::spaceMd * scale,
  });
  addResourceMeter(*resourceRow, "cpu-usage", "CPU", &m_cpuSummary, &m_cpuBar);
  addResourceMeter(*resourceRow, "memory", "RAM", &m_memorySummary, &m_memoryBar);
  addResourceMeter(*resourceRow, "storage", i18n::tr("dashboard.home.storage"), &m_storageSummary, &m_storageBar);
  userCard->addChild(std::move(resourceRow));

  // Wallpaper panel: full-card keyboard target; carved pointer target leaves the avatar clickable.
  const auto openWallpaperPanel = []() { PanelManager::instance().togglePanel("wallpaper"); };
  m_userCardKeyboardArea =
      addCardOverlay(*m_userCard, openWallpaperPanel, {.keyboardFocus = true, .pointerHitTest = false});
  m_userCardArea = addCardOverlay(*m_userCard, openWallpaperPanel, {.keyboardFocus = false, .pointerHitTest = true});

  // --- Shortcuts ---
  std::vector<ShortcutConfig> shortcuts;
  shortcuts.push_back({"wifi"});
  shortcuts.push_back({"bluetooth"});
  shortcuts.push_back({"vpn"});
  shortcuts.push_back({"notification"});
  shortcuts.push_back({"dark_mode"});
  shortcuts.push_back({"caffeine"});
  shortcuts.push_back({"audio"});
  shortcuts.push_back({"nightlight"});

  const std::size_t count = std::min(shortcuts.size(), std::size_t{8});

  auto grid = std::make_unique<GridView>();
  grid->setColumns(kHomeShortcutGridColumns);
  grid->setColumnGap(Style::spaceSm * scale);
  grid->setRowGap(Style::spaceSm * scale);
  grid->setPadding(0.0f);
  grid->setUniformCellSize(true);
  grid->setStretchItems(true);
  grid->setSquareCells(false);
  grid->setMinCellHeight(0.0f);
  grid->setFlexGrow(1.2f);
  m_shortcutsGrid = grid.get();
  m_shortcutPads.clear();

  for (std::size_t i = 0; i < count; ++i) {
    const auto& sc = shortcuts[i];
    std::unique_ptr<Shortcut> shortcut;
    if (sc.type == "vpn") {
      shortcut = std::make_unique<VpnShortcut>(m_services.network);
    } else {
      shortcut = ShortcutRegistry::create(sc.type, m_services);
    }
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
        .glyphSize = Style::fontSizeTitle * 1.35f * scale,
        .minHeight = 0.0f,
        .padding = Style::spaceSm * scale,
        .gap = Style::spaceXs * scale,
        .radius = Style::scaledRadiusXl(scale),
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
              button.label()->setMaxLines(1);
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

  middleColumn->addChild(std::move(userCard));
  middleColumn->addChild(std::move(grid));
  tab->addChild(std::move(middleColumn));

  // ================= Column 3: Right (Hardware Sliders) =================
  auto rightColumn = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::panelPadding * scale,
      .fillHeight = true,
      .flexGrow = 1.05f,
  });

  // --- Volume Card ---
  auto volumeCard = ui::column({
      .out = &m_volumeCard,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .fillHeight = true,
      .flexGrow = 1.0f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applyHomeCardStyle(card, scale, opacity, borders);
      },
  });

  auto volumeHeader = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::label({
          .text = i18n::tr("settings.widgets.types.volume"),
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0f,
      }),
      ui::label({
          .out = &m_volumeValueLabel,
          .text = "—",
          .fontSize = Style::fontSizeBody * scale,
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
      .presentation = SliderPresentation::LevelProminent,
      .glyph = "volume-high",
      .glyphSize = Style::fontSizeBody * scale,
      .trackHeight = 30.0f * scale,
      .thumbSize = 39.0f * scale,
      .controlHeight = 39.0f * scale,
      .flexGrow = 1.0f,
      .onValueChanged = [this](double value) {
        if (m_syncingVolumeSlider || m_audio == nullptr) {
          return;
        }
        m_audio->setVolume(static_cast<float>(value));
        if (m_volumeValueLabel != nullptr) {
          m_volumeValueLabel->setText(std::to_string(static_cast<int>(std::round(value * 100.0))) + "%");
        }
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
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  ));
  volumeCard->addChild(ui::slider({
      .out = &m_microphoneSlider,
      .minValue = 0.0f,
      .maxValue = 1.0f,
      .step = 0.01f,
      .value = 1.0f,
      .presentation = SliderPresentation::LevelProminent,
      .glyph = "microphone",
      .glyphSize = Style::fontSizeBody * scale,
      .trackHeight = 30.0f * scale,
      .thumbSize = 39.0f * scale,
      .controlHeight = 39.0f * scale,
      .onValueChanged = [this](double value) {
        if (m_syncingMicrophoneSlider || m_audio == nullptr) {
          return;
        }
        m_audio->setMicVolume(static_cast<float>(value));
      },
  }));
  volumeCard->addChild(ui::button({
      .out = &m_muteAllButton,
      .text = i18n::tr("dashboard.home.audio.mute-all"),
      .glyph = "volume-off",
      .controlHeight = Style::controlHeightSm * scale,
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
      .fillHeight = true,
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
          .fontSize = Style::fontSizeBody * scale,
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
      .presentation = SliderPresentation::LevelProminent,
      .glyph = "brightness-high",
      .glyphSize = Style::fontSizeBody * scale,
      .trackHeight = 30.0f * scale,
      .thumbSize = 39.0f * scale,
      .controlHeight = 39.0f * scale,
      .flexGrow = 1.0f,
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
          m_brightnessValueLabel->setText(std::to_string(static_cast<int>(std::round(value * 100.0))) + "%");
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
          .checked = m_nightLight != nullptr && m_nightLight->enabled(),
          .enabled = m_nightLight != nullptr,
          .scale = scale,
          .onChange = [this](bool enabled) {
            if (!m_syncingNightLight && m_nightLight != nullptr) {
              if (m_config != nullptr) {
                (void)m_config->setOverrides({
                    {{"nightlight", "enabled"}, enabled},
                    {{"nightlight", "force"}, enabled && m_config->config().nightlight.force},
                });
              } else {
                m_nightLight->setEnabled(enabled);
                if (!enabled) {
                  m_nightLight->setForceEnabled(false);
                }
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
          .controlHeight = Style::controlHeightSm * scale,
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

  rightColumn->addChild(std::move(volumeCard));
  rightColumn->addChild(std::move(brightnessCard));
  tab->addChild(std::move(rightColumn));

  return tab;
}

std::unique_ptr<Flex> HomeTab::createHeaderActions() {
  const float scale = contentScale();
  return ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .out = &m_settingsButton,
          .glyph = "settings",
          .onClick = []() { PanelManager::instance().openPanel("settings"); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      }),
      ui::button({
          .out = &m_sessionButton,
          .glyph = "shutdown",
          .onClick = []() { PanelManager::instance().togglePanel("session"); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
}

void HomeTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr) {
    return;
  }

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
            .gradientDirection = GradientDirection::Horizontal,
            .gradientStops =
                {GradientStop{0.0f, translucentSurface}, GradientStop{0.25f, translucentSurface},
                 GradientStop{0.9f, transparentSurface}, GradientStop{1.0f, transparentSurface}},
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
  m_bottomRow = nullptr;
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
  m_memorySummary = nullptr;
  m_storageSummary = nullptr;
  m_cpuBar = nullptr;
  m_memoryBar = nullptr;
  m_storageBar = nullptr;
  m_loadedAvatarPath.clear();
  m_loadedAvatarSize = 0;
  // The crisp fade animation is tagged with the m_wallpaperBg node as owner, so
  // it is cancelled automatically when the node tree is destroyed on close.
  m_wallpaperCrispAnimId = 0;
  m_crispWorkingPath.clear();
  m_crispWorkingSize = 0;
  m_crispShown = false;
  m_crispNeedsFade = false;
  m_wallpaperPlaceholder = nullptr;
  m_wallpaperBg = nullptr;
  m_wallpaperGradient = nullptr;
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

void HomeTab::syncScaledFonts() {
  const float s = contentScale();
  if (m_timeLabel != nullptr) {
    m_timeLabel->setFontSize(Style::fontSizeTitle * 2.35f * s);
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

  if (m_cpuSummary != nullptr && m_memorySummary != nullptr) {
    if (m_sysmon != nullptr && m_sysmon->isRunning()) {
      const auto& stats = m_sysmon->latest();
      m_cpuSummary->setText(std::format("{:.0f}%", stats.cpuUsagePercent));
      m_memorySummary->setText(std::format("{:.0f}%", stats.ramUsagePercent));
      if (m_cpuBar != nullptr) {
        m_cpuBar->setProgress(static_cast<float>(std::clamp(stats.cpuUsagePercent / 100.0, 0.0, 1.0)));
      }
      if (m_memoryBar != nullptr) {
        m_memoryBar->setProgress(static_cast<float>(std::clamp(stats.ramUsagePercent / 100.0, 0.0, 1.0)));
      }
      const float storage = m_sysmon->diskUsagePercent("/");
      if (m_storageSummary != nullptr) {
        m_storageSummary->setText(std::format("{:.0f}%", storage));
      }
      if (m_storageBar != nullptr) {
        m_storageBar->setProgress(std::clamp(storage / 100.0f, 0.0f, 1.0f));
      }
    } else {
      m_cpuSummary->setText("—");
      m_memorySummary->setText("—");
      if (m_storageSummary != nullptr) {
        m_storageSummary->setText("—");
      }
    }
  }

  if (m_weatherGlyph != nullptr && m_weatherLine != nullptr) {
    if (m_weather == nullptr || !m_weather->enabled()) {
      m_weatherGlyph->setGlyph("weather-cloud-off");
      m_weatherGlyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      m_weatherLine->setText(i18n::tr("control-center.home.weather.disabled"));
      if (m_locationLabel != nullptr) m_locationLabel->setText(i18n::tr("control-center.weather.no-location-title"));
      if (m_humidityLabel != nullptr) m_humidityLabel->setText("—");
      if (m_sunsetLabel != nullptr) m_sunsetLabel->setText("—");
    } else if (!m_weather->locationConfigured()) {
      m_weatherGlyph->setGlyph("weather-cloud");
      m_weatherGlyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      m_weatherLine->setText(i18n::tr("control-center.weather.no-location-title"));
      if (m_locationLabel != nullptr) m_locationLabel->setText(i18n::tr("control-center.weather.no-location-title"));
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
        if (m_locationLabel != nullptr) m_locationLabel->setText(snapshot.locationName);
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
        if (m_humidityLabel != nullptr) {
          const int humidity = snapshot.forecastHours.empty() ? 0 : snapshot.forecastHours.front().relativeHumidityPercent;
          m_humidityLabel->setText(humidity > 0 ? std::format("{}%", humidity) : "—");
        }
        if (m_sunsetLabel != nullptr) {
          std::string sunset = snapshot.forecastDays.empty() ? std::string{} : snapshot.forecastDays.front().sunsetIso;
          const auto separator = sunset.find('T');
          if (separator != std::string::npos && sunset.size() >= separator + 6) {
            sunset = sunset.substr(separator + 1, 5);
          }
          m_sunsetLabel->setText(
              sunset.empty() ? "—" : i18n::tr("dashboard.home.weather.sunset", "time", sunset)
          );
        }
      }
    }
  }

  if (m_mediaTrack != nullptr && m_mediaArtist != nullptr && m_mediaStatus != nullptr && m_mediaProgress != nullptr) {
    if (m_mpris == nullptr) {
      m_mediaTrack->setText(i18n::tr("control-center.home.media.playback-unavailable"));
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
      m_mediaPlayButton->setGlyph(hasPlayer && active->playbackStatus == "Playing" ? "player-pause-filled"
                                                                                   : "player-play-filled");
    }
  }

  if (m_volumeSlider != nullptr && m_audio != nullptr && !m_volumeSlider->dragging()) {
    const AudioNode* sink = m_audio->defaultSink();
    if (sink != nullptr) {
      m_syncingVolumeSlider = true;
      m_volumeSlider->setValue(sink->volume);
      m_syncingVolumeSlider = false;
      if (m_volumeValueLabel != nullptr) {
        m_volumeValueLabel->setText(std::to_string(static_cast<int>(std::round(sink->volume * 100.0f))) + "%");
      }
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
        if (m_microphoneValueLabel != nullptr) m_microphoneValueLabel->setText("—");
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
      if (m_brightnessValueLabel != nullptr) {
        m_brightnessValueLabel->setText(std::to_string(static_cast<int>(std::round(defaultDisplay->brightness * 100.0f))) + "%");
      }
      m_brightnessSlider->setEnabled(defaultDisplay->controllable);
    } else {
      m_brightnessSlider->setEnabled(false);
    }
  }

  if (m_nightLightToggle != nullptr && m_nightLight != nullptr) {
    m_syncingNightLight = true;
    m_nightLightToggle->setChecked(m_nightLight->enabled());
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
    }
  }
}
