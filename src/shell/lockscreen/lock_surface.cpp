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
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
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

  m_backgroundLayer->addChild(
      ui::box({
          .out = &m_tintOverlay,
          .visible = false,
          .configure = [](Box& box) { box.setZIndex(1); },
      })
  );

  m_backgroundLayer->addChild(
      ui::box({
          .out = &m_backdrop,
          .configure = [](Box& box) { box.setZIndex(-1); },
      })
  );

  m_root.addChild(
      ui::box({
          .out = &m_island,
          .visible = false,
          .configure = [](Box& box) {
            box.setZIndex(1);
            box.setFill(colorSpecFromRole(ColorRole::Surface, 0.96f));
            box.setBorder(colorSpecFromRole(ColorRole::Outline, 0.55f), Style::borderWidth);
          },
      })
  );

  m_root.addChild(
      ui::flex(
          FlexDirection::Vertical,
          {
              .out = &m_leftColumn,
              .align = FlexAlign::Stretch,
              .justify = FlexJustify::Start,
              .gap = Style::spaceMd,
              .configure = [](Flex& flex) { flex.setZIndex(2); },
          }
      )
  );

  m_root.addChild(
      ui::flex(
          FlexDirection::Vertical,
          {
              .out = &m_rightColumn,
              .align = FlexAlign::Stretch,
              .justify = FlexJustify::Start,
              .gap = Style::spaceMd,
              .configure = [](Flex& flex) { flex.setZIndex(2); },
          }
      )
  );

  m_root.addChild(
      ui::flex(
          FlexDirection::Vertical,
          {
              .out = &m_loginPanel,
              .align = FlexAlign::Center,
              .justify = FlexJustify::Center,
              .gap = Style::spaceLg,
              .paddingV = Style::spaceLg * 2.0f,
              .paddingH = Style::spaceLg * 2.0f,
              .configure = [](Flex& flex) { flex.setZIndex(2); },
          }
      )
  );

  m_loginPanel->addChild(
      ui::label({
          .out = &m_timeLabel,
          .text = "00:00",
          .fontSize = 88.0f,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .textAlign = TextAlign::Center,
      })
  );

  const auto addCard = [](Flex& column, Flex** out, float grow = 0.0f) {
    auto card = ui::flex(
        FlexDirection::Vertical,
        {
            .out = out,
            .align = FlexAlign::Stretch,
            .justify = FlexJustify::Center,
            .gap = Style::spaceXs,
            .padding = Style::spaceLg,
            .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.72f),
            .radius = Style::scaledRadiusXl(1.5f),
            .border = colorSpecFromRole(ColorRole::Outline, 0.32f),
            .borderWidth = Style::borderWidth,
            .flexGrow = grow,
        }
    );
    column.addChild(std::move(card));
  };
  addCard(*m_leftColumn, &m_weatherCard);
  m_weatherCard->addChild(
      ui::label({
          .out = &m_weatherTitleLabel,
          .fontSize = Style::fontSizeHeader,
          .fontWeight = FontWeight::SemiBold,
          .color = colorSpecFromRole(ColorRole::Primary),
          .maxLines = 1,
      })
  );
  m_weatherCard->addChild(
      ui::label({
          .out = &m_weatherDetailLabel,
          .fontSize = Style::fontSizeBody,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 3,
      })
  );
  addCard(*m_leftColumn, &m_fetchCard);
  m_fetchCard->addChild(
      ui::label({
          .out = &m_fetchLabel,
          .fontSize = Style::fontSizeCaption,
          .fontFamily = "monospace",
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 5,
      })
  );
  addCard(*m_leftColumn, &m_mediaCard, 1.0f);
  m_mediaCard->addChild(
      ui::label({
          .out = &m_mediaTitleLabel,
          .fontSize = Style::fontSizeTitle,
          .fontWeight = FontWeight::SemiBold,
          .color = colorSpecFromRole(ColorRole::Primary),
          .maxLines = 2,
          .textAlign = TextAlign::Center,
      })
  );
  m_mediaCard->addChild(
      ui::label({
          .out = &m_mediaArtistLabel,
          .fontSize = Style::fontSizeBody,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
          .textAlign = TextAlign::Center,
      })
  );

  addCard(*m_rightColumn, &m_resourcesCard);
  auto resources = ui::flex(
      FlexDirection::Horizontal,
      {.align = FlexAlign::Center, .justify = FlexJustify::SpaceBetween, .gap = Style::spaceSm}
  );
  resources->addChild(ui::label({.out = &m_cpuLabel, .fontSize = Style::fontSizeCaption, .color = colorSpecFromRole(ColorRole::Primary), .textAlign = TextAlign::Center}));
  resources->addChild(ui::label({.out = &m_memoryLabel, .fontSize = Style::fontSizeCaption, .color = colorSpecFromRole(ColorRole::Tertiary), .textAlign = TextAlign::Center}));
  resources->addChild(ui::label({.out = &m_storageLabel, .fontSize = Style::fontSizeCaption, .color = colorSpecFromRole(ColorRole::Secondary), .textAlign = TextAlign::Center}));
  m_resourcesCard->addChild(std::move(resources));
  addCard(*m_rightColumn, &m_notificationsCard, 1.0f);
  m_notificationsCard->addChild(
      ui::label({
          .out = &m_notificationsLabel,
          .fontSize = Style::fontSizeCaption,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 8,
      })
  );

  m_loginPanel->addChild(
      ui::label({
          .out = &m_dateLabel,
          .fontSize = Style::fontSizeTitle,
          .fontWeight = FontWeight::SemiBold,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .textAlign = TextAlign::Center,
      })
  );

  m_loginPanel->addChild(
      ui::box({
          .out = &m_avatarFrame,
          .fill = colorSpecFromRole(ColorRole::Primary, 0.22f),
          .radius = 56.0f,
          .width = 112.0f,
          .height = 112.0f,
      })
  );
  m_avatarFrame->addChild(
      ui::image({
          .out = &m_avatarImage,
          .fit = ImageFit::Cover,
          .radius = 56.0f,
          .width = 112.0f,
          .height = 112.0f,
          .visible = false,
      })
  );
  m_avatarFrame->addChild(
      ui::glyph({
          .out = &m_avatarGlyph,
          .glyph = "person",
          .glyphSize = 54.0f,
          .color = colorSpecFromRole(ColorRole::Primary),
          .width = 112.0f,
          .height = 112.0f,
      })
  );

  m_loginPanel->addChild(
      ui::label({
          .out = &m_userLabel,
          .fontSize = Style::fontSizeHeader,
          .fontWeight = FontWeight::SemiBold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .textAlign = TextAlign::Center,
      })
  );

  m_loginPanel->addChild(
      ui::flex(
          FlexDirection::Horizontal,
          {
              .out = &m_loginContentRow,
              .align = FlexAlign::Center,
              .justify = FlexJustify::Start,
              .gap = Style::spaceSm,
              .maxWidth = 400.0f,
              .widthPolicy = FlexSizePolicy::Fill,
              .heightPolicy = FlexSizePolicy::Content,
          }
      )
  );

  m_loginContentRow->addChild(
      ui::button({
          .out = &m_layoutChip,
          .text = "",
          .fontSize = Style::fontSizeCaption,
          .variant = ButtonVariant::Secondary,
          .visible = false,
          .onClick =
              [this]() {
                if (m_onCycleLayout) {
                  m_onCycleLayout();
                }
              },
          .configure = [](Button& button) { button.setZIndex(2); },
      })
  );

  m_loginContentRow->addChild(
      ui::input({
          .out = &m_passwordField,
          .placeholder = i18n::tr("lockscreen.password-placeholder"),
          .controlHeight = Style::controlHeightLg,
          .passwordMode = true,
          .onChange =
              [this](const std::string& value) {
                if (m_onPasswordChanged) {
                  m_onPasswordChanged(value);
                }
              },
          .onSubmit =
              [this](const std::string& /*value*/) {
                if (m_onLogin) {
                  m_onLogin();
                }
              },
          .configure =
              [](Input& input) {
                input.setZIndex(2);
                input.setFlexGrow(1.0f);
              },
      })
  );

  m_loginContentRow->addChild(
      ui::button({
          .out = &m_loginButton,
          .text = "",
          .glyph = "check",
          .glyphSize = 20.0f,
          .controlHeight = Style::controlHeightLg,
          .variant = ButtonVariant::Primary,
          .onClick =
              [this]() {
                if (m_onLogin) {
                  m_onLogin();
                }
              },
          .configure = [](Button& button) { button.setZIndex(2); },
      })
  );

  m_loginPanel->addChild(
      ui::label({
          .out = &m_statusLabel,
          .fontSize = Style::fontSizeCaption,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .textAlign = TextAlign::Center,
          .visible = false,
          .configure = [](Label& label) { label.setZIndex(2); },
      })
  );

  m_inputDispatcher.setSceneRoot(&m_root);
  m_inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_connection.setCursorShape(serial, shape);
  });

  m_root.setAnimationManager(&m_animations);
  setAnimationManager(&m_animations);
  setSceneRoot(&m_root);
  setConfigureCallback([this](std::uint32_t /*width*/, std::uint32_t /*height*/) { requestLayout(); });
  setPrepareFrameCallback([this](bool needsUpdate, bool needsLayout) { prepareFrame(needsUpdate, needsLayout); });
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
    m_introOpacity = 1.0f;
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
              .fill = colorForRole(ColorRole::Surface, tintIntensity),
              .fillMode = FillMode::Solid,
          }
      );
    }
  }

  const float centerScale = std::clamp(sh / 1440.0f, 0.72f, 1.0f);

  // The island owns the material.  The three content columns are transparent
  // children so their corners never double-stack or fight the reveal clip.
  m_loginPanel->clearFill();
  m_loginPanel->clearBorder();
  m_loginPanel->setSoftness(1.0f);
  m_loginPanel->setGap(Style::spaceLg * 1.35f * centerScale);
  m_loginPanel->setPadding(0.0f);

  if (m_timeLabel != nullptr) {
    m_timeLabel->setFontSize(112.0f * centerScale);
  }
  if (m_dateLabel != nullptr) {
    m_dateLabel->setFontSize(Style::fontSizeTitle * centerScale);
  }
  if (m_userLabel != nullptr) {
    m_userLabel->setFontSize(Style::fontSizeHeader * centerScale);
  }
  syncAvatar(*renderer, std::clamp(220.0f * centerScale, 120.0f, 220.0f));
  m_loginContentRow->setMinHeight(Style::controlHeightLg);
  m_passwordField->setSurfaceOpacity(1.0f);
  m_passwordField->setFrameRadius(Style::controlHeightLg * 0.5f);
  m_passwordField->setTextAlign(TextAlign::Center);
  m_loginButton->setRadius(Style::controlHeightLg * 0.5f);
  m_loginButton->setSize(Style::controlHeightLg, Style::controlHeightLg);

  const float outerInset = Style::spaceLg;
  float islandHeight = std::min(sh - outerInset * 2.0f, sh * 0.70f);
  float islandWidth = std::min(sw - outerInset * 2.0f, islandHeight * (16.0f / 9.0f));
  const float contentInset = Style::spaceLg * 2.0f;
  const float columnGap = 40.0f;
  const float desiredCenterWidth = 600.0f * centerScale;
  const float minSideWidth = 240.0f;
  const float availableContentWidth = std::max(1.0f, islandWidth - contentInset * 2.0f);
  bool fullLayout = (availableContentWidth - desiredCenterWidth - columnGap * 2.0f) * 0.5f >= minSideWidth;
  if (!fullLayout) {
    islandWidth = std::min(sw - outerInset * 2.0f, desiredCenterWidth + contentInset * 2.0f);
  }
  islandHeight = std::min(islandHeight, sh - outerInset * 2.0f);
  const float islandX = std::round((sw - islandWidth) * 0.5f);
  const float islandY = std::round((sh - islandHeight) * 0.5f);
  m_island->setPosition(islandX, islandY);
  m_island->setSize(islandWidth, islandHeight);
  m_island->setRadius(std::min(42.0f, islandHeight * 0.12f));
  m_island->setOpacity(m_introOpacity);

  const float innerHeight = std::max(1.0f, islandHeight - contentInset * 2.0f);
  const float innerWidth = std::max(1.0f, islandWidth - contentInset * 2.0f);
  const float centerWidth = fullLayout ? desiredCenterWidth : innerWidth;
  const float sideWidth = fullLayout ? std::max(1.0f, (innerWidth - centerWidth - columnGap * 2.0f) * 0.5f) : 0.0f;
  const float centerX = islandX + contentInset + (fullLayout ? sideWidth + columnGap : 0.0f);
  const float centerY = islandY + contentInset;
  m_loginBaseX = centerX;
  m_loginBaseY = centerY;
  m_loginPanel->setOpacity(m_introOpacity);
  m_loginPanel->arrange(*renderer, LayoutRect{centerX, centerY + m_introOffsetY, centerWidth, innerHeight});
  m_leftColumn->setVisible(loginVisible && fullLayout);
  m_rightColumn->setVisible(loginVisible && fullLayout);
  if (fullLayout) {
    m_leftColumn->arrange(*renderer, LayoutRect{islandX + contentInset, centerY, sideWidth, innerHeight});
    m_rightColumn->arrange(
        *renderer, LayoutRect{centerX + centerWidth + columnGap, centerY, sideWidth, innerHeight}
    );
    m_leftColumn->setOpacity(m_introOpacity);
    m_rightColumn->setOpacity(m_introOpacity);
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
    m_timeLabel->setText(formatLocalTime("{:%H:%M}"));
  }
  if (m_dateLabel != nullptr) {
    m_dateLabel->setText(formatLocalTime("{:%A  •  %d %B}"));
  }
  if (m_userLabel != nullptr) {
    m_userLabel->setText(m_user);
  }
  syncDashboardCopy();
  m_passwordField->setValue(m_password);
  m_passwordField->setEnabled(!m_authenticating);
  if (m_loginButton != nullptr) {
    m_loginButton->setEnabled(!m_authenticating);
  }

  if (m_statusLabel != nullptr) {
    bool isError = false;
    const std::string text = resolveStatusText(isError);
    const bool show = m_locked && !m_blackout && !text.empty();
    m_statusLabel->setVisible(show);
    if (show) {
      m_statusLabel->setText(text);
      m_statusLabel->setColor(colorSpecFromRole(isError ? ColorRole::Error : ColorRole::OnSurfaceVariant));
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
  if (m_weatherTitleLabel != nullptr) m_weatherTitleLabel->setText(m_dashboard.weatherTitle);
  if (m_weatherDetailLabel != nullptr) m_weatherDetailLabel->setText(m_dashboard.weatherDetail);
  if (m_fetchLabel != nullptr) m_fetchLabel->setText(m_dashboard.fetch);
  if (m_mediaTitleLabel != nullptr) m_mediaTitleLabel->setText(m_dashboard.mediaTitle);
  if (m_mediaArtistLabel != nullptr) m_mediaArtistLabel->setText(m_dashboard.mediaArtist);
  if (m_cpuLabel != nullptr) m_cpuLabel->setText(m_dashboard.cpu);
  if (m_memoryLabel != nullptr) m_memoryLabel->setText(m_dashboard.memory);
  if (m_storageLabel != nullptr) m_storageLabel->setText(m_dashboard.storage);
  if (m_notificationsLabel != nullptr) {
    m_notificationsLabel->setText(m_dashboard.showNotifications ? m_dashboard.notifications : "Unlock for Notifications");
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
