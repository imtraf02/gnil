#include "shell/lockscreen/lockscreen_layout.h"

#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace lockscreen_layout {

  namespace {

    float clampDimension(float value, float fallback) noexcept {
      return std::isfinite(value) && value > 0.0f ? value : fallback;
    }

    template <std::size_t N>
    std::array<float, N> distributeHeight(
        float available, const std::array<float, N>& minimums, const std::array<float, N>& weights
    ) noexcept {
      std::array<float, N> heights = minimums;
      const float totalMinimum = std::accumulate(minimums.begin(), minimums.end(), 0.0f);
      if (available <= totalMinimum) {
        if (totalMinimum <= 0.0f) {
          heights.fill(std::max(1.0f, available / static_cast<float>(N)));
          return heights;
        }
        const float fit = std::max(0.0f, available) / totalMinimum;
        for (auto& height : heights) {
          height = std::max(1.0f, height * fit);
        }
        return heights;
      }

      const float totalWeight = std::accumulate(weights.begin(), weights.end(), 0.0f);
      const float extra = available - totalMinimum;
      for (std::size_t i = 0; i < N; ++i) {
        const float weight = totalWeight > 0.0f ? weights[i] / totalWeight : 1.0f / static_cast<float>(N);
        heights[i] += extra * weight;
      }
      return heights;
    }

  } // namespace

  Layout resolve(float screenWidth, float screenHeight) noexcept {
    const float sw = clampDimension(screenWidth, 1.0f);
    const float sh = clampDimension(screenHeight, 1.0f);
    const float shortEdge = std::min(sw, sh);
    const float density = std::clamp(shortEdge / 900.0f, 0.72f, 1.0f);
    const float outerMargin = std::clamp(shortEdge * 0.03f, 16.0f, 40.0f);
    const float canvasWidth = std::min(std::max(1.0f, sw - outerMargin * 2.0f), 1680.0f);
    const float canvasHeight = std::min(std::max(1.0f, sh - outerMargin * 2.0f), 920.0f);
    const float canvasX = std::round((sw - canvasWidth) * 0.5f);
    const float canvasY = std::round((sh - canvasHeight) * 0.5f);
    const float gap = std::clamp(Style::spaceLg * density, 10.0f, 18.0f);

    Layout out;
    out.scale = std::clamp(std::min(sw / 1920.0f, sh / 1080.0f), 0.72f, 1.15f);
    out.density = density;
    out.outerMargin = outerMargin;
    out.gap = gap;
    out.cardPadding = std::clamp(Style::cardPadding * density, 10.0f, Style::cardPadding);
    out.contentGap = std::clamp(Style::spaceSm * density, 6.0f, Style::spaceSm);
    out.radius = std::clamp(18.0f * out.scale, 14.0f, 24.0f);
    out.canvas = Rect{canvasX, canvasY, canvasWidth, canvasHeight};

    const float avatarSize = std::clamp(56.0f * out.scale, 40.0f, 64.0f);
    const float artworkSize = std::clamp(72.0f * out.scale, 56.0f, 78.0f);
    const float identityMinimum = avatarSize + out.cardPadding * 2.0f;
    const float clockMinimum = std::clamp(120.0f * out.scale, 96.0f, 132.0f);
    const float mediaMinimum = artworkSize + Style::controlHeightSm + Style::fontSizeMini
        + out.cardPadding * 2.0f + out.contentGap * 2.0f;
    const float loginMinimum = Style::controlHeightLg + Style::controlHeightSm + Style::fontSizeCaption
        + out.contentGap * 2.0f;

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
      constexpr float kHeroMinimum = 88.0f;
      constexpr float kShortHeroMinimum = 64.0f;
      const float normalUsable = std::max(1.0f, out.centerColumn.height - gap * 3.0f);
      const float normalMinimum = identityMinimum + clockMinimum + kHeroMinimum + loginMinimum;
      const float shortMinimum = identityMinimum + clockMinimum + kShortHeroMinimum + loginMinimum;
      if (normalUsable >= normalMinimum) {
        const auto heights = distributeHeight(
            normalUsable,
            std::array{identityMinimum, clockMinimum, kHeroMinimum, loginMinimum},
            std::array{0.15f, 0.32f, 0.24f, 0.29f}
        );
        out.identityCard = Rect{out.centerColumn.x, out.centerColumn.y, compactWidth, heights[0]};
        out.clockBlock = Rect{out.centerColumn.x, out.identityCard.bottom() + gap, compactWidth, heights[1]};
        out.heroCard = Rect{out.centerColumn.x, out.clockBlock.bottom() + gap, compactWidth, heights[2]};
        out.loginBlock = Rect{out.centerColumn.x, out.heroCard.bottom() + gap, compactWidth, heights[3]};
      } else if (normalUsable >= shortMinimum) {
        const auto heights = distributeHeight(
            normalUsable,
            std::array{identityMinimum, clockMinimum, kShortHeroMinimum, loginMinimum},
            std::array{0.15f, 0.32f, 0.24f, 0.29f}
        );
        out.identityCard = Rect{out.centerColumn.x, out.centerColumn.y, compactWidth, heights[0]};
        out.clockBlock = Rect{out.centerColumn.x, out.identityCard.bottom() + gap, compactWidth, heights[1]};
        out.heroCard = Rect{out.centerColumn.x, out.clockBlock.bottom() + gap, compactWidth, heights[2]};
        out.loginBlock = Rect{out.centerColumn.x, out.heroCard.bottom() + gap, compactWidth, heights[3]};
      } else {
        out.showHero = false;
        const float usable = std::max(1.0f, out.centerColumn.height - gap * 2.0f);
        const auto heights = distributeHeight(
            usable,
            std::array{std::min(identityMinimum, 56.0f), 80.0f, std::min(loginMinimum, 92.0f)},
            std::array{0.20f, 0.48f, 0.32f}
        );
        out.identityCard = Rect{out.centerColumn.x, out.centerColumn.y, compactWidth, heights[0]};
        out.clockBlock = Rect{out.centerColumn.x, out.identityCard.bottom() + gap, compactWidth, heights[1]};
        out.heroCard = Rect{};
        out.loginBlock = Rect{out.centerColumn.x, out.clockBlock.bottom() + gap, compactWidth, heights[2]};
      }
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
      const auto leftHeights = distributeHeight(
          leftUsable,
          std::array{identityMinimum, clockMinimum, mediaMinimum},
          std::array{0.08f, 0.34f, 0.58f}
      );
      out.identityCard = Rect{out.leftColumn.x, out.leftColumn.y, leftWidth, leftHeights[0]};
      out.clockBlock = Rect{out.leftColumn.x, out.identityCard.bottom() + gap, leftWidth, leftHeights[1]};
      out.mediaCard = Rect{out.leftColumn.x, out.clockBlock.bottom() + gap, leftWidth, leftHeights[2]};

      const float centerUsable = std::max(1.0f, out.centerColumn.height - gap);
      const auto centerHeights = distributeHeight(
          centerUsable,
          std::array{std::clamp(240.0f * out.scale, 160.0f, 260.0f), loginMinimum},
          std::array{0.78f, 0.22f}
      );
      out.heroCard = Rect{out.centerColumn.x, out.centerColumn.y, centerWidth, centerHeights[0]};
      out.loginBlock = Rect{out.centerColumn.x, out.heroCard.bottom() + gap, centerWidth, centerHeights[1]};
    } else {
      const float leftWidth = columnsWidth * 0.44f;
      const float rightWidth = std::max(1.0f, columnsWidth - leftWidth);
      out.leftColumn = Rect{out.canvas.x, out.canvas.y, leftWidth, out.canvas.height};
      out.rightColumn = Rect{out.leftColumn.right() + gap, out.canvas.y, rightWidth, out.canvas.height};

      const float leftUsable = std::max(1.0f, out.leftColumn.height - gap * 3.0f);
      const auto leftHeights = distributeHeight(
          leftUsable,
          std::array{identityMinimum, clockMinimum, mediaMinimum, loginMinimum},
          std::array{0.10f, 0.30f, 0.30f, 0.30f}
      );
      out.identityCard = Rect{out.leftColumn.x, out.leftColumn.y, leftWidth, leftHeights[0]};
      out.clockBlock = Rect{out.leftColumn.x, out.identityCard.bottom() + gap, leftWidth, leftHeights[1]};
      out.mediaCard = Rect{out.leftColumn.x, out.clockBlock.bottom() + gap, leftWidth, leftHeights[2]};
      out.loginBlock = Rect{out.leftColumn.x, out.mediaCard.bottom() + gap, leftWidth, leftHeights[3]};
    }

    const float rightUsable = std::max(1.0f, out.rightColumn.height - gap * 2.0f);
    const auto rightHeights = distributeHeight(
        rightUsable,
        std::array{
            std::max(160.0f, 176.0f * density),
            std::max(120.0f, 132.0f * density),
            std::max(92.0f, 100.0f * density),
        },
        std::array{0.50f, 0.27f, 0.23f}
    );
    out.calendarCard = Rect{out.rightColumn.x, out.rightColumn.y, out.rightColumn.width, rightHeights[0]};
    out.weatherCard = Rect{
        out.rightColumn.x,
        out.calendarCard.bottom() + gap,
        out.rightColumn.width,
        rightHeights[1],
    };
    out.metricsCard = Rect{
        out.rightColumn.x,
        out.weatherCard.bottom() + gap,
        out.rightColumn.width,
        rightHeights[2],
    };
    return out;
  }

} // namespace lockscreen_layout
