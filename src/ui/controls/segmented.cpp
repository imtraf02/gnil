#include "ui/controls/segmented.h"

#include "render/core/render_styles.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_area.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/roving_list_nav.h"
#include "ui/controls/separator.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>
#include <utility>

Segmented::Segmented() {
  setDirection(FlexDirection::Horizontal);
  setAlign(FlexAlign::Stretch);
  setGap(0.0f);
  applyOuterStyle();

  auto area = std::make_unique<InputArea>();
  area->setFocusable(true);
  area->setHitTestVisible(false);
  m_focusArea = static_cast<InputArea*>(addChild(std::move(area)));
  m_focusArea->setParticipatesInLayout(false);
  m_focusArea->setZIndex(2);

  m_rovingNav.setOptions(
      RovingListNavController::Options{
          .axis = RovingListNavAxis::Horizontal,
          .mode = RovingListNavMode::FollowFocus,
          .scrollIntoView = {},
          .syncIndexFromSelection = [this]() { return m_selected; },
      }
  );
  m_rovingNav.bindFocusArea(m_focusArea);
}

std::size_t Segmented::addOption(std::string_view label) { return addOption(label, std::string_view{}); }

std::size_t Segmented::addOption(std::string_view label, std::string_view glyph) {
  const std::size_t index = m_buttons.size();
  if (index > 0) {
    auto sep = makeSegmentSeparator();
    m_separators.push_back(sep.get());
    addChild(std::move(sep));
  }
  auto btn = makeSegmentButton(label, glyph, index);
  Button* raw = btn.get();
  m_buttons.push_back(raw);
  m_rovingNav.registerItem(raw, [this, index]() { setSelectedIndex(index); });
  addChild(std::move(btn));
  refreshSeparatorVisibility();
  refreshVariants();
  return index;
}

void Segmented::setSelectedIndex(std::size_t index) {
  if (index >= m_buttons.size() || index == m_selected) {
    return;
  }
  m_selected = index;
  refreshVariants();
  m_rovingNav.notifyExternalSelectionChanged();
  if (m_onChange) {
    m_onChange(index);
  }
}

Button* Segmented::optionButton(std::size_t index) const noexcept {
  if (index >= m_buttons.size()) {
    return nullptr;
  }
  return m_buttons[index];
}

void Segmented::setFontSize(float size) {
  m_fontSize = size;
  const float fs = effectiveFontSize();
  for (Button* btn : m_buttons) {
    if (btn != nullptr) {
      btn->setFontSize(fs);
      btn->setGlyphSize(fs);
    }
  }
}

void Segmented::setScale(float scale) {
  m_scale = std::max(0.1f, scale);
  applyOuterStyle();
  const float fs = effectiveFontSize();
  for (Button* btn : m_buttons) {
    if (btn != nullptr) {
      applyButtonMetrics(*btn);
      btn->setFontSize(fs);
      btn->setGlyphSize(fs);
    }
  }
  const float ruleW = std::max(1.0f, Style::borderWidth * m_scale);
  for (Separator* sep : m_separators) {
    if (sep != nullptr) {
      sep->setThickness(ruleW);
    }
  }
  refreshVariants();
  markLayoutDirty();
}

void Segmented::setCompact(bool compact) {
  if (m_compact == compact) {
    return;
  }
  m_compact = compact;
  for (Button* btn : m_buttons) {
    if (btn != nullptr) {
      applyButtonMetrics(*btn);
    }
  }
  markLayoutDirty();
}

void Segmented::setPadding(float padding) {
  m_outerPadding = padding;
  Flex::setPadding(padding);
}

void Segmented::setPadding(float vertical, float horizontal) {
  m_outerPadding = vertical;
  Flex::setPadding(vertical, horizontal);
}

void Segmented::setPadding(float top, float right, float bottom, float left) {
  m_outerPadding = top;
  Flex::setPadding(top, right, bottom, left);
}

void Segmented::setOptionTooltip(std::size_t index, std::string_view text) {
  if (index < m_buttons.size() && m_buttons[index] != nullptr) {
    m_buttons[index]->setTooltip(text);
  }
}

void Segmented::clearOptions() {
  if (m_pressAnimId != 0 && animationManager() != nullptr) {
    animationManager()->cancel(m_pressAnimId);
    m_pressAnimId = 0;
  }
  m_pressProgress = 0.0f;
  m_pressedIndex.reset();
  for (Button* btn : m_buttons) {
    if (btn != nullptr) {
      (void)removeChild(btn);
    }
  }
  for (Separator* sep : m_separators) {
    if (sep != nullptr) {
      (void)removeChild(sep);
    }
  }
  m_buttons.clear();
  m_separators.clear();
  m_rovingNav.clearItems();
  m_selected = 0;
  markLayoutDirty();
}

void Segmented::setOnChange(std::function<void(std::size_t)> callback) { m_onChange = std::move(callback); }

void Segmented::setSurfaceOpacity(float opacity) {
  const float clamped = std::clamp(opacity, 0.0f, 1.0f);
  if (m_surfaceOpacity == clamped) {
    return;
  }
  m_surfaceOpacity = clamped;
  applyOuterStyle();
}

void Segmented::setSurfaceRole(ColorRole role) {
  if (m_surfaceRole == role) {
    return;
  }
  m_surfaceRole = role;
  applyOuterStyle();
}

void Segmented::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  if (!enabled && m_pressedIndex.has_value()) {
    if (m_pressAnimId != 0 && animationManager() != nullptr) {
      animationManager()->cancel(m_pressAnimId);
      m_pressAnimId = 0;
    }
    m_pressProgress = 0.0f;
    m_pressedIndex.reset();
    applyPressedSegment(0.0f);
  }
  m_enabled = enabled;
  for (Button* btn : m_buttons) {
    if (btn != nullptr) {
      btn->setEnabled(enabled);
    }
  }
  setOpacity(enabled ? 1.0f : 0.55f);
}

void Segmented::setPresentation(SegmentedPresentation presentation) {
  if (m_presentation == presentation) {
    return;
  }
  m_presentation = presentation;
  m_underlineSelection = presentation == SegmentedPresentation::Underline;
  if (presentation != SegmentedPresentation::Expressive) {
    if (m_pressAnimId != 0 && animationManager() != nullptr) {
      animationManager()->cancel(m_pressAnimId);
      m_pressAnimId = 0;
    }
    m_pressProgress = 0.0f;
    m_pressedIndex.reset();
  }
  applyOuterStyle();
  refreshSeparatorVisibility();
  refreshVariants();
  markLayoutDirty();
}

std::unique_ptr<Separator> Segmented::makeSegmentSeparator() {
  auto sep = std::make_unique<Separator>();
  sep->setOrientation(SeparatorOrientation::VerticalRule);
  sep->setThickness(std::max(1.0f, Style::borderWidth * m_scale));
  sep->setColor(colorSpecFromRole(ColorRole::Outline));
  sep->setFlexGrow(0.0f);
  return sep;
}

std::unique_ptr<Button>
Segmented::makeSegmentButton(std::string_view label, std::string_view glyph, std::size_t index) {
  auto btn = std::make_unique<Button>();
  if (!glyph.empty()) {
    btn->setGlyph(glyph);
    btn->setGlyphSize(effectiveFontSize());
  }
  if (!label.empty()) {
    btn->setText(label);
    btn->setFontSize(effectiveFontSize());
  }
  applyButtonMetrics(*btn);
  btn->setOnClick([this, index]() { setSelectedIndex(index); });
  btn->setOnPress([this, index](float /*x*/, float /*y*/, bool pressed) {
    animatePressedSegment(index, pressed);
  });
  btn->setTabStop(false);
  btn->setFlexGrow(m_equalSegmentWidths ? 1.0f : 0.0f);
  btn->setContentAlign(ButtonContentAlign::Center);
  btn->setEnabled(m_enabled);
  return btn;
}

void Segmented::applyButtonMetrics(Button& button) const {
  if (m_compact) {
    button.setMinHeight(Style::controlHeightSm * m_scale);
    button.setPadding(Style::spaceXs * m_scale, Style::spaceSm * m_scale);
    return;
  }

  button.setMinHeight(Style::controlHeight * m_scale);
  button.setPadding(Style::spaceXs * m_scale, Style::spaceMd * m_scale);
}

void Segmented::setEqualSegmentWidths(bool equalWidths) {
  if (m_equalSegmentWidths == equalWidths) {
    return;
  }
  m_equalSegmentWidths = equalWidths;
  for (Button* b : m_buttons) {
    if (b != nullptr) {
      b->setFlexGrow(m_equalSegmentWidths ? 1.0f : 0.0f);
    }
  }
  applyPressedSegment(m_pressProgress);
  markLayoutDirty();
}

void Segmented::setUnderlineSelection(bool underline) {
  const auto next = underline ? SegmentedPresentation::Underline : SegmentedPresentation::Joined;
  if (m_presentation == next) {
    return;
  }
  setPresentation(next);
}

void Segmented::setShowSeparators(bool show) {
  if (m_showSeparators == show) {
    return;
  }
  m_showSeparators = show;
  refreshSeparatorVisibility();
}

void Segmented::refreshVariants() {
  const std::size_t n = m_buttons.size();
  const float r = Style::scaledRadiusMd(m_scale);
  const float expressiveRadius = Style::scaledRadiusLg(m_scale);
  for (std::size_t i = 0; i < n; ++i) {
    if (m_buttons[i] == nullptr) {
      continue;
    }
    m_buttons[i]->setVariant(i == m_selected && !m_underlineSelection ? ButtonVariant::TabActive : ButtonVariant::Tab);
    Radii radii;
    if (m_presentation == SegmentedPresentation::Expressive) {
      radii = Radii{expressiveRadius};
    } else if (n == 1) {
      radii = Radii{r, r, r, r};
    } else if (i == 0) {
      radii = Radii{r, 0.0f, 0.0f, r};
    } else if (i == n - 1) {
      radii = Radii{0.0f, r, r, 0.0f};
    } else {
      radii = Radii{0.0f};
    }
    m_buttons[i]->setRadii(radii);
  }
  applyPressedSegment(m_pressProgress);
}

void Segmented::applyOuterStyle() {
  Flex::setPadding(m_outerPadding);
  if (m_presentation == SegmentedPresentation::Expressive) {
    setFill(colorSpecFromRole(m_surfaceRole, 0.0f));
    setGap(5.0f * m_scale);
  } else {
    setFill(colorSpecFromRole(m_surfaceRole, m_surfaceOpacity));
    setGap(0.0f);
  }
  clearBorder();
  setRadius(
      m_presentation == SegmentedPresentation::Expressive ? Style::scaledRadiusLg(m_scale)
                                                          : Style::scaledRadiusMd(m_scale)
  );
}

void Segmented::refreshSeparatorVisibility() {
  const bool visible = m_showSeparators && m_presentation == SegmentedPresentation::Joined;
  for (Separator* separator : m_separators) {
    if (separator != nullptr) {
      separator->setVisible(visible);
      separator->setParticipatesInLayout(visible);
    }
  }
  markLayoutDirty();
}

void Segmented::animatePressedSegment(std::optional<std::size_t> index, bool pressedState) {
  if (m_presentation != SegmentedPresentation::Expressive || !m_enabled) {
    return;
  }
  if (pressedState) {
    m_pressedIndex = index;
  }
  const float target = pressedState ? 1.0f : 0.0f;
  if (m_pressAnimId != 0 && animationManager() != nullptr) {
    animationManager()->cancel(m_pressAnimId);
    m_pressAnimId = 0;
  }
  if (animationManager() == nullptr) {
    m_pressProgress = target;
    applyPressedSegment(target);
    if (!pressedState) {
      m_pressedIndex.reset();
    }
    return;
  }
  m_pressAnimId = animationManager()->animate(
      m_pressProgress, target, static_cast<float>(Style::animNormal), Easing::FluidSpatial,
      [this](float value) {
        m_pressProgress = value;
        applyPressedSegment(value);
      },
      [this, target]() {
        m_pressProgress = target;
        m_pressAnimId = 0;
        applyPressedSegment(target);
        if (target <= 0.0f) {
          m_pressedIndex.reset();
        }
      },
      this
  );
  markPaintDirty();
}

void Segmented::applyPressedSegment(float progress) {
  const std::size_t count = m_buttons.size();
  if (count == 0) {
    return;
  }
  const bool activePress = m_presentation == SegmentedPresentation::Expressive && m_pressedIndex.has_value()
      && *m_pressedIndex < count;
  const std::size_t pressedIndex = activePress ? *m_pressedIndex : 0;
  const float amount = activePress ? std::clamp(progress, 0.0f, 1.0f) : 0.0f;
  const float largeRadius = Style::scaledRadiusLg(m_scale);
  const float smallRadius = Style::scaledRadiusSm(m_scale);

  for (std::size_t i = 0; i < count; ++i) {
    Button* button = m_buttons[i];
    if (button == nullptr) {
      continue;
    }
    float grow = m_equalSegmentWidths ? 1.0f : 0.0f;
    if (m_equalSegmentWidths && activePress && count > 1) {
      if (i == pressedIndex) {
        grow += 0.35f * amount;
      } else {
        const bool leftNeighbor = pressedIndex > 0 && i == pressedIndex - 1;
        const bool rightNeighbor = pressedIndex + 1 < count && i == pressedIndex + 1;
        if (leftNeighbor || rightNeighbor) {
          const bool hasTwoNeighbors = pressedIndex > 0 && pressedIndex + 1 < count;
          grow -= (hasTwoNeighbors ? 0.175f : 0.35f) * amount;
        }
      }
    }
    button->setFlexGrow(m_equalSegmentWidths ? std::max(0.05f, grow) : 0.0f);

    if (m_presentation == SegmentedPresentation::Expressive) {
      const float radius = i == pressedIndex ? largeRadius + (smallRadius - largeRadius) * amount : largeRadius;
      button->setRadii(Radii{radius});
    }
  }
  markLayoutDirty();
}

void Segmented::doLayout(Renderer& renderer) {
  Flex::doLayout(renderer);
  m_rovingNav.layoutOverlay(width(), height());
}

float Segmented::effectiveFontSize() const noexcept {
  return (m_fontSize > 0.0f ? m_fontSize : Style::fontSizeBody) * m_scale;
}
