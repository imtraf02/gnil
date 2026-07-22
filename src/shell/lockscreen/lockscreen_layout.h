#pragma once

namespace lockscreen_layout {

  struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    [[nodiscard]] constexpr float right() const noexcept { return x + width; }
    [[nodiscard]] constexpr float bottom() const noexcept { return y + height; }
    constexpr bool operator==(const Rect&) const = default;
  };

  struct Layout {
    bool fullDashboard = false;
    float scale = 1.0f;
    float gap = 0.0f;
    float radius = 0.0f;
    Rect island;
    Rect leftColumn;
    Rect centerColumn;
    Rect rightColumn;
    Rect weatherCard;
    Rect systemCard;
    Rect mediaCard;
    Rect metricsCard;
    Rect notificationsCard;
  };

  [[nodiscard]] Layout resolve(float screenWidth, float screenHeight) noexcept;

} // namespace lockscreen_layout
