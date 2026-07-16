#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

enum class ChromeEdge : std::uint8_t {
  None,
  Top,
  Right,
  Bottom,
  Left,
};

namespace ChromeJoinEdge {
  inline constexpr std::uint8_t None = 0;
  inline constexpr std::uint8_t Top = 1u << 0;
  inline constexpr std::uint8_t Right = 1u << 1;
  inline constexpr std::uint8_t Bottom = 1u << 2;
  inline constexpr std::uint8_t Left = 1u << 3;
} // namespace ChromeJoinEdge

// Panel paths use two distinct concave states. Horizontal concavity grows a
// wing to the left/right for a top/bottom join; vertical concavity grows it
// up/down for a left/right join.
enum class ChromeCornerMode : std::uint8_t {
  Convex,
  ConcaveHorizontal,
  ConcaveVertical,
};

struct ChromeCornerModes {
  ChromeCornerMode topLeft = ChromeCornerMode::Convex;
  ChromeCornerMode topRight = ChromeCornerMode::Convex;
  ChromeCornerMode bottomRight = ChromeCornerMode::Convex;
  ChromeCornerMode bottomLeft = ChromeCornerMode::Convex;
  bool operator==(const ChromeCornerModes&) const = default;
};

struct ChromeRect {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;

  [[nodiscard]] float right() const noexcept { return x + width; }
  [[nodiscard]] float bottom() const noexcept { return y + height; }
  bool operator==(const ChromeRect&) const = default;
};

struct ChromePoint {
  float x = 0.0f;
  float y = 0.0f;
  bool operator==(const ChromePoint&) const = default;
};

struct ChromeInsets {
  float left = 10.0f;
  float top = 10.0f;
  float right = 10.0f;
  float bottom = 10.0f;

  bool operator==(const ChromeInsets&) const = default;
};

struct ChromeGeometrySettings {
  float frameThickness = 10.0f;
  float rounding = 25.0f;
  // The frame is the inverse of one rounded inner aperture.  The bar does not
  // add another primitive: it changes the inset on its own edge.  Negative
  // values inherit frameThickness so non-bar callers stay direction-neutral.
  ChromeInsets apertureInsets{-1.0f, -1.0f, -1.0f, -1.0f};
  // Decorative smoothing. The output host enforces a radius-derived minimum
  // only for structural frame/panel contacts so a user value of zero cannot
  // reintroduce an antialiased cut at the join.
  float smoothing = 0.0f;
  float deformScale = 0.0f;
  bool operator==(const ChromeGeometrySettings&) const = default;
};

struct ChromePanelState {
  ChromeRect rect{};
  // Output-local source point for a bar-triggered panel.  The body may be
  // clamped near a screen corner, while the connector remains centred on the
  // icon so the panel still has an unambiguous spatial origin.
  ChromePoint triggerAnchor{};
  ChromeRect connector{};
  float radius = 25.0f;
  float deformation = 0.0f;
  float opacity = 1.0f;
  float progress = 1.0f;
  ChromeEdge edge = ChromeEdge::None;
  std::uint8_t joinedEdges = ChromeJoinEdge::None;
  bool hasTriggerAnchor = false;
  bool connectorVisible = false;
  bool attached = false;
  bool visible = false;
  bool inputEnabled = false;
  bool operator==(const ChromePanelState&) const = default;
};

struct ChromePanelShape {
  // Visual bounds include the concave wings. bodyInsets recover the logical
  // panel rect inside those bounds and are passed directly to the SDF.
  ChromeRect visualRect{};
  ChromeInsets bodyInsets{0.0f, 0.0f, 0.0f, 0.0f};
  ChromeCornerModes corners{};
  float effectiveRadius = 1.0f;
  bool operator==(const ChromePanelShape&) const = default;
};

[[nodiscard]] ChromePanelShape chromeResolvePanelShape(const ChromePanelState& panel) noexcept;

// One canonical geometry source for render, work-area reservation, input and
// edge clamps. Values are logical pixels; conversion to physical pixels is
// rounded once at the protocol/render boundary.
class ChromeGeometryModel {
public:
  ChromeGeometryModel() = default;
  explicit ChromeGeometryModel(ChromeGeometrySettings settings) : m_settings(settings) {}

  void setSettings(ChromeGeometrySettings settings) noexcept { m_settings = settings; }
  [[nodiscard]] const ChromeGeometrySettings& settings() const noexcept { return m_settings; }

  [[nodiscard]] ChromeInsets resolvedApertureInsets() const noexcept;
  [[nodiscard]] ChromeRect innerFrame(float outputWidth, float outputHeight) const noexcept;
  [[nodiscard]] ChromeRect
  clampPanel(ChromeRect panel, ChromeEdge edge, float outputWidth, float outputHeight) const noexcept;
  [[nodiscard]] ChromePanelState resolveAnchoredPanel(
      ChromePanelState panel, const ChromeRect& barRect, float outputWidth, float outputHeight
  ) const noexcept;
  [[nodiscard]] ChromePanelState
  resolveJoinedShape(ChromePanelState panel, float outputWidth, float outputHeight) const noexcept;
  [[nodiscard]] std::array<std::int32_t, 4>
  exclusiveZones(ChromeEdge barEdge, float barThickness, bool barVisible) const noexcept;
  [[nodiscard]] std::int32_t physicalFrameThickness(float scale) const noexcept;

private:
  ChromeGeometrySettings m_settings{};
};

class ChromeTransitionState {
public:
  void reset(const ChromePanelState& state) noexcept;
  void setTarget(const ChromePanelState& state) noexcept;
  [[nodiscard]] const ChromePanelState& displayed() const noexcept { return m_displayed; }
  [[nodiscard]] const ChromePanelState& target() const noexcept { return m_target; }
  [[nodiscard]] bool closing() const noexcept { return !m_target.visible; }

  // easedProgress may overshoot [0, 1]. Geometry retains that expressive
  // overshoot; opacity/progress/input coverage are clamped.
  [[nodiscard]] ChromePanelState sample(float easedProgress, float deformScale = 0.0f) noexcept;

private:
  ChromePanelState m_from{};
  ChromePanelState m_target{};
  ChromePanelState m_displayed{};
};

[[nodiscard]] float chromeRoundedRectDistance(float px, float py, const ChromeRect& rect, float radius) noexcept;
[[nodiscard]] float chromeSmoothUnion(float a, float b, float smoothing) noexcept;
[[nodiscard]] float chromeCircularSmoothUnion(float a, float b, float smoothing) noexcept;
// CPU mirror of the shader's render-only 20% bridge. Used by geometry tests;
// logical panel/input rectangles are never replaced with this value.
[[nodiscard]] ChromeRect chromeJoinedRenderRect(const ChromeRect& rect, std::uint8_t joinedEdges) noexcept;
[[nodiscard]] float chromeFrameDistance(
    float px, float py, float outputWidth, float outputHeight, const ChromeGeometrySettings& settings
) noexcept;
// Places the logical content/input body immediately outside the visible bar.
// The shared host keeps this rect unchanged, then derives per-corner visual
// wings from its frame contacts.
[[nodiscard]] ChromeRect
chromePlaceAttachedBody(const ChromeRect& panel, const ChromeRect& bar, ChromeEdge edge) noexcept;

// Reveals a frame-joined body without ever breaking its structural contact.
// At progress 0 the body is a one-logical-pixel strip on the joined edge; at
// progress 1 it is exactly `openBody`. This rect is shared by chrome, content
// clipping and input so an opening panel cannot visually lead its hit region.
[[nodiscard]] ChromeRect
chromeAnchoredRevealRect(const ChromeRect& openBody, ChromeEdge edge, float progress) noexcept;

// Intersects a moving attached body with the fixed portal on the application's
// side of the bar. This emulates the bar content occluding the panel even when
// Niri composites the panel's separate layer surface above the bar surface.
[[nodiscard]] ChromeRect chromeAttachedRevealClip(
    const ChromeRect& displayedBody, const ChromeRect& openBody, ChromeEdge edge
) noexcept;
