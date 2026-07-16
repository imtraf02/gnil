#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace noctalia::util {

  // Lowercase hex MD5 of a file's contents. Empty string if the file cannot be read.
  [[nodiscard]] std::string fileMd5Hex(const std::filesystem::path& path);
  [[nodiscard]] std::string stringMd5Hex(std::string_view value);

} // namespace noctalia::util
