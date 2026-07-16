#include "render/animation/animation.h"

#include <algorithm>
#include <cmath>

namespace {

  float cubicBezier(float p0, float p1, float p2, float p3, float t) {
    const float omt = 1.0f - t;
    return omt * omt * omt * p0 + 3.0f * omt * omt * t * p1 + 3.0f * omt * t * t * p2 + t * t * t * p3;
  }

  float cubicBezierDerivative(float p0, float p1, float p2, float p3, float t) {
    const float omt = 1.0f - t;
    return 3.0f * omt * omt * (p1 - p0) + 6.0f * omt * t * (p2 - p1) + 3.0f * t * t * (p3 - p2);
  }

  float solveBezierX(float x, float x1, float x2) {
    float t = x;
    // Newton converges quickly for this monotonic x curve.  Bisection keeps
    // the interpolation stable at the endpoints and on low precision GPUs.
    for (int i = 0; i < 5; ++i) {
      const float error = cubicBezier(0.0f, x1, x2, 1.0f, t) - x;
      const float slope = cubicBezierDerivative(0.0f, x1, x2, 1.0f, t);
      if (std::abs(slope) < 0.0001f) {
        break;
      }
      t = std::clamp(t - error / slope, 0.0f, 1.0f);
    }
    return t;
  }

} // namespace

float applyEasing(Easing easing, float t) {
  t = std::clamp(t, 0.0f, 1.0f);

  switch (easing) {
  case Easing::Linear:
    return t;

  case Easing::EaseInQuad:
    return t * t;

  case Easing::EaseInCubic:
    return t * t * t;

  case Easing::EaseOutQuad:
    return t * (2.0f - t);

  case Easing::EaseInOutQuad:
    if (t < 0.5f) {
      return 2.0f * t * t;
    }
    return -1.0f + (4.0f - 2.0f * t) * t;

  case Easing::EaseOutCubic: {
    const float f = t - 1.0f;
    return f * f * f + 1.0f;
  }

  case Easing::EaseInOutCubic:
    if (t < 0.5f) {
      return 4.0f * t * t * t;
    } else {
      const float f = 2.0f * t - 2.0f;
      return 0.5f * f * f * f + 1.0f;
    }

  case Easing::EaseOutBack: {
    constexpr float c1 = 1.70158f;
    constexpr float c3 = c1 + 1.0f;
    const float f = t - 1.0f;
    return 1.0f + c3 * f * f * f + c1 * f * f;
  }

  case Easing::CaelestiaExpressiveSpatial: {
    const float parameter = solveBezierX(t, 0.38f, 0.22f);
    return cubicBezier(0.0f, 1.21f, 1.0f, 1.0f, parameter);
  }

  case Easing::FluidSpatial: {
    const float parameter = solveBezierX(t, 0.16f, 0.30f);
    return cubicBezier(0.0f, 1.0f, 1.0f, 1.0f, parameter);
  }

  case Easing::CaelestiaDefaultEffects: {
    const float parameter = solveBezierX(t, 0.34f, 0.34f);
    return cubicBezier(0.0f, 0.8f, 1.0f, 1.0f, parameter);
  }

  case Easing::CaelestiaSlowEffects: {
    const float parameter = solveBezierX(t, 0.34f, 0.34f);
    return cubicBezier(0.0f, 0.88f, 1.0f, 1.0f, parameter);
  }
  }

  return t;
}
