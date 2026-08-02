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
    ok &= check(
        layout.cardPadding >= 10.0f && layout.cardPadding <= 14.0f
            && layout.contentGap >= 6.0f && layout.contentGap <= 8.0f,
        "wide layout exposes bounded responsive spacing tokens"
    );
  }

  for (const auto [width, height] :
       std::array{std::pair{1024.0f, 768.0f}, std::pair{960.0f, 540.0f}, std::pair{840.0f, 480.0f}}) {
    const auto layout = lockscreen_layout::resolve(width, height);
    ok &= check(layout.mode == lockscreen_layout::Mode::Medium, "mid-size landscape output uses two columns");
    ok &= check(positive(layout.leftColumn) && positive(layout.rightColumn), "medium columns have positive size");
    ok &= check(layout.centerColumn.width == 0.0f, "medium mode removes the standalone hero column");
    for (const auto& rect : {layout.identityCard, layout.clockBlock, layout.mediaCard, layout.loginBlock,
                             layout.calendarCard, layout.weatherCard, layout.metricsCard}) {
      ok &= check(contains(layout.canvas, rect), "medium region stays inside the safe canvas");
    }
    ok &= check(layout.identityCard.height >= 59.0f, "medium identity card fits its avatar and padding");
    ok &= check(layout.loginBlock.height >= 100.0f, "medium login block fits prompt, status, and actions");
    ok &= check(layout.metricsCard.height >= 92.0f, "medium metrics card retains readable rows");
    ok &= check(layout.identityCard.bottom() <= layout.clockBlock.y, "medium identity and clock do not overlap");
    ok &= check(layout.clockBlock.bottom() <= layout.mediaCard.y, "medium clock and media do not overlap");
    ok &= check(layout.mediaCard.bottom() <= layout.loginBlock.y, "medium media and login do not overlap");
    ok &= check(layout.calendarCard.bottom() <= layout.weatherCard.y, "medium calendar and weather do not overlap");
    ok &= check(layout.weatherCard.bottom() <= layout.metricsCard.y, "medium weather and metrics do not overlap");
  }

  for (const auto [width, height] : std::array{std::pair{800.0f, 1280.0f}, std::pair{720.0f, 480.0f}}) {
    const auto layout = lockscreen_layout::resolve(width, height);
    ok &= check(layout.mode == lockscreen_layout::Mode::Compact, "narrow or portrait output uses compact mode");
    ok &= check(contains(layout.canvas, layout.centerColumn), "compact column remains inside the safe canvas");
    ok &= check(layout.centerColumn.width > 0.0f && layout.centerColumn.width <= 598.0f, "compact width is bounded");
    ok &= check(layout.leftColumn.width == 0.0f && layout.rightColumn.width == 0.0f, "compact mode hides auxiliary columns");
    ok &= check(layout.showHero, "normal compact outputs retain the hero card");
    for (const auto& rect : {layout.identityCard, layout.clockBlock, layout.heroCard, layout.loginBlock}) {
      ok &= check(positive(rect) && contains(layout.centerColumn, rect), "compact primary region remains usable");
    }
    ok &= check(layout.loginBlock.height >= 100.0f, "compact login block does not crowd authentication controls");
    ok &= check(layout.identityCard.bottom() <= layout.clockBlock.y, "compact identity and clock do not overlap");
    ok &= check(layout.clockBlock.bottom() <= layout.heroCard.y, "compact clock and hero do not overlap");
    ok &= check(layout.heroCard.bottom() <= layout.loginBlock.y, "compact hero and login do not overlap");
  }

  {
    const auto layout = lockscreen_layout::resolve(480.0f, 360.0f);
    ok &= check(layout.mode == lockscreen_layout::Mode::Compact, "short output remains compact");
    ok &= check(!layout.showHero, "short compact output prioritizes login over decorative hero content");
    for (const auto& rect : {layout.identityCard, layout.clockBlock, layout.loginBlock}) {
      ok &= check(positive(rect) && contains(layout.centerColumn, rect), "short compact core controls stay visible");
    }
    ok &= check(layout.identityCard.bottom() <= layout.clockBlock.y, "short compact identity and clock do not overlap");
    ok &= check(layout.clockBlock.bottom() <= layout.loginBlock.y, "short compact clock and login do not overlap");
  }

  const auto invalid = lockscreen_layout::resolve(0.0f, -1.0f);
  ok &= check(invalid.canvas.width > 0.0f && invalid.canvas.height > 0.0f, "invalid dimensions resolve safely");
  return ok ? 0 : 1;
}
