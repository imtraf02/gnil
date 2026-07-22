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
    const float outerMargin = std::clamp(shortEdge * 0.025f, 10.0f, 28.0f);
    const float maxIslandWidth = std::max(1.0f, sw - outerMargin * 2.0f);
    const float maxIslandHeight = std::max(1.0f, sh - outerMargin * 2.0f);
    const float islandHeight = std::min(maxIslandHeight, sh * 0.70f);
    const float islandWidth = std::min(maxIslandWidth, islandHeight * (16.0f / 9.0f));
    const float islandX = std::round((sw - islandWidth) * 0.5f);
    const float islandY = std::round((sh - islandHeight) * 0.5f);
    const float gap = std::clamp(islandHeight * 0.014f, 6.0f, 14.0f);

    Layout out;
    out.scale = std::clamp(sh / 1440.0f, 0.62f, 1.0f);
    out.gap = gap;
    out.radius = std::clamp(islandHeight * 0.045f, 18.0f, 36.0f);
    out.island = Rect{islandX, islandY, islandWidth, islandHeight};
    out.fullDashboard = islandWidth >= 840.0f && islandHeight >= 480.0f;

    if (!out.fullDashboard) {
      const float centerWidth = std::min(480.0f * out.scale, std::max(1.0f, islandWidth - gap * 2.0f));
      out.centerColumn = Rect{
          std::round((sw - centerWidth) * 0.5f),
          islandY + gap,
          centerWidth,
          std::max(1.0f, islandHeight - gap * 2.0f),
      };
      return out;
    }

    const float contentX = out.island.x + gap;
    const float contentY = out.island.y + gap;
    const float contentWidth = std::max(1.0f, out.island.width - gap * 2.0f);
    const float contentHeight = std::max(1.0f, out.island.height - gap * 2.0f);
    const float columnsWidth = std::max(1.0f, contentWidth - gap * 2.0f);
    const float leftWidth = columnsWidth * 0.30f;
    const float centerWidth = columnsWidth * 0.40f;
    const float rightWidth = std::max(1.0f, columnsWidth - leftWidth - centerWidth);

    out.leftColumn = Rect{contentX, contentY, leftWidth, contentHeight};
    out.centerColumn = Rect{out.leftColumn.right() + gap, contentY, centerWidth, contentHeight};
    out.rightColumn = Rect{out.centerColumn.right() + gap, contentY, rightWidth, contentHeight};

    const float leftUsable = std::max(1.0f, contentHeight - gap * 2.0f);
    const float weatherHeight = leftUsable * 0.23f;
    const float systemHeight = leftUsable * 0.55f;
    const float mediaHeight = std::max(1.0f, leftUsable - weatherHeight - systemHeight);
    out.weatherCard = Rect{contentX, contentY, leftWidth, weatherHeight};
    out.systemCard = Rect{contentX, out.weatherCard.bottom() + gap, leftWidth, systemHeight};
    out.mediaCard = Rect{contentX, out.systemCard.bottom() + gap, leftWidth, mediaHeight};

    const float rightUsable = std::max(1.0f, contentHeight - gap);
    const float metricsHeight = rightUsable * 0.55f;
    out.metricsCard = Rect{out.rightColumn.x, contentY, rightWidth, metricsHeight};
    out.notificationsCard = Rect{
        out.rightColumn.x,
        out.metricsCard.bottom() + gap,
        rightWidth,
        std::max(1.0f, rightUsable - metricsHeight),
    };
    return out;
  }

} // namespace lockscreen_layout
