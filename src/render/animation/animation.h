#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

enum class Easing : std::uint8_t {
  Linear,
  EaseInQuad,
  EaseInCubic,
  EaseOutQuad,
  EaseInOutQuad,
  EaseOutCubic,
  EaseInOutCubic,
  EaseOutBack,
  // Cubic-bezier(0.38, 1.21, 0.22, 1), the expressive spatial curve used by
  // Caelestia.  The small overshoot is intentional; output is not clamped.
  CaelestiaExpressiveSpatial,
  // Monotonic expressive spatial curve for geometry whose bounds must never
  // overshoot (notably attached panel width/height and corner morphs).
  FluidSpatial,
  // Opacity curves are deliberately calmer than the spatial morph so content
  // never appears to bounce while the shared chrome settles.
  CaelestiaDefaultEffects,
  CaelestiaSlowEffects,
};

float applyEasing(Easing easing, float t);

struct Animation {
  float startValue = 0.0f;
  float endValue = 0.0f;
  float durationMs = 0.0f;
  float elapsedMs = 0.0f;
  std::chrono::steady_clock::time_point startedAt;
  Easing easing = Easing::EaseOutQuad;
  std::function<void(float)> setter;
  std::function<void()> onComplete;
  bool finished = false;
};
