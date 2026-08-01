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

  bool positive(const lockscreen_layout::Rect& rect) { return rect.width > 0.0f && rect.height > 0.0f; }

} // namespace

int main() {
  bool ok = true;
  for (const auto [width, height] : std::array{
           std::pair{1280.0f, 720.0f},
           std::pair{1920.0f, 1080.0f},
           std::pair{2560.0f, 1440.0f},
       }) {
    const auto layout = lockscreen_layout::resolve(width, height);
    ok &= check(layout.mode == lockscreen_layout::Mode::Wide, "large landscape output uses the wide editorial layout");
    for (const auto& rect : {
             layout.leftColumn,
             layout.centerColumn,
             layout.rightColumn,
             layout.identityCard,
             layout.clockBlock,
             layout.mediaCard,
             layout.heroCard,
             layout.loginBlock,
             layout.calendarCard,
             layout.weatherCard,
             layout.metricsCard,
             layout.notificationButton,
             layout.notificationPanel,
         }) {
      ok &= check(positive(rect), "wide dashboard regions have positive size");
      ok &= check(contains(layout.canvas, rect), "wide dashboard regions stay inside the safe canvas");
    }
    ok &= check(layout.leftColumn.right() <= layout.centerColumn.x, "left and center columns do not overlap");
    ok &= check(layout.centerColumn.right() <= layout.rightColumn.x, "center and right columns do not overlap");
    ok &= check(layout.identityCard.bottom() <= layout.clockBlock.y, "identity and clock do not overlap");
    ok &= check(layout.clockBlock.bottom() <= layout.mediaCard.y, "clock and media do not overlap");
    ok &= check(layout.heroCard.bottom() <= layout.loginBlock.y, "hero and login do not overlap");
    ok &= check(layout.calendarCard.bottom() <= layout.weatherCard.y, "calendar and weather do not overlap");
    ok &= check(layout.weatherCard.bottom() <= layout.metricsCard.y, "weather and metrics do not overlap");
    ok &= check(layout.notificationButton.width >= 44.0f, "notification hit target is at least 44px");
  }

  for (const auto [width, height] : std::array{std::pair{1024.0f, 768.0f}, std::pair{960.0f, 540.0f}}) {
    const auto layout = lockscreen_layout::resolve(width, height);
    ok &= check(layout.mode == lockscreen_layout::Mode::Medium, "mid-size landscape output uses two columns");
    ok &= check(positive(layout.leftColumn) && positive(layout.rightColumn), "medium columns have positive size");
    ok &= check(layout.centerColumn.width == 0.0f, "medium mode removes the standalone hero column");
    for (const auto& rect : {layout.identityCard, layout.clockBlock, layout.mediaCard, layout.loginBlock,
                             layout.calendarCard, layout.weatherCard, layout.metricsCard}) {
      ok &= check(contains(layout.canvas, rect), "medium region stays inside the safe canvas");
    }
  }

  for (const auto [width, height] : std::array{std::pair{800.0f, 1280.0f}, std::pair{720.0f, 480.0f}}) {
    const auto layout = lockscreen_layout::resolve(width, height);
    ok &= check(layout.mode == lockscreen_layout::Mode::Compact, "narrow or portrait output uses compact mode");
    ok &= check(contains(layout.canvas, layout.centerColumn), "compact column remains inside the safe canvas");
    ok &= check(layout.centerColumn.width > 0.0f && layout.centerColumn.width <= 598.0f, "compact width is bounded");
    ok &= check(layout.leftColumn.width == 0.0f && layout.rightColumn.width == 0.0f, "compact mode hides auxiliary columns");
    for (const auto& rect : {layout.identityCard, layout.clockBlock, layout.heroCard, layout.loginBlock}) {
      ok &= check(positive(rect) && contains(layout.centerColumn, rect), "compact primary region remains usable");
    }
  }

  const auto invalid = lockscreen_layout::resolve(0.0f, -1.0f);
  ok &= check(invalid.canvas.width > 0.0f && invalid.canvas.height > 0.0f, "invalid dimensions resolve safely");
  return ok ? 0 : 1;
}
