#include "render/text/glyph_registry.h"

#include <iostream>
#include <string>
#include <string_view>

namespace {
  bool check(bool condition, std::string_view message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
  }
} // namespace

int main() {
  bool ok = true;
  const auto& symbols = GlyphRegistry::materialSymbols();
  ok &= check(symbols.size() > 3000, "official Material Symbols catalog is loaded");
  ok &= check(GlyphRegistry::contains("close"), "native Material name resolves");
  ok &= check(GlyphRegistry::contains("arrow-back"), "hyphenated Material name normalizes");
  ok &= check(
      GlyphRegistry::lookup("shutdown") == GlyphRegistry::lookup("power_settings_new"),
      "semantic power alias resolves to Material"
  );
  ok &= check(GlyphRegistry::contains("wifi-0"), "zero-strength Wi-Fi state resolves to Material");
  ok &= check(GlyphRegistry::contains("wifi-question"), "unknown Wi-Fi state resolves to Material");
  ok &= check(GlyphRegistry::contains("wifi-exclamation"), "failed Wi-Fi state resolves to Material");
  ok &= check(
      GlyphRegistry::lookup("definitely-not-a-symbol") == GlyphRegistry::lookup("question_mark"),
      "unknown name uses Material placeholder"
  );
  for (const auto& [name, target] : GlyphRegistry::aliases()) {
    ok &= check(symbols.contains(std::string(target)), name);
  }
  return ok ? 0 : 1;
}
