#include "ui/controls/slider.h"

#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/core/render_styles.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "render/animation/animation_manager.h"
#include "ui/controls/glyph.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/clamp.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <memory>

namespace {

  constexpr double kValueEpsilon = 0.0001;

  RoundedRectStyle solidStyle(const Color& fill, float radius) {
    return RoundedRectStyle{
        .fill = fill,
        .border = fill,
        .fillMode = FillMode::Solid,
        .radius = radius,
        .softness = 1.0f,
        .borderWidth = 0.0f,
    };
  }

  Color resolved(ColorRole role, float alpha = 1.0f) { return colorForRole(role, alpha); }

} // namespace

Slider::Slider() {
  auto track = std::make_unique<RectNode>();
  m_track = static_cast<RectNode*>(addChild(std::move(track)));

  auto fill = std::make_unique<RectNode>();
  m_fill = static_cast<RectNode*>(addChild(std::move(fill)));

  auto thumb = std::make_unique<RectNode>();
  m_thumb = static_cast<RectNode*>(addChild(std::move(thumb)));

  auto stop = std::make_unique<RectNode>();
  m_stop = static_cast<RectNode*>(addChild(std::move(stop)));

  auto glyph = std::make_unique<Glyph>();
  m_glyph = static_cast<Glyph*>(addChild(std::move(glyph)));
  m_glyph->setGlyphSize(m_glyphSizePx);
  m_glyph->setVisible(false);

  auto area = std::make_unique<InputArea>();
  area->setOnEnter([this](const InputArea::PointerData& /*data*/) {
    applyVisualState();
    markPaintDirty();
  });
  area->setOnLeave([this]() {
    applyVisualState();
    markPaintDirty();
  });
  area->setOnPress([this](const InputArea::PointerData& data) {
    if (!m_enabled || data.button != BTN_LEFT) {
      return;
    }
    if (!data.pressed) {
      updateGeometry();
      applyVisualState();
      markPaintDirty();
      if (m_onDragEnd) {
        m_onDragEnd();
      }
      return;
    }
    updateGeometry();
    applyVisualState();
    updateFromLocalX(data.localX);
    markPaintDirty();
  });
  area->setOnMotion([this](const InputArea::PointerData& data) {
    if (!m_enabled || m_inputArea == nullptr || !m_inputArea->pressed()) {
      return;
    }
    updateFromLocalX(data.localX);
  });
  area->setOnAxisHandler([this](const InputArea::PointerData& data) {
    if (!m_enabled || !m_wheelAdjustEnabled || data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return false;
    }
    const auto lines = static_cast<double>(data.scrollSteps());
    if (lines == 0.0) {
      return false;
    }
    // Per-line step: use the slider's snap step, else 5% of range.
    const double step = m_step > 0.0 ? m_step : (m_max - m_min) * 0.05;
    if (step <= 0.0) {
      return false;
    }
    // Wayland convention: positive axisLines = scroll down. Scroll up should increase.
    setValue(m_value - lines * step);
    if (m_onDragEnd) {
      m_onDragEnd();
    }
    return true;
  });
  area->setFocusable(true);
  area->setOnFocusGain([this]() {
    applyVisualState();
    markPaintDirty();
  });
  area->setOnFocusLoss([this]() {
    applyVisualState();
    markPaintDirty();
  });
  area->setOnKeyDown([this](const InputArea::KeyData& key) {
    if (!key.pressed || !m_enabled) {
      return;
    }
    const double step = m_step > 0.0 ? m_step : (m_max - m_min) * 0.05;
    if (step <= 0.0) {
      return;
    }
    if (KeybindMatcher::matches(KeybindAction::Left, key.sym, key.modifiers)) {
      setValue(m_value - step);
      if (m_onDragEnd) {
        m_onDragEnd();
      }
    } else if (KeybindMatcher::matches(KeybindAction::Right, key.sym, key.modifiers)) {
      setValue(m_value + step);
      if (m_onDragEnd) {
        m_onDragEnd();
      }
    } else if (KeySymbol::isPageDown(key.sym)) {
      setValue(m_value - step * 10.0);
      if (m_onDragEnd) {
        m_onDragEnd();
      }
    } else if (KeySymbol::isPageUp(key.sym)) {
      setValue(m_value + step * 10.0);
      if (m_onDragEnd) {
        m_onDragEnd();
      }
    } else if (KeySymbol::isHome(key.sym)) {
      setValue(m_min);
      if (m_onDragEnd) {
        m_onDragEnd();
      }
    } else if (KeySymbol::isEnd(key.sym)) {
      setValue(m_max);
      if (m_onDragEnd) {
        m_onDragEnd();
      }
    }
  });
  m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));
  m_inputArea->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);

  applyVisualState();
}

void Slider::setRange(double minValue, double maxValue) {
  if (maxValue < minValue) {
    std::swap(minValue, maxValue);
  }
  if (m_min == minValue && m_max == maxValue) {
    return;
  }
  m_min = minValue;
  m_max = maxValue;
  const double next = snapped(m_value);
  const bool valueChanged = std::abs(next - m_value) >= kValueEpsilon;
  m_value = next;
  setVisualValue(next, false);
  updateGeometry();
  markPaintDirty();
  if (valueChanged && m_onValueChanged) {
    m_onValueChanged(m_value);
  }
}

void Slider::setStep(double step) {
  m_step = std::max(step, 0.0);
  const double next = snapped(m_value);
  const bool valueChanged = std::abs(next - m_value) >= kValueEpsilon;
  m_value = next;
  setVisualValue(next, false);
  updateGeometry();
  markPaintDirty();
  if (valueChanged && m_onValueChanged) {
    m_onValueChanged(m_value);
  }
}

void Slider::setValue(double value) {
  setValueInternal(value, m_presentation != SliderPresentation::Standard && !dragging());
}

void Slider::setValueInternal(double value, bool animateVisual) {
  const double next = snapped(value);
  if (std::abs(next - m_value) < kValueEpsilon) {
    return;
  }
  m_value = next;
  setVisualValue(next, animateVisual);
  if (m_onValueChanged) {
    m_onValueChanged(m_value);
  }
}

void Slider::setVisualValue(double value, bool animate) {
  if (m_valueAnimId != 0 && animationManager() != nullptr) {
    animationManager()->cancel(m_valueAnimId);
    m_valueAnimId = 0;
  }
  if (!animate || animationManager() == nullptr || std::abs(value - m_visualValue) < kValueEpsilon) {
    m_visualValue = value;
    updateGeometry();
    applyVisualState();
    markPaintDirty();
    return;
  }

  const float from = static_cast<float>(m_visualValue);
  const float to = static_cast<float>(value);
  m_valueAnimId = animationManager()->animate(
      from, to, static_cast<float>(Style::animNormal), Easing::FluidSpatial,
      [this](float current) {
        m_visualValue = current;
        updateGeometry();
        applyVisualState();
        markPaintDirty();
      },
      [this, value]() {
        m_visualValue = value;
        m_valueAnimId = 0;
        updateGeometry();
        applyVisualState();
        markPaintDirty();
      },
      this
  );
  markPaintDirty();
}

void Slider::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  if (m_inputArea != nullptr) {
    m_inputArea->setEnabled(enabled);
  }
  applyVisualState();
  markPaintDirty();
}

void Slider::setPresentation(SliderPresentation presentation) {
  if (m_presentation == presentation) {
    return;
  }
  m_presentation = presentation;
  switch (presentation) {
  case SliderPresentation::Standard:
    m_trackHeight = Style::sliderTrackHeight;
    m_thumbSizePx = Style::sliderThumbSize;
    m_controlHeightPx = Style::controlHeight;
    break;
  case SliderPresentation::LevelCompact:
    m_trackHeight = 18.0f;
    m_thumbSizePx = 33.0f;
    m_controlHeightPx = 33.0f;
    break;
  case SliderPresentation::LevelProminent:
    m_trackHeight = 30.0f;
    m_thumbSizePx = 39.0f;
    m_controlHeightPx = 39.0f;
    break;
  }
  updateGeometry();
  applyVisualState();
  markLayoutDirty();
}

void Slider::setGlyph(std::string glyph) {
  if (m_glyph == nullptr) {
    return;
  }
  const bool visible = !glyph.empty();
  m_hasGlyph = visible;
  if (visible) {
    (void)m_glyph->setGlyph(glyph);
  }
  m_glyph->setVisible(m_hasGlyph && m_presentation != SliderPresentation::Standard);
  updateGeometry();
  markLayoutDirty();
}

void Slider::setGlyphSize(float size) {
  m_glyphSizePx = std::max(1.0f, size);
  if (m_glyph != nullptr) {
    m_glyph->setGlyphSize(m_glyphSizePx);
  }
  updateGeometry();
  markLayoutDirty();
}

void Slider::setTrackHeight(float height) {
  m_trackHeight = std::max(1.0f, height);
  updateGeometry();
  markLayoutDirty();
}

void Slider::setThumbSize(float size) {
  m_thumbSizePx = std::max(1.0f, size);
  updateGeometry();
  markLayoutDirty();
}

void Slider::setControlHeight(float height) {
  m_controlHeightPx = std::max(1.0f, height);
  updateGeometry();
  markLayoutDirty();
}

void Slider::setWheelAdjustEnabled(bool enabled) { m_wheelAdjustEnabled = enabled; }

void Slider::setOnValueChanged(std::function<void(double)> callback) { m_onValueChanged = std::move(callback); }

void Slider::setOnDragEnd(std::function<void()> callback) { m_onDragEnd = std::move(callback); }

bool Slider::dragging() const noexcept { return m_inputArea != nullptr && m_inputArea->pressed(); }

void Slider::doLayout(Renderer& renderer) {
  if (m_glyph != nullptr && m_glyph->visible()) {
    m_glyph->measure(renderer);
  }
  updateGeometry();
  applyVisualState();
}

LayoutSize Slider::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return measureByLayout(renderer, constraints);
}

void Slider::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

void Slider::updateGeometry() {
  const float widthPx = width() > 0.0f ? width() : Style::sliderDefaultWidth;
  const bool level = m_presentation != SliderPresentation::Standard;
  if (m_glyph != nullptr) {
    m_glyph->setVisible(level && m_hasGlyph);
  }

  if (level) {
    const float baseTrackHeight = m_presentation == SliderPresentation::LevelCompact ? 18.0f : 30.0f;
    const float presentationScale = m_trackHeight / baseTrackHeight;
    const bool pressed = m_inputArea != nullptr && m_inputArea->pressed();
    const float handleWidth = (pressed ? 1.5f : 3.0f) * presentationScale;
    const float heightPx = std::max({m_thumbSizePx, m_trackHeight, m_controlHeightPx});
    const float trackY = (heightPx - m_trackHeight) * 0.5f;
    const float handleMargin = 4.0f * presentationScale;
    const float trackX = 0.0f;
    const float effectiveWidth = std::max(0.0f, widthPx - handleMargin * 2.0f);
    const float t = normalizedVisualValue();
    const float thumbX = handleMargin + t * effectiveWidth;
    const float gap = 4.0f * presentationScale;

    setSize(widthPx, heightPx);
    m_fill->setPosition(trackX, trackY);
    m_fill->setFrameSize(std::max(0.0f, thumbX - gap - handleWidth * 0.5f), m_trackHeight);
    m_fill->setVisible(m_fill->width() > 0.0f);

    const float inactiveX = thumbX + gap + handleWidth * 0.5f;
    m_track->setPosition(inactiveX, trackY);
    m_track->setFrameSize(std::max(0.0f, widthPx - inactiveX), m_trackHeight);
    m_track->setVisible(m_track->width() > 0.0f);

    m_thumb->setPosition(thumbX - handleWidth * 0.5f, 0.0f);
    m_thumb->setFrameSize(handleWidth, heightPx);
    m_stop->setVisible(false);

    if (m_glyph != nullptr && m_glyph->visible()) {
      const float glyphW = std::max(m_glyph->width(), m_glyphSizePx);
      const float glyphH = std::max(m_glyph->height(), m_glyphSizePx);
      const float inset = std::max(6.0f, (m_trackHeight - glyphW) * 0.5f);
      float glyphX = widthPx - inset - glyphW;
      if (t >= 0.9f) {
        glyphX = std::max(inset, thumbX - gap - inset - glyphW);
      }
      m_glyph->setPosition(glyphX, (heightPx - glyphH) * 0.5f);
      m_glyph->setFrameSize(glyphW, glyphH);
    }

    m_inputArea->setPosition(0.0f, 0.0f);
    m_inputArea->setFrameSize(widthPx, heightPx);
    return;
  }

  const float handleRestWidth = std::max(Style::sliderHandlePressedWidth, m_thumbSizePx * (4.0f / 44.0f));
  const bool interacting = m_inputArea != nullptr && (m_inputArea->pressed() || m_inputArea->focused());
  const float handleWidth = interacting ? std::max(Style::sliderHandlePressedWidth, handleRestWidth * 0.5f)
                                       : handleRestWidth;
  const float heightPx = std::max({m_thumbSizePx, m_trackHeight, m_controlHeightPx});
  setSize(widthPx, heightPx);

  const float trackY = (heightPx - m_trackHeight) * 0.5f;
  const float trackX = handleRestWidth * 0.5f;
  const float trackW = std::max(0.0f, widthPx - handleRestWidth);
  const float t = normalizedValue();
  const float thumbX = trackX + t * trackW;
  const float thumbY = (heightPx - m_thumbSizePx) * 0.5f;
  const float gap = handleRestWidth * 0.5f + Style::sliderHandleTrackGap;

  // The two segments deliberately stop before the handle, producing the
  // Material 3 handle-to-track gap rather than a bar hidden underneath it.
  m_fill->setPosition(trackX, trackY);
  m_fill->setFrameSize(std::max(0.0f, thumbX - gap - trackX), m_trackHeight);
  m_fill->setVisible(m_fill->width() > 0.0f);

  const float inactiveX = thumbX + gap;
  m_track->setPosition(inactiveX, trackY);
  m_track->setFrameSize(std::max(0.0f, trackX + trackW - inactiveX), m_trackHeight);
  m_track->setVisible(m_track->width() > 0.0f);

  m_thumb->setPosition(util::clampOrdered(thumbX - handleWidth * 0.5f, 0.0f, widthPx - handleWidth), thumbY);
  m_thumb->setFrameSize(handleWidth, m_thumbSizePx);

  const float stopSize = std::min(Style::sliderStopIndicatorSize, m_trackHeight * 0.5f);
  m_stop->setPosition(trackX + trackW - stopSize - m_trackHeight * 0.5f, (heightPx - stopSize) * 0.5f);
  m_stop->setFrameSize(stopSize, stopSize);
  m_stop->setVisible(m_enabled && m_track->visible());

  m_inputArea->setPosition(0.0f, 0.0f);
  m_inputArea->setFrameSize(widthPx, heightPx);
}

void Slider::updateFromLocalX(float x) {
  const float widthPx = width() > 0.0f ? width() : Style::sliderDefaultWidth;
  if (m_presentation != SliderPresentation::Standard) {
    const float baseTrackHeight = m_presentation == SliderPresentation::LevelCompact ? 18.0f : 30.0f;
    const float handleMargin = 4.0f * (m_trackHeight / baseTrackHeight);
    const float effectiveWidth = widthPx - handleMargin * 2.0f;
    if (effectiveWidth <= 0.0f) {
      return;
    }
    const double t = static_cast<double>(std::clamp((x - handleMargin) / effectiveWidth, 0.0f, 1.0f));
    setValueInternal(m_min + t * (m_max - m_min), false);
    return;
  }
  const float handleRestWidth = std::max(Style::sliderHandlePressedWidth, m_thumbSizePx * (4.0f / 44.0f));
  const float trackX = handleRestWidth * 0.5f;
  const float trackW = std::max(0.0f, widthPx - handleRestWidth);
  if (trackW <= 0.0f) {
    return;
  }
  const double t = static_cast<double>(std::clamp((x - trackX) / trackW, 0.0f, 1.0f));
  setValueInternal(m_min + t * (m_max - m_min), false);
}

void Slider::applyVisualState() {
  const bool hovering = m_inputArea != nullptr && m_inputArea->hovered();
  const bool focused = m_inputArea != nullptr && m_inputArea->focused();
  const bool level = m_presentation != SliderPresentation::Standard;

  Color trackColor = level
      ? lerpColor(
            resolved(ColorRole::SurfaceVariant), resolved(ColorRole::Secondary), isResolvedLightTheme() ? 0.16f : 0.22f
        )
      : resolved(ColorRole::SurfaceVariant);
  Color fillColor = resolved(ColorRole::Primary);
  Color thumbColor = resolved(ColorRole::Primary);
  Color thumbBorder = thumbColor;

  m_thumb->setVisible(true);

  if (!m_enabled) {
    trackColor = resolved(ColorRole::OnSurface, 0.12f);
    fillColor = resolved(ColorRole::OnSurface, 0.38f);
    thumbColor = resolved(ColorRole::OnSurface, 0.38f);
    thumbBorder = thumbColor;
  } else if (focused) {
    thumbBorder = resolveColorSpec(focusRingColorSpec());
  } else if (hovering) {
    thumbBorder = resolved(ColorRole::Hover);
  }

  const float trackRadius = m_presentation == SliderPresentation::LevelCompact ? m_trackHeight / 3.0f
      : m_presentation == SliderPresentation::LevelProminent                 ? m_trackHeight * 0.3f
                                                                              : Style::sliderInsideCornerRadius;
  const float baseTrackHeight = m_presentation == SliderPresentation::LevelCompact ? 18.0f : 30.0f;
  const float insideRadius = m_presentation == SliderPresentation::Standard
      ? Style::sliderInsideCornerRadius
      : 2.0f * (m_trackHeight / baseTrackHeight);
  auto trackStyle = solidStyle(trackColor, trackRadius);
  trackStyle.radius = m_presentation == SliderPresentation::Standard
      ? Radii{trackRadius}
      : Radii{insideRadius, trackRadius, trackRadius, insideRadius};
  m_track->setStyle(trackStyle);

  auto fillStyle = solidStyle(fillColor, trackRadius);
  fillStyle.radius = m_presentation == SliderPresentation::Standard
      ? Radii{trackRadius}
      : Radii{trackRadius, insideRadius, insideRadius, trackRadius};
  m_fill->setStyle(fillStyle);

  auto thumbStyle = solidStyle(thumbColor, m_thumb->width() * 0.5f);
  thumbStyle.border = thumbBorder;
  thumbStyle.borderWidth = focused ? Style::focusRingWidth : 0.0f;
  m_thumb->setStyle(thumbStyle);
  m_stop->setStyle(solidStyle(fillColor, m_stop->width() * 0.5f));
  if (m_glyph != nullptr && m_glyph->visible()) {
    const float glyphCenter = m_glyph->x() + m_glyph->width() * 0.5f;
    const float fillEnd = m_fill->x() + m_fill->width();
    m_glyph->setColor(colorSpecFromRole(glyphCenter <= fillEnd ? ColorRole::OnPrimary : ColorRole::OnSurfaceVariant));
  }
}

float Slider::normalizedValue() const noexcept {
  if (m_max <= m_min) {
    return 0.0f;
  }
  return static_cast<float>(std::clamp((m_value - m_min) / (m_max - m_min), 0.0, 1.0));
}

float Slider::normalizedVisualValue() const noexcept {
  if (m_max <= m_min) {
    return 0.0f;
  }
  return static_cast<float>(std::clamp((m_visualValue - m_min) / (m_max - m_min), 0.0, 1.0));
}

double Slider::snapped(double value) const noexcept {
  const double clamped = std::clamp(value, m_min, m_max);
  if (m_step <= 0.0 || m_max <= m_min) {
    return clamped;
  }

  const double steps = std::round((clamped - m_min) / m_step);
  return std::clamp(m_min + steps * m_step, m_min, m_max);
}
