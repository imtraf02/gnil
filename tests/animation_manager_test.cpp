#include "render/animation/animation_manager.h"
#include "render/animation/animation.h"
#include "render/animation/motion_service.h"

#include <cmath>
#include <iostream>

namespace {

  bool check(bool cond, const char* msg) {
    if (!cond) {
      std::cerr << "FAIL: " << msg << '\n';
    }
    return cond;
  }

  bool nearlyEqual(float a, float b) { return std::fabs(a - b) < 0.0001f; }

  void resetMotion() {
    auto& motion = MotionService::instance();
    motion.setSpeed(1.0f);
    motion.setEnabled(true);
  }

} // namespace

int main() {
  bool ok = true;
  resetMotion();

  {
    const float start = applyEasing(Easing::CaelestiaExpressiveSpatial, 0.0f);
    const float middle = applyEasing(Easing::CaelestiaExpressiveSpatial, 0.5f);
    const float end = applyEasing(Easing::CaelestiaExpressiveSpatial, 1.0f);
    ok &= check(nearlyEqual(start, 0.0f), "Caelestia curve must start at zero");
    ok &= check(middle > 0.5f, "Caelestia curve should have an expressive fast reveal");
    ok &= check(nearlyEqual(end, 1.0f), "Caelestia curve must settle at one");
    const float defaultEffect = applyEasing(Easing::CaelestiaDefaultEffects, 0.5f);
    const float slowEffect = applyEasing(Easing::CaelestiaSlowEffects, 0.5f);
    ok &= check(defaultEffect > 0.5f && defaultEffect < 1.0f, "default effects curve must ease opacity in");
    ok &= check(slowEffect >= defaultEffect, "slow effects curve should reveal at least as gently as default");

    float previousFluid = 0.0f;
    for (int step = 0; step <= 100; ++step) {
      const float fluid = applyEasing(Easing::FluidSpatial, static_cast<float>(step) / 100.0f);
      ok &= check(fluid >= 0.0f && fluid <= 1.0f, "fluid spatial geometry must not overshoot");
      ok &= check(fluid >= previousFluid, "fluid spatial geometry must be monotonic");
      previousFluid = fluid;
    }
    ok &= check(nearlyEqual(previousFluid, 1.0f), "fluid spatial curve must settle at one");
  }

  {
    float previous = 0.0f;
    for (const float progress : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
      const float value = applyEasing(Easing::EaseInCubic, progress);
      ok &= check(value >= 0.0f && value <= 1.0f, "exit curve must not overshoot");
      ok &= check(value >= previous, "exit curve must be monotonic");
      previous = value;
    }
    ok &= check(nearlyEqual(previous, 1.0f), "exit curve must finish at one");
  }

  {
    AnimationManager manager;
    MotionService::instance().setEnabled(false);

    float value = 0.0f;
    bool completed = false;
    const auto id = manager.animate(
        0.0f, 10.0f, 200.0f, Easing::Linear, [&value](float v) { value = v; }, [&completed]() { completed = true; });

    ok &= check(id != 0, "reduced-motion animation with completion should remain cancellable");
    ok &= check(nearlyEqual(value, 10.0f), "reduced-motion animation did not snap to target");
    ok &= check(!completed, "reduced-motion completion ran synchronously");

    manager.tick(0.0f);

    ok &= check(completed, "reduced-motion completion did not run on tick");
    ok &= check(!manager.hasActive(), "reduced-motion animation stayed active after completion");
  }

  resetMotion();

  {
    AnimationManager manager;

    float value = 0.0f;
    bool completed = false;
    const auto id = manager.animate(
        0.0f, 10.0f, 1000.0f, Easing::Linear, [&value](float v) { value = v; }, [&completed]() { completed = true; });

    ok &= check(id != 0, "normal animation did not start");

    MotionService::instance().setEnabled(false);

    ok &= check(nearlyEqual(value, 10.0f), "active animation did not snap when motion was disabled");
    ok &= check(!completed, "active animation completion ran synchronously when motion was disabled");
    ok &= check(manager.hasActive(), "active animation did not remain pending for async completion");

    manager.tick(0.0f);

    ok &= check(completed, "active animation completion did not run after reduced-motion snap");
  }

  resetMotion();

  {
    AnimationManager manager;
    MotionService::instance().setEnabled(false);

    float value = -1.0f;
    bool completed = false;
    const auto id = manager.animateTimer(
        1.0f, 0.0f, 1000.0f, Easing::Linear, [&value](float v) { value = v; }, [&completed]() { completed = true; });

    ok &= check(id != 0, "timer animation did not start while motion was disabled");
    ok &= check(nearlyEqual(value, -1.0f), "timer animation snapped when motion was disabled");

    manager.tick(0.0f);

    ok &= check(!completed, "timer animation completed early while motion was disabled");
    ok &= check(manager.hasActive(), "timer animation was removed early while motion was disabled");
    manager.cancel(id);
  }

  resetMotion();
  return ok ? 0 : 1;
}
