#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace gnil::theme {

  // Color generation strategies. The first nine are Material Design 3 schemes
  // (TonalPalette + tone tables, built on top of material_color_utilities).
  // The last five are custom HSL-space generators with very different
  // aesthetics — they are not Material You and will produce different output.
  enum class Scheme {
    TonalSpot,
    Vibrant,
    Expressive,
    Fidelity,
    Content,
    FruitSalad,
    Rainbow,
    Neutral,
    Monochrome,
    CustomVibrant,
    Faithful,
    Soft,
    Dysfunctional,
    Muted,
  };

  // True for the Material Design 3 schemes.
  constexpr bool isMaterialScheme(Scheme s) {
    return s == Scheme::TonalSpot
        || s == Scheme::Vibrant
        || s == Scheme::Expressive
        || s == Scheme::Fidelity
        || s == Scheme::Content
        || s == Scheme::FruitSalad
        || s == Scheme::Rainbow
        || s == Scheme::Neutral
        || s == Scheme::Monochrome;
  }

  // Parse a scheme from its CLI string (e.g. "m3-tonal-spot", "vibrant").
  // Returns nullopt for unknown values.
  std::optional<Scheme> schemeFromString(std::string_view s);

  // String form used in CLI / JSON output.
  std::string_view schemeToString(Scheme s);

} // namespace gnil::theme
