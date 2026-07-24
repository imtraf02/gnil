#include "shell/control_center/tabs/media_tab.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/files/resource_paths.h"
#include "core/log.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "net/http_client.h"
#include "pipewire/pipewire_spectrum.h"
#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"
#include "render/core/image_decoder.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/node.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel_manager.h"
#include "system/lyrics_service.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/controls/glyph.h"
#include "ui/visuals/audio_visualizer.h"
#include "util/file_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace control_center;
using namespace mpris;

namespace {

  const Logger kLog{"media_tab"};

  // Layout-grid unit for the media tab. Artwork uses its own deliberate large
  // bound so the dashboard reads as an album player, not a compact toolbar.
  constexpr float kMediaUnit = 36.0f;

  constexpr float kArtworkSize = kMediaUnit * 13;
  constexpr float kMediaNowCardMinHeight = kMediaUnit * 11 + Style::spaceSm * 2;
  constexpr float kMediaControlsHeight = kMediaUnit + Style::spaceXs;
  constexpr float kMediaPlayPauseHeight = kMediaUnit + Style::spaceSm;
  constexpr float kMediaArtworkMinHeight = kMediaUnit * 4;
  // The standalone reference player uses a roomy, two-column 680px canvas.
  // ContentPanel contributes 16px of outer inset on both sides.
  constexpr float kReferenceCanvasWidth = 648.0f;
  constexpr float kReferenceCardPadding = 24.0f;
  constexpr float kReferenceCardGap = 16.0f;
  constexpr float kReferenceArtworkSize = 264.0f;
  constexpr float kReferenceBongoWidth = 190.0f;
  constexpr float kReferenceBongoSlotHeight = 150.0f;
  constexpr float kReferenceVisualizerHeight = 190.0f;
  constexpr auto kNoActivePlayerGrace = std::chrono::milliseconds(2000);
  constexpr auto kTransientPositionRegressionWindow = std::chrono::milliseconds(1500);
  constexpr std::int64_t kTransientPositionRegressionFloorUs = 5'000'000;
  constexpr std::int64_t kTransientPositionRegressionCeilingUs = 1'500'000;
  constexpr std::int64_t kTransientPositionRegressionDeltaUs = 5'000'000;
  constexpr std::int64_t kSeekArrivedToleranceUs = 1'500'000;
  constexpr std::int64_t kSeekNearZeroUs = 2'000'000;
  constexpr auto kProgressSettleHold = std::chrono::milliseconds(2500);
  constexpr auto kPendingSeekTimeout = std::chrono::milliseconds(5000);

  std::string playPauseGlyph(const std::string& playbackStatus) {
    return playbackStatus == "Playing" ? "media-pause" : "media-play";
  }

  [[nodiscard]] int mediaTabArtDecodeSize(float scale) {
    // Match the widest artwork layout bound (see mediaWidth in doLayout).
    return static_cast<int>(std::round(kArtworkSize * scale));
  }

  std::string repeatGlyph(const std::string& loopStatus) { return loopStatus == "Track" ? "repeat-once" : "repeat"; }

  ButtonVariant toggleVariant(bool active) { return active ? ButtonVariant::Primary : ButtonVariant::Ghost; }
  constexpr int kVisualizerBandCount = 32;
  constexpr float kBongoNaturalWidth = 260.0f;
  constexpr int kMaxBongoFrames = 128;
  constexpr std::size_t kMaxBongoRgbaBytes = 32ull * 1024 * 1024;

  Color trackDominantColor(std::string_view title, std::string_view artist) {
    if (title.empty()) {
      return colorForRole(ColorRole::Primary);
    }
    std::size_t hash = std::hash<std::string_view>{}(title);
    if (!artist.empty()) {
      hash ^= std::hash<std::string_view>{}(artist) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    }
    const float hue = static_cast<float>(hash % 360);
    return hsl(hue, 0.8f, 0.6f, 1.0f);
  }

  std::string formatDuration(std::int64_t us) {
    if (us <= 0) return "0:00";
    const auto totalSeconds = us / 1'000'000;
    const auto hours = totalSeconds / 3600;
    const auto minutes = (totalSeconds % 3600) / 60;
    const auto seconds = totalSeconds % 60;
    if (hours > 0) {
      return std::format("{}:{}:{:02d}", hours, minutes, seconds);
    }
    return std::format("{}:{:02d}", minutes, seconds);
  }

} // namespace

MediaTab::MediaTab(
    MprisService* mpris, HttpClient* httpClient, PipeWireSpectrum* spectrum, ConfigService* config,
    WaylandConnection* wayland, RenderContext* renderContext, MediaTabPresentation presentation
)
    : m_mpris(mpris), m_httpClient(httpClient), m_spectrum(spectrum), m_config(config), m_wayland(wayland),
      m_renderContext(renderContext), m_presentation(presentation) {
  const std::weak_ptr<void> alive = m_aliveGuard;
  m_lyricsService = std::make_unique<LyricsService>(m_httpClient, [alive]() {
    if (alive.expired()) {
      return;
    }
    PanelManager::instance().requestUpdateOnly();
    PanelManager::instance().requestRedraw();
  });
}

MediaTab::~MediaTab() {
  m_aliveGuard.reset();
  unloadBongoFrames();
}

void MediaTab::openPlayerMenu() {
  if (m_playerMenuPopup == nullptr || m_mpris == nullptr || m_playerMenuButton == nullptr) {
    return;
  }

  const auto pinnedBusName = m_mpris->pinnedPlayerPreference();
  std::vector<ContextMenuControlEntry> entries;
  entries.reserve(m_playerBusNames.size() + 1);
  entries.push_back(
      {.id = 0,
       .label = i18n::tr("control-center.media.active-player"),
       .enabled = true,
       .separator = false,
       .hasSubmenu = false}
  );
  for (std::size_t i = 0; i < m_playerBusNames.size(); ++i) {
    const auto& busName = m_playerBusNames[i];
    const bool selected = pinnedBusName.has_value() && busName == *pinnedBusName;
    std::string identity;
    if (auto it = m_mpris->players().find(busName); it != m_mpris->players().end()) {
      identity = it->second.identity;
    }
    const std::string label = (selected ? "• " : "") + (identity.empty() ? busName : identity);
    entries.push_back(
        {.id = static_cast<std::int32_t>(i + 1),
         .label = label,
         .enabled = true,
         .separator = false,
         .hasSubmenu = false}
    );
  }

  Flex* anchor = m_playerMenuButton->parent() != nullptr ? static_cast<Flex*>(m_playerMenuButton->parent())
                                                         : static_cast<Flex*>(m_nowCard);
  if (anchor == nullptr) {
    return;
  }

  const auto parentCtx = PanelManager::instance().fallbackPopupParentContext();
  if (!parentCtx.has_value()) {
    return;
  }

  float anchorAbsX = 0.0f;
  float anchorAbsY = 0.0f;
  Node::absolutePosition(anchor, anchorAbsX, anchorAbsY);

  const float scale = contentScale();
  // Cap at the card width so a pre-layout card (width ~0) yields a card-fitting
  // menu instead of an inverted std::clamp range (hi < lo).
  const float cardWidth = m_nowCard != nullptr ? std::max(1.0f, m_nowCard->width()) : 240.0f * scale;
  const float menuWidth = std::min(cardWidth, std::max(kMediaUnit * 4.2f * scale, kMediaUnit * 6.0f * scale));

  if (m_config != nullptr) {
    m_playerMenuPopup->setShadowConfig(m_config->config().shell.shadow);
  }
  PanelManager::instance().beginAttachedPopup(parentCtx->surface);
  PanelManager::instance().setActivePopup(m_playerMenuPopup.get());

  m_playerMenuPopup->setOnDismissed([parentSurface = parentCtx->surface]() {
    PanelManager::instance().clearActivePopup();
    PanelManager::instance().endAttachedPopup(parentSurface);
  });

  m_playerMenuPopup->open(
      ContextMenuPopupRequest{
          .entries = std::move(entries),
          .menuWidth = menuWidth,
          .maxVisible = 10,
          .anchor =
              PopupAnchorRect{
                  .x = static_cast<std::int32_t>(anchorAbsX),
                  .y = static_cast<std::int32_t>(anchorAbsY),
                  .width = static_cast<std::int32_t>(anchor->width()),
                  .height = static_cast<std::int32_t>(anchor->height()),
              },
          .parent = PopupSurfaceParent{
              .layerSurface = parentCtx->layerSurface,
              .output = parentCtx->output,
          },
      }
  );

  m_playerMenuOpen = true;
}

std::unique_ptr<Flex> MediaTab::create() {
  const float scale = contentScale();
  const bool referenceLayout = m_presentation == MediaTabPresentation::ReferencePanel;

  auto tab = ui::row({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });

  auto mediaColumn = ui::column({
      .out = &m_mediaColumn,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .flexGrow = 7.0f,
  });

  auto nowCard = ui::column({
      .out = &m_nowCard,
      .gap = Style::spaceMd * scale,
      .minHeight = kMediaNowCardMinHeight * scale,
      .flexGrow = 1.0f,
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) {
        applySectionCardStyle(card, scale, opacity, /*showBorder=*/false);
        card.setRadius(28.0f * scale);
      },
  });

  auto mediaStack = ui::column({
      .out = &m_mediaStack,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .flexGrow = 1.0f,
  });

  auto artContainer = ui::column({
      .out = &m_artContainer,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .configure = [scale](Flex& box) {
        box.setFill(colorSpecFromRole(ColorRole::SurfaceVariant, 0.45f));
        box.setRadius(22.0f * scale);
      },
  });
  artContainer->addChild(ui::image({
      .out = &m_artwork,
      .fit = ImageFit::Cover,
      .radius = 22.0f * scale,
      .width = kArtworkSize * scale,
      .height = kArtworkSize * scale,
  }));
  artContainer->addChild(ui::glyph({
      .out = &m_artFallbackGlyph,
      .glyph = "disc-filled",
      .glyphSize = 64.0f * scale,
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
  }));

  auto artworkRow = ui::row(
      {.out = &m_artworkRow, .align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = 0.0f, .flexGrow = 1.0f},
      std::move(artContainer)
  );
  mediaStack->addChild(std::move(artworkRow));

  auto titleRow = ui::row(
      {.align = FlexAlign::Center,
       .justify = referenceLayout ? FlexJustify::Start : FlexJustify::Center,
       .fillWidth = true},
      ui::label({
          .out = &m_trackTitle,
          .text = i18n::tr("control-center.media.nothing-playing"),
          .fontSize = Style::fontSizeHeader * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      })
  );

  auto infoColumn = ui::column(
      {.align = referenceLayout ? FlexAlign::Start : FlexAlign::Center,
       .gap = Style::spaceXs * 0.5f * scale,
       .fillWidth = true},
      std::move(titleRow),
      ui::label({
          .out = &m_trackArtist,
          .text = i18n::tr("control-center.media.start-playback"),
          .fontSize = Style::fontSizeBody * 0.95f * scale,
          .fontWeight = FontWeight::Medium,
          .color = colorSpecFromRole(referenceLayout ? ColorRole::OnSurfaceVariant : ColorRole::Primary),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      }),
      ui::label({
          .out = &m_trackAlbum,
          .text = " ",
          .fontSize = Style::fontSizeBody * 0.9f * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      })
  );
  mediaStack->addChild(std::move(infoColumn));

  auto progressRow = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true},
      ui::label({
          .out = &m_timeElapsedLabel,
          .text = "0:00",
          .fontSize = Style::fontSizeCaption * 0.9f * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      }),
      ui::slider({
          .out = &m_progressSlider,
          .minValue = 0.0f,
          .maxValue = 100.0f,
          .step = 1.0f,
          .trackHeight = 6.0f * scale,
          .thumbSize = 16.0f * scale,
          .controlHeight = (Style::controlHeight + Style::spaceXs) * scale,
          .flexGrow = 1.0f,
          .onValueChanged =
              [this](double value) {
                if (m_syncingProgress || m_mpris == nullptr) {
                  return;
                }
                const auto active = m_mpris->activePlayer();
                const auto targetUs = static_cast<std::int64_t>(std::llround(value * 1000000.0));
                const auto now = std::chrono::steady_clock::now();
                m_positionUs = targetUs;
                m_positionSampleAt = now;
                m_pendingSeekBusName = active.has_value()
                    ? active->busName
                    : (!m_positionBusName.empty() ? m_positionBusName : std::string{});
                m_pendingSeekUs = targetUs;
                m_pendingSeekUntil = now + kPendingSeekTimeout;
                m_progressSettleUntil = now + kProgressSettleHold;
              },
          .onDragEnd =
              [this]() {
                if (m_syncingProgress || m_mpris == nullptr || m_progressSlider == nullptr) {
                  return;
                }
                commitPendingSeek(m_progressSlider->value());
              },
      }),
      ui::label({
          .out = &m_timeRemainingLabel,
          .text = "0:00",
          .fontSize = Style::fontSizeCaption * 0.9f * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  mediaStack->addChild(std::move(progressRow));

  auto controls = ui::row({
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .gap = Style::spaceLg * scale,
      .fillWidth = true,
  });

  controls->addChild(
      ui::button({
          .out = &m_shuffleButton,
          .glyph = "shuffle",
          .variant = ButtonVariant::Ghost,
          .minWidth = kMediaControlsHeight * scale,
          .minHeight = kMediaControlsHeight * scale,
          .padding = Style::spaceSm * scale,
          .radius = Style::scaledRadiusLg(scale),
          .onClick = [this]() {
            const std::weak_ptr<void> aliveGuard = m_aliveGuard;
            DeferredCall::callLater([this, aliveGuard]() {
              if (aliveGuard.expired() || m_mpris == nullptr) {
                return;
              }
              const bool enabled = m_mpris->shuffleActive().value_or(false);
              (void)m_mpris->setShuffleActive(!enabled);
              PanelManager::instance().refresh();
            });
          },
      })
  );

  controls->addChild(
      ui::button({
          .out = &m_prevButton,
          .glyph = "media-prev",
          .variant = ButtonVariant::Ghost,
          .minWidth = kMediaControlsHeight * scale,
          .minHeight = kMediaControlsHeight * scale,
          .padding = Style::spaceSm * scale,
          .radius = Style::scaledRadiusLg(scale),
          .onClick = [this]() {
            const std::weak_ptr<void> aliveGuard = m_aliveGuard;
            DeferredCall::callLater([this, aliveGuard]() {
              if (aliveGuard.expired() || m_mpris == nullptr) {
                return;
              }
              (void)m_mpris->previousActive();
              PanelManager::instance().refresh();
            });
          },
      })
  );

  controls->addChild(
      ui::button({
          .out = &m_playPauseButton,
          .glyph = "media-play",
          .variant = ButtonVariant::Primary,
          .minWidth = 52.0f * scale,
          .minHeight = 52.0f * scale,
          .padding = Style::spaceSm * scale,
          .radius = 28.0f * scale,
          .onClick = [this]() {
            const std::weak_ptr<void> aliveGuard = m_aliveGuard;
            DeferredCall::callLater([this, aliveGuard]() {
              if (aliveGuard.expired() || m_mpris == nullptr) {
                return;
              }
              (void)m_mpris->playPauseActive();
              PanelManager::instance().refresh();
            });
          },
      })
  );

  controls->addChild(
      ui::button({
          .out = &m_nextButton,
          .glyph = "media-next",
          .variant = ButtonVariant::Ghost,
          .minWidth = kMediaControlsHeight * scale,
          .minHeight = kMediaControlsHeight * scale,
          .padding = Style::spaceSm * scale,
          .radius = Style::scaledRadiusLg(scale),
          .onClick = [this]() {
            const std::weak_ptr<void> aliveGuard = m_aliveGuard;
            DeferredCall::callLater([this, aliveGuard]() {
              if (aliveGuard.expired() || m_mpris == nullptr) {
                return;
              }
              (void)m_mpris->nextActive();
              PanelManager::instance().refresh();
            });
          },
      })
  );

  controls->addChild(
      ui::button({
          .out = &m_repeatButton,
          .glyph = "repeat",
          .variant = ButtonVariant::Ghost,
          .minWidth = kMediaControlsHeight * scale,
          .minHeight = kMediaControlsHeight * scale,
          .padding = Style::spaceSm * scale,
          .radius = Style::scaledRadiusLg(scale),
          .onClick = [this]() {
            const std::weak_ptr<void> aliveGuard = m_aliveGuard;
            DeferredCall::callLater([this, aliveGuard]() {
              if (aliveGuard.expired() || m_mpris == nullptr) {
                return;
              }
              const auto current = m_mpris->loopStatusActive().value_or("None");
              const std::string next = current == "None" ? "Playlist" : (current == "Playlist" ? "Track" : "None");
              (void)m_mpris->setLoopStatusActive(next);
              PanelManager::instance().refresh();
            });
          },
      })
  );

  mediaStack->addChild(std::move(controls));

  auto volumePill = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceXs * scale,
      .padding = 0.0f,
  });
  volumePill->addChild(ui::slider({
      .out = &m_volumeSlider,
      .minValue = 0.0f,
      .maxValue = 100.0f,
      .step = 1.0f,
      .value = 80.0f,
      .presentation = SliderPresentation::LevelCompact,
      .glyph = "volume-high",
      .glyphSize = Style::fontSizeCaption * scale,
      .trackHeight = 18.0f * scale,
      .thumbSize = 33.0f * scale,
      .controlHeight = 33.0f * scale,
      .flexGrow = 1.0f,
      .onValueChanged =
          [this](double value) {
            if (m_mpris == nullptr) {
              return;
            }
            (void)m_mpris->setVolumeActive(value / 100.0);
          },
  }));

  mediaStack->addChild(std::move(volumePill));

  nowCard->addChild(std::move(mediaStack));
  mediaColumn->addChild(std::move(nowCard));

  auto visualizerColumn = ui::column({
      .out = &m_visualizerColumn,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .clipChildren = true,
      .flexGrow = 4.0f,
  });

  {
    auto lyricsCard = ui::column({
        .out = &m_lyricsCard,
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .gap = Style::spaceSm * scale,
        .fillWidth = true,
        .flexGrow = referenceLayout ? 0.0f : 1.0f,
        .configure = [scale, opacity = panelCardOpacity()](Flex& card) {
          applySectionCardStyle(card, scale, opacity, /*showBorder=*/false);
          card.setAlign(FlexAlign::Center);
          card.setJustify(FlexJustify::Center);
          card.setRadius(20.0f * scale);
        },
    });

    auto optionBtn = ui::button({
        .out = &m_playerMenuButton,
        .glyph = "more-vertical",
        .glyphSize = Style::fontSizeBody * scale,
        .enabled = false,
        .variant = ButtonVariant::Ghost,
        .minWidth = Style::controlHeightSm * scale,
        .minHeight = Style::controlHeightSm * scale,
        .padding = Style::spaceXs * scale,
        .onClick = [this]() {
          if (m_playerBusNames.empty()) {
            return;
          }
          if (m_playerMenuPopup != nullptr && m_playerMenuPopup->isOpen()) {
            m_playerMenuPopup->close();
            PanelManager::instance().clearActivePopup();
          } else {
            openPlayerMenu();
          }
        },
    });
    if (referenceLayout) {
      auto goodVibesHeader = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * scale,
          .fillWidth = true,
      });
      goodVibesHeader->addChild(ui::glyph({
          .glyph = "sparkles",
          .glyphSize = Style::fontSizeTitle * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      }));
      goodVibesHeader->addChild(ui::label({
          .text = i18n::tr("control-center.media.good-vibes"),
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0f,
      }));
      goodVibesHeader->addChild(std::move(optionBtn));
      lyricsCard->addChild(std::move(goodVibesHeader));
    } else {
      optionBtn->setParticipatesInLayout(false);
      lyricsCard->addChild(std::move(optionBtn));
    }

    if (!referenceLayout) {
      auto lyricsHeader = ui::row(
          {.align = FlexAlign::Center,
           .justify = FlexJustify::SpaceBetween,
           .gap = Style::spaceSm * scale,
           .minHeight = Style::controlHeightSm * scale},
          ui::label({
              .text = i18n::tr("control-center.media.sync-lyrics"),
              .fontSize = Style::fontSizeHeader * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::OnSurface),
          }),
          ui::glyph({
              .glyph = "more-vertical",
              .glyphSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
      lyricsCard->addChild(std::move(lyricsHeader));
      lyricsCard->addChild(ui::box({
          .fill = colorSpecFromRole(ColorRole::Outline, 0.15f),
          .height = 1.0f * scale,
      }));
    }

    auto lyricsCenterBox = ui::column({
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .gap = Style::spaceSm * scale,
        .fillWidth = true,
        .flexGrow = referenceLayout ? 0.0f : 1.0f,
    });

    auto lyricsEmptyState = ui::column({
        .out = &m_lyricsEmptyState,
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .gap = Style::spaceSm * scale,
        .fillWidth = true,
        .flexGrow = 1.0f,
    });
    lyricsEmptyState->addChild(
        ui::row(
            {.align = FlexAlign::Center,
             .justify = FlexJustify::Center,
             .minHeight = (referenceLayout ? kReferenceBongoSlotHeight : 170.0f) * scale,
             .fillWidth = true},
            ui::image({
                .out = &m_bongoCat,
                .fit = ImageFit::Contain,
            })
        )
    );

    lyricsCenterBox->addChild(std::move(lyricsEmptyState));
    if (!referenceLayout) {
      auto lyricsLines = ui::column({
          .out = &m_lyricsLines,
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
          .gap = Style::spaceSm * scale,
          .fillWidth = true,
          .flexGrow = 1.0f,
          .visible = false,
          .participatesInLayout = false,
      });
      lyricsLines->addChild(ui::label({
          .out = &m_lyricsPrevious,
          .text = "",
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.65f),
          .visible = false,
      }));
      lyricsLines->addChild(ui::label({
          .out = &m_lyricsCurrent,
          .text = "",
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::Primary),
          .visible = false,
      }));
      lyricsLines->addChild(ui::label({
          .out = &m_lyricsNext,
          .text = "",
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .visible = false,
      }));
      lyricsCenterBox->addChild(std::move(lyricsLines));
    }
    lyricsCard->addChild(std::move(lyricsCenterBox));
    if (referenceLayout) {
      lyricsCard->addChild(ui::label({
          .text = i18n::tr("control-center.media.have-a-nice-day"),
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Medium,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .textAlign = TextAlign::Center,
      }));
    }
    visualizerColumn->addChild(std::move(lyricsCard));

  }

  auto visualizerCard = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .minHeight = 108.0f * scale,
      .fillWidth = true,
      .flexGrow = 1.0f,
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) {
        applySectionCardStyle(card, scale, opacity, /*showBorder=*/false);
        card.setRadius(20.0f * scale);
        card.setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
      },
  });

  auto visualizerHeader = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true},
      ui::glyph({
          .glyph = "wave-sine",
          .glyphSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::Primary),
      }),
      ui::label({
          .text = i18n::tr("control-center.media.audio-output"),
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0f,
      })
  );
  visualizerCard->addChild(std::move(visualizerHeader));

  auto visualizerBody = ui::row({
      .out = &m_visualizerBody,
      .align = FlexAlign::End,
      .justify = FlexJustify::Center,
      .minHeight = 75.0f * scale,
      .fillWidth = true,
      .flexGrow = 1.0f,
  });

  auto visualizerSpectrum = std::make_unique<AudioVisualizer>();
  visualizerSpectrum->setGradient(colorForRole(ColorRole::Secondary), colorForRole(ColorRole::Tertiary));
  visualizerSpectrum->setOrientation(AudioSpectrumOrientation::Horizontal);
  visualizerSpectrum->setMirrored(false);
  visualizerSpectrum->setCentered(false);
  visualizerSpectrum->setReflection(false);
  visualizerSpectrum->setValues(std::vector<float>(kVisualizerBandCount, 0.0f));
  visualizerSpectrum->tick(0.0f);
  visualizerSpectrum->setFlexGrow(1.0f);
  m_visualizerSpectrum = visualizerSpectrum.get();
  visualizerBody->addChild(std::move(visualizerSpectrum));
  visualizerCard->addChild(std::move(visualizerBody));

  visualizerCard->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true},
      ui::glyph({
          .glyph = "volume",
          .glyphSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      }),
      ui::label({
          .out = &m_visualizerStatus,
          .text = i18n::tr("control-center.media.waiting-for-audio"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  ));

  visualizerColumn->addChild(std::move(visualizerCard));
  tab->addChild(std::move(mediaColumn));
  tab->addChild(std::move(visualizerColumn));

  if (m_wayland != nullptr && m_renderContext != nullptr) {
    m_playerMenuPopup = std::make_unique<ContextMenuPopup>(*m_wayland, *m_renderContext);
    m_playerMenuPopup->setOnActivate([this](const ContextMenuControlEntry& entry) {
      const std::weak_ptr<void> aliveGuard = m_aliveGuard;
      DeferredCall::callLater([this, aliveGuard, entry]() {
        if (aliveGuard.expired() || m_mpris == nullptr) {
          return;
        }
        if (entry.id == 0) {
          m_mpris->clearPinnedPlayerPreference();
        } else {
          const auto idx = static_cast<std::size_t>(entry.id - 1);
          if (idx < m_playerBusNames.size()) {
            m_mpris->setPinnedPlayerPreference(m_playerBusNames[idx]);
          }
        }
        PanelManager::instance().refresh();
      });
    });
  }

  return tab;
}

void MediaTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr || m_nowCard == nullptr || m_mediaStack == nullptr) {
    return;
  }

  const float scale = contentScale();
  const bool referenceLayout = m_presentation == MediaTabPresentation::ReferencePanel;
  const float referenceScale = referenceLayout ? contentWidth / kReferenceCanvasWidth : scale;
  ensureBongoLoaded(renderer);

  if (referenceLayout) {
    const float cardPadding = kReferenceCardPadding * referenceScale;
    m_rootLayout->setGap(kReferenceCardGap * referenceScale);
    m_mediaColumn->setGap(0.0f);
    m_nowCard->setPadding(cardPadding, cardPadding);
    m_nowCard->setGap(kReferenceCardGap * referenceScale);
    m_visualizerColumn->setGap(kReferenceCardGap * referenceScale);
    m_mediaStack->setGap(6.0f * referenceScale);
    if (m_artworkRow != nullptr) {
      const float artSide = kReferenceArtworkSize * referenceScale;
      m_artworkRow->setMinHeight(artSide);
      m_artworkRow->setMaxHeight(artSide);
    }
    if (m_visualizerBody != nullptr) {
      m_visualizerBody->setMinHeight(kReferenceVisualizerHeight * referenceScale);
    }
    if (m_progressSlider != nullptr) {
      m_progressSlider->setControlHeight(28.0f * referenceScale);
      m_progressSlider->setTrackHeight(4.0f * referenceScale);
      m_progressSlider->setThumbSize(12.0f * referenceScale);
    }
    if (m_volumeSlider != nullptr) {
      m_volumeSlider->setControlHeight(24.0f * referenceScale);
      m_volumeSlider->setTrackHeight(4.0f * referenceScale);
      m_volumeSlider->setThumbSize(10.0f * referenceScale);
    }
  }
  m_rootLayout->setSize(contentWidth, std::max(0.0f, bodyHeight));
  if (bodyHeight <= 0.0f) {
    m_rootLayout->setHeightPolicy(FlexSizePolicy::Content);
  }
  m_rootLayout->layout(renderer);

  const float cardInnerWidth =
      std::max(0.0f, m_nowCard->width() - (m_nowCard->paddingLeft() + m_nowCard->paddingRight()));
  const float mediaWidth = referenceLayout ? cardInnerWidth : std::clamp(cardInnerWidth, 1.0f, kArtworkSize * scale);
  const float mediaStackHeight = m_mediaStack->height();
  m_mediaStack->setSize(mediaWidth, mediaStackHeight);

  if (m_artworkRow != nullptr) {
    // Horizontal Flex with justify Center under-reports its width when the child is narrower than
    // the stretched cross-axis; min width keeps the row full-bleed so art centers.
    m_artworkRow->setMinWidth(mediaWidth);
  }

  if (m_artwork != nullptr) {
    const float sideButtonSize = referenceLayout ? 32.0f * referenceScale : kMediaControlsHeight * scale;
    const float playPauseButtonSize = referenceLayout ? 56.0f * referenceScale : kMediaPlayPauseHeight * scale;
    const float sideGlyphSize = referenceLayout ? 20.0f * referenceScale : Style::fontSizeTitle * scale;
    const float playPauseGlyphSize = referenceLayout
        ? 26.0f * referenceScale
        : (Style::fontSizeTitle + Style::spaceXs) * scale;

    for (auto* button : {m_repeatButton, m_prevButton, m_nextButton, m_shuffleButton}) {
      if (button != nullptr) {
        button->setMinWidth(sideButtonSize);
        button->setMinHeight(sideButtonSize);
        button->setGlyphSize(sideGlyphSize);
        button->setPadding(referenceLayout ? 4.0f * referenceScale : Style::spaceSm * scale,
                           referenceLayout ? 4.0f * referenceScale : Style::spaceSm * scale);
        button->setRadius(Style::scaledRadiusLg(scale));
      }
    }
    if (m_playPauseButton != nullptr) {
      m_playPauseButton->setMinWidth(playPauseButtonSize);
      m_playPauseButton->setMinHeight(playPauseButtonSize);
      m_playPauseButton->setGlyphSize(playPauseGlyphSize);
      m_playPauseButton->setPadding(referenceLayout ? 10.0f * referenceScale : Style::spaceSm * scale,
                                    referenceLayout ? 10.0f * referenceScale : Style::spaceSm * scale);
      m_playPauseButton->setRadius(playPauseButtonSize * 0.5f);
    }
  }

  if (m_trackTitle != nullptr) {
    m_trackTitle->setMaxWidth(mediaWidth);
  }
  if (m_trackArtist != nullptr) {
    m_trackArtist->setMaxWidth(mediaWidth);
  }
  if (m_trackAlbum != nullptr) {
    m_trackAlbum->setMaxWidth(mediaWidth);
  }
  if (m_progressSlider != nullptr) {
    m_progressSlider->setSize(mediaWidth, 0.0f);
  }

  m_mediaStack->layout(renderer);

  if (m_artwork != nullptr && m_artworkRow != nullptr) {
    const float artWidth =
        std::max(1.0f, m_artworkRow->width() - (m_artworkRow->paddingLeft() + m_artworkRow->paddingRight()));
    const float artHeight = referenceLayout
        ? kReferenceArtworkSize * referenceScale
        : std::max(
              kMediaArtworkMinHeight * scale,
              m_artworkRow->height() - (m_artworkRow->paddingTop() + m_artworkRow->paddingBottom())
          );
    // Media art is always presented as a square (album-art convention).
    const float side = std::min(artWidth, artHeight);
    if (m_artContainer != nullptr) {
      m_artContainer->setSize(side, side);
      m_artContainer->setRadius(22.0f * scale);
    }
    if (m_artwork != nullptr) {
      m_artwork->setSize(side, side);
      m_artwork->setRadius(22.0f * scale);
      m_artwork->setVisible(m_artwork->hasImage());
    }
    if (m_artFallbackGlyph != nullptr) {
      m_artFallbackGlyph->setVisible(m_artwork == nullptr || !m_artwork->hasImage());
    }
    if (!referenceLayout && m_playerMenuButton != nullptr && m_lyricsCard != nullptr) {
      const float btnSize = referenceLayout ? 26.0f * referenceScale : kMediaControlsHeight * scale;
      const float offsetX = 12.0f * (referenceLayout ? referenceScale : scale);
      const float offsetY = 10.0f * (referenceLayout ? referenceScale : scale);
      m_playerMenuButton->setSize(btnSize, btnSize);
      m_playerMenuButton->setPosition(
          std::max(0.0f, m_lyricsCard->width() - btnSize - offsetX),
          offsetY
      );
    }
    m_mediaStack->layout(renderer);
  }

  if (m_visualizerBody != nullptr && m_visualizerSpectrum != nullptr) {
    const float bodyWidth = std::max(
        0.0f, m_visualizerBody->width() - (m_visualizerBody->paddingLeft() + m_visualizerBody->paddingRight())
    );
    const float bodyHeightAvail = std::max(
        0.0f, m_visualizerBody->height() - (m_visualizerBody->paddingTop() + m_visualizerBody->paddingBottom())
    );
    const float spectrumWidth = std::max(1.0f, bodyWidth);
    const float spectrumHeight = std::max(1.0f, bodyHeightAvail);
    m_visualizerSpectrum->setSize(spectrumWidth, spectrumHeight);
    m_visualizerBody->layout(renderer);
  }

  if (m_bongoCat != nullptr && m_bongoCat->hasImage() && m_bongoCat->parent() != nullptr) {
    auto* parentFlex = static_cast<Flex*>(m_bongoCat->parent());
    if (referenceLayout) {
      const float bongoSlotHeight = kReferenceBongoSlotHeight * referenceScale;
      parentFlex->setMinHeight(bongoSlotHeight);
      parentFlex->setMaxHeight(bongoSlotHeight);
    }
    const float parentWidth = std::max(1.0f, parentFlex->width());
    const float parentHeight = std::max(1.0f, parentFlex->height());
    float catWidth = referenceLayout
        ? std::min(kReferenceBongoWidth * referenceScale, parentWidth * 0.9f)
        : std::min(kBongoNaturalWidth * scale, parentWidth * 0.9f);
    float catHeight = catWidth / std::max(0.01f, m_bongoCat->aspectRatio());
    if (catHeight > parentHeight && parentHeight > 0.0f) {
      catHeight = parentHeight;
      catWidth = catHeight * m_bongoCat->aspectRatio();
    }
    m_bongoCat->setSize(catWidth, catHeight);
    parentFlex->layout(renderer);
    if (auto* grandParent = parentFlex->parent(); grandParent != nullptr) {
      grandParent->layout(renderer);
    }
  }
}

void MediaTab::doPrepareIntrinsicLayout(Renderer& renderer, float contentWidth, float maxBodyHeight) {
  (void)maxBodyHeight;
  // Pass height 0.0f so m_rootLayout measures intrinsic height based on content
  // instead of forcing maxBodyHeight.
  doLayout(renderer, contentWidth, 0.0f);
}

void MediaTab::doUpdate(Renderer& renderer) {
  if (!m_active) {
    m_progressTimer.stop();
    return;
  }
  if (m_visualizerSpectrum != nullptr && m_spectrum != nullptr && m_spectrumListenerId != 0) {
    if (!m_spectrum->idle() || !m_visualizerSpectrum->converged()) {
      m_visualizerSpectrum->setValues(m_spectrum->values(m_spectrumListenerId));
    }
  }

  const auto active = m_mpris != nullptr ? m_mpris->activePlayer() : std::nullopt;
  syncLyrics(active);
  const auto now = std::chrono::steady_clock::now();
  const bool hasPendingSeek = m_pendingSeekUs >= 0 && now < m_pendingSeekUntil;
  const bool withinProgressSettle =
      m_progressSettleUntil != std::chrono::steady_clock::time_point{} && now < m_progressSettleUntil;
  const bool playing = active.has_value() && active->playbackStatus == "Playing";
  if (playing || hasPendingSeek || withinProgressSettle) {
    if (!m_progressTimer.active()) {
      m_progressTimer.startRepeating(std::chrono::milliseconds(1000), [this]() {
        if (!m_active) {
          return;
        }
        PanelManager::instance().requestUpdateOnly();
        PanelManager::instance().requestRedraw();
      });
    }
  } else {
    m_progressTimer.stop();
  }

  refresh(renderer);
}

void MediaTab::onFrameTick(float deltaMs) {
  if (!m_active) {
    return;
  }

  if (m_visualizerSpectrum != nullptr) {
    if (m_spectrum != nullptr && m_spectrumListenerId != 0) {
      if (!m_spectrum->idle() || !m_visualizerSpectrum->converged()) {
        m_visualizerSpectrum->setValues(m_spectrum->values(m_spectrumListenerId));
      }
    }
    m_visualizerSpectrum->tick(deltaMs);
  }
}

void MediaTab::setActive(bool active) {
  const bool becameActive = active && !m_active;
  m_active = active;
  if (m_spectrum != nullptr) {
    if (active && m_spectrumListenerId == 0) {
      m_spectrumListenerId = m_spectrum->addChangeListener(kVisualizerBandCount, [this]() {
        if (!m_active || m_spectrum->idle()) {
          return;
        }
        PanelManager::instance().requestFrameTick();
      });
    } else if (!active && m_spectrumListenerId != 0) {
      m_spectrum->removeChangeListener(m_spectrumListenerId);
      m_spectrumListenerId = 0;
    }
  }
  if (!active) {
    m_bongoTimer.stop();
    m_progressTimer.stop();
    m_positionSampleAt = {};
    m_positionTrackSignature.clear();
    m_progressSettleUntil = {};
    m_nextRealtimeUpdateAt = {};
    m_lastRealtimeMprisPollAt = {};
  }
  if (becameActive && m_mpris != nullptr) {
    m_positionSampleAt = {};
  }
  if (active) {
    syncBongoPlayback(m_bongoPlaying);
  }
}

void MediaTab::onClose() {
  m_bongoTimer.stop();
  m_progressTimer.stop();
  if (m_spectrum != nullptr) {
    if (m_spectrumListenerId != 0) {
      m_spectrum->removeChangeListener(m_spectrumListenerId);
      m_spectrumListenerId = 0;
    }
  }
  m_active = false;
  m_rootLayout = nullptr;
  m_mediaColumn = nullptr;
  m_visualizerColumn = nullptr;
  m_visualizerBody = nullptr;
  m_lyricsEmptyState = nullptr;
  m_lyricsLines = nullptr;
  m_visualizerSpectrum = nullptr;
  if (m_bongoCat != nullptr && m_bongoRenderer != nullptr) {
    m_bongoCat->clear(*m_bongoRenderer);
  }
  m_bongoCat = nullptr;
  unloadBongoFrames();
  m_bongoLoadAttempted = false;
  m_bongoPlaying = false;
  m_artwork = nullptr;
  m_artContainer = nullptr;
  m_artworkRow = nullptr;
  m_nowCard = nullptr;
  m_mediaStack = nullptr;
  m_playerMenuButton = nullptr;
  if (m_playerMenuPopup != nullptr) {
    PanelManager::instance().clearActivePopup();
    m_playerMenuPopup->close();
  }
  m_playerMenuOpen = false;
  m_trackTitle = nullptr;
  m_trackArtist = nullptr;
  m_trackAlbum = nullptr;
  m_lyricsPrevious = nullptr;
  m_lyricsCurrent = nullptr;
  m_lyricsNext = nullptr;
  m_visualizerStatus = nullptr;
  m_progressSlider = nullptr;
  m_prevButton = nullptr;
  m_playPauseButton = nullptr;
  m_nextButton = nullptr;
  m_repeatButton = nullptr;
  m_shuffleButton = nullptr;
  m_volumeSlider = nullptr;
  m_volumeLabel = nullptr;
  m_lastArtPath.clear();
  m_lastBusName.clear();
  m_lastPlaybackStatus.clear();
  m_lastLoopStatus.clear();
  m_playerBusNames.clear();
  m_lastActiveSnapshot.reset();
  m_pendingSeekBusName.clear();
  m_pendingSeekUs = -1;
  m_progressSettleUntil = {};
  m_positionTrackSignature.clear();
  m_nextRealtimeUpdateAt = {};
  m_lastRealtimeMprisPollAt = {};
}

bool MediaTab::dismissTransientUi() {
  if (m_playerMenuPopup == nullptr || !m_playerMenuPopup->isOpen()) {
    return false;
  }
  m_playerMenuPopup->close();
  PanelManager::instance().clearActivePopup();
  return true;
}

void MediaTab::syncLyrics(const std::optional<MprisPlayerInfo>& active) {
  if (m_presentation == MediaTabPresentation::ReferencePanel) {
    if (m_lyricsEmptyState != nullptr) {
      m_lyricsEmptyState->setVisible(true);
      m_lyricsEmptyState->setParticipatesInLayout(true);
    }
    return;
  }
  if (m_lyricsService == nullptr) {
    return;
  }
  const bool enabled = m_config == nullptr || m_config->config().dashboard.media.lyricsEnabled;
  m_lyricsService->syncTrack(active, enabled);

  const auto current = active.has_value() ? m_lyricsService->currentLine(active->positionUs) : std::nullopt;
  const auto& lines = m_lyricsService->lines();
  const bool hasCurrent = current.has_value() && *current < lines.size();
  if (m_lyricsEmptyState != nullptr) {
    m_lyricsEmptyState->setVisible(!hasCurrent);
    m_lyricsEmptyState->setParticipatesInLayout(!hasCurrent);
  }
  if (m_lyricsLines != nullptr) {
    m_lyricsLines->setVisible(hasCurrent);
    m_lyricsLines->setParticipatesInLayout(hasCurrent);
  }
  const auto syncLine = [&lines](Label* label, std::optional<std::size_t> line) {
    if (label == nullptr) {
      return;
    }
    const bool visible = line.has_value() && *line < lines.size();
    label->setText(visible ? lines[*line].text : "");
    label->setVisible(visible);
    label->setParticipatesInLayout(visible);
  };
  syncLine(m_lyricsPrevious, hasCurrent && *current > 0 ? std::optional<std::size_t>{*current - 1} : std::nullopt);
  syncLine(m_lyricsCurrent, hasCurrent ? current : std::nullopt);
  syncLine(
      m_lyricsNext,
      hasCurrent && *current + 1 < lines.size() ? std::optional<std::size_t>{*current + 1} : std::nullopt
  );
}

void MediaTab::clearArt(Renderer& renderer) {
  if (m_artwork != nullptr) {
    m_artwork->clear(renderer);
    m_artwork->setOpacity(1.0f);
    m_artwork->setVisible(false);
  }
  if (m_artFallbackGlyph != nullptr) {
    m_artFallbackGlyph->setVisible(true);
  }
}

void MediaTab::ensureBongoLoaded(Renderer& renderer) {
  if (m_bongoCat == nullptr) {
    return;
  }

  // External textures are intentionally dropped by Image after a GPU reset.
  // Recreate the small bundled animation when that happens.
  if (!m_bongoFrames.empty() && !m_bongoCat->hasImage()) {
    unloadBongoFrames();
    m_bongoLoadAttempted = false;
  }
  m_bongoRenderer = &renderer;
  if (m_bongoLoadAttempted) {
    return;
  }
  m_bongoLoadAttempted = true;

  const auto gifPath = paths::assetPath("images/bongocat.gif");
  const auto bytes = FileUtils::readBinaryFile(gifPath.string());
  if (bytes.empty()) {
    kLog.warn("bongocat asset is missing: {}", gifPath.string());
    return;
  }

  auto decoded = decodeAnimatedGif(bytes.data(), bytes.size(), kMaxBongoFrames, kMaxBongoRgbaBytes);
  if (!decoded) {
    kLog.warn("failed to decode bongocat GIF: {}", decoded.error());
    return;
  }
  if (decoded->truncated) {
    kLog.warn("bongocat GIF truncated at {} frames (resource cap)", decoded->frames.size());
  }

  m_bongoFrames.reserve(decoded->frames.size());
  for (const auto& frame : decoded->frames) {
    auto texture =
        renderer.textureManager().loadFromRgba(frame.rgba.data(), decoded->width, decoded->height, /*mipmap=*/false);
    if (!texture.valid()) {
      kLog.warn("failed to upload a bongocat GIF frame");
      unloadBongoFrames();
      return;
    }
    m_bongoFrames.push_back(
        BongoFrame{
            .texture = texture,
            .duration = std::chrono::milliseconds(frame.durationMs),
        }
    );
  }

  if (m_bongoFrames.empty()) {
    return;
  }
  m_bongoFrame = 0;
  m_bongoCat->setExternalTexture(renderer, m_bongoFrames.front().texture);
  syncBongoPlayback(m_bongoPlaying);
}

void MediaTab::unloadBongoFrames() {
  m_bongoTimer.stop();
  if (m_bongoRenderer != nullptr) {
    for (auto& frame : m_bongoFrames) {
      m_bongoRenderer->textureManager().unload(frame.texture);
    }
  }
  m_bongoFrames.clear();
  m_bongoFrame = 0;
  m_bongoRenderer = nullptr;
}

void MediaTab::syncBongoPlayback(bool playing) {
  m_bongoPlaying = playing;
  const bool shouldAnimate = m_active && playing && m_bongoFrames.size() > 1 && MotionService::instance().enabled();
  if (!shouldAnimate) {
    m_bongoTimer.stop();
    return;
  }
  if (!m_bongoTimer.active()) {
    scheduleNextBongoFrame();
  }
}

void MediaTab::scheduleNextBongoFrame() {
  if (!m_active || !m_bongoPlaying || m_bongoFrames.size() <= 1 || !MotionService::instance().enabled()) {
    return;
  }
  const float speed = MotionService::instance().speed();
  const auto nativeMs = m_bongoFrames[m_bongoFrame].duration.count();
  const auto adjustedMs = static_cast<std::int64_t>(std::llround(static_cast<double>(nativeMs) / speed));
  m_bongoTimer.start(std::chrono::milliseconds(std::max<std::int64_t>(10, adjustedMs)), [this]() {
    advanceBongoFrame();
  });
}

void MediaTab::advanceBongoFrame() {
  if (m_bongoCat == nullptr
      || m_bongoRenderer == nullptr
      || !m_active
      || !m_bongoPlaying
      || m_bongoFrames.size() <= 1
      || !MotionService::instance().enabled()) {
    return;
  }
  m_bongoFrame = (m_bongoFrame + 1) % m_bongoFrames.size();
  m_bongoCat->setExternalTexture(*m_bongoRenderer, m_bongoFrames[m_bongoFrame].texture);
  PanelManager::instance().requestRedraw();
  scheduleNextBongoFrame();
}

void MediaTab::commitPendingSeek(double valueSeconds) {
  if (m_mpris == nullptr) {
    return;
  }

  const auto targetUs = static_cast<std::int64_t>(std::llround(valueSeconds * 1000000.0));
  const auto now = std::chrono::steady_clock::now();
  m_positionUs = targetUs;
  m_positionSampleAt = now;
  const auto active = m_mpris->activePlayer();
  const std::string seekBusName =
      active.has_value() ? active->busName : (!m_positionBusName.empty() ? m_positionBusName : std::string{});
  m_pendingSeekBusName = seekBusName;
  m_pendingSeekUs = targetUs;
  m_pendingSeekUntil = now + kPendingSeekTimeout;
  m_progressSettleUntil = now + kProgressSettleHold;

  const std::weak_ptr<void> aliveGuard = m_aliveGuard;
  DeferredCall::callLater([this, aliveGuard, seekBusName, targetUs]() {
    if (aliveGuard.expired() || m_mpris == nullptr) {
      return;
    }
    if (!seekBusName.empty()) {
      (void)m_mpris->setPosition(seekBusName, targetUs);
    } else {
      (void)m_mpris->setPositionActive(targetUs);
    }
    PanelManager::instance().refresh();
  });
}

void MediaTab::refresh(Renderer& renderer) {
  std::vector<MprisPlayerInfo> players;
  std::optional<MprisPlayerInfo> active;
  const auto now = std::chrono::steady_clock::now();
  if (m_mpris != nullptr) {
    players = m_mpris->listPlayers();
    active = m_mpris->activePlayer();
    kLog.debug(
        "media tab refresh initial players={} active={} active_bus=\"{}\"", players.size(), active.has_value(),
        active.has_value() ? active->busName : std::string{}
    );
  }

  if (!active.has_value() && m_lastActiveSnapshot.has_value() && now - m_lastActiveSeenAt <= kNoActivePlayerGrace) {
    // Keep last player briefly to hide transient MPRIS discovery gaps.
    active = m_lastActiveSnapshot;
  }

  if (m_playerMenuButton != nullptr) {
    const auto pinnedBusName = m_mpris != nullptr ? m_mpris->pinnedPlayerPreference() : std::nullopt;
    std::vector<std::string> playerBusNames;
    playerBusNames.reserve(players.size());
    std::vector<ContextMenuControlEntry> entries;
    entries.reserve(players.size() + 1);
    entries.push_back(
        {.id = 0,
         .label = i18n::tr("control-center.media.active-player"),
         .enabled = true,
         .separator = false,
         .hasSubmenu = false}
    );

    for (std::size_t i = 0; i < players.size(); ++i) {
      const auto& player = players[i];
      playerBusNames.push_back(player.busName);
      const bool selected = pinnedBusName.has_value() && player.busName == *pinnedBusName;
      const std::string label = (selected ? "• " : "") + (player.identity.empty() ? player.busName : player.identity);
      entries.push_back(
          {.id = static_cast<std::int32_t>(i + 1),
           .label = label,
           .enabled = true,
           .separator = false,
           .hasSubmenu = false}
      );
    }

    m_playerBusNames = std::move(playerBusNames);
    m_playerMenuButton->setEnabled(!m_playerBusNames.empty());
    m_playerMenuButton->setVariant(!m_playerBusNames.empty() ? ButtonVariant::Ghost : ButtonVariant::Default);
    if (m_playerBusNames.empty() && m_playerMenuPopup != nullptr && m_playerMenuPopup->isOpen()) {
      m_playerMenuPopup->close();
      PanelManager::instance().clearActivePopup();
    }
  }

  if (m_trackTitle == nullptr
      || m_trackArtist == nullptr
      || m_progressSlider == nullptr
      || m_playPauseButton == nullptr
      || m_repeatButton == nullptr
      || m_shuffleButton == nullptr) {
    return;
  }

  if (active.has_value()) {
    const auto& player = *active;
    m_lastActiveSnapshot = player;
    m_lastActiveSeenAt = now;
    const std::string trackSignature = std::format(
        "{}\n{}\n{}\n{}\n{}", player.trackId, player.title, joinArtists(player.artists), player.album, player.sourceUrl
    );
    std::int64_t livePositionUs = player.positionUs;
    if (player.lengthUs > 0) {
      livePositionUs = std::clamp<std::int64_t>(livePositionUs, 0, player.lengthUs);
    } else {
      livePositionUs = std::max<std::int64_t>(0, livePositionUs);
    }

    const bool pendingMatchesPlayer = m_pendingSeekBusName.empty() || m_pendingSeekBusName == player.busName;
    const bool seekArrived = pendingMatchesPlayer
        && m_pendingSeekUs >= 0
        && std::llabs(livePositionUs - m_pendingSeekUs) <= kSeekArrivedToleranceUs
        && (m_pendingSeekUs <= kSeekNearZeroUs
            || livePositionUs > kSeekNearZeroUs
            || livePositionUs >= m_pendingSeekUs - kSeekArrivedToleranceUs);
    const bool seekPending = pendingMatchesPlayer && m_pendingSeekUs >= 0 && !seekArrived && now < m_pendingSeekUntil;
    const bool withinProgressSettle =
        m_progressSettleUntil != std::chrono::steady_clock::time_point{} && now < m_progressSettleUntil;
    const bool sameDisplayedTrack = m_positionBusName == player.busName && m_positionTrackSignature == trackSignature;
    const bool withinTransientRegressionWindow = m_positionSampleAt != std::chrono::steady_clock::time_point{}
        && now - m_positionSampleAt <= kTransientPositionRegressionWindow;
    const bool preserveDisplayedPosition = !seekPending
        && sameDisplayedTrack
        && m_lastPlaybackStatus == "Playing"
        && player.playbackStatus == "Playing"
        && m_positionUs >= kTransientPositionRegressionFloorUs
        && livePositionUs <= kTransientPositionRegressionCeilingUs
        && livePositionUs + kTransientPositionRegressionDeltaUs < m_positionUs
        && withinTransientRegressionWindow;
    if (preserveDisplayedPosition) {
      livePositionUs = m_positionUs;
    }

    std::int64_t displayPositionUs = livePositionUs;
    if (seekPending) {
      displayPositionUs = m_pendingSeekUs;
    } else if (withinProgressSettle && livePositionUs + kTransientPositionRegressionDeltaUs < m_positionUs) {
      displayPositionUs = m_positionUs;
    } else if (preserveDisplayedPosition) {
      displayPositionUs = m_positionUs;
    }

    const bool samePlayerAsDisplayed = m_positionBusName == player.busName || m_pendingSeekBusName == player.busName;

    m_positionBusName = player.busName;
    m_positionTrackId = player.trackId;
    m_positionTrackSignature = trackSignature;
    m_positionUs = displayPositionUs;
    m_positionSampleAt = now;

    if (seekArrived) {
      m_pendingSeekBusName.clear();
      m_pendingSeekUs = -1;
    }

    m_trackTitle->setText(player.title.empty() ? player.identity : player.title);
    m_trackArtist->setText(joinArtists(player.artists).empty() ? player.identity : joinArtists(player.artists));
    if (m_trackAlbum != nullptr) {
      m_trackAlbum->setText(player.album.empty() ? " " : player.album);
      const bool showAlbum = m_presentation != MediaTabPresentation::ReferencePanel || !player.album.empty();
      m_trackAlbum->setVisible(showAlbum);
      m_trackAlbum->setParticipatesInLayout(showAlbum);
    }
    if (m_volumeSlider != nullptr && !m_volumeSlider->dragging()) {
      const int volPct = std::clamp(static_cast<int>(std::round(player.volume * 100.0)), 0, 100);
      m_volumeSlider->setValue(static_cast<double>(volPct));
    }

    if (!player.title.empty()) {
      const Color glowColor = trackDominantColor(player.title, joinArtists(player.artists));
      if (m_artContainer != nullptr) {
        m_artContainer->clearShadow();
      }
      if (m_artworkRow != nullptr) {
        m_artworkRow->clearShadow();
      }
      if (m_visualizerSpectrum != nullptr) {
        m_visualizerSpectrum->setGradient(glowColor, colorForRole(ColorRole::Secondary));
      }
      if (m_playPauseButton != nullptr) {
        auto controlPalette = Button::defaultPalette(ButtonVariant::Primary);
        controlPalette.normal.bg = fixedColorSpec(glowColor);
        controlPalette.normal.label = fixedColorSpec(readableTextColorForBackground(glowColor));
        controlPalette.hover.bg = fixedColorSpec(brighten(glowColor, 0.9f));
        controlPalette.hover.label = controlPalette.normal.label;
        controlPalette.pressed.bg = fixedColorSpec(brighten(glowColor, 0.8f));
        controlPalette.pressed.label = controlPalette.normal.label;
        m_playPauseButton->setCustomPalette(controlPalette);
      }
    } else {
      if (m_artworkRow != nullptr) {
        m_artworkRow->clearShadow();
      }
      if (m_visualizerSpectrum != nullptr) {
        m_visualizerSpectrum->setGradient(colorForRole(ColorRole::Secondary), colorForRole(ColorRole::Tertiary));
      }
      if (m_playPauseButton != nullptr) {
        m_playPauseButton->setVariant(ButtonVariant::Primary);
      }
    }

    const std::string resolvedArtUrl = effectiveArtUrl(player);
    const std::string artPath = resolveArtworkSource(
        m_httpClient, m_pendingArtDownloads, resolvedArtUrl,
        [this] {
          m_lastArtPath.clear();
          PanelManager::instance().refresh();
        },
        m_aliveGuard
    );

    if (m_artwork != nullptr
        && (!resolvedArtUrl.empty() && (resolvedArtUrl != m_lastArtPath || !m_artwork->hasImage()))) {
      bool loaded = false;
      if (artPath.empty()) {
        kLog.debug("artwork unresolved url=\"{}\"", resolvedArtUrl);
        clearArt(renderer);
      } else if (!m_artwork->setSourceFile(renderer, artPath, mediaTabArtDecodeSize(contentScale()), true, true)) {
        kLog.warn(R"(artwork load failed url="{}" path="{}")", resolvedArtUrl, artPath);
        clearArt(renderer);
      } else {
        kLog.debug(R"(artwork loaded url="{}" path="{}")", resolvedArtUrl, artPath);
        loaded = true;
      }
      if (m_artwork != nullptr) {
        m_artwork->setVisible(loaded);
        if (loaded) {
          if (auto* animations = m_artwork->animationManager(); animations != nullptr) {
            m_artwork->setOpacity(0.0f);
            animations->animate(
                0.0f, 1.0f, static_cast<float>(Style::animNormal), Easing::CaelestiaDefaultEffects,
                [artwork = m_artwork](float value) { artwork->setOpacity(value); }, {}, m_artwork
            );
            PanelManager::instance().requestFrameTick();
          } else {
            m_artwork->setOpacity(1.0f);
          }
        }
      }
      if (m_artFallbackGlyph != nullptr) {
        m_artFallbackGlyph->setVisible(!loaded);
      }

      // Only lock this URL once we actually have an image.
      // Otherwise keep retrying while metadata/download catches up.
      m_lastArtPath = loaded ? resolvedArtUrl : std::string{};
      if (loaded) {
        PanelManager::instance().requestLayout();
      }
    } else if (m_artwork != nullptr && resolvedArtUrl.empty()) {
      clearArt(renderer);
      m_lastArtPath.clear();
    }

    std::int64_t trackLengthUs = player.lengthUs;
    if (trackLengthUs > 0) {
      m_lastTrackLengthUs = trackLengthUs;
    } else if (m_lastTrackLengthUs > 0 && samePlayerAsDisplayed) {
      trackLengthUs = m_lastTrackLengthUs;
    }
    const bool progressInteracting = m_progressSlider->dragging() || seekPending || withinProgressSettle;
    const bool progressEnabled = player.canSeek && (trackLengthUs > 0 || progressInteracting);

    m_syncingProgress = true;
    m_progressSlider->setEnabled(progressEnabled);
    if (trackLengthUs > 0) {
      m_progressSlider->setRange(0.0, static_cast<double>(trackLengthUs) / 1000000.0);
    }
    if (!m_progressSlider->dragging()) {
      const double sliderMax = m_progressSlider->maxValue();
      const double nextValue =
          sliderMax > 0.0 ? std::clamp(static_cast<double>(displayPositionUs) / 1000000.0, 0.0, sliderMax) : 0.0;
      m_progressSlider->setValue(nextValue);
    }
    m_syncingProgress = false;

    if (m_timeElapsedLabel != nullptr) {
      m_timeElapsedLabel->setText(formatDuration(displayPositionUs));
    }
    if (m_timeRemainingLabel != nullptr) {
      if (trackLengthUs > 0) {
        const std::int64_t remainingUs = std::max<std::int64_t>(0, trackLengthUs - displayPositionUs);
        m_timeRemainingLabel->setText("-" + formatDuration(remainingUs));
      } else {
        m_timeRemainingLabel->setText("0:00");
      }
    }

    m_playPauseButton->setGlyph(playPauseGlyph(player.playbackStatus));
    m_playPauseButton->setVariant(ButtonVariant::Primary);
    if (m_prevButton != nullptr) {
      m_prevButton->setEnabled(player.canGoPrevious);
    }
    if (m_nextButton != nullptr) {
      m_nextButton->setEnabled(player.canGoNext);
    }
    m_repeatButton->setGlyph(repeatGlyph(player.loopStatus));
    m_repeatButton->setVariant(toggleVariant(player.loopStatus != "None"));
    m_shuffleButton->setVariant(toggleVariant(player.shuffle));
    syncBongoPlayback(player.playbackStatus == "Playing");
    if (m_visualizerStatus != nullptr) {
      const bool live = player.playbackStatus == "Playing" && m_spectrum != nullptr;
      m_visualizerStatus->setText(i18n::tr(
          live ? "control-center.media.live-visualization" : "control-center.media.waiting-for-audio"
      ));
    }

    m_lastBusName = player.busName;
    m_lastPlaybackStatus = player.playbackStatus;
    m_lastLoopStatus = player.loopStatus;
    m_lastShuffle = player.shuffle;
    return;
  }

  m_pendingSeekBusName.clear();
  m_pendingSeekUs = -1;
  m_progressSettleUntil = {};
  m_lastActiveSnapshot.reset();
  m_positionBusName.clear();
  m_positionTrackId.clear();
  m_positionTrackSignature.clear();
  m_positionUs = 0;
  m_lastTrackLengthUs = 0;
  m_positionSampleAt = {};
  m_trackTitle->setText(i18n::tr("control-center.media.nothing-playing"));
  m_trackArtist->setText(i18n::tr("control-center.media.start-playback"));
  if (m_trackAlbum != nullptr) {
    m_trackAlbum->setText(" ");
    const bool showAlbum = m_presentation != MediaTabPresentation::ReferencePanel;
    m_trackAlbum->setVisible(showAlbum);
    m_trackAlbum->setParticipatesInLayout(showAlbum);
  }
  clearArt(renderer);
  m_lastArtPath.clear();
  if (m_artContainer != nullptr) {
    m_artContainer->clearShadow();
  }
  if (m_artworkRow != nullptr) {
    m_artworkRow->clearShadow();
  }
  if (m_visualizerSpectrum != nullptr) {
    m_visualizerSpectrum->setGradient(colorForRole(ColorRole::Secondary), colorForRole(ColorRole::Tertiary));
  }
  if (m_playPauseButton != nullptr) {
    m_playPauseButton->setVariant(ButtonVariant::Primary);
  }
  m_syncingProgress = true;
  m_progressSlider->setEnabled(false);
  m_progressSlider->setRange(0.0f, 100.0f);
  m_progressSlider->setValue(0.0f);
  m_syncingProgress = false;
  if (m_timeElapsedLabel != nullptr) {
    m_timeElapsedLabel->setText("0:00");
  }
  if (m_timeRemainingLabel != nullptr) {
    m_timeRemainingLabel->setText("0:00");
  }
  m_playPauseButton->setGlyph("media-play");
  if (m_prevButton != nullptr) {
    m_prevButton->setEnabled(false);
  }
  if (m_nextButton != nullptr) {
    m_nextButton->setEnabled(false);
  }
  m_repeatButton->setGlyph("repeat");
  m_repeatButton->setVariant(ButtonVariant::Ghost);
  m_shuffleButton->setVariant(ButtonVariant::Ghost);
  syncBongoPlayback(false);
  if (m_visualizerStatus != nullptr) {
    m_visualizerStatus->setText(i18n::tr("control-center.media.waiting-for-audio"));
  }
  m_lastBusName.clear();
  m_lastPlaybackStatus.clear();
  m_lastLoopStatus.clear();
  m_lastShuffle = false;
}
