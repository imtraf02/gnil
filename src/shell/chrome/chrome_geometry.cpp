#include "shell/chrome/chrome_geometry.h"

#include <algorithm>
#include <cmath>

namespace {

  float interpolate(float from, float to, float t) noexcept { return from + (to - from) * t; }

  ChromeRect interpolate(const ChromeRect& from, const ChromeRect& to, float t) noexcept {
    return {
        .x = interpolate(from.x, to.x, t),
        .y = interpolate(from.y, to.y, t),
        .width = std::max(1.0f, interpolate(from.width, to.width, t)),
        .height = std::max(1.0f, interpolate(from.height, to.height, t)),
    };
  }

  ChromePoint interpolate(const ChromePoint& from, const ChromePoint& to, float t) noexcept {
    return {.x = interpolate(from.x, to.x, t), .y = interpolate(from.y, to.y, t)};
  }

  std::uint8_t edgeBit(ChromeEdge edge) noexcept {
    switch (edge) {
    case ChromeEdge::Top:
      return ChromeJoinEdge::Top;
    case ChromeEdge::Right:
      return ChromeJoinEdge::Right;
    case ChromeEdge::Bottom:
      return ChromeJoinEdge::Bottom;
    case ChromeEdge::Left:
      return ChromeJoinEdge::Left;
    case ChromeEdge::None:
      return ChromeJoinEdge::None;
    }
    return ChromeJoinEdge::None;
  }

} // namespace

ChromePanelShape chromeResolvePanelShape(const ChromePanelState& panel) noexcept {
  const auto joined = [edges = panel.joinedEdges](std::uint8_t edge) { return (edges & edge) != 0; };
  const bool top = joined(ChromeJoinEdge::Top);
  const bool right = joined(ChromeJoinEdge::Right);
  const bool bottom = joined(ChromeJoinEdge::Bottom);
  const bool left = joined(ChromeJoinEdge::Left);
  const bool verticalPanel = panel.edge == ChromeEdge::Left || panel.edge == ChromeEdge::Right;

  // At a screen corner, left/right panels keep the vertical join and
  // top/bottom panels keep the horizontal join. Single-edge contacts have no
  // ambiguity.
  const auto cornerMode = [verticalPanel](bool horizontalJoin, bool verticalJoin) {
    if (horizontalJoin && verticalJoin) {
      return verticalPanel ? ChromeCornerMode::ConcaveVertical : ChromeCornerMode::ConcaveHorizontal;
    }
    if (horizontalJoin) return ChromeCornerMode::ConcaveHorizontal;
    if (verticalJoin) return ChromeCornerMode::ConcaveVertical;
    return ChromeCornerMode::Convex;
  };

  ChromePanelShape shape{
      .visualRect = panel.rect,
      .corners = ChromeCornerModes{
          .topLeft = cornerMode(top, left),
          .topRight = cornerMode(top, right),
          .bottomRight = cornerMode(bottom, right),
          .bottomLeft = cornerMode(bottom, left),
      },
  };
  // Flatten the radius before expanding the visual bounds; otherwise a short
  // panel would reserve wings larger than the radius the shader can draw.
  const float radius = std::min(
      std::max(1.0f, panel.radius),
      std::max(1.0f, std::min(panel.rect.width, panel.rect.height) * 0.5f)
  );
  shape.effectiveRadius = radius;
  const auto growHorizontal = [&](ChromeCornerMode mode, bool leftSide) {
    if (mode != ChromeCornerMode::ConcaveHorizontal) return;
    if (leftSide) shape.bodyInsets.left = radius;
    else shape.bodyInsets.right = radius;
  };
  const auto growVertical = [&](ChromeCornerMode mode, bool topSide) {
    if (mode != ChromeCornerMode::ConcaveVertical) return;
    if (topSide) shape.bodyInsets.top = radius;
    else shape.bodyInsets.bottom = radius;
  };
  growHorizontal(shape.corners.topLeft, true);
  growHorizontal(shape.corners.bottomLeft, true);
  growHorizontal(shape.corners.topRight, false);
  growHorizontal(shape.corners.bottomRight, false);
  growVertical(shape.corners.topLeft, true);
  growVertical(shape.corners.topRight, true);
  growVertical(shape.corners.bottomLeft, false);
  growVertical(shape.corners.bottomRight, false);

  shape.visualRect.x -= shape.bodyInsets.left;
  shape.visualRect.y -= shape.bodyInsets.top;
  shape.visualRect.width += shape.bodyInsets.left + shape.bodyInsets.right;
  shape.visualRect.height += shape.bodyInsets.top + shape.bodyInsets.bottom;

  // The frame and panel are rasterized from one SDF, but two primitives that
  // merely touch still meet exactly on the antialiasing threshold. Give only
  // the painted panel primitive a one-logical-pixel contact overlap so the
  // union stays fully covered. Logical/input geometry remains panel.rect.
  constexpr float contactOverlap = 1.0f;
  if (left) {
    shape.visualRect.x -= contactOverlap;
    shape.visualRect.width += contactOverlap;
  }
  if (right) {
    shape.visualRect.width += contactOverlap;
  }
  if (top) {
    shape.visualRect.y -= contactOverlap;
    shape.visualRect.height += contactOverlap;
  }
  if (bottom) {
    shape.visualRect.height += contactOverlap;
  }
  return shape;
}

ChromeInsets ChromeGeometryModel::resolvedApertureInsets() const noexcept {
  const float frame = std::max(0.0f, m_settings.frameThickness);
  const auto resolve = [frame](float value) { return value < 0.0f ? frame : std::max(0.0f, value); };
  return {
      .left = resolve(m_settings.apertureInsets.left),
      .top = resolve(m_settings.apertureInsets.top),
      .right = resolve(m_settings.apertureInsets.right),
      .bottom = resolve(m_settings.apertureInsets.bottom),
  };
}

ChromeRect ChromeGeometryModel::innerFrame(float outputWidth, float outputHeight) const noexcept {
  const ChromeInsets insets = resolvedApertureInsets();
  return {
      .x = insets.left,
      .y = insets.top,
      .width = std::max(1.0f, outputWidth - insets.left - insets.right),
      .height = std::max(1.0f, outputHeight - insets.top - insets.bottom),
  };
}

ChromeRect ChromeGeometryModel::clampPanel(
    ChromeRect panel, ChromeEdge edge, float outputWidth, float outputHeight
) const noexcept {
  const ChromeRect inner = innerFrame(outputWidth, outputHeight);
  panel.width = std::clamp(panel.width, 1.0f, inner.width);
  panel.height = std::clamp(panel.height, 1.0f, inner.height);
  panel.x = std::clamp(panel.x, inner.x, inner.right() - panel.width);
  panel.y = std::clamp(panel.y, inner.y, inner.bottom() - panel.height);

  switch (edge) {
  case ChromeEdge::Top:
    panel.y = inner.y;
    break;
  case ChromeEdge::Right:
    panel.x = inner.right() - panel.width;
    break;
  case ChromeEdge::Bottom:
    panel.y = inner.bottom() - panel.height;
    break;
  case ChromeEdge::Left:
    panel.x = inner.x;
    break;
  case ChromeEdge::None:
    break;
  }
  return panel;
}

ChromePanelState ChromeGeometryModel::resolveAnchoredPanel(
    ChromePanelState panel, const ChromeRect& barRect, float outputWidth, float outputHeight
) const noexcept {
  if (!panel.hasTriggerAnchor || panel.edge == ChromeEdge::None) {
    panel.rect = clampPanel(panel.rect, panel.edge, outputWidth, outputHeight);
    panel.connector = {};
    panel.connectorVisible = false;
    panel.joinedEdges = ChromeJoinEdge::None;
    return panel;
  }

  const ChromeRect inner = innerFrame(outputWidth, outputHeight);
  panel.rect.width = std::clamp(panel.rect.width, 1.0f, inner.width);
  panel.rect.height = std::clamp(panel.rect.height, 1.0f, inner.height);

  const bool vertical = panel.edge == ChromeEdge::Left || panel.edge == ChromeEdge::Right;
  const float bodyExtent = vertical ? panel.rect.height : panel.rect.width;
  const float innerStart = vertical ? inner.y : inner.x;
  const float innerEnd = vertical ? inner.bottom() : inner.right();
  const float anchor = std::clamp(vertical ? panel.triggerAnchor.y : panel.triggerAnchor.x, innerStart, innerEnd);
  const float safeShoulder = std::min(bodyExtent * 0.5f, std::max(1.0f, panel.radius));
  const float outputMin = innerStart;
  const float outputMax = std::max(outputMin, innerEnd - bodyExtent);
  const float anchorMin = anchor - (bodyExtent - safeShoulder);
  const float anchorMax = anchor - safeShoulder;
  const float feasibleMin = std::max(outputMin, anchorMin);
  const float feasibleMax = std::min(outputMax, anchorMax);
  const float desired = anchor - bodyExtent * 0.5f;
  const float bodyStart = feasibleMin <= feasibleMax ? std::clamp(desired, feasibleMin, feasibleMax)
                                                     : std::clamp(desired, outputMin, outputMax);

  if (vertical) {
    panel.rect.y = bodyStart;
    panel.rect.x = std::clamp(panel.rect.x, inner.x, inner.right() - panel.rect.width);
  } else {
    panel.rect.x = bodyStart;
    panel.rect.y = std::clamp(panel.rect.y, inner.y, inner.bottom() - panel.rect.height);
  }

  // The connector overlaps both primitives.  It normally disappears inside
  // the already-overlapping rail/body union, but becomes the stable neck when
  // clamping would otherwise make the perceived origin drift away from the
  // clicked icon.
  const float connectorSpan =
      std::min(bodyExtent, std::max(m_settings.rounding * 2.0f, m_settings.frameThickness * 2.0f));
  constexpr float overlap = 1.0f;
  if (vertical) {
    const float bodyNear = panel.edge == ChromeEdge::Left ? panel.rect.x : panel.rect.right();
    const float barNear = panel.edge == ChromeEdge::Left ? barRect.right() : barRect.x;
    if (std::abs(bodyNear - barNear) <= overlap) {
      panel.connector = {};
      panel.connectorVisible = false;
      panel.joinedEdges = ChromeJoinEdge::None;
      return panel;
    }
    const float start = std::min(bodyNear, barNear) - overlap;
    const float end = std::max(bodyNear, barNear) + overlap;
    panel.connector = {
        .x = start,
        .y = anchor - connectorSpan * 0.5f,
        .width = std::max(1.0f, end - start),
        .height = connectorSpan,
    };
  } else {
    const float bodyNear = panel.edge == ChromeEdge::Top ? panel.rect.y : panel.rect.bottom();
    const float barNear = panel.edge == ChromeEdge::Top ? barRect.bottom() : barRect.y;
    if (std::abs(bodyNear - barNear) <= overlap) {
      panel.connector = {};
      panel.connectorVisible = false;
      panel.joinedEdges = ChromeJoinEdge::None;
      return panel;
    }
    const float start = std::min(bodyNear, barNear) - overlap;
    const float end = std::max(bodyNear, barNear) + overlap;
    panel.connector = {
        .x = anchor - connectorSpan * 0.5f,
        .y = start,
        .width = connectorSpan,
        .height = std::max(1.0f, end - start),
    };
  }
  panel.connectorVisible = panel.connector.width > 0.0f && panel.connector.height > 0.0f;
  panel.joinedEdges = ChromeJoinEdge::None;
  return panel;
}

ChromePanelState ChromeGeometryModel::resolveJoinedShape(
    ChromePanelState panel, float outputWidth, float outputHeight
) const noexcept {
  const ChromeRect inner = innerFrame(outputWidth, outputHeight);
  constexpr float contactEpsilon = 0.5f;

  std::uint8_t joined = panel.attached ? edgeBit(panel.edge) : ChromeJoinEdge::None;
  // An icon-anchored bar popout has exactly one physical origin: its bar
  // edge. A tall dynamic panel may be clamped against another frame edge, but
  // that incidental contact must keep a normal convex corner instead of
  // becoming another blob attachment. Non-anchored chrome such as the
  // top-right toast and fixed frame panels may intentionally join every edge
  // they touch.
  if (!panel.hasTriggerAnchor) {
    if (std::abs(panel.rect.y - inner.y) <= contactEpsilon) {
      joined |= ChromeJoinEdge::Top;
    }
    if (std::abs(panel.rect.right() - inner.right()) <= contactEpsilon) {
      joined |= ChromeJoinEdge::Right;
    }
    if (std::abs(panel.rect.bottom() - inner.bottom()) <= contactEpsilon) {
      joined |= ChromeJoinEdge::Bottom;
    }
    if (std::abs(panel.rect.x - inner.x) <= contactEpsilon) {
      joined |= ChromeJoinEdge::Left;
    }
  }

  // Keep logical/input geometry unchanged. ChromeOutputHost consumes these
  // contact bits only for its render-only underlap and frame sink.
  panel.joinedEdges = joined;
  return panel;
}

std::array<std::int32_t, 4>
ChromeGeometryModel::exclusiveZones(ChromeEdge barEdge, float barThickness, bool barVisible) const noexcept {
  const auto frame = static_cast<std::int32_t>(std::lround(std::max(0.0f, m_settings.frameThickness)));
  std::array<std::int32_t, 4> zones = {frame, frame, frame, frame}; // left, top, right, bottom
  if (!barVisible) {
    return zones;
  }
  // The rail replaces the frame on its own edge, so its thickness is already
  // the complete reservation there (rather than frame + bar).
  const auto bar = std::max(frame, static_cast<std::int32_t>(std::lround(std::max(0.0f, barThickness))));
  switch (barEdge) {
  case ChromeEdge::Left:
    zones[0] = bar;
    break;
  case ChromeEdge::Top:
    zones[1] = bar;
    break;
  case ChromeEdge::Right:
    zones[2] = bar;
    break;
  case ChromeEdge::Bottom:
    zones[3] = bar;
    break;
  case ChromeEdge::None:
    break;
  }
  return zones;
}

std::int32_t ChromeGeometryModel::physicalFrameThickness(float scale) const noexcept {
  return static_cast<std::int32_t>(std::lround(std::max(0.0f, m_settings.frameThickness) * std::max(0.0f, scale)));
}

void ChromeTransitionState::reset(const ChromePanelState& state) noexcept {
  m_from = state;
  m_target = state;
  m_displayed = state;
}

void ChromeTransitionState::setTarget(const ChromePanelState& state) noexcept {
  // A reversal starts from the actual currently displayed state, never from a
  // reconstructed endpoint. This is what prevents a square flash/teleport.
  m_from = m_displayed;
  m_target = state;
  if (!state.visible) {
    m_displayed.inputEnabled = false;
  }
}

ChromePanelState ChromeTransitionState::sample(float easedProgress, float deformScale) noexcept {
  const float coverage = std::clamp(easedProgress, 0.0f, 1.0f);
  m_displayed = m_target;
  // Geometry is a physical/input boundary. Even if a caller deliberately uses
  // an overshooting effects curve, it must not grow past either endpoint.
  m_displayed.rect = interpolate(m_from.rect, m_target.rect, coverage);
  m_displayed.triggerAnchor = interpolate(m_from.triggerAnchor, m_target.triggerAnchor, coverage);
  m_displayed.connector = interpolate(m_from.connector, m_target.connector, coverage);
  m_displayed.radius = std::max(1.0f, interpolate(m_from.radius, m_target.radius, coverage));
  m_displayed.opacity = std::clamp(interpolate(m_from.opacity, m_target.opacity, coverage), 0.0f, 1.0f);
  m_displayed.progress = std::clamp(interpolate(m_from.progress, m_target.progress, coverage), 0.0f, 1.0f);
  const float travel = std::hypot(m_target.rect.x - m_from.rect.x, m_target.rect.y - m_from.rect.y);
  const float normalizedTravel = travel / std::max(1.0f, std::max(m_target.rect.width, m_target.rect.height));
  const float impulse = std::sin(coverage * 3.14159265358979323846f) * normalizedTravel * deformScale;
  m_displayed.deformation =
      std::clamp(interpolate(m_from.deformation, m_target.deformation, coverage) + impulse, -0.12f, 0.12f);
  m_displayed.visible = m_target.visible || coverage < 1.0f;
  m_displayed.inputEnabled = m_target.inputEnabled && m_target.visible && coverage > 0.001f;
  return m_displayed;
}

float chromeRoundedRectDistance(float px, float py, const ChromeRect& rect, float radius) noexcept {
  const float safeRadius = std::clamp(radius, 1.0f, std::max(1.0f, std::min(rect.width, rect.height) * 0.5f));
  const float qx = std::abs(px - (rect.x + rect.width * 0.5f)) - (rect.width * 0.5f - safeRadius);
  const float qy = std::abs(py - (rect.y + rect.height * 0.5f)) - (rect.height * 0.5f - safeRadius);
  return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - safeRadius;
}

float chromeSmoothUnion(float a, float b, float smoothing) noexcept {
  if (smoothing <= 0.001f) {
    return std::min(a, b);
  }
  const float h = std::clamp(0.5f + 0.5f * (b - a) / smoothing, 0.0f, 1.0f);
  return std::lerp(b, a, h) - smoothing * h * (1.0f - h);
}

float chromeCircularSmoothUnion(float a, float b, float smoothing) noexcept {
  if (smoothing <= 0.001f) {
    return std::min(a, b);
  }
  const float supportA = std::max(0.0f, smoothing - a);
  const float supportB = std::max(0.0f, smoothing - b);
  return std::max(smoothing, std::min(a, b)) - std::hypot(supportA, supportB);
}

ChromeRect chromeJoinedRenderRect(const ChromeRect& rect, std::uint8_t joinedEdges) noexcept {
  ChromeRect rendered = rect;
  const float horizontalUnderlap = rect.width * 0.2f;
  const float verticalUnderlap = rect.height * 0.2f;
  if ((joinedEdges & ChromeJoinEdge::Top) != 0) {
    rendered.y -= verticalUnderlap;
    rendered.height += verticalUnderlap;
  }
  if ((joinedEdges & ChromeJoinEdge::Right) != 0) {
    rendered.width += horizontalUnderlap;
  }
  if ((joinedEdges & ChromeJoinEdge::Bottom) != 0) {
    rendered.height += verticalUnderlap;
  }
  if ((joinedEdges & ChromeJoinEdge::Left) != 0) {
    rendered.x -= horizontalUnderlap;
    rendered.width += horizontalUnderlap;
  }
  return rendered;
}

ChromeRect chromePlaceAttachedBody(const ChromeRect& panel, const ChromeRect& bar, ChromeEdge edge) noexcept {
  ChromeRect placed = panel;
  switch (edge) {
  case ChromeEdge::Top:
    placed.y = bar.bottom();
    break;
  case ChromeEdge::Right:
    placed.x = bar.x - placed.width;
    break;
  case ChromeEdge::Bottom:
    placed.y = bar.y - placed.height;
    break;
  case ChromeEdge::Left:
    placed.x = bar.right();
    break;
  case ChromeEdge::None:
    break;
  }
  return placed;
}

ChromeRect chromeAnchoredRevealRect(const ChromeRect& openBody, ChromeEdge edge, float progress) noexcept {
  const float coverage = std::clamp(progress, 0.0f, 1.0f);
  ChromeRect revealed = openBody;
  switch (edge) {
  case ChromeEdge::Top:
    revealed.height = std::max(1.0f, openBody.height * coverage);
    break;
  case ChromeEdge::Right:
    revealed.width = std::max(1.0f, openBody.width * coverage);
    revealed.x = openBody.right() - revealed.width;
    break;
  case ChromeEdge::Bottom:
    revealed.height = std::max(1.0f, openBody.height * coverage);
    revealed.y = openBody.bottom() - revealed.height;
    break;
  case ChromeEdge::Left:
    revealed.width = std::max(1.0f, openBody.width * coverage);
    break;
  case ChromeEdge::None:
    revealed.width = std::max(1.0f, openBody.width * coverage);
    revealed.height = std::max(1.0f, openBody.height * coverage);
    revealed.x = openBody.x + (openBody.width - revealed.width) * 0.5f;
    revealed.y = openBody.y + (openBody.height - revealed.height) * 0.5f;
    break;
  }
  return revealed;
}

ChromeRect chromeAttachedRevealClip(
    const ChromeRect& displayedBody, const ChromeRect& openBody, ChromeEdge edge
) noexcept {
  float left = displayedBody.x;
  float top = displayedBody.y;
  float right = displayedBody.right();
  float bottom = displayedBody.bottom();

  switch (edge) {
  case ChromeEdge::Top:
    top = std::max(top, openBody.y);
    break;
  case ChromeEdge::Right:
    right = std::min(right, openBody.right());
    break;
  case ChromeEdge::Bottom:
    bottom = std::min(bottom, openBody.bottom());
    break;
  case ChromeEdge::Left:
    left = std::max(left, openBody.x);
    break;
  case ChromeEdge::None:
    break;
  }

  right = std::max(left, right);
  bottom = std::max(top, bottom);
  return ChromeRect{.x = left, .y = top, .width = right - left, .height = bottom - top};
}

float chromeFrameDistance(
    float px, float py, float outputWidth, float outputHeight, const ChromeGeometrySettings& settings
) noexcept {
  const ChromeGeometryModel model(settings);
  return -chromeRoundedRectDistance(px, py, model.innerFrame(outputWidth, outputHeight), settings.rounding);
}
