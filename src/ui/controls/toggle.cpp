#include "ui/controls/toggle.h"

#include "core/input/keybind_matcher.h"
#include "render/animation/animation_manager.h"
#include "render/core/color.h"
#include "render/core/render_styles.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>

Toggle::Toggle() {
  setAlign(FlexAlign::Center);
  setDirection(FlexDirection::Horizontal);
  setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);

  auto thumb = std::make_unique<RectNode>();
  m_thumb = static_cast<RectNode*>(addChild(std::move(thumb)));

  auto area = std::make_unique<InputArea>();
  area->setOnEnter([this](const InputArea::PointerData& /*data*/) { applyAnimatedState(m_animationProgress); });
  area->setOnLeave([this]() { applyAnimatedState(m_animationProgress); });
  area->setOnPress([this](const InputArea::PointerData& data) {
    animatePressState(m_enabled && data.pressed);
  });
  area->setOnClick([this](const InputArea::PointerData& /*data*/) {
    if (!m_enabled) {
      return;
    }
    activateFromInput();
  });
  area->setFocusable(true);
  area->setOnFocusGain([this]() { applyAnimatedState(m_animationProgress); });
  area->setOnFocusLoss([this]() { applyAnimatedState(m_animationProgress); });
  area->setOnKeyDown([this](const InputArea::KeyData& key) {
    if (!key.pressed || !m_enabled) {
      return;
    }
    if (KeybindMatcher::matches(KeybindAction::Validate, key.sym, key.modifiers)) {
      activateFromInput();
    }
  });
  m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));
  m_inputArea->setParticipatesInLayout(false);
  m_inputArea->setZIndex(1);

  applySize();
  applyAnimatedState(m_animationProgress);
  m_paletteConn = paletteChanged().connect([this] { applyAnimatedState(m_animationProgress); });
}

void Toggle::setChecked(bool checked) {
  if (m_checked == checked) {
    return;
  }
  m_checked = checked;

  if (animationManager() != nullptr) {
    if (m_animId != 0) {
      animationManager()->cancel(m_animId);
    }
    float from = m_animationProgress;
    float to = m_checked ? 1.0f : 0.0f;
    m_animId = animationManager()->animate(
        from, to, Style::animNormal, Easing::FluidSpatial, [this](float t) { applyAnimatedState(t); },
        [this]() { m_animId = 0; }, this
    );
    // Mark dirty so the surface's frame loop restarts and ticks the animation
    markPaintDirty();
  } else {
    applyState();
  }
}

void Toggle::setCheckedImmediate(bool checked) {
  if (m_checked == checked) {
    return;
  }
  m_checked = checked;
  if (m_animId != 0 && animationManager() != nullptr) {
    animationManager()->cancel(m_animId);
    m_animId = 0;
  }
  applyState();
}

void Toggle::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  if (!enabled) {
    animatePressState(false);
  }
  if (m_inputArea != nullptr) {
    m_inputArea->setEnabled(enabled);
  }
  applyAnimatedState(m_animationProgress);
}

void Toggle::setToggleSize(ToggleSize size) {
  if (m_size == size) {
    return;
  }
  m_size = size;
  applySize();
  applyAnimatedState(m_animationProgress);
}

void Toggle::setScale(float scale) {
  m_scale = std::max(0.1f, scale);
  applySize();
  applyAnimatedState(m_animationProgress);
  markLayoutDirty();
}

void Toggle::setOnChange(std::function<void(bool)> callback) { m_onChange = std::move(callback); }

void Toggle::setTabFocusKey(std::string key) {
  if (m_inputArea != nullptr) {
    m_inputArea->setTabFocusKey(std::move(key));
  }
}

void Toggle::activateFromInput() {
  const bool next = !m_checked;
  setChecked(next);
  if (m_onChange) {
    m_onChange(next);
  }
}

bool Toggle::hovered() const noexcept { return m_inputArea != nullptr && m_inputArea->hovered(); }

bool Toggle::pressed() const noexcept { return m_inputArea != nullptr && m_inputArea->pressed(); }

void Toggle::doLayout(Renderer& renderer) {
  Flex::doLayout(renderer);

  if (m_inputArea != nullptr) {
    m_inputArea->setPosition(0.0f, 0.0f);
    m_inputArea->setFrameSize(width(), height());
  }
}

LayoutSize Toggle::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return measureByLayout(renderer, constraints);
}

void Toggle::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

void Toggle::applySize() {
  switch (m_size) {
  case ToggleSize::Small:
    m_thumbSize = Style::toggleThumbSizeSm * m_scale;
    m_inset = Style::toggleInsetSm * m_scale;
    m_travel = Style::toggleTravelSm * m_scale;
    break;
  case ToggleSize::Medium:
    m_thumbSize = Style::toggleThumbSizeMd * m_scale;
    m_inset = Style::toggleInsetMd * m_scale;
    m_travel = Style::toggleTravelMd * m_scale;
    break;
  case ToggleSize::Large:
    m_thumbSize = Style::toggleThumbSizeLg * m_scale;
    m_inset = Style::toggleInsetLg * m_scale;
    m_travel = Style::toggleTravelLg * m_scale;
    break;
  }

  setRadius((m_thumbSize + (m_inset * 2.0f)) * 0.5f);
}

void Toggle::applyState() { applyAnimatedState(m_checked ? 1.0f : 0.0f); }

void Toggle::applyAnimatedState(float t) {
  m_animationProgress = t;
  const Color trackColor = lerpColor(colorForRole(ColorRole::Outline), colorForRole(ColorRole::Primary), t);
  const Color thumbColor = lerpColor(colorForRole(ColorRole::Surface), colorForRole(ColorRole::OnPrimary), t);
  const float trackHeight = m_thumbSize + m_inset * 2.0f;
  const float trackWidth = trackHeight + m_travel;
  const float offSize = m_thumbSize * (2.0f / 3.0f);
  const float checkedSize = m_thumbSize;
  const float pressedSize = m_thumbSize * (7.0f / 6.0f);
  const float baseSize = offSize + (checkedSize - offSize) * t;
  const float visualSize = baseSize + (pressedSize - baseSize) * m_pressProgress;
  const float centerX = trackHeight * 0.5f + m_travel * t;
  const float thumbX = centerX - visualSize * 0.5f;
  const float thumbY = (trackHeight - visualSize) * 0.5f;
  ColorSpec borderColor = colorSpecFromRole(ColorRole::Outline);

  if (m_enabled) {
    if (m_inputArea != nullptr && m_inputArea->focused()) {
      borderColor = focusRingColorSpec();
    } else if (hovered()) {
      borderColor = colorSpecFromRole(ColorRole::Hover);
    } else if (t >= 0.5f) {
      borderColor = colorSpecFromRole(ColorRole::Primary);
    }
  }

  setFill(trackColor);
  setBorder(borderColor, m_inputArea != nullptr && m_inputArea->focused() ? Style::focusRingWidth : Style::borderWidth);
  m_thumb->setPosition(thumbX, thumbY);
  m_thumb->setFrameSize(visualSize, visualSize);

  auto thumbStyle = m_thumb->style();
  thumbStyle.fillMode = FillMode::Solid;
  thumbStyle.radius = visualSize * 0.5f;
  thumbStyle.softness = 1.0f;
  thumbStyle.borderWidth = 0.0f;
  thumbStyle.fill = thumbColor;
  m_thumb->setStyle(thumbStyle);

  // Padding keeps Flex size consistent regardless of thumb position
  const float rightPad = std::max(0.0f, trackWidth - thumbX - visualSize);
  setPadding(thumbY, rightPad, thumbY, thumbX);

  if (m_enabled) {
    setOpacity(1.0f);
  } else {
    setOpacity(0.55f);
  }
}

void Toggle::animatePressState(bool pressedState) {
  const float target = pressedState ? 1.0f : 0.0f;
  if (m_pressAnimId != 0 && animationManager() != nullptr) {
    animationManager()->cancel(m_pressAnimId);
    m_pressAnimId = 0;
  }
  if (animationManager() == nullptr) {
    m_pressProgress = target;
    applyAnimatedState(m_animationProgress);
    return;
  }
  m_pressAnimId = animationManager()->animate(
      m_pressProgress, target, static_cast<float>(Style::animNormal), Easing::FluidSpatial,
      [this](float value) {
        m_pressProgress = value;
        applyAnimatedState(m_animationProgress);
      },
      [this, target]() {
        m_pressProgress = target;
        m_pressAnimId = 0;
        applyAnimatedState(m_animationProgress);
      },
      this
  );
  markPaintDirty();
}
