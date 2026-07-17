#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

// Material Symbols Rounded registry. Names resolve in this order:
// 1. Explicit codepoint literals such as U+E5CD or 0xE5CD.
// 2. Stable GNIL semantic aliases.
// 3. Native Material Symbol names (underscores; hyphens are normalized).
namespace GlyphRegistry {

  [[nodiscard]] bool contains(std::string_view name);
  [[nodiscard]] char32_t lookup(std::string_view name);

  // Full catalog loaded from the pinned official .codepoints file.
  [[nodiscard]] const std::unordered_map<std::string, char32_t>& materialSymbols();
  // Stable GNIL-facing name -> native Material Symbol name.
  [[nodiscard]] const std::unordered_map<std::string, std::string_view>& aliases();

} // namespace GlyphRegistry
