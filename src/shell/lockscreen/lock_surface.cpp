#include "shell/lockscreen/lock_surface.h"

#include "capture/screencopy_capture.h"
#include "core/ui_phase.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/core/blur_cache.h"
#include "render/core/render_styles.h"
#include "render/core/shared_texture_cache.h"
#include "render/render_context.h"
#include "render/scene/countdown_ring_node.h"
#include "render/scene/wallpaper_node.h"
#include "shell/lockscreen/lockscreen_layout.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/controls/spinner.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <memory>
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

  constexpr Color kLockscreenBase = rgbHex(0x08110B);
  constexpr Color kLockscreenIsland = rgbHex(0x0D1710);
  constexpr Color kLockscreenCard = rgbHex(0x172319);
  constexpr Color kLockscreenCardRaised = rgbHex(0x1B291D);
  constexpr Color kLockscreenText = rgbHex(0xE5EEE7);
  constexpr Color kLockscreenMuted = rgbHex(0xA3B2A6);
  constexpr Color kLockscreenOutline = rgbaHex(0xD7E4DA1F);

  Color readableLockscreenAccent(ColorRole role) {
    Color color = colorForRole(role);
    color.a = 1.0f;
    if (relativeLuminance(color) < 0.38f) {
      color = lerpColor(color, rgbHex(0xFFFFFF), 0.48f);
    }
    return color;
  }

  ColorSpec lockscreenTextSpec() { return fixedColorSpec(kLockscreenText); }
  ColorSpec lockscreenMutedSpec() { return fixedColorSpec(kLockscreenMuted); }
  ColorSpec lockscreenCardSpec() { return fixedColorSpec(kLockscreenCard); }
  ColorSpec lockscreenOutlineSpec() { return fixedColorSpec(kLockscreenOutline); }

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
  m_root.addChild(ui::box({
      .out = &m_island,
      .fill = fixedColorSpec(kLockscreenIsland),
      .visible = false,
      .configure = [](Box& box) {
        box.setZIndex(1);
        box.setBorder(kLockscreenOutline, Style::borderWidth);
      },
  }));

  {
    auto node = std::make_unique<Node>();
    node->setZIndex(2);
    m_leftColumn = m_root.addChild(std::move(node));
  }
  {
    auto node = std::make_unique<Node>();
    node->setZIndex(2);
    m_rightColumn = m_root.addChild(std::move(node));
  }
  m_root.addChild(ui::column({
      .out = &m_loginPanel,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .gap = Style::spaceMd,
      .configure = [](Flex& flex) { flex.setZIndex(2); },
  }));

  const auto addCard = [](Node& parent, Flex** out, Color fill = kLockscreenCard) {
    parent.addChild(ui::column({
        .out = out,
        .align = FlexAlign::Stretch,
        .justify = FlexJustify::Center,
        .gap = Style::spaceXs,
        .padding = Style::spaceLg,
        .fill = fixedColorSpec(fill),
        .radius = Style::scaledRadiusXl(1.35f),
        .border = lockscreenOutlineSpec(),
        .borderWidth = Style::borderWidth,
        .clipChildren = true,
    }));
  };

  addCard(*m_leftColumn, &m_weatherCard);
  m_weatherCard->addChild(ui::label({
      .out = &m_weatherTitleLabel,
      .text = i18n::tr("lockscreen.dashboard.weather"),
      .fontSize = Style::fontSizeHeader,
      .fontWeight = FontWeight::SemiBold,
      .color = fixedColorSpec(readableLockscreenAccent(ColorRole::Secondary)),
      .maxLines = 1,
      .textAlign = TextAlign::Center,
  }));
  auto weatherBody = ui::row({
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceSm,
  });
  weatherBody->addChild(ui::glyph({
      .out = &m_weatherGlyph,
      .glyph = "weather-cloud-off",
      .glyphSize = 48.0f,
      .color = fixedColorSpec(readableLockscreenAccent(ColorRole::Primary)),
  }));
  weatherBody->addChild(ui::column(
      {.align = FlexAlign::End, .justify = FlexJustify::Center, .gap = Style::spaceXs, .flexGrow = 1.0f},
      ui::label({
          .out = &m_weatherTemperatureLabel,
          .fontSize = Style::fontSizeHeader * 1.1f,
          .fontWeight = FontWeight::Bold,
          .color = lockscreenTextSpec(),
          .maxLines = 1,
          .textAlign = TextAlign::End,
      }),
      ui::label({
          .out = &m_weatherConditionLabel,
          .fontSize = Style::fontSizeCaption,
          .color = lockscreenMutedSpec(),
          .maxLines = 1,
          .textAlign = TextAlign::End,
      })
  ));
  m_weatherCard->addChild(std::move(weatherBody));
  m_weatherCard->addChild(ui::label({
      .out = &m_weatherDetailLabel,
      .fontSize = Style::fontSizeMini,
      .color = lockscreenMutedSpec(),
      .maxLines = 2,
      .textAlign = TextAlign::Center,
  }));

  addCard(*m_leftColumn, &m_fetchCard, kLockscreenCardRaised);
  m_fetchCard->addChild(ui::label({
      .text = "❯  gnilfetch.sh",
      .fontSize = Style::fontSizeMini,
      .fontWeight = FontWeight::Medium,
      .fontFamily = "monospace",
      .color = fixedColorSpec(readableLockscreenAccent(ColorRole::Secondary)),
      .maxLines = 1,
  }));
  auto systemBody = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd});
  systemBody->addChild(ui::image({
      .out = &m_distroImage,
      .fit = ImageFit::Contain,
      .width = 72.0f,
      .height = 72.0f,
      .visible = false,
  }));
  systemBody->addChild(ui::glyph({
      .out = &m_distroGlyph,
      .glyph = "terminal",
      .glyphSize = 62.0f,
      .color = fixedColorSpec(readableLockscreenAccent(ColorRole::Primary)),
      .width = 72.0f,
      .height = 72.0f,
  }));
  systemBody->addChild(ui::column(
      {.align = FlexAlign::Stretch, .justify = FlexJustify::Center, .gap = Style::spaceXs, .flexGrow = 1.0f},
      ui::label({
          .out = &m_systemIdentityLabel,
          .fontSize = Style::fontSizeCaption,
          .fontWeight = FontWeight::SemiBold,
          .fontFamily = "monospace",
          .color = lockscreenTextSpec(),
          .maxLines = 2,
      }),
      ui::label({
          .out = &m_fetchLabel,
          .fontSize = Style::fontSizeMini,
          .fontFamily = "monospace",
          .color = lockscreenMutedSpec(),
          .maxLines = 5,
      })
  ));
  m_fetchCard->addChild(std::move(systemBody));
  auto paletteRow = ui::row({
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .gap = Style::spaceXs,
  });
  for (std::size_t i = 0; i < m_paletteDots.size(); ++i) {
    paletteRow->addChild(ui::box({
        .out = &m_paletteDots[i],
        .fill = fixedColorSpec(kLockscreenMuted),
        .radius = 5.0f,
        .width = 10.0f,
        .height = 10.0f,
    }));
  }
  m_fetchCard->addChild(std::move(paletteRow));

  addCard(*m_leftColumn, &m_mediaCard);
  m_mediaCard->addChild(ui::label({
      .out = &m_mediaHeaderLabel,
      .text = i18n::tr("lockscreen.dashboard.now-playing"),
      .fontSize = Style::fontSizeMini,
      .fontFamily = "monospace",
      .color = lockscreenMutedSpec(),
      .maxLines = 1,
  }));
  m_mediaCard->addChild(ui::label({
      .out = &m_mediaTitleLabel,
      .fontSize = Style::fontSizeBody,
      .fontWeight = FontWeight::SemiBold,
      .color = lockscreenTextSpec(),
      .maxLines = 1,
      .textAlign = TextAlign::Center,
      .ellipsize = TextEllipsize::End,
  }));
  m_mediaCard->addChild(ui::label({
      .out = &m_mediaArtistLabel,
      .fontSize = Style::fontSizeMini,
      .color = lockscreenMutedSpec(),
      .maxLines = 1,
      .textAlign = TextAlign::Center,
      .ellipsize = TextEllipsize::End,
  }));
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
      .onClick = [this]() { if (m_onMediaPrevious) m_onMediaPrevious(); },
  }));
  mediaControls->addChild(ui::button({
      .out = &m_mediaPlayPauseButton,
      .glyph = "media-play",
      .glyphSize = 20.0f,
      .controlHeight = Style::controlHeight,
      .variant = ButtonVariant::Secondary,
      .radius = Style::controlHeight * 0.5f,
      .onClick = [this]() { if (m_onMediaPlayPause) m_onMediaPlayPause(); },
  }));
  mediaControls->addChild(ui::button({
      .out = &m_mediaNextButton,
      .glyph = "media-next",
      .glyphSize = 18.0f,
      .controlHeight = Style::controlHeightSm,
      .variant = ButtonVariant::Ghost,
      .radius = Style::controlHeightSm * 0.5f,
      .onClick = [this]() { if (m_onMediaNext) m_onMediaNext(); },
  }));
  m_mediaCard->addChild(std::move(mediaControls));

  auto resources = ui::column({
      .out = &m_resourcesCard,
      .align = FlexAlign::Stretch,
      .justify = FlexJustify::Start,
      .participatesInLayout = false,
  });
  for (auto& metric : m_metricViews) {
    auto tile = ui::column({
        .out = &metric.card,
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .fill = lockscreenCardSpec(),
        .radius = Style::scaledRadiusXl(1.25f),
        .border = lockscreenOutlineSpec(),
        .borderWidth = Style::borderWidth,
    });
    auto track = std::make_unique<CountdownRingNode>();
    track->setParticipatesInLayout(false);
    track->setHitTestVisible(false);
    track->setProgress(1.0f);
    track->setZIndex(0);
    metric.track = static_cast<CountdownRingNode*>(tile->addChild(std::move(track)));
    auto progress = std::make_unique<CountdownRingNode>();
    progress->setParticipatesInLayout(false);
    progress->setHitTestVisible(false);
    progress->setProgress(0.0f);
    progress->setZIndex(1);
    metric.progress = static_cast<CountdownRingNode*>(tile->addChild(std::move(progress)));
    tile->addChild(ui::glyph({
        .out = &metric.glyph,
        .glyph = "memory",
        .glyphSize = 24.0f,
        .color = lockscreenTextSpec(),
        .participatesInLayout = false,
        .configure = [](Glyph& glyph) { glyph.setZIndex(2); },
    }));
    tile->addChild(ui::label({
        .out = &metric.value,
        .text = "—",
        .fontSize = Style::fontSizeMini,
        .fontWeight = FontWeight::SemiBold,
        .fontFamily = "monospace",
        .color = lockscreenMutedSpec(),
        .textAlign = TextAlign::Center,
        .participatesInLayout = false,
        .configure = [](Label& label) { label.setZIndex(2); },
    }));
    resources->addChild(std::move(tile));
  }
  m_rightColumn->addChild(std::move(resources));

  addCard(*m_rightColumn, &m_notificationsCard, kLockscreenCardRaised);
  m_notificationsCard->setJustify(FlexJustify::Start);
  m_notificationsCard->addChild(ui::label({
      .out = &m_notificationsHeaderLabel,
      .text = i18n::tr("lockscreen.dashboard.notifications"),
      .fontSize = Style::fontSizeMini,
      .fontFamily = "monospace",
      .color = lockscreenMutedSpec(),
      .maxLines = 1,
  }));
  m_notificationsCard->addChild(ui::column(
      {.out = &m_notificationsEmpty,
       .align = FlexAlign::Center,
       .justify = FlexJustify::Center,
       .gap = Style::spaceSm,
       .flexGrow = 1.0f},
      ui::glyph({
          .out = &m_notificationsEmptyGlyph,
          .glyph = "landscape",
          .glyphSize = 52.0f,
          .color = fixedColorSpec(withAlpha(kLockscreenMuted, 0.42f)),
      }),
      ui::label({
          .out = &m_notificationsLabel,
          .fontSize = Style::fontSizeCaption,
          .fontFamily = "monospace",
          .color = lockscreenMutedSpec(),
          .maxLines = 3,
          .textAlign = TextAlign::Center,
      })
  ));
  for (auto& preview : m_notificationViews) {
    m_notificationsCard->addChild(ui::column(
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
            .color = fixedColorSpec(readableLockscreenAccent(ColorRole::Secondary)),
            .maxLines = 1,
        }),
        ui::label({
            .out = &preview.summary,
            .fontSize = Style::fontSizeCaption,
            .color = lockscreenMutedSpec(),
            .maxLines = 2,
        })
    ));
  }

  m_loginPanel->addChild(ui::label({
      .out = &m_timeLabel,
      .text = "00:00",
      .fontSize = 88.0f,
      .fontWeight = FontWeight::Bold,
      .color = lockscreenTextSpec(),
      .textAlign = TextAlign::Center,
  }));
  m_loginPanel->addChild(ui::label({
      .out = &m_dateLabel,
      .fontSize = Style::fontSizeTitle,
      .fontWeight = FontWeight::SemiBold,
      .color = fixedColorSpec(readableLockscreenAccent(ColorRole::Secondary)),
      .textAlign = TextAlign::Center,
  }));
  m_loginPanel->addChild(ui::box({
      .out = &m_avatarFrame,
      .fill = fixedColorSpec(kLockscreenCardRaised),
      .radius = 56.0f,
      .width = 112.0f,
      .height = 112.0f,
  }));
  m_avatarFrame->addChild(ui::image({
      .out = &m_avatarImage,
      .fit = ImageFit::Cover,
      .radius = 56.0f,
      .width = 112.0f,
      .height = 112.0f,
      .visible = false,
  }));
  m_avatarFrame->addChild(ui::glyph({
      .out = &m_avatarGlyph,
      .glyph = "person",
      .glyphSize = 54.0f,
      .color = fixedColorSpec(readableLockscreenAccent(ColorRole::Primary)),
      .width = 112.0f,
      .height = 112.0f,
  }));
  m_loginPanel->addChild(ui::label({
      .out = &m_userLabel,
      .fontSize = Style::fontSizeCaption,
      .fontWeight = FontWeight::Medium,
      .color = lockscreenMutedSpec(),
      .textAlign = TextAlign::Center,
  }));
  m_loginPanel->addChild(ui::row({
      .out = &m_loginContentRow,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Center,
      .gap = Style::spaceSm,
      .maxWidth = 460.0f,
      .widthPolicy = FlexSizePolicy::Fill,
      .heightPolicy = FlexSizePolicy::Content,
  }));
  m_loginContentRow->addChild(ui::row(
      {.out = &m_passwordCapsule,
       .align = FlexAlign::Center,
       .justify = FlexJustify::Start,
       .gap = Style::spaceXs,
       .paddingH = Style::spaceXs,
       .fill = fixedColorSpec(kLockscreenCardRaised),
       .radius = Style::controlHeightLg * 0.5f,
       .border = lockscreenOutlineSpec(),
       .borderWidth = Style::borderWidth,
       .minHeight = Style::controlHeightLg,
       .flexGrow = 1.0f},
      ui::glyph({
          .out = &m_passwordLockGlyph,
          .glyph = "lock",
          .glyphSize = 16.0f,
          .color = lockscreenMutedSpec(),
          .width = Style::controlHeightSm,
          .height = Style::controlHeightSm,
      }),
      ui::input({
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
          .onSubmit = [this](const std::string&) { if (m_onLogin) m_onLogin(); },
          .configure = [](Input& input) { input.setZIndex(2); },
      }),
      ui::button({
          .out = &m_loginButton,
          .glyph = "arrow-right",
          .glyphSize = 18.0f,
          .controlHeight = Style::controlHeightSm,
          .variant = ButtonVariant::Ghost,
          .radius = Style::controlHeightSm * 0.5f,
          .onClick = [this]() { if (m_onLogin) m_onLogin(); },
          .configure = [](Button& button) { button.setZIndex(2); },
      }),
      ui::spinner({
          .out = &m_loginSpinner,
          .color = fixedColorSpec(readableLockscreenAccent(ColorRole::Primary)),
          .spinnerSize = Style::controlHeightSm * 0.55f,
          .thickness = 2.0f,
          .spinning = false,
          .width = Style::controlHeightSm,
          .height = Style::controlHeightSm,
          .visible = false,
      })
  ));
  m_loginPanel->addChild(ui::button({
      .out = &m_layoutChip,
      .text = "",
      .fontSize = Style::fontSizeMini,
      .controlHeight = Style::controlHeightSm,
      .variant = ButtonVariant::Ghost,
      .visible = false,
      .onClick = [this]() { if (m_onCycleLayout) m_onCycleLayout(); },
      .configure = [](Button& button) { button.setZIndex(2); },
  }));
  m_loginPanel->addChild(ui::label({
      .out = &m_statusLabel,
      .fontSize = Style::fontSizeCaption,
      .color = lockscreenMutedSpec(),
      .textAlign = TextAlign::Center,
      .visible = false,
      .configure = [](Label& label) { label.setZIndex(2); },
  }));

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
    focusPasswordField();
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
  m_introOffsetY = 42.0f;
  m_introSideOffset = 34.0f;
  m_introOpacity = 0.0f;
  m_loginPanel->setPosition(m_loginBaseX, m_loginBaseY + m_introOffsetY);
  m_loginPanel->setOpacity(m_introOpacity);
  if (m_island != nullptr) m_island->setOpacity(m_introOpacity);
  if (m_leftColumn != nullptr) m_leftColumn->setOpacity(m_introOpacity);
  if (m_rightColumn != nullptr) m_rightColumn->setOpacity(m_introOpacity);

  m_animations.animate(
      m_introOffsetY, 0.0f, static_cast<float>(Style::animChromeSpatial), Easing::CaelestiaExpressiveSpatial,
      [this](float value) {
        m_introOffsetY = value;
        if (m_loginPanel != nullptr) {
          m_loginPanel->setPosition(m_loginBaseX, m_loginBaseY + m_introOffsetY);
        }
      },
      {}, m_loginPanel
  );
  m_animations.animate(
      m_introSideOffset, 0.0f, static_cast<float>(Style::animChromeSpatial), Easing::CaelestiaExpressiveSpatial,
      [this](float value) {
        m_introSideOffset = value;
        requestLayout();
      },
      {}, m_leftColumn
  );
  m_animations.animate(
      m_introOpacity, 1.0f, static_cast<float>(Style::animSlow), Easing::EaseOutCubic,
      [this](float value) {
        m_introOpacity = std::clamp(value, 0.0f, 1.0f);
        if (m_loginPanel != nullptr) {
          m_loginPanel->setOpacity(m_introOpacity);
        }
        if (m_island != nullptr) m_island->setOpacity(m_introOpacity);
        if (m_leftColumn != nullptr) m_leftColumn->setOpacity(m_introOpacity);
        if (m_rightColumn != nullptr) m_rightColumn->setOpacity(m_introOpacity);
      },
      {}, m_loginPanel
  );
  requestFrameTick();
}

void LockSurface::startPasswordErrorAnimation() {
  if (m_passwordCapsule == nullptr || m_blackout || !m_locked) {
    return;
  }
  m_animations.cancelForOwner(m_passwordCapsule);
  m_animations.animate(
      0.0f, 1.0f, 280.0f, Easing::EaseOutCubic,
      [this](float progress) {
        constexpr float kPi = 3.14159265358979323846f;
        m_passwordErrorOffsetX = std::sin(progress * kPi * 4.0f) * (1.0f - progress) * 9.0f;
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
  m_animations.cancelAll();
  m_animations.animate(
      m_loginPanel != nullptr ? m_loginPanel->opacity() : 1.0f, 0.0f, Style::animChromeExit, Easing::EaseInOutQuad,
      [this](float value) {
        if (m_loginPanel != nullptr) m_loginPanel->setOpacity(value);
        if (m_leftColumn != nullptr) m_leftColumn->setOpacity(value);
        if (m_rightColumn != nullptr) m_rightColumn->setOpacity(value);
      },
      {}, m_loginPanel
  );
  m_animations.animate(
      m_island != nullptr ? m_island->opacity() : 1.0f, 0.0f, Style::animChromeExit, Easing::EaseInOutQuad,
      [this](float value) {
        if (m_island != nullptr) m_island->setOpacity(value);
      },
      [this, finished = std::move(finished)]() mutable {
        m_unlocking = false;
        if (finished) finished();
      },
      m_island
  );
  requestFrameTick();
}

void LockSurface::applyLockscreenPalette() {
  const Color primary = readableLockscreenAccent(ColorRole::Primary);
  const Color secondary = readableLockscreenAccent(ColorRole::Secondary);
  const Color tertiary = readableLockscreenAccent(ColorRole::Tertiary);
  const Color error = readableLockscreenAccent(ColorRole::Error);
  const std::array accents = {primary, secondary, tertiary, error, secondary, primary, error, tertiary};

  if (m_island != nullptr) {
    m_island->setFill(kLockscreenIsland);
    m_island->setBorder(kLockscreenOutline, Style::borderWidth);
  }
  for (Flex* card : {m_weatherCard, m_mediaCard}) {
    if (card != nullptr) {
      card->setFill(kLockscreenCard);
    }
  }
  if (m_resourcesCard != nullptr) {
    m_resourcesCard->clearFill();
  }
  for (Flex* card : {m_fetchCard, m_notificationsCard}) {
    if (card != nullptr) {
      card->setFill(kLockscreenCardRaised);
    }
  }
  for (std::size_t i = 0; i < m_paletteDots.size(); ++i) {
    if (m_paletteDots[i] != nullptr) {
      m_paletteDots[i]->setFill(accents[i]);
    }
  }
  if (m_weatherTitleLabel != nullptr) m_weatherTitleLabel->setColor(secondary);
  if (m_weatherGlyph != nullptr) m_weatherGlyph->setColor(primary);
  if (m_dateLabel != nullptr) m_dateLabel->setColor(secondary);
  if (m_avatarGlyph != nullptr) m_avatarGlyph->setColor(primary);
  if (m_distroGlyph != nullptr) m_distroGlyph->setColor(primary);
  if (m_loginSpinner != nullptr) m_loginSpinner->setColor(primary);

  const std::array metricColors = {primary, tertiary, secondary, readableLockscreenAccent(ColorRole::Primary)};
  for (std::size_t i = 0; i < m_metricViews.size(); ++i) {
    auto& metric = m_metricViews[i];
    if (metric.card != nullptr) {
      metric.card->setFill(kLockscreenCard);
      metric.card->setBorder(kLockscreenOutline, Style::borderWidth);
    }
    if (metric.track != nullptr) metric.track->setColor(withAlpha(metricColors[i], 0.16f));
    if (metric.progress != nullptr) metric.progress->setColor(metricColors[i]);
  }
  for (auto& preview : m_notificationViews) {
    if (preview.app != nullptr) preview.app->setColor(secondary);
  }
  if (m_passwordCapsule != nullptr) {
    m_passwordCapsule->setFill(kLockscreenCardRaised);
    m_passwordCapsule->setBorder(m_error ? error : kLockscreenOutline, m_error ? 1.5f : Style::borderWidth);
  }
}

void LockSurface::layoutMetricViews(Renderer& renderer, float width, float height, float scale, float gap) {
  if (m_resourcesCard == nullptr) {
    return;
  }
  const float tileGap = std::max(4.0f, gap * 0.55f);
  const float tileWidth = std::max(1.0f, (width - tileGap) * 0.5f);
  const float tileHeight = std::max(1.0f, (height - tileGap) * 0.5f);
  const float ringSize = std::max(24.0f, std::min(tileWidth, tileHeight) * 0.68f);
  const float thickness = std::clamp(4.0f * scale, 2.5f, 6.0f);

  for (std::size_t i = 0; i < m_metricViews.size(); ++i) {
    auto& metric = m_metricViews[i];
    if (metric.card == nullptr || metric.track == nullptr || metric.progress == nullptr
        || metric.glyph == nullptr || metric.value == nullptr) {
      continue;
    }
    const float tileX = (i % 2U == 0U) ? 0.0f : tileWidth + tileGap;
    const float tileY = (i < 2U) ? 0.0f : tileHeight + tileGap;
    metric.card->arrange(renderer, LayoutRect{tileX, tileY, tileWidth, tileHeight});

    const float ringX = std::round((tileWidth - ringSize) * 0.5f);
    const float ringY = std::round((tileHeight - ringSize) * 0.5f);
    for (CountdownRingNode* ring : {metric.track, metric.progress}) {
      ring->setPosition(ringX, ringY);
      ring->setFrameSize(ringSize, ringSize);
      ring->setThickness(thickness);
    }

    const float glyphSlot = ringSize * 0.46f;
    metric.glyph->setGlyphSize(ringSize * 0.27f);
    metric.glyph->setPosition(
        std::round((tileWidth - glyphSlot) * 0.5f),
        std::round((tileHeight - glyphSlot) * 0.5f - 4.0f * scale)
    );
    metric.glyph->setSize(glyphSlot, glyphSlot);

    metric.value->setFontSize(std::max(8.0f, Style::fontSizeMini * 0.82f * scale));
    metric.value->setMaxWidth(tileWidth - Style::spaceSm * 2.0f);
    metric.value->measure(renderer);
    metric.value->setPosition(
        std::round((tileWidth - metric.value->width()) * 0.5f),
        std::round(tileHeight * 0.66f)
    );
  }
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

bool LockSurface::passwordFieldContainsPoint(float sceneX, float sceneY) const {
  return m_passwordField != nullptr && m_passwordField->containsScenePoint(sceneX, sceneY);
}

void LockSurface::focusPasswordField() {
  if (!m_locked || m_blackout || m_passwordField == nullptr) {
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

  if (m_blackout) {
    m_root.setSize(sw, sh);
    m_backgroundLayer->setPosition(0.0f, 0.0f);
    m_backgroundLayer->setSize(sw, sh);
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
    m_island->setVisible(false);
    m_leftColumn->setVisible(false);
    m_rightColumn->setVisible(false);
    m_loginPanel->setVisible(false);
    m_passwordField->setVisible(false);
    m_loginButton->setVisible(false);
    if (m_layoutChip != nullptr) {
      m_layoutChip->setVisible(false);
    }
    if (m_statusLabel != nullptr) {
      m_statusLabel->setVisible(false);
    }
    return;
  }

  applyWallpaperTexture();

  m_wallpaper->setVisible(true);
  // The lock surface may map a frame before the compositor confirms the secure
  // lock. Keep the card hidden until then so the intro cannot flash at its final
  // position and restart when `locked` arrives.
  const bool loginVisible = m_locked;
  m_loginPanel->setVisible(loginVisible);
  m_leftColumn->setVisible(loginVisible);
  m_rightColumn->setVisible(loginVisible);
  m_island->setVisible(loginVisible);
  m_loginContentRow->setVisible(loginVisible);
  m_passwordField->setVisible(loginVisible);
  m_loginButton->setVisible(loginVisible);

  m_root.setSize(sw, sh);

  m_backgroundLayer->setPosition(0.0f, 0.0f);
  m_backgroundLayer->setSize(sw, sh);

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
    const float tintIntensity = m_tintIntensity;
    const bool showTint = tintIntensity > 0.0f;
    m_tintOverlay->setVisible(showTint);
    if (showTint) {
      m_tintOverlay->setStyle(
          RoundedRectStyle{
              .fill = withAlpha(kLockscreenBase, std::clamp(tintIntensity, 0.0f, 1.0f)),
              .fillMode = FillMode::Solid,
          }
      );
    }
  }

  const auto layout = lockscreen_layout::resolve(sw, sh);
  const float centerScale = layout.scale;
  m_loginPanel->clearFill();
  m_loginPanel->clearBorder();
  m_loginPanel->setSoftness(1.0f);
  m_loginPanel->setGap(Style::spaceMd * centerScale);
  m_loginPanel->setPadding(0.0f);

  if (m_timeLabel != nullptr) {
    m_timeLabel->setFontSize(std::clamp(74.0f * centerScale, 46.0f, 96.0f));
  }
  if (m_dateLabel != nullptr) {
    m_dateLabel->setFontSize(Style::fontSizeBody * 1.1f * centerScale);
  }
  if (m_userLabel != nullptr) {
    m_userLabel->setFontSize(Style::fontSizeCaption * centerScale);
  }
  syncAvatar(*renderer, std::clamp(142.0f * centerScale, 92.0f, 168.0f));
  m_loginContentRow->setMinHeight(Style::controlHeightLg);
  m_passwordField->setSurfaceOpacity(0.0f);
  m_passwordField->setTextAlign(TextAlign::Center);
  m_loginButton->setRadius(Style::controlHeightSm * 0.5f);

  m_island->setPosition(layout.island.x, layout.island.y);
  m_island->setSize(layout.island.width, layout.island.height);
  m_island->setRadius(layout.radius);
  m_island->setOpacity(m_introOpacity);

  m_loginBaseX = layout.centerColumn.x;
  m_loginBaseY = layout.centerColumn.y;
  m_loginPanel->setOpacity(m_introOpacity);
  m_loginPanel->arrange(
      *renderer,
      LayoutRect{
          layout.centerColumn.x,
          layout.centerColumn.y + m_introOffsetY,
          layout.centerColumn.width,
          layout.centerColumn.height,
      }
  );
  if (m_loginContentRow != nullptr && m_passwordErrorOffsetX != 0.0f) {
    m_loginContentRow->setPosition(m_loginContentRow->x() + m_passwordErrorOffsetX, m_loginContentRow->y());
  }

  m_leftColumn->setVisible(loginVisible && layout.fullDashboard);
  m_rightColumn->setVisible(loginVisible && layout.fullDashboard);
  if (layout.fullDashboard) {
    if (m_distroImage != nullptr && m_distroGlyph != nullptr) {
      const int distroSize = std::max(32, static_cast<int>(std::round(72.0f * centerScale)));
      if (m_dashboard.distroAssetPath != m_loadedDistroPath || distroSize != m_loadedDistroSize) {
        if (m_distroImage->hasImage()) m_distroImage->clear(*renderer);
        const bool loaded = !m_dashboard.distroAssetPath.empty()
            && m_distroImage->setSourceFile(
                *renderer, m_dashboard.distroAssetPath, distroSize, false, true
            );
        m_distroImage->setVisible(loaded);
        m_distroGlyph->setVisible(!loaded);
        m_loadedDistroPath = m_dashboard.distroAssetPath;
        m_loadedDistroSize = distroSize;
      }
    }
    m_leftColumn->setPosition(layout.leftColumn.x - m_introSideOffset, layout.leftColumn.y);
    m_leftColumn->setSize(layout.leftColumn.width, layout.leftColumn.height);
    m_rightColumn->setPosition(layout.rightColumn.x + m_introSideOffset, layout.rightColumn.y);
    m_rightColumn->setSize(layout.rightColumn.width, layout.rightColumn.height);
    m_leftColumn->setOpacity(m_introOpacity);
    m_rightColumn->setOpacity(m_introOpacity);

    const auto localRect = [](const lockscreen_layout::Rect& child, const lockscreen_layout::Rect& parent) {
      return LayoutRect{child.x - parent.x, child.y - parent.y, child.width, child.height};
    };
    m_weatherCard->setPadding(Style::spaceSm * centerScale);
    m_fetchCard->setPadding(Style::spaceMd * centerScale);
    m_mediaCard->setPadding(Style::spaceSm * centerScale);
    m_notificationsCard->setPadding(Style::spaceSm * centerScale);
    m_weatherCard->arrange(*renderer, localRect(layout.weatherCard, layout.leftColumn));
    m_fetchCard->arrange(*renderer, localRect(layout.systemCard, layout.leftColumn));
    m_mediaCard->arrange(*renderer, localRect(layout.mediaCard, layout.leftColumn));

    m_resourcesCard->setPosition(
        layout.metricsCard.x - layout.rightColumn.x,
        layout.metricsCard.y - layout.rightColumn.y
    );
    m_resourcesCard->setSize(layout.metricsCard.width, layout.metricsCard.height);
    layoutMetricViews(*renderer, layout.metricsCard.width, layout.metricsCard.height, centerScale, layout.gap);
    m_notificationsCard->arrange(*renderer, localRect(layout.notificationsCard, layout.rightColumn));
  }
  if (m_introPending) {
    startIntroAnimation();
  }
}

std::string LockSurface::resolveStatusText(bool& isError) const {
  isError = false;
  // A live authentication/error message always wins, then any other transient status
  // (e.g. "password cleared"), then the caps-lock warning, then the idle password hint.
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
  return i18n::tr("lockscreen.ready");
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
  syncDashboardCopy();
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
    const Color outline = m_error ? readableLockscreenAccent(ColorRole::Error) : kLockscreenOutline;
    m_passwordCapsule->setBorder(outline, m_error ? 1.5f : Style::borderWidth);
  }

  if (m_statusLabel != nullptr) {
    bool isError = false;
    const std::string text = resolveStatusText(isError);
    const bool show = m_locked && !m_blackout && !text.empty();
    m_statusLabel->setVisible(show);
    if (show) {
      m_statusLabel->setText(text);
      m_statusLabel->setColor(isError ? readableLockscreenAccent(ColorRole::Error) : kLockscreenMuted);
    }
  }

  if (m_layoutChip != nullptr) {
    const bool show = m_locked && !m_blackout && m_hasMultipleLayouts;
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
  if (m_weatherDetailLabel != nullptr) m_weatherDetailLabel->setText(m_dashboard.weatherDetail);
  if (m_systemIdentityLabel != nullptr) m_systemIdentityLabel->setText(m_dashboard.systemIdentity);
  if (m_fetchLabel != nullptr) m_fetchLabel->setText(m_dashboard.systemDetails);
  if (m_mediaTitleLabel != nullptr) m_mediaTitleLabel->setText(m_dashboard.mediaTitle);
  if (m_mediaArtistLabel != nullptr) m_mediaArtistLabel->setText(m_dashboard.mediaArtist);
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
          view.displayedProgress, target, 420.0f, Easing::EaseOutCubic,
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
    m_notificationsEmptyGlyph->setGlyph(m_dashboard.showNotifications ? "landscape" : "notifications-off");
  }
  if (m_notificationsLabel != nullptr) {
    m_notificationsLabel->setText(
        !m_dashboard.showNotifications ? i18n::tr("lockscreen.dashboard.notifications-private")
        : m_dashboard.notificationCount == 0 ? i18n::tr("lockscreen.dashboard.no-notifications")
                                             : std::string{}
    );
  }
  if (m_notificationsHeaderLabel != nullptr) {
    const std::string count = m_dashboard.showNotifications && m_dashboard.notificationCount > 0
        ? std::format(" ({})", m_dashboard.notificationCount)
        : std::string{};
    m_notificationsHeaderLabel->setText(i18n::tr("lockscreen.dashboard.notifications") + count);
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
