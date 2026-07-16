#pragma once

#include <algorithm>
#include <cstddef>

namespace launcher_visual {

  [[nodiscard]] inline float dynamicListHeight(
      float chromeHeight, float rowHeight, float rowGap, std::size_t rowCount, std::size_t maxVisibleRows,
      float emptyHeight = 28.0f, float minHeight = 108.0f, float maxHeight = 540.0f
  ) noexcept {
    const std::size_t visibleRows = std::min(rowCount, maxVisibleRows);
    const float gaps = visibleRows > 1 ? rowGap * static_cast<float>(visibleRows - 1) : 0.0f;
    const float resultsHeight = visibleRows == 0
        ? emptyHeight
        : std::max(1.0f, rowHeight) * static_cast<float>(visibleRows) + gaps;
    return std::clamp(chromeHeight + resultsHeight, minHeight, maxHeight);
  }

} // namespace launcher_visual
