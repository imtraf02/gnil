#pragma once

#include "config/config_types.h"
#include "shell/chrome/chrome_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

[[nodiscard]] inline std::int32_t chromeFrameThickness(const ShellConfig& shell) noexcept {
  return static_cast<std::int32_t>(std::lround(std::max(1.0f, shell.chrome.frameThickness)));
}

struct BarVisibleRect {
  std::int32_t left = 0;
  std::int32_t top = 0;
  std::int32_t right = 0;
  std::int32_t bottom = 0;

  constexpr bool operator==(const BarVisibleRect&) const noexcept = default;
};

struct ChromeLayoutContext {
  std::int32_t outputWidth = 0;
  std::int32_t outputHeight = 0;
  BarVisibleRect barRect{};
  ChromeEdge barEdge = ChromeEdge::None;
  ChromeGeometrySettings geometry{};

  bool operator==(const ChromeLayoutContext&) const = default;
};

/// Output-local visible rail geometry shared by chrome panels and tray menus.
/// The rail replaces the frame on its docked edge and retains one frame-sized
/// margin at both main-axis ends. Legacy independent bar margins are not part
/// of this model.
[[nodiscard]] inline BarVisibleRect resolveBarVisibleRect(
    std::string_view position, std::int32_t thickness, std::int32_t outputWidth, std::int32_t outputHeight,
    std::int32_t frameThickness = 0
) noexcept {
  const bool bottom = position == "bottom";
  const bool left = position == "left";
  const bool right = position == "right";
  const bool vertical = left || right;
  thickness = std::max(0, thickness);
  frameThickness = std::max(0, frameThickness);
  const std::int32_t mainExtent = std::max(0, vertical ? outputHeight : outputWidth);
  const std::int32_t endMargin = std::min(frameThickness, mainExtent / 2);
  const std::int32_t mainStart = endMargin;
  const std::int32_t mainEnd = mainExtent - endMargin;
  const std::int32_t rectLeft = right ? std::max(0, outputWidth - thickness) : 0;
  const std::int32_t rectTop = bottom ? std::max(0, outputHeight - thickness) : 0;
  const std::int32_t rectRight = vertical ? rectLeft + thickness : outputWidth;
  const std::int32_t rectBottom = vertical ? outputHeight : rectTop + thickness;

  return BarVisibleRect{
      .left = vertical ? rectLeft : mainStart,
      .top = vertical ? mainStart : rectTop,
      .right = vertical ? std::min(outputWidth, rectRight) : mainEnd,
      .bottom = vertical ? mainEnd : std::min(outputHeight, rectBottom),
  };
}

/// Keep widget ink one frame-thickness away from both ends of the bar.
/// Hit targets may still extend to the output edge, but the visible start/end
/// sections use this value on their main axis (Y for a left/right bar).
[[nodiscard]] inline float resolvedBarMainAxisPadding(const BarConfig& barConfig, const ShellConfig& shell) noexcept {
  return std::max(static_cast<float>(std::max(0, barConfig.padding)), std::max(1.0f, shell.chrome.frameThickness));
}

[[nodiscard]] inline ChromeLayoutContext resolveChromeLayoutContext(
    const BarConfig& barConfig, const ShellConfig& shell, std::int32_t outputWidth, std::int32_t outputHeight,
    float effectiveBarThickness = -1.0f
) noexcept {
  const float frame = std::max(1.0f, shell.chrome.frameThickness);
  const float barThickness = effectiveBarThickness < 0.0f
      ? static_cast<float>(std::max(0, barConfig.thickness))
      : std::max(0.0f, effectiveBarThickness);
  const auto sampledThickness = static_cast<std::int32_t>(std::lround(barThickness));
  const ChromeEdge edge = barConfig.position == "top" ? ChromeEdge::Top
      : barConfig.position == "right"                 ? ChromeEdge::Right
      : barConfig.position == "bottom"                ? ChromeEdge::Bottom
                                                       : ChromeEdge::Left;
  ChromeInsets aperture{frame, frame, frame, frame};
  switch (edge) {
  case ChromeEdge::Top:
    aperture.top = std::max(frame, barThickness);
    break;
  case ChromeEdge::Right:
    aperture.right = std::max(frame, barThickness);
    break;
  case ChromeEdge::Bottom:
    aperture.bottom = std::max(frame, barThickness);
    break;
  case ChromeEdge::Left:
    aperture.left = std::max(frame, barThickness);
    break;
  case ChromeEdge::None:
    break;
  }

  return ChromeLayoutContext{
      .outputWidth = outputWidth,
      .outputHeight = outputHeight,
      .barRect = resolveBarVisibleRect(
          barConfig.position, sampledThickness, outputWidth, outputHeight, chromeFrameThickness(shell)
      ),
      .barEdge = edge,
      .geometry = ChromeGeometrySettings{
          .frameThickness = shell.chrome.frameThickness,
          .rounding = shell.chrome.rounding,
          .apertureInsets = aperture,
          .smoothing = shell.chrome.smoothing,
          .deformScale = shell.chrome.deformScale,
      },
  };
}

[[nodiscard]] inline BarVisibleRect
resolveBarVisibleRect(const BarConfig& barConfig, std::int32_t outputWidth, std::int32_t outputHeight) noexcept {
  return resolveBarVisibleRect(
      barConfig.position, barConfig.thickness, outputWidth, outputHeight
  );
}

[[nodiscard]] inline BarVisibleRect resolveBarVisibleRect(
    const BarConfig& barConfig, const ShellConfig& shell, std::int32_t outputWidth, std::int32_t outputHeight
) noexcept {
  return resolveBarVisibleRect(
      barConfig.position, barConfig.thickness, outputWidth, outputHeight, chromeFrameThickness(shell)
  );
}

/// Layer-shell exclusive zone the bar reserves on its anchored screen edge.
/// Canonical definition shared by the bar surface and attached panels so a panel
/// can anchor against the same reserved edge the compositor places the bar on.
[[nodiscard]] inline std::int32_t
reservedBarExclusiveZone(const BarConfig& barConfig, const ShellConfig::ShadowConfig& shadowConfig) {
  (void)shadowConfig;
  return std::max<std::int32_t>(0, barConfig.thickness);
}

/// Layer-shell margin the bar applies on its anchored screen edge. The edge gap
/// (marginEdge) is split: the part covered by the surface's own edge-side shadow
/// bleed lives inside the surface, and the remainder becomes the layer margin.
/// Auto-hide folds the whole gap into the surface as a gutter, so the margin is 0.
[[nodiscard]] inline std::int32_t
barEdgeLayerMargin(const BarConfig& barConfig, const ShellConfig::ShadowConfig& shadowConfig) {
  (void)barConfig;
  (void)shadowConfig;
  return 0;
}

/// Total distance reserved from the screen edge for a rail and the remaining
/// frame. The bar width already includes the frame material on its own edge.
[[nodiscard]] inline std::int32_t
reservedBarEdgeDistance(const BarConfig& barConfig, const ShellConfig::ShadowConfig& shadowConfig) {
  return reservedBarExclusiveZone(barConfig, shadowConfig) + barEdgeLayerMargin(barConfig, shadowConfig);
}

[[nodiscard]] inline std::int32_t
reservedBarEdgeDistance(const BarConfig& barConfig, const ShellConfig& shell) {
  return std::max(chromeFrameThickness(shell), reservedBarEdgeDistance(barConfig, shell.shadow));
}
