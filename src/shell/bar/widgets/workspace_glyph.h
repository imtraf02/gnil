#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace workspace_glyph {

  enum class StateTone {
    Active,
    Urgent,
    Occupied,
    Empty,
  };

  [[nodiscard]] std::string forApp(std::string_view appId, std::string_view desktopCategories = {});
  [[nodiscard]] std::vector<std::string> limit(std::vector<std::string> glyphs, std::size_t maximum);
  [[nodiscard]] StateTone stateTone(bool active, bool urgent, bool occupied) noexcept;

} // namespace workspace_glyph
