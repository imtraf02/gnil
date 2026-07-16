#include "shell/surface/shadow.h"

#include "render/core/color.h"

#include <algorithm>

namespace shell::surface_shadow {

  bool enabled(bool /*componentShadow*/, const ShellConfig::ShadowConfig& /*shadow*/) noexcept {
    return false;
  }

  Bleed bleed(bool componentShadow, const ShellConfig::ShadowConfig& shadow) noexcept {
    (void)componentShadow;
    (void)shadow;
    return {};
  }

  RoundedRectStyle
  style(const ShellConfig::ShadowConfig& shadow, float backgroundOpacity, const Shape& shape) noexcept {
    (void)shadow;
    (void)backgroundOpacity;
    return RoundedRectStyle{
        .fill = Color{},
        .border = Color{},
        .fillMode = FillMode::Solid,
        .corners = shape.corners,
        .logicalInset = shape.logicalInset,
        .radius = shape.radius,
        .softness = 0.0f,
        .borderWidth = 0.0f,
        .outerShadow = false,
        .shadowCutoutOffsetX = 0.0f,
        .shadowCutoutOffsetY = 0.0f,
    };
  }

  bool sameSurfaceMetrics(const ShellConfig::ShadowConfig& previous, const ShellConfig::ShadowConfig& next) noexcept {
    (void)previous;
    (void)next;
    return true;
  }

} // namespace shell::surface_shadow
