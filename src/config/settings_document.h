#pragma once

#include "core/toml.h"

#include <cstdint>
#include <optional>
#include <string>

namespace gnil::settings_document {

inline constexpr std::int64_t kSchemaVersion = 1;

// The public configuration format.  Runtime code still consumes Config while
// the settings UI is being moved over, so this module is the single adapter at
// that boundary; no legacy document is read or written beside settings.toml.
[[nodiscard]] toml::table defaults();
[[nodiscard]] std::optional<toml::table> toRuntimeOverrides(const toml::table& document, std::string* error = nullptr);
void syncFromRuntimeOverrides(toml::table& document, const toml::table& overrides);

} // namespace gnil::settings_document
