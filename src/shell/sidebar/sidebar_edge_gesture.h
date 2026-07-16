#pragma once

#include <algorithm>

namespace sidebar_edge_gesture {

  [[nodiscard]] inline bool revealReached(float startX, float currentX, float threshold) noexcept {
    return startX - currentX >= std::max(1.0f, threshold);
  }

} // namespace sidebar_edge_gesture
