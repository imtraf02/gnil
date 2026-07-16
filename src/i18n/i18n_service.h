#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace i18n {

  // Transparent hash/eq so Catalog::find() can take string_view without
  // allocating a temporary std::string.
  struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    std::size_t operator()(const std::string& s) const noexcept { return std::hash<std::string_view>{}(s); }
    std::size_t operator()(const char* s) const noexcept { return std::hash<std::string_view>{}(s); }
  };

  struct StringEq {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
  };

  using Catalog = std::unordered_map<std::string, std::string, StringHash, StringEq>;

  // GNIL is English-only. The catalog remains an internal keyed-string table
  // while inherited UI call sites are gradually simplified.
  class Service {
  public:
    static Service& instance();

    // The argument is retained for source compatibility and deliberately
    // ignored: GNIL never reads locale environment variables or user language
    // configuration.
    void init(std::string_view preferredLang = {});

    [[nodiscard]] std::string_view language() const noexcept { return m_language; }

    // Returns a view into the active or fallback catalog, or {} if the key
    // exists in neither.
    [[nodiscard]] std::string_view lookup(std::string_view dottedKey) const;

  private:
    bool loadCatalog(std::string_view lang, Catalog& out) const;

    Catalog m_active;
    std::string m_language;
  };

} // namespace i18n
