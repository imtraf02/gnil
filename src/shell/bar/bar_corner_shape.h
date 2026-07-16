#pragma once

#include "config/config_types.h"
#include "render/core/render_styles.h"

#include <algorithm>
#include <cstdint>
#include <string_view>

/// The bar's four corners flagged for whether they sit on the bar's inner edge —
/// the edge facing away from the docked screen edge. Only inner-edge corners can
/// grow a concave notch into reserved surface space, so this is the single source
/// of truth for concave-shape mode decisions in barConcaveShape.
struct BarConcaveCorners {
  bool topLeft = false;
  bool topRight = false;
  bool bottomLeft = false;
  bool bottomRight = false;
};

/// Corners on the bar's inner edge for a given docked position. Unknown/empty
/// positions fall through to "top", matching barConcaveShape's else branch.
[[nodiscard]] inline BarConcaveCorners barInnerEdgeCorners(std::string_view position) {
  if (position == "bottom") {
    return {.topLeft = true, .topRight = true};
  }
  if (position == "left") {
    return {.topRight = true, .bottomRight = true};
  }
  if (position == "right") {
    return {.topLeft = true, .bottomLeft = true};
  }
  return {.bottomLeft = true, .bottomRight = true}; // top
}

// concave_edge_corners carves concave corners on one of the bar's two long
// edges, chosen by the margin configuration (both require margin_edge == 0):
// - Inner-edge: full-length bar (margin_ends == 0). Carves the corners on the
//   edge facing away from the screen.
// - Screen-edge: bar inset from its ends (margin_ends > 0). The bar flares
//   outward into those end margins; carves the corners touching the screen edge.
struct BarConcaveShape {
  CornerShapes corners{};
  Radii radii;
  RectInsets logicalInset{};
  float innerBulge = 0.0f; // px the surface/box grow on the inner edge
};

[[nodiscard]] inline BarConcaveShape barConcaveShape(const BarConfig& cfg) {
  (void)cfg;
  // The output frame and every joined panel now share ChromeGeometryModel.
  // Keep this compatibility helper neutral until the remaining callers are
  // folded into the SDF host; it must never re-introduce per-corner geometry.
  return {};
}
