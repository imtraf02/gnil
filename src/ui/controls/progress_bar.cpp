#include "ui/controls/progress_bar.h"

#include "render/core/render_styles.h"
#include "render/scene/rect_node.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>

ProgressBar::ProgressBar() {
  auto track = std::make_unique<RectNode>();
  m_track = static_cast<RectNode*>(addChild(std::move(track)));

  // The fill is a full-size copy of the track revealed through a clip node, so
  // its rounded (closed) end always matches the track exactly instead of
  // squaring off at small progress values.
  auto fillClip = std::make_unique<Node>();
  fillClip->setClipChildren(true);
  m_fillClip = addChild(std::move(fillClip));

  auto fill = std::make_unique<RectNode>();
  m_fill = static_cast<RectNode*>(m_fillClip->addChild(std::move(fill)));

  setTrack(colorSpecFromRole(ColorRole::SurfaceVariant));
  setFill(colorSpecFromRole(ColorRole::Primary));
  setRadius(Style::scaledRadiusSm());
  m_paletteConn = paletteChanged().connect([this] { applyPalette(); });
}

void ProgressBar::setFill(const ColorSpec& color) { setFillColor(color); }

void ProgressBar::setFillColor(const ColorSpec& color) {
  m_fillColor = color;
  m_fillColor2 = std::nullopt;
  applyPalette();
}

void ProgressBar::setFill(const Color& color) { setFillColor(color); }

void ProgressBar::setFillColor(const Color& color) { setFillColor(fixedColorSpec(color)); }

void ProgressBar::setFillGradient(const ColorSpec& color1, const ColorSpec& color2) {
  m_fillColor = color1;
  m_fillColor2 = color2;
  applyPalette();
}

void ProgressBar::setFillGradient(const Color& color1, const Color& color2) {
  setFillGradient(fixedColorSpec(color1), fixedColorSpec(color2));
}

void ProgressBar::setTrack(const ColorSpec& color) { setTrackColor(color); }

void ProgressBar::setTrackColor(const ColorSpec& color) {
  m_trackColor = color;
  applyPalette();
}

void ProgressBar::setTrack(const Color& color) { setTrackColor(color); }

void ProgressBar::setTrackColor(const Color& color) { setTrackColor(fixedColorSpec(color)); }

void ProgressBar::setRadius(float radius) {
  auto style = m_track->style();
  style.radius = radius;
  m_track->setStyle(style);
  auto fillStyle = m_fill->style();
  fillStyle.radius = radius;
  m_fill->setStyle(fillStyle);
}

void ProgressBar::setSoftness(float softness) {
  auto style = m_track->style();
  style.softness = softness;
  m_track->setStyle(style);
  auto fillStyle = m_fill->style();
  fillStyle.softness = softness;
  m_fill->setStyle(fillStyle);
}

void ProgressBar::setProgress(float progress) {
  m_progress = std::clamp(progress, 0.0f, 1.0f);
  updateGeometry();
}

void ProgressBar::setSize(float w, float h) {
  Node::setSize(w, h);
  updateGeometry();
}

void ProgressBar::setOrientation(ProgressBarOrientation orientation) {
  m_orientation = orientation;
  updateGeometry();
}

void ProgressBar::applyPalette() {
  auto trackStyle = m_track->style();
  trackStyle.fill = resolveColorSpec(m_trackColor);
  trackStyle.fillMode = FillMode::Solid;
  m_track->setStyle(trackStyle);

  auto fillStyle = m_fill->style();
  if (m_fillColor2.has_value()) {
    fillStyle.fillMode = FillMode::LinearGradient;
    fillStyle.gradientDirection = (m_orientation == ProgressBarOrientation::Vertical) ? GradientDirection::Vertical : GradientDirection::Horizontal;
    fillStyle.gradientStops = {
        GradientStop{0.0f, resolveColorSpec(m_fillColor)},
        GradientStop{1.0f, resolveColorSpec(*m_fillColor2)},
        GradientStop{1.0f, resolveColorSpec(*m_fillColor2)},
        GradientStop{1.0f, resolveColorSpec(*m_fillColor2)},
    };
  } else {
    fillStyle.fill = resolveColorSpec(m_fillColor);
    fillStyle.fillMode = FillMode::Solid;
  }
  m_fill->setStyle(fillStyle);
}

void ProgressBar::updateGeometry() {
  const float w = width();
  const float h = height();
  m_track->setFrameSize(w, h);

  if (m_orientation == ProgressBarOrientation::HorizontalCentered) {
    // Both ends are leading edges, so keep the fill's own rounded shape (a
    // shrinking pill) rather than clipping it to flat slice edges.
    const float fillW = w * m_progress;
    m_fillClip->setPosition(0.0f, 0.0f);
    m_fillClip->setFrameSize(w, h);
    m_fill->setFrameSize(fillW, h);
    m_fill->setPosition((w - fillW) * 0.5f, 0.0f);
    return;
  }

  // Anchored fill: reveal a full-size copy of the track through the clip so the
  // rounded closed end always matches the track instead of squaring off.
  m_fill->setFrameSize(w, h);
  float clipX = 0.0f;
  float clipY = 0.0f;
  float clipW = w;
  float clipH = h;
  if (m_orientation == ProgressBarOrientation::Vertical) {
    clipH = h * m_progress;
    clipY = h - clipH;
  } else {
    clipW = w * m_progress;
  }

  m_fillClip->setPosition(clipX, clipY);
  m_fillClip->setFrameSize(clipW, clipH);
  m_fill->setPosition(-clipX, -clipY);
}
