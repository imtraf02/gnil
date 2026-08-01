#include "shell/lockscreen/lock_surface.h"

#include "capture/screencopy_capture.h"
#include "core/ui_phase.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/core/blur_cache.h"
#include "render/core/render_styles.h"
#include "render/core/shared_texture_cache.h"
#include "render/render_context.h"
#include "render/scene/wallpaper_node.h"
#include "shell/lockscreen/lockscreen_layout.h"
#include "time/time_format.h"
#include "ui/builders/actions.h"
#include "ui/builders/display.h"
#include "ui/builders/input.h"
#include "ui/builders/layout.h"
#include "ui/controls/label.h"
#include "ui/controls/progress_bar.h"
#include "ui/controls/spinner.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
#include <memory>
#include <numbers>
#include <string_view>

namespace {

  const ext_session_lock_surface_v1_listener kLockSurfaceListener = {
      .configure = &LockSurface::handleConfigure,
  };

  bool parseColorWallpaperPath(std::string_view path, Color& out) {
    constexpr std::string_view kPrefix = "color:";
    if (!path.starts_with(kPrefix)) {
      return false;
    }
    return tryParseHexColor(path.substr(kPrefix.size()), out);
  }

  std::string formatMediaTime(std::int64_t microseconds) {
    const std::int64_t totalSeconds = std::max<std::int64_t>(0, microseconds / 1'000'000);
    const std::int64_t minutes = totalSeconds / 60;
    const std::int64_t seconds = totalSeconds % 60;
    return std::format("{}:{:02}", minutes, seconds);
  }

} // namespace

LockSurface::LockSurface(WaylandConnection& connection, ConfigService* config) : Surface(connection), m_config(config) {
  {
    auto backgroundLayer = std::make_unique<Node>();
    backgroundLayer->setZIndex(0);
    m_backgroundLayer = m_root.addChild(std::move(backgroundLayer));
  }

  auto wallpaper = std::make_unique<WallpaperNode>();
  m_wallpaper = static_cast<WallpaperNode*>(m_backgroundLayer->addChild(std::move(wallpaper)));
  m_wallpaper->setZIndex(0);
  m_backgroundLayer->addChild(ui::box({
      .out = &m_tintOverlay,
      .visible = false,
      .configure = [](Box& box) { box.setZIndex(1); },
  }));
  m_backgroundLayer->addChild(ui::box({
      .out = &m_backdrop,
      .configure = [](Box& box) { box.setZIndex(-1); },
  }));

  {
    auto node = std::make_unique<Node>();
    node->setZIndex(2);
    m_leftColumn = m_root.addChild(std::move(node));
  }
  {
    auto node = std::make_unique<Node>();
    node->setZIndex(2);
    m_centerColumn = m_root.addChild(std::move(node));
  }
  {
    auto node = std::make_unique<Node>();
    node->setZIndex(2);
    m_rightColumn = m_root.addChild(std::move(node));
  }

  const auto card = [](Node& parent, Flex** out, float opacity = 0.78f) {
    parent.addChild(ui::column({
        .out = out,
        .align = FlexAlign::Stretch,
        .justify = FlexJustify::Center,
        .gap = Style::spaceSm,
        .padding = Style::cardPadding,
        .fill = colorSpecFromRole(ColorRole::Surface, opacity),
        .radius = Style::scaledRadiusXl(1.35f),
        .border = colorSpecFromRole(ColorRole::Outline, 0.24f),
        .borderWidth = Style::borderWidth,
        .clipChildren = true,
    }));
  };

  card(*m_leftColumn, &m_identityCard, 0.72f);
  auto identityRow = ui::row({
      .align = FlexAlign::Center,
      .justify = FlexJustify::Start,
      .gap = Style::spaceMd,
  });
  identityRow->addChild(ui::box({
      .out = &m_avatarFrame,
      .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.88f),
      .radius = 28.0f,
      .width = 56.0f,
      .height = 56.0f,
  }));
  m_avatarFrame->addChild(ui::image({
      .out = &m_avatarImage,
      .fit = ImageFit::Cover,
      .radius = 28.0f,
      .width = 56.0f,
      .height = 56.0f,
      .visible = false,
  }));
  m_avatarFrame->addChild(ui::glyph({
      .out = &m_avatarGlyph,
      .glyph = "person",
      .glyphSize = 26.0f,
      .color = colorSpecFromRole(ColorRole::Primary),
      .width = 56.0f,
      .height = 56.0f,
  }));
  identityRow->addChild(ui::column(
      {.align = FlexAlign::Stretch, .justify = FlexJustify::Center, .gap = Style::spaceXs, .flexGrow = 1.0f},
      ui::label({
          .out = &m_userLabel,
          .fontSize = Style::fontSizeBody,
          .fontWeight = FontWeight::SemiBold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      }),
      ui::label({
          .out = &m_identityHostLabel,
          .fontSize = Style::fontSizeMini,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      })
  ));
  m_identityCard->addChild(std::move(identityRow));

  m_leftColumn->addChild(ui::column({
      .out = &m_clockBlock,
      .align = FlexAlign::Start,
      .justify = FlexJustify::Center,
      .gap = Style::spaceXs,
      .paddingH = Style::spaceLg,
  }));
  m_clockBlock->addChild(ui::label({
      .out = &m_timeLabel,
      .text = "00:00",
      .fontSize = 88.0f,
      .fontWeight = FontWeight::Bold,
      .color = colorSpecFromRole(ColorRole::OnSurface),
      .maxLines = 1,
  }));
  m_clockBlock->addChild(ui::label({
      .out = &m_dateLabel,
      .fontSize = Style::fontSizeTitle,
      .fontWeight = FontWeight::Medium,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .maxLines = 1,
      .ellipsize = TextEllipsize::End,
  }));

  card(*m_leftColumn, &m_mediaCard, 0.80f);
  m_mediaCard->setJustify(FlexJustify::SpaceBetween);
  auto mediaHeader = ui::row({
      .align = FlexAlign::Center,
      .justify = FlexJustify::Start,
      .gap = Style::spaceMd,
  });
  mediaHeader->addChild(ui::box({
      .out = &m_mediaArtworkFrame,
      .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.9f),
      .radius = Style::scaledRadiusLg(),
      .width = 72.0f,
      .height = 72.0f,
  }));
  m_mediaArtworkFrame->addChild(ui::image({
      .out = &m_mediaArtwork,
      .fit = ImageFit::Cover,
      .radius = Style::scaledRadiusLg(),
      .width = 72.0f,
      .height = 72.0f,
      .visible = false,
  }));
  m_mediaArtworkFrame->addChild(ui::glyph({
      .out = &m_mediaArtworkFallback,
      .glyph = "music",
      .glyphSize = 30.0f,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .width = 72.0f,
      .height = 72.0f,
  }));
  mediaHeader->addChild(ui::column(
      {.align = FlexAlign::Stretch, .justify = FlexJustify::Center, .gap = Style::spaceXs, .flexGrow = 1.0f},
      ui::label({
          .out = &m_mediaHeaderLabel,
          .text = i18n::tr("lockscreen.dashboard.now-playing"),
          .fontSize = Style::fontSizeMini,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
      }),
      ui::label({
          .out = &m_mediaTitleLabel,
          .fontSize = Style::fontSizeBody,
          .fontWeight = FontWeight::SemiBold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      }),
      ui::label({
          .out = &m_mediaArtistLabel,
          .fontSize = Style::fontSizeCaption,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      })
  ));
  m_mediaCard->addChild(std::move(mediaHeader));
  m_mediaCard->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm},
      ui::progressBar({
          .out = &m_mediaProgress,
          .fill = colorSpecFromRole(ColorRole::Primary),
          .track = colorSpecFromRole(ColorRole::Outline, 0.28f),
          .radius = 3.0f,
          .progress = 0.0f,
          .height = 5.0f,
          .flexGrow = 1.0f,
      }),
      ui::label({
          .out = &m_mediaDurationLabel,
          .text = "0:00",
          .fontSize = Style::fontSizeMini,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
      })
  ));
  auto mediaControls = ui::row({
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .gap = Style::spaceSm,
  });
  mediaControls->addChild(ui::button({
      .out = &m_mediaPreviousButton,
      .glyph = "media-prev",
      .glyphSize = 18.0f,
      .controlHeight = Style::controlHeightSm,
      .variant = ButtonVariant::Ghost,
      .radius = Style::controlHeightSm * 0.5f,
      .onClick = [this]() {
        if (m_onMediaPrevious) m_onMediaPrevious();
      },
  }));
  mediaControls->addChild(ui::button({
      .out = &m_mediaPlayPauseButton,
      .glyph = "media-play",
      .glyphSize = 20.0f,
      .controlHeight = Style::controlHeight,
      .variant = ButtonVariant::Primary,
      .radius = Style::controlHeight * 0.5f,
      .onClick = [this]() {
        if (m_onMediaPlayPause) m_onMediaPlayPause();
      },
  }));
  mediaControls->addChild(ui::button({
      .out = &m_mediaNextButton,
      .glyph = "media-next",
      .glyphSize = 18.0f,
      .controlHeight = Style::controlHeightSm,
      .variant = ButtonVariant::Ghost,
      .radius = Style::controlHeightSm * 0.5f,
      .onClick = [this]() {
        if (m_onMediaNext) m_onMediaNext();
      },
  }));
  m_mediaCard->addChild(std::move(mediaControls));

  m_centerColumn->addChild(ui::node({
      .out = &m_heroLayer,
      .clipChildren = false,
  }));
  m_heroLayer->addChild(ui::box({
      .out = &m_heroBackSheetA,
      .fill = colorSpecFromRole(ColorRole::Surface, 0.58f),
      .radius = Style::scaledRadiusLg(),
      .participatesInLayout = false,
      .configure = [](Box& box) {
        box.setZIndex(0);
        box.setRotation(std::numbers::pi_v<float> * 0.018f);
      },
  }));
  m_heroLayer->addChild(ui::box({
      .out = &m_heroBackSheetB,
      .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.72f),
      .radius = Style::scaledRadiusLg(),
      .participatesInLayout = false,
      .configure = [](Box& box) {
        box.setZIndex(1);
        box.setRotation(-std::numbers::pi_v<float> * 0.014f);
      },
  }));
  m_heroLayer->addChild(ui::column({
      .out = &m_heroCard,
      .align = FlexAlign::Stretch,
      .justify = FlexJustify::Center,
      .gap = Style::spaceSm,
      .padding = Style::spaceMd,
      .fill = colorSpecFromRole(ColorRole::Surface, 0.9f),
      .radius = Style::scaledRadiusLg(),
      .border = colorSpecFromRole(ColorRole::Outline, 0.22f),
      .borderWidth = Style::borderWidth,
      .clipChildren = true,
      .participatesInLayout = false,
      .configure = [](Flex& flex) {
        flex.setZIndex(2);
        flex.setRotation(-std::numbers::pi_v<float> * 0.011f);
        flex.setShadow(colorSpecFromRole(ColorRole::Shadow, 0.18f), 18.0f, 0.0f, 6.0f);
      },
  }));
  m_heroCard->addChild(ui::node({
      .out = &m_heroImageFrame,
      .clipChildren = true,
  }));
  m_heroImageFrame->addChild(ui::image({
      .out = &m_heroImage,
      .fit = ImageFit::Cover,
      .radius = Style::scaledRadiusMd(),
      .visible = false,
      .participatesInLayout = false,
  }));
  m_heroImageFrame->addChild(ui::glyph({
      .out = &m_heroFallbackGlyph,
      .glyph = "landscape",
      .glyphSize = 72.0f,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.55f),
      .participatesInLayout = false,
  }));
  m_heroCard->addChild(ui::label({
      .out = &m_heroCaptionLabel,
      .text = i18n::tr("lockscreen.dashboard.nothing-playing"),
      .fontSize = Style::fontSizeTitle,
      .fontWeight = FontWeight::Medium,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .maxLines = 1,
      .textAlign = TextAlign::Center,
      .ellipsize = TextEllipsize::End,
  }));

  m_root.addChild(ui::column({
      .out = &m_loginPanel,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .gap = Style::spaceXs,
      .configure = [](Flex& flex) { flex.setZIndex(3); },
  }));
  m_loginPanel->addChild(ui::node({
      .out = &m_promptHost,
      .width = 460.0f,
      .height = Style::controlHeightLg,
  }));
  m_promptHost->addChild(ui::button({
      .out = &m_unlockButton,
      .text = i18n::tr("lockscreen.unlock"),
      .glyph = "lock",
      .fontSize = Style::fontSizeBody,
      .glyphSize = 18.0f,
      .controlHeight = Style::controlHeightLg,
      .variant = ButtonVariant::Primary,
      .minWidth = 210.0f,
      .radius = Style::controlHeightLg * 0.5f,
      .participatesInLayout = false,
      .onClick = [this]() { revealPasswordPrompt(); },
  }));
  m_promptHost->addChild(ui::row({
      .out = &m_loginContentRow,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .gap = Style::spaceSm,
      .fill = colorSpecFromRole(ColorRole::Surface, 0.88f),
      .radius = Style::controlHeightLg * 0.5f,
      .border = colorSpecFromRole(ColorRole::Outline, 0.28f),
      .borderWidth = Style::borderWidth,
      .visible = false,
      .participatesInLayout = false,
  }));
  m_passwordCapsule = m_loginContentRow;
  m_loginContentRow->addChild(ui::glyph({
      .out = &m_passwordLockGlyph,
      .glyph = "lock",
      .glyphSize = 16.0f,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .width = Style::controlHeightSm,
      .height = Style::controlHeightSm,
  }));
  m_loginContentRow->addChild(ui::input({
      .out = &m_passwordField,
      .placeholder = i18n::tr("lockscreen.password-placeholder"),
      .controlHeight = Style::controlHeightLg,
      .horizontalPadding = Style::spaceXs,
      .clearButtonEnabled = false,
      .passwordMode = true,
      .frameVisible = false,
      .textAlign = TextAlign::Center,
      .surfaceOpacity = 0.0f,
      .flexGrow = 1.0f,
      .onChange = [this](const std::string& value) {
        if (m_onPasswordChanged) m_onPasswordChanged(value);
      },
      .onSubmit = [this](const std::string&) {
        if (m_onLogin) m_onLogin();
      },
  }));
  m_loginContentRow->addChild(ui::button({
      .out = &m_loginButton,
      .glyph = "arrow-right",
      .glyphSize = 18.0f,
      .controlHeight = Style::controlHeightSm,
      .variant = ButtonVariant::Ghost,
      .radius = Style::controlHeightSm * 0.5f,
      .onClick = [this]() {
        if (m_onLogin) m_onLogin();
      },
  }));
  m_loginContentRow->addChild(ui::spinner({
      .out = &m_loginSpinner,
      .color = colorSpecFromRole(ColorRole::Primary),
      .spinnerSize = 18.0f,
      .thickness = 2.0f,
      .spinning = false,
      .width = Style::controlHeightSm,
      .height = Style::controlHeightSm,
      .visible = false,
  }));
  m_loginPanel->addChild(ui::button({
      .out = &m_layoutChip,
      .fontSize = Style::fontSizeMini,
      .controlHeight = Style::controlHeightSm,
      .variant = ButtonVariant::Ghost,
      .visible = false,
      .onClick = [this]() {
        if (m_onCycleLayout) m_onCycleLayout();
      },
  }));
  m_loginPanel->addChild(ui::label({
      .out = &m_statusLabel,
      .fontSize = Style::fontSizeCaption,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .textAlign = TextAlign::Center,
      .visible = false,
  }));

  card(*m_rightColumn, &m_calendarCard, 0.82f);
  auto calendarHeader = ui::row({
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceSm,
  });
  calendarHeader->addChild(ui::button({
      .out = &m_calendarPreviousButton,
      .glyph = "chevron-left",
      .glyphSize = 16.0f,
      .controlHeight = Style::controlHeightSm,
      .variant = ButtonVariant::Ghost,
      .tooltip = i18n::tr("lockscreen.calendar.previous"),
      .radius = Style::controlHeightSm * 0.5f,
      .onClick = [this]() { changeCalendarMonth(-1); },
  }));
  calendarHeader->addChild(ui::button({
      .out = &m_calendarMonthButton,
      .text = "",
      .fontSize = Style::fontSizeBody,
      .variant = ButtonVariant::Ghost,
      .tooltip = i18n::tr("control-center.calendar.today"),
      .flexGrow = 1.0f,
      .onClick = [this]() { changeCalendarMonth(-m_calendarMonthOffset); },
  }));
  calendarHeader->addChild(ui::button({
      .out = &m_calendarNextButton,
      .glyph = "chevron-right",
      .glyphSize = 16.0f,
      .controlHeight = Style::controlHeightSm,
      .variant = ButtonVariant::Ghost,
      .tooltip = i18n::tr("lockscreen.calendar.next"),
      .radius = Style::controlHeightSm * 0.5f,
      .onClick = [this]() { changeCalendarMonth(1); },
  }));
  m_calendarCard->addChild(std::move(calendarHeader));
  m_calendarCard->addChild(ui::column({
      .out = &m_calendarGrid,
      .align = FlexAlign::Stretch,
      .justify = FlexJustify::SpaceBetween,
      .gap = 2.0f,
      .flexGrow = 1.0f,
  }));
  {
    auto weekdays = ui::row({
        .align = FlexAlign::Center,
        .justify = FlexJustify::SpaceBetween,
        .gap = 2.0f,
    });
    for (auto& weekday : m_calendarWeekdayLabels) {
      weekdays->addChild(ui::label({
          .out = &weekday,
          .fontSize = Style::fontSizeMini,
          .fontWeight = FontWeight::Medium,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .textAlign = TextAlign::Center,
          .flexGrow = 1.0f,
      }));
    }
    m_calendarGrid->addChild(std::move(weekdays));
  }
  for (std::size_t rowIndex = 0; rowIndex < 6; ++rowIndex) {
    auto week = ui::row({
        .align = FlexAlign::Center,
        .justify = FlexJustify::SpaceBetween,
        .gap = 2.0f,
        .flexGrow = 1.0f,
    });
    for (std::size_t column = 0; column < 7; ++column) {
      const std::size_t index = rowIndex * 7 + column;
      auto day = ui::column({
          .out = &m_calendarCells[index].cell,
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
          .gap = 1.0f,
          .radius = Style::scaledRadiusMd(),
          .flexGrow = 1.0f,
      });
      day->addChild(ui::label({
          .out = &m_calendarCells[index].label,
          .fontSize = Style::fontSizeCaption,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .textAlign = TextAlign::Center,
      }));
      day->addChild(ui::box({
          .out = &m_calendarCells[index].eventDot,
          .fill = colorSpecFromRole(ColorRole::Primary),
          .radius = 1.5f,
          .width = 3.0f,
          .height = 3.0f,
          .visible = false,
      }));
      week->addChild(std::move(day));
    }
    m_calendarGrid->addChild(std::move(week));
  }

  card(*m_rightColumn, &m_weatherCard, 0.78f);
  auto weatherTop = ui::row({
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceMd,
  });
  weatherTop->addChild(ui::glyph({
      .out = &m_weatherGlyph,
      .glyph = "weather-cloud-off",
      .glyphSize = 46.0f,
      .color = colorSpecFromRole(ColorRole::Primary),
  }));
  weatherTop->addChild(ui::column(
      {.align = FlexAlign::End, .justify = FlexJustify::Center, .gap = Style::spaceXs, .flexGrow = 1.0f},
      ui::label({
          .out = &m_weatherTemperatureLabel,
          .fontSize = Style::fontSizeHeader * 1.5f,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
      }),
      ui::label({
          .out = &m_weatherConditionLabel,
          .fontSize = Style::fontSizeCaption,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      })
  ));
  m_weatherCard->addChild(std::move(weatherTop));
  m_weatherCard->addChild(ui::label({
      .out = &m_weatherTitleLabel,
      .text = "",
      .fontSize = Style::fontSizeMini,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .maxLines = 1,
      .ellipsize = TextEllipsize::End,
  }));
  m_weatherCard->addChild(ui::label({
      .out = &m_weatherDetailLabel,
      .fontSize = Style::fontSizeMini,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .maxLines = 2,
  }));

  card(*m_rightColumn, &m_resourcesCard, 0.80f);
  m_resourcesCard->setPadding(Style::spaceMd, Style::spaceLg);
  const std::array<std::string, 3> metricLabels = {
      i18n::tr("lockscreen.dashboard.battery"),
      i18n::tr("lockscreen.dashboard.memory"),
      i18n::tr("lockscreen.dashboard.storage"),
  };
  for (std::size_t i = 0; i < m_metricViews.size(); ++i) {
    auto& metric = m_metricViews[i];
    auto row = ui::row({
        .out = &metric.row,
        .align = FlexAlign::Center,
        .justify = FlexJustify::SpaceBetween,
        .gap = Style::spaceSm,
        .flexGrow = 1.0f,
    });
    row->addChild(ui::glyph({
        .out = &metric.glyph,
        .glyph = i == 0 ? "battery" : i == 1 ? "memory" : "storage",
        .glyphSize = 16.0f,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .width = 22.0f,
        .height = 22.0f,
    }));
    row->addChild(ui::label({
        .out = &metric.label,
        .text = metricLabels[i],
        .fontSize = Style::fontSizeMini,
        .color = colorSpecFromRole(ColorRole::OnSurface),
        .minWidth = 54.0f,
        .maxLines = 1,
    }));
    row->addChild(ui::progressBar({
        .out = &metric.progress,
        .fill = colorSpecFromRole(ColorRole::Primary),
        .track = colorSpecFromRole(ColorRole::Outline, 0.26f),
        .radius = 3.0f,
        .progress = 0.0f,
        .height = 6.0f,
        .flexGrow = 1.0f,
    }));
    row->addChild(ui::label({
        .out = &metric.value,
        .text = "—",
        .fontSize = Style::fontSizeMini,
        .fontWeight = FontWeight::Medium,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .minWidth = 38.0f,
        .maxLines = 1,
        .textAlign = TextAlign::End,
    }));
    m_resourcesCard->addChild(std::move(row));
  }

  m_root.addChild(ui::button({
      .out = &m_notificationButton,
      .glyph = "notification",
      .glyphSize = 18.0f,
      .controlHeight = Style::controlHeightLg,
      .variant = ButtonVariant::Secondary,
      .tooltip = i18n::tr("lockscreen.dashboard.notifications"),
      .radius = Style::controlHeightLg * 0.5f,
      .onClick = [this]() { setNotificationPanelOpen(!m_notificationPanelOpen); },
      .configure = [](Button& button) { button.setZIndex(4); },
  }));
  m_root.addChild(ui::inputArea({
      .out = &m_notificationBackdropArea,
      .propagateEvents = false,
      .visible = false,
      .participatesInLayout = false,
      .onClick = [this](const InputArea::PointerData& data) {
        if (m_notificationPanel == nullptr
            || data.localX < m_notificationPanel->x()
            || data.localX > m_notificationPanel->x() + m_notificationPanel->width()
            || data.localY < m_notificationPanel->y()
            || data.localY > m_notificationPanel->y() + m_notificationPanel->height()) {
          setNotificationPanelOpen(false);
        }
      },
      .configure = [](InputArea& area) { area.setZIndex(10); },
  }));
  m_root.addChild(ui::column({
      .out = &m_notificationPanel,
      .align = FlexAlign::Stretch,
      .justify = FlexJustify::Start,
      .gap = Style::spaceSm,
      .padding = Style::cardPadding,
      .fill = colorSpecFromRole(ColorRole::Surface, 0.96f),
      .radius = Style::scaledRadiusXl(1.35f),
      .border = colorSpecFromRole(ColorRole::Outline, 0.28f),
      .borderWidth = Style::borderWidth,
      .clipChildren = true,
      .visible = false,
      .participatesInLayout = false,
      .configure = [](Flex& flex) {
        flex.setZIndex(11);
        flex.setShadow(colorSpecFromRole(ColorRole::Shadow, 0.18f), 18.0f, 0.0f, 5.0f);
      },
  }));
  auto notificationHeader = ui::row({
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceSm,
  });
  notificationHeader->addChild(ui::label({
      .out = &m_notificationsHeaderLabel,
      .text = i18n::tr("lockscreen.dashboard.notifications"),
      .fontSize = Style::fontSizeBody,
      .fontWeight = FontWeight::SemiBold,
      .color = colorSpecFromRole(ColorRole::OnSurface),
      .maxLines = 1,
      .flexGrow = 1.0f,
  }));
  notificationHeader->addChild(ui::button({
      .out = &m_notificationCloseButton,
      .glyph = "close",
      .glyphSize = 16.0f,
      .controlHeight = Style::controlHeightSm,
      .variant = ButtonVariant::Ghost,
      .tooltip = i18n::tr("lockscreen.notifications.close"),
      .radius = Style::scaledRadiusMd(),
      .onClick = [this]() { setNotificationPanelOpen(false); },
  }));
  m_notificationPanel->addChild(std::move(notificationHeader));
  m_notificationPanel->addChild(ui::column(
      {.out = &m_notificationsEmpty,
       .align = FlexAlign::Center,
       .justify = FlexJustify::Center,
       .gap = Style::spaceSm,
       .flexGrow = 1.0f},
      ui::glyph({
          .out = &m_notificationsEmptyGlyph,
          .glyph = "notifications-off",
          .glyphSize = 42.0f,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.5f),
      }),
      ui::label({
          .out = &m_notificationsLabel,
          .fontSize = Style::fontSizeCaption,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
          .textAlign = TextAlign::Center,
      })
  ));
  for (auto& preview : m_notificationViews) {
    m_notificationPanel->addChild(ui::column(
        {.out = &preview.row,
         .align = FlexAlign::Stretch,
         .justify = FlexJustify::Start,
         .gap = Style::spaceXs,
         .paddingV = Style::spaceXs,
         .visible = false},
        ui::label({
            .out = &preview.app,
            .fontSize = Style::fontSizeMini,
            .fontWeight = FontWeight::SemiBold,
            .color = colorSpecFromRole(ColorRole::Primary),
            .maxLines = 1,
            .ellipsize = TextEllipsize::End,
        }),
        ui::label({
            .out = &preview.summary,
            .fontSize = Style::fontSizeCaption,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 2,
            .ellipsize = TextEllipsize::End,
        })
    ));
  }

  m_inputDispatcher.setSceneRoot(&m_root);
  m_inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_connection.setCursorShape(serial, shape);
  });
  m_root.setAnimationManager(&m_animations);
  setAnimationManager(&m_animations);
  setSceneRoot(&m_root);
  setConfigureCallback([this](std::uint32_t, std::uint32_t) { requestLayout(); });
  setPrepareFrameCallback([this](bool needsUpdate, bool needsLayout) { prepareFrame(needsUpdate, needsLayout); });
  applyLockscreenPalette();
  requestUpdate();
}

LockSurface::~LockSurface() {
  releaseCaptureTextures();
  if (m_wallpaperTexture.id != 0) {
    releaseWallpaperTextureRef(m_textureWallpaperPath);
  }
  m_connection.unregisterSurface(m_surface);
  if (m_lockSurface != nullptr) {
    ext_session_lock_surface_v1_destroy(m_lockSurface);
    m_lockSurface = nullptr;
  }
}

bool LockSurface::initialize(ext_session_lock_v1* lock, wl_output* output, std::int32_t scale) {
  if (lock == nullptr || output == nullptr || renderContext() == nullptr) {
    return false;
  }

  if (!createWlSurface()) {
    return false;
  }
  m_inputDispatcher.setTextInputContext(m_surface, m_connection.textInputService());

  m_output = output;
  m_connection.registerSurfaceOutput(m_surface, output);
  setBufferScale(scale);

  m_lockSurface = ext_session_lock_v1_get_lock_surface(lock, m_surface, output);
  if (m_lockSurface == nullptr) {
    destroySurface();
    return false;
  }

  if (ext_session_lock_surface_v1_add_listener(m_lockSurface, &kLockSurfaceListener, this) != 0) {
    ext_session_lock_surface_v1_destroy(m_lockSurface);
    m_lockSurface = nullptr;
    destroySurface();
    return false;
  }

  setRunning(true);
  return true;
}

void LockSurface::setLockedState(bool locked) {
  if (m_locked == locked) {
    return;
  }
  m_locked = locked;
  if (m_locked) {
    m_introPending = true;
    m_introStarted = false;
    m_calendarMonthOffset = 0;
    setNotificationPanelOpen(false, false);
    setPasswordPromptVisible(false, false);
    m_inputDispatcher.setFocus(nullptr);
  } else {
    m_animations.cancelAll();
    m_unlocking = false;
    m_introPending = false;
    m_introStarted = false;
    m_introOffsetY = 0.0f;
    m_introSideOffset = 0.0f;
    m_introOpacity = 1.0f;
    m_passwordErrorOffsetX = 0.0f;
    m_lastPromptWasError = false;
    m_passwordPromptVisible = false;
    m_notificationPanelOpen = false;
    m_inputDispatcher.setFocus(nullptr);
  }
  requestUpdate();
}

void LockSurface::onSecondTick() {
  if (!m_blackout) {
    requestUpdateOnly();
  }
}

void LockSurface::startIntroAnimation() {
  if (!m_locked || m_blackout || m_introStarted || m_loginPanel == nullptr) {
    return;
  }

  m_introPending = false;
  m_introStarted = true;
  m_introOffsetY = 18.0f;
  m_introSideOffset = 14.0f;
  m_introOpacity = 0.0f;
  for (Node* node : {static_cast<Node*>(m_loginPanel), m_leftColumn, m_centerColumn, m_rightColumn}) {
    if (node != nullptr) {
      node->setOpacity(0.0f);
    }
  }

  m_animations.animate(
      m_introOffsetY, 0.0f, 200.0f, Easing::CaelestiaExpressiveSpatial,
      [this](float value) {
        m_introOffsetY = value;
        requestLayout();
      },
      {}, m_loginPanel
  );
  m_animations.animate(
      m_introSideOffset, 0.0f, 200.0f, Easing::CaelestiaExpressiveSpatial,
      [this](float value) {
        m_introSideOffset = value;
        requestLayout();
      },
      {}, m_leftColumn
  );
  m_animations.animate(
      m_introOpacity, 1.0f, 180.0f, Easing::EaseOutCubic,
      [this](float value) {
        m_introOpacity = std::clamp(value, 0.0f, 1.0f);
        for (Node* node : {static_cast<Node*>(m_loginPanel), m_leftColumn, m_centerColumn, m_rightColumn}) {
          if (node != nullptr) {
            node->setOpacity(m_introOpacity);
          }
        }
      },
      {}, m_centerColumn
  );
  requestFrameTick();
}

void LockSurface::startPasswordErrorAnimation() {
  if (m_passwordCapsule == nullptr || m_blackout || !m_locked) {
    return;
  }
  m_animations.cancelForOwner(m_passwordCapsule);
  m_animations.animate(
      0.0f, 1.0f, 180.0f, Easing::EaseOutCubic,
      [this](float progress) {
        m_passwordErrorOffsetX =
            std::sin(progress * std::numbers::pi_v<float> * 4.0f) * (1.0f - progress) * 8.0f;
        requestLayout();
      },
      [this]() {
        m_passwordErrorOffsetX = 0.0f;
        requestLayout();
      },
      m_passwordCapsule
  );
  requestFrameTick();
}

void LockSurface::beginUnlockAnimation(std::function<void()> finished) {
  if (m_unlocking) {
    return;
  }
  m_unlocking = true;
  setNotificationPanelOpen(false, false);
  m_animations.cancelAll();
  const float startOpacity = m_loginPanel != nullptr ? m_loginPanel->opacity() : 1.0f;
  m_animations.animate(
      startOpacity, 0.0f, 150.0f, Easing::EaseInOutQuad,
      [this](float value) {
        for (Node* node : {static_cast<Node*>(m_loginPanel), m_leftColumn, m_centerColumn, m_rightColumn,
                           static_cast<Node*>(m_notificationButton)}) {
          if (node != nullptr) {
            node->setOpacity(value);
          }
        }
      },
      [this, finished = std::move(finished)]() mutable {
        m_unlocking = false;
        if (finished) {
          finished();
        }
      },
      m_loginPanel
  );
  requestFrameTick();
}

void LockSurface::applyLockscreenPalette() {
  const ColorSpec surface = colorSpecFromRole(ColorRole::Surface, 0.80f);
  const ColorSpec elevated = colorSpecFromRole(ColorRole::Surface, 0.96f);
  const ColorSpec outline = colorSpecFromRole(ColorRole::Outline, 0.26f);
  const ColorSpec primary = colorSpecFromRole(ColorRole::Primary);
  const ColorSpec muted = colorSpecFromRole(ColorRole::OnSurfaceVariant);

  for (Flex* card : {m_identityCard, m_mediaCard, m_calendarCard, m_weatherCard, m_resourcesCard}) {
    if (card != nullptr) {
      card->setFill(surface);
      card->setBorder(outline, Style::borderWidth);
    }
  }
  if (m_notificationPanel != nullptr) {
    m_notificationPanel->setFill(elevated);
    m_notificationPanel->setBorder(outline, Style::borderWidth);
    m_notificationPanel->setShadow(colorSpecFromRole(ColorRole::Shadow, 0.18f), 18.0f, 0.0f, 5.0f);
  }
  if (m_heroCard != nullptr) {
    m_heroCard->setFill(colorSpecFromRole(ColorRole::Surface, 0.90f));
    m_heroCard->setBorder(colorSpecFromRole(ColorRole::Outline, 0.22f), Style::borderWidth);
    m_heroCard->setShadow(colorSpecFromRole(ColorRole::Shadow, 0.18f), 18.0f, 0.0f, 6.0f);
  }
  if (m_heroBackSheetA != nullptr) {
    m_heroBackSheetA->setFill(colorForRole(ColorRole::Surface, 0.58f));
  }
  if (m_heroBackSheetB != nullptr) {
    m_heroBackSheetB->setFill(colorForRole(ColorRole::SurfaceVariant, 0.72f));
  }
  if (m_passwordCapsule != nullptr) {
    m_passwordCapsule->setFill(colorSpecFromRole(ColorRole::Surface, 0.88f));
    m_passwordCapsule->setBorder(
        m_error ? colorSpecFromRole(ColorRole::Error) : outline,
        m_error ? 1.5f : Style::borderWidth
    );
  }
  if (m_weatherGlyph != nullptr) m_weatherGlyph->setColor(primary);
  if (m_avatarGlyph != nullptr) m_avatarGlyph->setColor(primary);
  if (m_loginSpinner != nullptr) m_loginSpinner->setColor(colorForRole(ColorRole::Primary));
  for (auto& metric : m_metricViews) {
    if (metric.progress != nullptr) {
      metric.progress->setFill(primary);
      metric.progress->setTrack(colorSpecFromRole(ColorRole::Outline, 0.26f));
    }
  }
  for (auto& preview : m_notificationViews) {
    if (preview.app != nullptr) preview.app->setColor(colorForRole(ColorRole::Primary));
  }
  if (m_statusLabel != nullptr && !m_error) {
    m_statusLabel->setColor(colorForRole(ColorRole::OnSurfaceVariant));
  }
  if (m_heroCaptionLabel != nullptr) {
    m_heroCaptionLabel->setColor(muted);
  }
}

void LockSurface::setPasswordPromptVisible(bool visible, bool animate) {
  if (m_passwordPromptVisible == visible && m_promptTransition == (visible ? 1.0f : 0.0f)) {
    if (visible) {
      focusPasswordField();
    }
    return;
  }
  m_passwordPromptVisible = visible;
  if (m_unlockButton == nullptr || m_loginContentRow == nullptr) {
    return;
  }

  m_animations.cancelForOwner(m_promptHost);
  m_unlockButton->setVisible(true);
  m_loginContentRow->setVisible(true);
  const float target = visible ? 1.0f : 0.0f;
  const auto apply = [this](float value) {
    m_promptTransition = std::clamp(value, 0.0f, 1.0f);
    if (m_unlockButton != nullptr) m_unlockButton->setOpacity(1.0f - m_promptTransition);
    if (m_loginContentRow != nullptr) m_loginContentRow->setOpacity(m_promptTransition);
  };
  const auto finish = [this, visible]() {
    if (m_unlockButton != nullptr) m_unlockButton->setVisible(!visible);
    if (m_loginContentRow != nullptr) m_loginContentRow->setVisible(visible);
  };

  if (!animate) {
    apply(target);
    finish();
  } else {
    m_animations.animate(m_promptTransition, target, 180.0f, Easing::EaseOutCubic, apply, finish, m_promptHost);
    requestFrameTick();
  }

  if (visible) {
    focusPasswordField();
  } else {
    m_inputDispatcher.setFocus(nullptr);
  }
  requestLayout();
}

void LockSurface::revealPasswordPrompt() {
  if (!m_locked || m_blackout) {
    return;
  }
  setNotificationPanelOpen(false);
  setPasswordPromptVisible(true);
}

void LockSurface::collapsePasswordPrompt() {
  if (!m_locked || m_authenticating || !m_password.empty()) {
    return;
  }
  setPasswordPromptVisible(false);
}

void LockSurface::setNotificationPanelOpen(bool open, bool animate) {
  if (m_notificationPanel == nullptr || m_notificationBackdropArea == nullptr) {
    m_notificationPanelOpen = open;
    return;
  }
  if (m_notificationPanelOpen == open && m_notificationPanel->visible() == open) {
    return;
  }

  m_notificationPanelOpen = open;
  m_animations.cancelForOwner(m_notificationPanel);
  if (open) {
    m_notificationBackdropArea->setVisible(true);
    m_notificationPanel->setVisible(true);
    m_notificationPanel->setOpacity(animate ? 0.0f : 1.0f);
    m_notificationPanelOffsetY = animate ? -4.0f : 0.0f;
    if (animate) {
      m_animations.animate(
          0.0f, 1.0f, 120.0f, Easing::EaseOutCubic,
          [this](float value) {
            if (m_notificationPanel != nullptr) m_notificationPanel->setOpacity(value);
            m_notificationPanelOffsetY = -4.0f * (1.0f - value);
            requestLayout();
          },
          {}, m_notificationPanel
      );
      requestFrameTick();
    }
  } else {
    m_notificationBackdropArea->setVisible(false);
    if (!animate) {
      m_notificationPanel->setOpacity(0.0f);
      m_notificationPanel->setVisible(false);
      m_notificationPanelOffsetY = 0.0f;
    } else {
      m_animations.animate(
          m_notificationPanel->opacity(), 0.0f, 80.0f, Easing::EaseInOutQuad,
          [this](float value) {
            if (m_notificationPanel != nullptr) m_notificationPanel->setOpacity(value);
            m_notificationPanelOffsetY = -4.0f * (1.0f - value);
            requestLayout();
          },
          [this]() {
            if (m_notificationPanel != nullptr) m_notificationPanel->setVisible(false);
            m_notificationPanelOffsetY = 0.0f;
          },
          m_notificationPanel
      );
      requestFrameTick();
    }
  }
  requestLayout();
}

bool LockSurface::dismissTransientUi() {
  if (m_notificationPanelOpen) {
    setNotificationPanelOpen(false);
    return true;
  }
  return false;
}

void LockSurface::changeCalendarMonth(int delta) {
  if (delta == 0) {
    return;
  }
  m_calendarMonthOffset = std::clamp(m_calendarMonthOffset + delta, -12, 12);
  if (m_calendarGrid == nullptr) {
    requestUpdate();
    return;
  }
  m_animations.cancelForOwner(m_calendarGrid);
  m_calendarGrid->setOpacity(0.0f);
  m_calendarSlideOffset = delta > 0 ? 8.0f : -8.0f;
  requestUpdate();
  m_animations.animate(
      m_calendarSlideOffset, 0.0f, 160.0f, Easing::EaseOutCubic,
      [this](float value) {
        m_calendarSlideOffset = value;
        const float progress = 1.0f - std::min(1.0f, std::abs(value) / 8.0f);
        if (m_calendarGrid != nullptr) m_calendarGrid->setOpacity(progress);
        requestLayout();
      },
      [this]() {
        if (m_calendarGrid != nullptr) m_calendarGrid->setOpacity(1.0f);
        m_calendarSlideOffset = 0.0f;
      },
      m_calendarGrid
  );
  requestFrameTick();
}

void LockSurface::syncAvatar(Renderer& renderer, float avatarSize) {
  if (m_avatarFrame == nullptr || m_avatarImage == nullptr || m_avatarGlyph == nullptr) {
    return;
  }

  m_avatarFrame->setSize(avatarSize, avatarSize);
  m_avatarFrame->setRadius(avatarSize * 0.5f);
  m_avatarImage->setPosition(0.0f, 0.0f);
  m_avatarImage->setSize(avatarSize, avatarSize);
  m_avatarImage->setRadius(avatarSize * 0.5f);
  m_avatarGlyph->setPosition(0.0f, 0.0f);
  m_avatarGlyph->setSize(avatarSize, avatarSize);
  m_avatarGlyph->setGlyphSize(avatarSize * 0.48f);

  const std::string avatarPath = !m_dashboard.avatarPath.empty()
      ? m_dashboard.avatarPath
      : (m_config != nullptr ? m_config->config().shell.avatarPath : std::string{});
  const int targetSize = std::max(1, static_cast<int>(std::round(avatarSize)));
  if (avatarPath != m_loadedAvatarPath || targetSize != m_loadedAvatarSize) {
    if (m_avatarImage->hasImage()) {
      m_avatarImage->clear(renderer);
    }
    const bool loaded = !avatarPath.empty()
        && m_avatarImage->setSourceFile(renderer, avatarPath, targetSize, false, true);
    m_avatarImage->setVisible(loaded);
    m_avatarGlyph->setVisible(!loaded);
    m_loadedAvatarPath = avatarPath;
    m_loadedAvatarSize = targetSize;
  }
}

void LockSurface::syncHeroArtwork(Renderer& renderer, float artworkSize) {
  if (m_heroImage == nullptr || m_heroFallbackGlyph == nullptr) {
    return;
  }

  const int targetSize = std::max(1, static_cast<int>(std::round(artworkSize)));
  const std::string avatarPath = !m_dashboard.avatarPath.empty()
      ? m_dashboard.avatarPath
      : (m_config != nullptr ? m_config->config().shell.avatarPath : std::string{});
  const std::string preferredPath = !m_dashboard.mediaArtworkPath.empty() ? m_dashboard.mediaArtworkPath : avatarPath;
  bool loaded = false;
  if (!preferredPath.empty()) {
    if (preferredPath != m_loadedHeroPath || targetSize != m_loadedHeroSize || !m_heroImage->hasImage()) {
      loaded = m_heroImage->setSourceFile(renderer, preferredPath, targetSize, true, true);
      m_loadedHeroPath = loaded ? preferredPath : std::string{};
      m_loadedHeroSize = targetSize;
    } else {
      loaded = true;
    }
  } else {
    const TextureHandle background = m_blurredDesktopTexture.valid() ? m_blurredDesktopTexture
        : m_blurredWallpaperTexture.valid()                          ? m_blurredWallpaperTexture
        : m_wallpaperTexture;
    if (background.valid()) {
      m_heroImage->setExternalTexture(renderer, background);
      m_loadedHeroPath.clear();
      m_loadedHeroSize = targetSize;
      loaded = true;
    } else if (m_heroImage->hasImage()) {
      m_heroImage->clear(renderer);
      m_loadedHeroPath.clear();
    }
  }
  m_heroImage->setVisible(loaded);
  m_heroFallbackGlyph->setVisible(!loaded);

  if (m_mediaArtwork != nullptr && m_mediaArtworkFallback != nullptr) {
    const int thumbnailSize = std::max(48, static_cast<int>(std::round(72.0f * artworkSize / 420.0f)));
    bool mediaLoaded = false;
    if (!m_dashboard.mediaArtworkPath.empty()) {
      if (m_dashboard.mediaArtworkPath != m_loadedMediaArtworkPath
          || thumbnailSize != m_loadedMediaArtworkSize
          || !m_mediaArtwork->hasImage()) {
        mediaLoaded = m_mediaArtwork->setSourceFile(
            renderer, m_dashboard.mediaArtworkPath, thumbnailSize, true, true
        );
        m_loadedMediaArtworkPath = mediaLoaded ? m_dashboard.mediaArtworkPath : std::string{};
        m_loadedMediaArtworkSize = thumbnailSize;
      } else {
        mediaLoaded = true;
      }
    } else if (m_mediaArtwork->hasImage()) {
      m_mediaArtwork->clear(renderer);
      m_loadedMediaArtworkPath.clear();
    }
    m_mediaArtwork->setVisible(mediaLoaded);
    m_mediaArtworkFallback->setVisible(!mediaLoaded);
  }
}

bool LockSurface::passwordFieldContainsPoint(float sceneX, float sceneY) const {
  return m_passwordField != nullptr && m_passwordField->containsScenePoint(sceneX, sceneY);
}

void LockSurface::focusPasswordField() {
  if (!m_locked || m_blackout || !m_passwordPromptVisible || m_passwordField == nullptr) {
    return;
  }
  m_inputDispatcher.setFocus(m_passwordField->inputArea());
}

void LockSurface::setPromptState(
    std::string user, std::string password, std::string status, bool error, bool authenticating
) {
  if (m_user == user
      && m_password == password
      && m_status == status
      && m_error == error
      && m_authenticating == authenticating) {
    return;
  }
  m_user = std::move(user);
  m_password = std::move(password);
  m_status = std::move(status);
  m_error = error;
  m_authenticating = authenticating;
  if (m_locked && (m_authenticating || m_error || !m_password.empty())) {
    setPasswordPromptVisible(true);
  }
  if (m_error && !m_lastPromptWasError) {
    startPasswordErrorAnimation();
  }
  m_lastPromptWasError = m_error;
  requestUpdate();
}

void LockSurface::setKeyboardIndicators(
    bool capsLock, bool hasMultipleLayouts, bool layoutSwitchable, std::string layoutLabel
) {
  if (m_capsLock == capsLock
      && m_hasMultipleLayouts == hasMultipleLayouts
      && m_layoutSwitchable == layoutSwitchable
      && m_layoutLabel == layoutLabel) {
    return;
  }
  m_capsLock = capsLock;
  m_hasMultipleLayouts = hasMultipleLayouts;
  m_layoutSwitchable = layoutSwitchable;
  m_layoutLabel = std::move(layoutLabel);
  requestUpdate();
}

void LockSurface::setDashboardState(LockscreenDashboardState state) {
  if (m_dashboard == state) {
    return;
  }
  m_dashboard = std::move(state);
  requestUpdateOnly();
}

void LockSurface::setWallpaperPath(std::string wallpaperPath) {
  if (m_wallpaperPath == wallpaperPath) {
    return;
  }

  if (m_blurredWallpaperTexture.id != 0 && renderContext() != nullptr) {
    renderContext()->backend().makeCurrentNoSurface();
    renderContext()->textureManager().unload(m_blurredWallpaperTexture);
    m_blurredWallpaperTexture = {};
  }

  // Keep the current wallpaper visible until applyWallpaperTexture() loads the new path.
  m_wallpaperPath = std::move(wallpaperPath);
  m_wallpaperDirty = true;
  requestLayout();
}

void LockSurface::setWallpaperFillMode(WallpaperFillMode fillMode) {
  if (m_wallpaperFillMode == fillMode) {
    return;
  }
  m_wallpaperFillMode = fillMode;
  if (m_wallpaper != nullptr) {
    m_wallpaper->setFillMode(m_wallpaperFillMode);
  }
  requestRedraw();
}

void LockSurface::setWallpaperFillColor(Color fillColor) {
  if (m_wallpaperFillColor == fillColor) {
    return;
  }
  m_wallpaperFillColor = fillColor;
  if (m_wallpaper != nullptr) {
    m_wallpaper->setFillColor(m_wallpaperFillColor);
  }
  if (m_backdrop != nullptr) {
    m_backdrop->setVisible(m_wallpaperFillColor.a > 0.0f);
    m_backdrop->setStyle(
        RoundedRectStyle{
            .fill = m_wallpaperFillColor,
            .fillMode = FillMode::Solid,
        }
    );
  }
  requestRedraw();
}

void LockSurface::setDesktopCapture(std::optional<ScreencopyImage> capture) {
  m_desktopCapture = std::move(capture);
  m_captureDirty = true;
  releaseCaptureTextures();
  requestLayout();
}

bool LockSurface::hasDesktopCapture() const noexcept {
  return m_desktopCapture.has_value() && !m_desktopCapture->rgba.empty();
}

void LockSurface::setBackgroundStyle(float blurIntensity, float tintIntensity) {
  if (m_blurIntensity == blurIntensity && m_tintIntensity == tintIntensity) {
    return;
  }
  m_blurIntensity = blurIntensity;
  m_tintIntensity = tintIntensity;
  m_captureDirty = true;
  m_blurCache.invalidate();
  m_wallpaperDirty = true;
  m_wallpaperBlurCache.invalidate();
  requestLayout();
}

void LockSurface::setBlackout(bool blackout) {
  if (m_blackout == blackout) {
    return;
  }
  m_blackout = blackout;
  if (m_blackout) {
    m_inputDispatcher.setFocus(nullptr);
  }
  requestLayout();
}

void LockSurface::setOnLogin(std::function<void()> onLogin) { m_onLogin = std::move(onLogin); }

void LockSurface::setOnCycleLayout(std::function<void()> onCycleLayout) { m_onCycleLayout = std::move(onCycleLayout); }

void LockSurface::setOnPasswordChanged(std::function<void(const std::string&)> onPasswordChanged) {
  m_onPasswordChanged = std::move(onPasswordChanged);
}

void LockSurface::setOnMediaPrevious(std::function<void()> callback) { m_onMediaPrevious = std::move(callback); }

void LockSurface::setOnMediaPlayPause(std::function<void()> callback) { m_onMediaPlayPause = std::move(callback); }

void LockSurface::setOnMediaNext(std::function<void()> callback) { m_onMediaNext = std::move(callback); }

void LockSurface::selectAllPassword() {
  if (m_passwordField == nullptr) {
    return;
  }
  m_passwordField->selectAll();
  requestLayout();
}

void LockSurface::clearPasswordSelection() {
  if (m_passwordField == nullptr) {
    return;
  }
  m_passwordField->clearSelection();
  requestLayout();
}

void LockSurface::onPointerEvent(const PointerEvent& event) {
  if (m_blackout) {
    return;
  }

  switch (event.type) {
  case PointerEvent::Type::Enter:
    m_inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    break;
  case PointerEvent::Type::Leave:
    m_inputDispatcher.pointerLeave();
    break;
  case PointerEvent::Type::Motion:
    m_inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    break;
  case PointerEvent::Type::Button: {
    const bool pressed = event.state == WL_POINTER_BUTTON_STATE_PRESSED;
    const auto x = static_cast<float>(event.sx);
    const auto y = static_cast<float>(event.sy);
    if (m_locked && pressed && passwordFieldContainsPoint(x, y)) {
      focusPasswordField();
    }
    m_inputDispatcher.pointerButton(x, y, event.button, pressed);
    if (m_locked && pressed && passwordFieldContainsPoint(x, y)) {
      focusPasswordField();
      requestRedraw();
    }
    break;
  }
  case PointerEvent::Type::Axis:
    m_inputDispatcher.pointerAxis(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
        event.axisDiscrete, event.axisValue120, event.axisLines
    );
    break;
  }

  if (m_root.paintDirty() || m_root.layoutDirty()) {
    if (m_root.layoutDirty()) {
      requestLayout();
    } else {
      requestRedraw();
    }
  }
}

void LockSurface::onThemeChanged() {
  m_captureDirty = true;
  applyLockscreenPalette();
  requestLayout();
}

void LockSurface::onKeyboardEvent(const KeyboardEvent& event) {
  if (m_blackout) {
    return;
  }

  if (m_locked
      && event.pressed
      && m_passwordField != nullptr
      && m_inputDispatcher.focusedArea() != m_passwordField->inputArea()) {
    focusPasswordField();
  }
  m_inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  if (m_root.paintDirty() || m_root.layoutDirty()) {
    if (m_root.layoutDirty()) {
      requestLayout();
    } else {
      requestRedraw();
    }
  }
}

void LockSurface::handleConfigure(
    void* data, ext_session_lock_surface_v1* lockSurface, std::uint32_t serial, std::uint32_t width,
    std::uint32_t height
) {
  auto* self = static_cast<LockSurface*>(data);
  if (self->width() != width || self->height() != height) {
    self->m_firstFrameRendered = false;
  }
  ext_session_lock_surface_v1_ack_configure(lockSurface, serial);
  self->Surface::onConfigure(width, height);
}

void LockSurface::prepareFrame(bool needsUpdate, bool needsLayout) {
  auto* renderer = renderContext();
  if (renderer == nullptr || width() == 0 || height() == 0) {
    return;
  }

  renderer->makeCurrent(renderTarget());

  if (needsUpdate) {
    UiPhaseScope updatePhase(UiPhase::Update);
    updateCopy();
  }

  if (needsUpdate || needsLayout) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    layoutScene(width(), height());
  }
}

void LockSurface::layoutScene(std::uint32_t width, std::uint32_t height) {
  auto* renderer = renderContext();
  if (renderer == nullptr) {
    return;
  }

  const auto sw = static_cast<float>(width);
  const auto sh = static_cast<float>(height);
  m_root.setSize(sw, sh);
  m_backgroundLayer->setPosition(0.0f, 0.0f);
  m_backgroundLayer->setSize(sw, sh);

  if (m_blackout) {
    m_wallpaper->setVisible(false);
    m_tintOverlay->setVisible(false);
    m_backdrop->setPosition(0.0f, 0.0f);
    m_backdrop->setSize(sw, sh);
    m_backdrop->setVisible(true);
    m_backdrop->setStyle(
        RoundedRectStyle{
            .fill = rgba(0.0f, 0.0f, 0.0f, 1.0f),
            .fillMode = FillMode::Solid,
        }
    );
    for (Node* node : {m_leftColumn, m_centerColumn, m_rightColumn, static_cast<Node*>(m_loginPanel),
                       static_cast<Node*>(m_notificationButton), static_cast<Node*>(m_notificationPanel),
                       static_cast<Node*>(m_notificationBackdropArea)}) {
      if (node != nullptr) node->setVisible(false);
    }
    return;
  }

  applyWallpaperTexture();
  m_wallpaper->setVisible(true);
  m_wallpaper->setPosition(0.0f, 0.0f);
  m_wallpaper->setSize(sw, sh);
  m_wallpaper->setFillMode(m_wallpaperFillMode);
  m_wallpaper->setFillColor(m_wallpaperFillColor);

  m_backdrop->setPosition(0.0f, 0.0f);
  m_backdrop->setSize(sw, sh);
  m_backdrop->setVisible(m_wallpaperFillColor.a > 0.0f);
  m_backdrop->setStyle(
      RoundedRectStyle{
          .fill = m_wallpaperFillColor,
          .fillMode = FillMode::Solid,
      }
  );

  if (m_tintOverlay != nullptr) {
    m_tintOverlay->setPosition(0.0f, 0.0f);
    m_tintOverlay->setSize(sw, sh);
    const float tintIntensity = std::clamp(m_tintIntensity, 0.0f, 1.0f);
    m_tintOverlay->setVisible(tintIntensity > 0.0f);
    if (tintIntensity > 0.0f) {
      m_tintOverlay->setStyle(
          RoundedRectStyle{
              .fill = colorForRole(ColorRole::Surface, tintIntensity),
              .fillMode = FillMode::Solid,
          }
      );
    }
  }

  const bool contentVisible = m_locked;
  const auto layout = lockscreen_layout::resolve(sw, sh);
  const float scale = layout.scale;
  const bool wide = layout.mode == lockscreen_layout::Mode::Wide;
  const bool medium = layout.mode == lockscreen_layout::Mode::Medium;
  const bool compact = layout.mode == lockscreen_layout::Mode::Compact;

  m_leftColumn->setVisible(contentVisible);
  m_centerColumn->setVisible(contentVisible && (wide || compact));
  m_rightColumn->setVisible(contentVisible && !compact);
  m_loginPanel->setVisible(contentVisible);
  m_notificationButton->setVisible(contentVisible);
  if (!contentVisible) {
    m_notificationPanel->setVisible(false);
    m_notificationBackdropArea->setVisible(false);
    return;
  }

  const auto localRect = [](const lockscreen_layout::Rect& child, const lockscreen_layout::Rect& parent) {
    return LayoutRect{child.x - parent.x, child.y - parent.y, child.width, child.height};
  };

  const lockscreen_layout::Rect leftFrame = compact ? layout.centerColumn : layout.leftColumn;
  m_leftColumn->setPosition(leftFrame.x - m_introSideOffset, leftFrame.y);
  m_leftColumn->setSize(leftFrame.width, leftFrame.height);
  m_leftColumn->setOpacity(m_introOpacity);
  m_identityCard->arrange(*renderer, localRect(layout.identityCard, leftFrame));
  m_clockBlock->arrange(*renderer, localRect(layout.clockBlock, leftFrame));
  m_mediaCard->setVisible(!compact);
  if (!compact) {
    m_mediaCard->arrange(*renderer, localRect(layout.mediaCard, leftFrame));
  }

  if (m_timeLabel != nullptr) {
    const float base = compact ? 74.0f : wide ? 86.0f : 78.0f;
    m_timeLabel->setFontSize(std::clamp(base * scale, 48.0f, 96.0f));
    m_timeLabel->setMaxWidth(layout.clockBlock.width - Style::spaceLg * 2.0f);
  }
  if (m_dateLabel != nullptr) {
    m_dateLabel->setFontSize(std::max(Style::fontSizeCaption, Style::fontSizeTitle * scale));
    m_dateLabel->setMaxWidth(layout.clockBlock.width - Style::spaceLg * 2.0f);
  }
  syncAvatar(*renderer, std::clamp(56.0f * scale, 44.0f, 64.0f));

  if (m_mediaArtworkFrame != nullptr) {
    const float thumb = std::clamp(72.0f * scale, 56.0f, 78.0f);
    m_mediaArtworkFrame->setSize(thumb, thumb);
    m_mediaArtworkFrame->setRadius(Style::scaledRadiusLg(scale));
    m_mediaArtwork->setPosition(0.0f, 0.0f);
    m_mediaArtwork->setSize(thumb, thumb);
    m_mediaArtwork->setRadius(Style::scaledRadiusLg(scale));
    m_mediaArtworkFallback->setPosition(0.0f, 0.0f);
    m_mediaArtworkFallback->setSize(thumb, thumb);
    m_mediaArtworkFallback->setGlyphSize(thumb * 0.42f);
  }

  const lockscreen_layout::Rect heroFrame = layout.heroCard;
  if (wide || compact) {
    const lockscreen_layout::Rect centerFrame = layout.centerColumn;
    m_centerColumn->setPosition(centerFrame.x, centerFrame.y + m_introOffsetY);
    m_centerColumn->setSize(centerFrame.width, centerFrame.height);
    m_centerColumn->setOpacity(m_introOpacity);
    m_heroLayer->setPosition(heroFrame.x - centerFrame.x, heroFrame.y - centerFrame.y);
    m_heroLayer->setSize(heroFrame.width, heroFrame.height);

    const float heroHeight = heroFrame.height * 0.94f;
    const float heroWidth = compact
        ? std::min(heroFrame.width * 0.72f, heroHeight * 1.25f)
        : std::min(heroFrame.width * 0.82f, heroHeight * 0.82f);
    const float heroX = std::round((heroFrame.width - heroWidth) * 0.5f);
    const float heroY = std::round((heroFrame.height - heroHeight) * 0.5f);
    m_heroBackSheetA->setPosition(heroX + 9.0f * scale, heroY + 5.0f * scale);
    m_heroBackSheetA->setSize(heroWidth, heroHeight);
    m_heroBackSheetB->setPosition(heroX - 8.0f * scale, heroY + 8.0f * scale);
    m_heroBackSheetB->setSize(heroWidth, heroHeight);
    const float captionHeight = compact ? 24.0f : 34.0f;
    const float imageHeight = std::max(1.0f, heroHeight - captionHeight - Style::spaceMd * 2.0f - Style::spaceSm);
    m_heroImageFrame->setSize(std::max(1.0f, heroWidth - Style::spaceMd * 2.0f), imageHeight);
    m_heroCard->arrange(*renderer, LayoutRect{heroX, heroY, heroWidth, heroHeight});
    m_heroImage->setPosition(0.0f, 0.0f);
    m_heroImage->setSize(m_heroImageFrame->width(), m_heroImageFrame->height());
    m_heroImage->setRadius(Style::scaledRadiusMd(scale));
    m_heroFallbackGlyph->setPosition(0.0f, 0.0f);
    m_heroFallbackGlyph->setSize(m_heroImageFrame->width(), m_heroImageFrame->height());
    m_heroFallbackGlyph->setGlyphSize(std::clamp(imageHeight * 0.22f, 36.0f, 82.0f));
    syncHeroArtwork(*renderer, std::max(heroWidth, imageHeight));
  } else {
    m_centerColumn->setVisible(false);
    syncHeroArtwork(*renderer, 300.0f * scale);
  }

  if (!compact) {
    m_rightColumn->setPosition(layout.rightColumn.x + m_introSideOffset, layout.rightColumn.y);
    m_rightColumn->setSize(layout.rightColumn.width, layout.rightColumn.height);
    m_rightColumn->setOpacity(m_introOpacity);
    m_calendarCard->arrange(*renderer, localRect(layout.calendarCard, layout.rightColumn));
    m_weatherCard->arrange(*renderer, localRect(layout.weatherCard, layout.rightColumn));
    m_resourcesCard->arrange(*renderer, localRect(layout.metricsCard, layout.rightColumn));
    if (m_calendarGrid != nullptr && m_calendarSlideOffset != 0.0f) {
      m_calendarGrid->setPosition(m_calendarGrid->x() + m_calendarSlideOffset, m_calendarGrid->y());
    }
  }

  m_loginBaseX = layout.loginBlock.x;
  m_loginBaseY = layout.loginBlock.y;
  m_loginPanel->setOpacity(m_introOpacity);
  m_loginPanel->setGap(Style::spaceXs * scale);
  const float loginY = layout.loginBlock.y + m_introOffsetY;
  const float promptWidth = std::min(460.0f * scale, std::max(210.0f, layout.loginBlock.width));
  const float promptHeight = Style::controlHeightLg;
  m_promptHost->setSize(promptWidth, promptHeight);
  m_loginPanel->arrange(
      *renderer,
      LayoutRect{layout.loginBlock.x, loginY, layout.loginBlock.width, layout.loginBlock.height}
  );
  const float unlockWidth = std::min(230.0f * scale, promptWidth);
  m_unlockButton->arrange(
      *renderer,
      LayoutRect{std::round((promptWidth - unlockWidth) * 0.5f), 0.0f, unlockWidth, promptHeight}
  );
  m_loginContentRow->arrange(
      *renderer,
      LayoutRect{m_passwordErrorOffsetX, 0.0f, promptWidth, promptHeight}
  );

  m_notificationButton->arrange(
      *renderer,
      LayoutRect{
          layout.notificationButton.x,
          layout.notificationButton.y,
          layout.notificationButton.width,
          layout.notificationButton.height,
      }
  );
  m_notificationBackdropArea->setPosition(0.0f, 0.0f);
  m_notificationBackdropArea->setSize(sw, sh);
  if (m_notificationPanel->visible()) {
    m_notificationPanel->arrange(
        *renderer,
        LayoutRect{
            layout.notificationPanel.x,
            layout.notificationPanel.y + m_notificationPanelOffsetY,
            layout.notificationPanel.width,
            layout.notificationPanel.height,
        }
    );
  }

  m_passwordField->setSurfaceOpacity(0.0f);
  m_passwordField->setTextAlign(TextAlign::Center);
  m_loginButton->setRadius(Style::controlHeightSm * 0.5f);

  if (medium) {
    m_centerColumn->setVisible(false);
  }
  if (m_introPending) {
    startIntroAnimation();
  }
}

std::string LockSurface::resolveStatusText(bool& isError) const {
  isError = false;
  if (m_authenticating || m_error) {
    isError = m_error;
    return m_status;
  }
  if (!m_status.empty()) {
    return m_status;
  }
  if (m_capsLock) {
    isError = true;
    return i18n::tr("lockscreen.caps-lock-on");
  }
  return m_passwordPromptVisible ? i18n::tr("lockscreen.ready") : i18n::tr("lockscreen.glance-hint");
}

void LockSurface::updateCopy() {
  if (m_timeLabel != nullptr) {
    const char* format = m_config != nullptr ? m_config->config().shell.timeFormat.c_str() : "{:%H:%M}";
    m_timeLabel->setText(formatLocalTime(format));
  }
  if (m_dateLabel != nullptr) {
    const char* format = m_config != nullptr ? m_config->config().shell.dateFormat.c_str() : "%A, %x";
    m_dateLabel->setText(formatLocalTime(format));
  }
  if (m_userLabel != nullptr) {
    m_userLabel->setText(m_user);
  }
  if (m_identityHostLabel != nullptr) {
    m_identityHostLabel->setText(m_dashboard.systemIdentity);
  }

  syncDashboardCopy();
  syncCalendarCopy();

  m_passwordField->setValue(m_password);
  m_passwordField->setEnabled(!m_authenticating);
  if (m_loginButton != nullptr) {
    m_loginButton->setEnabled(!m_authenticating);
    m_loginButton->setVisible(!m_authenticating);
  }
  if (m_loginSpinner != nullptr) {
    m_loginSpinner->setVisible(m_authenticating);
    if (m_authenticating) {
      m_loginSpinner->start();
    } else {
      m_loginSpinner->stop();
    }
  }
  if (m_passwordCapsule != nullptr) {
    m_passwordCapsule->setBorder(
        m_error ? colorSpecFromRole(ColorRole::Error) : colorSpecFromRole(ColorRole::Outline, 0.28f),
        m_error ? 1.5f : Style::borderWidth
    );
  }

  if (m_statusLabel != nullptr) {
    bool isError = false;
    const std::string text = resolveStatusText(isError);
    const bool show = m_locked && !m_blackout && !text.empty();
    m_statusLabel->setVisible(show);
    if (show) {
      m_statusLabel->setText(text);
      m_statusLabel->setColor(
          isError ? colorSpecFromRole(ColorRole::Error) : colorSpecFromRole(ColorRole::OnSurfaceVariant)
      );
    }
  }

  if (m_layoutChip != nullptr) {
    const bool show = m_locked && !m_blackout && m_passwordPromptVisible && m_hasMultipleLayouts;
    m_layoutChip->setVisible(show);
    if (show) {
      m_layoutChip->setText(m_layoutLabel);
      m_layoutChip->setEnabled(m_layoutSwitchable);
    }
  }
}

void LockSurface::syncDashboardCopy() {
  if (m_weatherGlyph != nullptr) m_weatherGlyph->setGlyph(m_dashboard.weatherGlyph);
  if (m_weatherTemperatureLabel != nullptr) {
    m_weatherTemperatureLabel->setText(
        m_dashboard.weatherAvailable ? m_dashboard.weatherTemperature : i18n::tr("lockscreen.dashboard.unavailable")
    );
  }
  if (m_weatherConditionLabel != nullptr) m_weatherConditionLabel->setText(m_dashboard.weatherCondition);
  if (m_weatherTitleLabel != nullptr) {
    m_weatherTitleLabel->setText(
        m_dashboard.weatherLocation.empty() ? i18n::tr("lockscreen.dashboard.weather") : m_dashboard.weatherLocation
    );
  }
  if (m_weatherDetailLabel != nullptr) m_weatherDetailLabel->setText(m_dashboard.weatherDetail);

  if (m_mediaTitleLabel != nullptr) m_mediaTitleLabel->setText(m_dashboard.mediaTitle);
  if (m_mediaArtistLabel != nullptr) m_mediaArtistLabel->setText(m_dashboard.mediaArtist);
  if (m_heroCaptionLabel != nullptr) {
    m_heroCaptionLabel->setText(
        m_dashboard.mediaAvailable ? m_dashboard.mediaTitle : i18n::tr("lockscreen.hero-caption")
    );
  }
  if (m_mediaPreviousButton != nullptr) {
    m_mediaPreviousButton->setEnabled(m_dashboard.mediaAvailable && m_dashboard.mediaCanPrevious);
  }
  if (m_mediaPlayPauseButton != nullptr) {
    m_mediaPlayPauseButton->setEnabled(m_dashboard.mediaAvailable && m_dashboard.mediaCanPlayPause);
    m_mediaPlayPauseButton->setGlyph(m_dashboard.mediaPlaying ? "media-pause" : "media-play");
  }
  if (m_mediaNextButton != nullptr) {
    m_mediaNextButton->setEnabled(m_dashboard.mediaAvailable && m_dashboard.mediaCanNext);
  }
  if (m_mediaProgress != nullptr) {
    const float progress = m_dashboard.mediaLengthUs > 0
        ? static_cast<float>(std::clamp(
              static_cast<double>(m_dashboard.mediaPositionUs) / static_cast<double>(m_dashboard.mediaLengthUs),
              0.0, 1.0
          ))
        : 0.0f;
    m_mediaProgress->setProgress(progress);
  }
  if (m_mediaDurationLabel != nullptr) {
    const std::int64_t displayed =
        m_dashboard.mediaLengthUs > 0 ? m_dashboard.mediaLengthUs : m_dashboard.mediaPositionUs;
    m_mediaDurationLabel->setText(formatMediaTime(displayed));
  }

  bool metricAnimationStarted = false;
  for (std::size_t i = 0; i < m_metricViews.size(); ++i) {
    auto& view = m_metricViews[i];
    const auto& state = m_dashboard.metrics[i];
    if (view.glyph != nullptr) view.glyph->setGlyph(state.glyph);
    if (view.value != nullptr) view.value->setText(state.available ? state.value : "—");
    const float target = state.available ? std::clamp(state.progress, 0.0f, 1.0f) : 0.0f;
    if (view.progress != nullptr && std::abs(view.displayedProgress - target) > 0.001f) {
      m_animations.cancelForOwner(view.progress);
      auto* viewPtr = &view;
      m_animations.animate(
          view.displayedProgress, target, 200.0f, Easing::EaseOutCubic,
          [viewPtr](float value) {
            viewPtr->displayedProgress = value;
            if (viewPtr->progress != nullptr) viewPtr->progress->setProgress(value);
          },
          {}, view.progress
      );
      metricAnimationStarted = true;
    }
  }
  if (metricAnimationStarted) requestFrameTick();

  const bool showPreviews = m_dashboard.showNotifications && m_dashboard.notificationCount > 0;
  if (m_notificationsEmpty != nullptr) m_notificationsEmpty->setVisible(!showPreviews);
  if (m_notificationsEmptyGlyph != nullptr) {
    m_notificationsEmptyGlyph->setGlyph(m_dashboard.showNotifications ? "notification" : "notifications-off");
  }
  if (m_notificationsLabel != nullptr) {
    m_notificationsLabel->setText(
        !m_dashboard.showNotifications ? i18n::tr("lockscreen.dashboard.notifications-private")
        : m_dashboard.notificationCount == 0 ? i18n::tr("lockscreen.dashboard.no-notifications")
                                             : std::string{}
    );
  }
  const std::string count = m_dashboard.showNotifications && m_dashboard.notificationCount > 0
      ? std::format(" ({})", m_dashboard.notificationCount)
      : std::string{};
  if (m_notificationsHeaderLabel != nullptr) {
    m_notificationsHeaderLabel->setText(i18n::tr("lockscreen.dashboard.notifications") + count);
  }
  if (m_notificationButton != nullptr) {
    m_notificationButton->setBadge(
        m_dashboard.showNotifications && m_dashboard.notificationCount > 0
            ? std::to_string(m_dashboard.notificationCount)
            : std::string{}
    );
  }
  for (std::size_t i = 0; i < m_notificationViews.size(); ++i) {
    auto& view = m_notificationViews[i];
    const bool visible = showPreviews && i < std::min(m_dashboard.notificationCount, m_notificationViews.size());
    if (view.row != nullptr) view.row->setVisible(visible);
    if (visible) {
      if (view.app != nullptr) view.app->setText(m_dashboard.notificationPreviews[i].app);
      if (view.summary != nullptr) view.summary->setText(m_dashboard.notificationPreviews[i].summary);
    }
  }
}

void LockSurface::syncCalendarCopy() {
  if (m_calendarMonthButton == nullptr || m_calendarGrid == nullptr) {
    return;
  }

  const std::time_t nowRaw = std::time(nullptr);
  std::tm today{};
  localtime_r(&nowRaw, &today);
  std::tm first = today;
  first.tm_mday = 1;
  first.tm_mon += m_calendarMonthOffset;
  first.tm_hour = 12;
  first.tm_min = 0;
  first.tm_sec = 0;
  first.tm_isdst = -1;
  if (std::mktime(&first) == -1) {
    return;
  }

  m_calendarMonthButton->setText(formatStrftime("%B %Y", first));
  const int firstDay = localeFirstDayOfWeek();
  for (int i = 0; i < 7; ++i) {
    std::tm weekday{};
    weekday.tm_wday = (firstDay + i) % 7;
    weekday.tm_mday = 1;
    if (m_calendarWeekdayLabels[static_cast<std::size_t>(i)] != nullptr) {
      m_calendarWeekdayLabels[static_cast<std::size_t>(i)]->setText(formatStrftime("%a", weekday));
    }
  }

  std::tm previousEnd = first;
  previousEnd.tm_mday = 0;
  std::mktime(&previousEnd);
  const int previousMonthDays = previousEnd.tm_mday;
  const int offset = (first.tm_wday - firstDay + 7) % 7;
  std::tm nextMonth = first;
  ++nextMonth.tm_mon;
  nextMonth.tm_mday = 0;
  std::mktime(&nextMonth);
  const int currentMonthDays = nextMonth.tm_mday;
  const int currentYear = today.tm_year + 1900;
  const int currentMonth = today.tm_mon + 1;
  const int displayYear = first.tm_year + 1900;
  const int displayMonth = first.tm_mon + 1;

  for (int index = 0; index < 42; ++index) {
    int day = index - offset + 1;
    int cellYear = displayYear;
    int cellMonth = displayMonth;
    bool inDisplayMonth = true;
    if (day <= 0) {
      day = previousMonthDays + day;
      std::tm previous = first;
      --previous.tm_mon;
      std::mktime(&previous);
      cellYear = previous.tm_year + 1900;
      cellMonth = previous.tm_mon + 1;
      inDisplayMonth = false;
    } else if (day > currentMonthDays) {
      day -= currentMonthDays;
      std::tm next = first;
      ++next.tm_mon;
      std::mktime(&next);
      cellYear = next.tm_year + 1900;
      cellMonth = next.tm_mon + 1;
      inDisplayMonth = false;
    }

    const int key = cellYear * 10000 + cellMonth * 100 + day;
    const bool isToday =
        cellYear == currentYear && cellMonth == currentMonth && day == today.tm_mday;
    const bool hasEvent = std::ranges::binary_search(m_dashboard.calendarEventDateKeys, key);
    auto& cell = m_calendarCells[static_cast<std::size_t>(index)];
    if (cell.label != nullptr) {
      cell.label->setText(std::to_string(day));
      cell.label->setColor(
          isToday ? colorSpecFromRole(ColorRole::OnPrimary)
                  : inDisplayMonth ? colorSpecFromRole(ColorRole::OnSurface)
                                   : colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.58f)
      );
    }
    if (cell.cell != nullptr) {
      cell.cell->setFill(isToday ? colorSpecFromRole(ColorRole::Primary) : clearColorSpec());
    }
    if (cell.eventDot != nullptr) {
      cell.eventDot->setVisible(hasEvent);
      cell.eventDot->setFill(
          isToday ? colorForRole(ColorRole::OnPrimary) : colorForRole(ColorRole::Primary)
      );
    }
  }
}

void LockSurface::releaseWallpaperTextureRef(const std::string& path) {
  if (m_wallpaperTexture.id == 0) {
    return;
  }
  const std::string& releasePath = !path.empty() ? path : m_textureWallpaperPath;
  if (m_textureCache != nullptr && m_textureCache->shared()) {
    if (releasePath.empty()) {
      m_wallpaperTexture = {};
      return;
    }
    m_textureCache->release(m_wallpaperTexture, releasePath);
  } else if (renderContext() != nullptr) {
    renderContext()->backend().makeCurrentNoSurface();
    renderContext()->textureManager().unload(m_wallpaperTexture);
    m_wallpaperTexture = {};
  }
  if (m_textureWallpaperPath == releasePath || path.empty()) {
    m_textureWallpaperPath.clear();
  }
}

void LockSurface::applyWallpaperTexture() {
  if (m_desktopCapture.has_value() && !m_desktopCapture->rgba.empty()) {
    applyBlurredDesktopTexture();
    if (m_blurredDesktopTexture.id != 0) {
      return;
    }
  }

  if (!m_wallpaperDirty) {
    return;
  }

  bool loaded = true;
  Color color = rgba(0.0f, 0.0f, 0.0f, 1.0f);
  if (parseColorWallpaperPath(m_wallpaperPath, color)) {
    if (m_wallpaperTexture.id != 0) {
      releaseWallpaperTextureRef(m_textureWallpaperPath);
    }
    if (m_blurredWallpaperTexture.id != 0 && renderContext() != nullptr) {
      renderContext()->backend().makeCurrentNoSurface();
      renderContext()->textureManager().unload(m_blurredWallpaperTexture);
      m_blurredWallpaperTexture = {};
    }
    m_wallpaper->setSources(
        WallpaperSourceKind::Color, {}, color, WallpaperSourceKind::Image, {}, rgba(0.0f, 0.0f, 0.0f, 1.0f), 0.0f, 0.0f,
        0.0f, 0.0f
    );
    m_wallpaper->setTransition(WallpaperTransition::Fade, 0.0f, TransitionParams{});
    m_wallpaper->setFillMode(m_wallpaperFillMode);
    m_wallpaper->setFillColor(m_wallpaperFillColor);
  } else if (m_textureCache != nullptr && !m_wallpaperPath.empty()) {
    const bool needsReload = m_wallpaperTexture.id == 0 || m_textureWallpaperPath != m_wallpaperPath;
    TextureHandle newTexture = m_wallpaperTexture;
    if (needsReload) {
      newTexture = m_textureCache->acquire(m_wallpaperPath);
      if (newTexture.id == 0 && !m_textureCache->shared() && renderContext() != nullptr) {
        renderContext()->backend().makeCurrentNoSurface();
        newTexture = renderContext()->textureManager().loadFromFile(m_wallpaperPath, 0, true);
      }
    }

    if (newTexture.id == 0) {
      loaded = false;
    } else {
      if (needsReload && m_wallpaperTexture.id != 0 && m_textureWallpaperPath != m_wallpaperPath) {
        releaseWallpaperTextureRef(m_textureWallpaperPath);
      }
      m_wallpaperTexture = newTexture;
      m_textureWallpaperPath = m_wallpaperPath;

      TextureHandle textureToDisplay = m_wallpaperTexture;
      if (m_blurredWallpaperTexture.id != 0 && renderContext() != nullptr) {
        renderContext()->backend().makeCurrentNoSurface();
        renderContext()->textureManager().unload(m_blurredWallpaperTexture);
        m_blurredWallpaperTexture = {};
      }
      if (m_blurIntensity > 0.0f && renderContext() != nullptr) {
        auto* renderer = renderContext();
        renderer->makeCurrent(renderTarget());
        static constexpr int kBlurRounds = 3;
        const float blurRadius = m_blurIntensity * 40.0f;
        const std::uint32_t blurWidth = renderTarget().bufferWidth();
        const std::uint32_t blurHeight = renderTarget().bufferHeight();
        m_blurredWallpaperTexture = m_wallpaperBlurCache.get(
            renderer->backend(), m_wallpaperTexture, blurWidth, blurHeight, blurRadius, kBlurRounds
        );
        if (m_blurredWallpaperTexture.id != 0) {
          textureToDisplay = m_blurredWallpaperTexture;
        }
      }
      m_wallpaper->setTextures(
          textureToDisplay.id, {}, static_cast<float>(textureToDisplay.width),
          static_cast<float>(textureToDisplay.height), 0.0f, 0.0f
      );
      m_wallpaper->setTransition(WallpaperTransition::Fade, 0.0f, TransitionParams{});
      m_wallpaper->setFillMode(m_wallpaperFillMode);
      m_wallpaper->setFillColor(m_wallpaperFillColor);
    }
  } else if (m_wallpaperPath.empty()) {
    if (m_wallpaperTexture.id != 0) {
      releaseWallpaperTextureRef(m_textureWallpaperPath);
    }
    m_wallpaper->setTextures({}, {}, 0.0f, 0.0f, 0.0f, 0.0f);
  } else {
    loaded = false;
  }

  m_wallpaperDirty = !loaded;
}

void LockSurface::releaseCaptureTextures() {
  if (renderContext() == nullptr) {
    m_blurredWallpaperTexture = {};
    m_captureSourceTexture = {};
    m_blurredDesktopTexture = {};
    m_blurCache.destroy();
    m_wallpaperBlurCache.destroy();
    return;
  }

  auto& tm = renderContext()->textureManager();
  renderContext()->backend().makeCurrentNoSurface();
  if (m_blurredWallpaperTexture.id != 0) {
    tm.unload(m_blurredWallpaperTexture);
    m_blurredWallpaperTexture = {};
  }
  if (m_captureSourceTexture.id != 0) {
    tm.unload(m_captureSourceTexture);
    m_captureSourceTexture = {};
  }
  if (m_blurredDesktopTexture.id != 0) {
    tm.unload(m_blurredDesktopTexture);
    m_blurredDesktopTexture = {};
  }
  m_blurCache.destroy();
  m_wallpaperBlurCache.destroy();
}

void LockSurface::applyBlurredDesktopTexture() {
  if (!m_captureDirty || !m_desktopCapture.has_value() || m_desktopCapture->rgba.empty()) {
    return;
  }

  auto* renderer = renderContext();
  if (renderer == nullptr) {
    return;
  }

  const ScreencopyImage& capture = *m_desktopCapture;
  const int texW = capture.width;
  const int texH = capture.height;
  if (texW <= 0 || texH <= 0) {
    return;
  }

  renderer->makeCurrent(renderTarget());
  auto& tm = renderer->textureManager();
  if (m_captureSourceTexture.id != 0) {
    tm.unload(m_captureSourceTexture);
    m_captureSourceTexture = {};
  }
  if (m_blurredDesktopTexture.id != 0) {
    tm.unload(m_blurredDesktopTexture);
    m_blurredDesktopTexture = {};
  }

  m_captureSourceTexture = tm.loadFromRgba(capture.rgba.data(), texW, texH, false);
  if (m_captureSourceTexture.id == 0) {
    return;
  }

  static constexpr int kBlurRounds = 3;
  const float blurRadius = m_blurIntensity * 40.0f;
  const std::uint32_t blurWidth = renderTarget().bufferWidth();
  const std::uint32_t blurHeight = renderTarget().bufferHeight();
  m_blurredDesktopTexture =
      m_blurCache.get(renderer->backend(), m_captureSourceTexture, blurWidth, blurHeight, blurRadius, kBlurRounds);
  if (m_blurredDesktopTexture.id == 0) {
    return;
  }

  m_wallpaper->setTextures(
      m_blurredDesktopTexture.id, {}, static_cast<float>(m_blurredDesktopTexture.width),
      static_cast<float>(m_blurredDesktopTexture.height), 0.0f, 0.0f
  );
  m_wallpaper->setTransition(WallpaperTransition::Fade, 0.0f, TransitionParams{});
  m_wallpaper->setFillMode(m_wallpaperFillMode);
  m_wallpaper->setFillColor(rgba(0.0f, 0.0f, 0.0f, 0.0f));
  m_backdrop->setVisible(false);
  m_captureDirty = false;
  m_wallpaperDirty = false;
}

void LockSurface::onGpuResourcesInvalidated() {
  releaseCaptureTextures();

  if (!m_wallpaperPath.empty() && m_textureCache != nullptr) {
    if (m_textureCache->shared()) {
      m_wallpaperTexture = m_textureCache->peek(m_wallpaperPath);
    } else if (renderContext() != nullptr) {
      renderContext()->backend().textureManager().unload(m_wallpaperTexture);
      if (!m_wallpaperPath.empty()) {
        m_wallpaperTexture = renderContext()->backend().textureManager().loadFromFile(m_wallpaperPath, 0, true);
      }
    }
  }

  m_captureDirty = true;
  m_wallpaperDirty = true;
  requestLayout();
}

void LockSurface::prepareForGraphicsReset() noexcept {
  m_blurCache.abandon();
  m_wallpaperBlurCache.abandon();
  m_wallpaperTexture = {};
  m_blurredWallpaperTexture = {};
  m_captureSourceTexture = {};
  m_blurredDesktopTexture = {};
  m_captureDirty = true;
  m_wallpaperDirty = true;
}

void LockSurface::render() {
  Surface::render();
  if (!m_firstFrameRendered) {
    m_firstFrameRendered = true;
    if (m_renderCallback) {
      m_renderCallback();
    }
  }
}
