#pragma once

namespace lockscreen_layout {

  enum class Mode {
    Wide,
    Medium,
    Compact,
  };

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
    Mode mode = Mode::Compact;
    float scale = 1.0f;
    float density = 1.0f;
    float outerMargin = 0.0f;
    float gap = 0.0f;
    float cardPadding = 0.0f;
    float contentGap = 0.0f;
    float radius = 0.0f;
    bool showHero = true;
    Rect canvas;
    Rect leftColumn;
    Rect centerColumn;
    Rect rightColumn;
    Rect identityCard;
    Rect clockBlock;
    Rect mediaCard;
    Rect heroCard;
    Rect loginBlock;
    Rect calendarCard;
    Rect weatherCard;
    Rect metricsCard;
    Rect notificationButton;
    Rect notificationPanel;
  };

  [[nodiscard]] Layout resolve(float screenWidth, float screenHeight) noexcept;

} // namespace lockscreen_layout
