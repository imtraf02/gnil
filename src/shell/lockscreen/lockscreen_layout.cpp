#include "shell/lockscreen/lockscreen_layout.h"

#include <algorithm>
#include <cmath>

namespace lockscreen_layout {

  namespace {

    float clampDimension(float value, float fallback) noexcept {
      return std::isfinite(value) && value > 0.0f ? value : fallback;
    }

  } // namespace

  Layout resolve(float screenWidth, float screenHeight) noexcept {
    const float sw = clampDimension(screenWidth, 1.0f);
    const float sh = clampDimension(screenHeight, 1.0f);
    const float shortEdge = std::min(sw, sh);
    const float outerMargin = std::clamp(shortEdge * 0.035f, 20.0f, 48.0f);
    const float canvasWidth = std::min(std::max(1.0f, sw - outerMargin * 2.0f), 1680.0f);
    const float canvasHeight = std::min(std::max(1.0f, sh - outerMargin * 2.0f), 920.0f);
    const float canvasX = std::round((sw - canvasWidth) * 0.5f);
    const float canvasY = std::round((sh - canvasHeight) * 0.5f);
    const float gap = std::clamp(canvasWidth * 0.012f, 10.0f, 22.0f);

    Layout out;
    out.scale = std::clamp(std::min(sw / 1920.0f, sh / 1080.0f), 0.72f, 1.15f);
    out.gap = gap;
    out.radius = std::clamp(18.0f * out.scale, 14.0f, 24.0f);
    out.canvas = Rect{canvasX, canvasY, canvasWidth, canvasHeight};

    const bool landscape = sw >= sh;
    if (landscape && sw >= 1180.0f && sh >= 650.0f) {
      out.mode = Mode::Wide;
    } else if (landscape && sw >= 840.0f && sh >= 480.0f) {
      out.mode = Mode::Medium;
    } else {
      out.mode = Mode::Compact;
    }

    const float buttonSize = std::max(44.0f, 44.0f * out.scale);
    out.notificationButton = Rect{
        out.canvas.right() - buttonSize,
        out.canvas.y,
        buttonSize,
        buttonSize,
    };
    const float panelWidth = std::min(320.0f * out.scale, out.canvas.width);
    const float panelHeight = std::min(330.0f * out.scale, std::max(1.0f, out.canvas.height - buttonSize - gap));
    out.notificationPanel = Rect{
        out.canvas.right() - panelWidth,
        out.notificationButton.bottom() + gap,
        panelWidth,
        panelHeight,
    };

    if (out.mode == Mode::Compact) {
      const float compactWidth = std::min(520.0f * out.scale, out.canvas.width);
      out.centerColumn = Rect{
          std::round((sw - compactWidth) * 0.5f),
          out.canvas.y,
          compactWidth,
          out.canvas.height,
      };
      const float usable = std::max(1.0f, out.centerColumn.height - gap * 3.0f);
      const float identityHeight = usable * 0.15f;
      const float clockHeight = usable * 0.32f;
      const float heroHeight = usable * 0.24f;
      out.identityCard = Rect{out.centerColumn.x, out.centerColumn.y, compactWidth, identityHeight};
      out.clockBlock = Rect{out.centerColumn.x, out.identityCard.bottom() + gap, compactWidth, clockHeight};
      out.heroCard = Rect{out.centerColumn.x, out.clockBlock.bottom() + gap, compactWidth, heroHeight};
      out.loginBlock = Rect{
          out.centerColumn.x,
          out.heroCard.bottom() + gap,
          compactWidth,
          std::max(1.0f, out.centerColumn.bottom() - out.heroCard.bottom() - gap),
      };
      return out;
    }

    const float columnsWidth = std::max(1.0f, out.canvas.width - (out.mode == Mode::Wide ? gap * 2.0f : gap));
    if (out.mode == Mode::Wide) {
      const float leftWidth = columnsWidth * 0.27f;
      const float centerWidth = columnsWidth * 0.43f;
      const float rightWidth = std::max(1.0f, columnsWidth - leftWidth - centerWidth);
      out.leftColumn = Rect{out.canvas.x, out.canvas.y, leftWidth, out.canvas.height};
      out.centerColumn = Rect{out.leftColumn.right() + gap, out.canvas.y, centerWidth, out.canvas.height};
      out.rightColumn = Rect{out.centerColumn.right() + gap, out.canvas.y, rightWidth, out.canvas.height};

      const float leftUsable = std::max(1.0f, out.leftColumn.height - gap * 2.0f);
      const float identityHeight = leftUsable * 0.14f;
      const float clockHeight = leftUsable * 0.34f;
      out.identityCard = Rect{out.leftColumn.x, out.leftColumn.y, leftWidth, identityHeight};
      out.clockBlock = Rect{out.leftColumn.x, out.identityCard.bottom() + gap, leftWidth, clockHeight};
      out.mediaCard = Rect{
          out.leftColumn.x,
          out.clockBlock.bottom() + gap,
          leftWidth,
          std::max(1.0f, out.leftColumn.bottom() - out.clockBlock.bottom() - gap),
      };

      const float centerUsable = std::max(1.0f, out.centerColumn.height - gap);
      const float heroHeight = centerUsable * 0.78f;
      out.heroCard = Rect{out.centerColumn.x, out.centerColumn.y, centerWidth, heroHeight};
      out.loginBlock = Rect{
          out.centerColumn.x,
          out.heroCard.bottom() + gap,
          centerWidth,
          std::max(1.0f, out.centerColumn.bottom() - out.heroCard.bottom() - gap),
      };
    } else {
      const float leftWidth = columnsWidth * 0.44f;
      const float rightWidth = std::max(1.0f, columnsWidth - leftWidth);
      out.leftColumn = Rect{out.canvas.x, out.canvas.y, leftWidth, out.canvas.height};
      out.rightColumn = Rect{out.leftColumn.right() + gap, out.canvas.y, rightWidth, out.canvas.height};

      const float leftUsable = std::max(1.0f, out.leftColumn.height - gap * 3.0f);
      const float identityHeight = leftUsable * 0.13f;
      const float clockHeight = leftUsable * 0.29f;
      const float mediaHeight = leftUsable * 0.27f;
      out.identityCard = Rect{out.leftColumn.x, out.leftColumn.y, leftWidth, identityHeight};
      out.clockBlock = Rect{out.leftColumn.x, out.identityCard.bottom() + gap, leftWidth, clockHeight};
      out.mediaCard = Rect{out.leftColumn.x, out.clockBlock.bottom() + gap, leftWidth, mediaHeight};
      out.loginBlock = Rect{
          out.leftColumn.x,
          out.mediaCard.bottom() + gap,
          leftWidth,
          std::max(1.0f, out.leftColumn.bottom() - out.mediaCard.bottom() - gap),
      };
    }

    const float rightUsable = std::max(1.0f, out.rightColumn.height - gap * 2.0f);
    const float calendarHeight = rightUsable * 0.50f;
    const float weatherHeight = rightUsable * 0.27f;
    out.calendarCard = Rect{out.rightColumn.x, out.rightColumn.y, out.rightColumn.width, calendarHeight};
    out.weatherCard = Rect{
        out.rightColumn.x,
        out.calendarCard.bottom() + gap,
        out.rightColumn.width,
        weatherHeight,
    };
    out.metricsCard = Rect{
        out.rightColumn.x,
        out.weatherCard.bottom() + gap,
        out.rightColumn.width,
        std::max(1.0f, out.rightColumn.bottom() - out.weatherCard.bottom() - gap),
    };
    return out;
  }

} // namespace lockscreen_layout
