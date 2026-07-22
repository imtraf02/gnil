#include "shell/lockscreen/lockscreen_layout.h"

#include <array>
#include <cmath>
#include <iostream>
#include <string_view>

namespace {

  bool check(bool condition, std::string_view message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
  }

  bool contains(const lockscreen_layout::Rect& outer, const lockscreen_layout::Rect& inner) {
    constexpr float epsilon = 0.01f;
    return inner.x + epsilon >= outer.x
        && inner.y + epsilon >= outer.y
        && inner.right() <= outer.right() + epsilon
        && inner.bottom() <= outer.bottom() + epsilon;
  }

} // namespace

int main() {
  bool ok = true;
  for (const auto [width, height] : std::array{
           std::pair{1280.0f, 720.0f},
           std::pair{1024.0f, 768.0f},
           std::pair{1920.0f, 1080.0f},
           std::pair{2560.0f, 1440.0f},
       }) {
    const auto layout = lockscreen_layout::resolve(width, height);
    ok &= check(layout.fullDashboard, "wide landscape output uses the full dashboard");
    for (const auto& rect : {
             layout.leftColumn,
             layout.centerColumn,
             layout.rightColumn,
             layout.weatherCard,
             layout.systemCard,
             layout.mediaCard,
             layout.metricsCard,
             layout.notificationsCard,
         }) {
      ok &= check(rect.width > 0.0f && rect.height > 0.0f, "dashboard regions have positive size");
      ok &= check(contains(layout.island, rect), "dashboard regions stay inside the island");
    }
    ok &= check(layout.leftColumn.right() <= layout.centerColumn.x, "left and center columns do not overlap");
    ok &= check(layout.centerColumn.right() <= layout.rightColumn.x, "center and right columns do not overlap");
    ok &= check(layout.weatherCard.bottom() <= layout.systemCard.y, "weather and system cards do not overlap");
    ok &= check(layout.systemCard.bottom() <= layout.mediaCard.y, "system and media cards do not overlap");
    ok &= check(layout.metricsCard.bottom() <= layout.notificationsCard.y, "right cards do not overlap");
    ok &= check(layout.centerColumn.width >= 320.0f, "wide password column remains usable");
    ok &= check(layout.island.height <= height * 0.70f + 0.01f, "dashboard island stays compact");
    ok &= check(
        std::abs(layout.island.width / layout.island.height - 16.0f / 9.0f) < 0.01f,
        "dashboard island keeps a 16:9 frame"
    );
  }

  for (const auto [width, height] : std::array{
           std::pair{800.0f, 1280.0f},
           std::pair{720.0f, 480.0f},
       }) {
    const auto layout = lockscreen_layout::resolve(width, height);
    ok &= check(!layout.fullDashboard, "narrow or portrait output uses compact mode");
    ok &= check(contains(layout.island, layout.centerColumn), "compact login remains inside the island");
    ok &= check(layout.centerColumn.width > 0.0f && layout.centerColumn.width <= 576.0f, "compact login width is bounded");
    ok &= check(layout.leftColumn.width == 0.0f && layout.rightColumn.width == 0.0f, "compact mode hides auxiliary columns");
  }

  const auto invalid = lockscreen_layout::resolve(0.0f, -1.0f);
  ok &= check(invalid.island.width > 0.0f && invalid.island.height > 0.0f, "invalid dimensions resolve safely");
  return ok ? 0 : 1;
}
