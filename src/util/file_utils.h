#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FileUtils {

  [[nodiscard]] inline std::filesystem::path expandUserPath(const std::string& path) {
    if (path.empty() || path[0] != '~') {
      return std::filesystem::path(path);
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') {
      return std::filesystem::path(path);
    }
    if (path.size() == 1) {
      return std::filesystem::path(home);
    }
    if (path[1] == '/') {
      return std::filesystem::path(home) / path.substr(2);
    }
    return std::filesystem::path(path);
  }

  // Expands $NAME and ${NAME} environment-variable references. NAME matches
  // [A-Za-z_][A-Za-z0-9_]*; undefined variables expand to the empty string. A
  // lone '$' or '$' followed by a non-name character is left verbatim. This is a
  // deliberate, minimal substitution — never wordexp()/shell expansion, which
  // would run command substitution on config input.
  [[nodiscard]] inline std::string expandEnvVars(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    const auto isNameStart = [](char c) { return (std::isalpha(static_cast<unsigned char>(c)) != 0) || c == '_'; };
    const auto isNameChar = [](char c) { return (std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '_'; };

    for (std::size_t i = 0; i < in.size();) {
      if (in[i] != '$') {
        out.push_back(in[i]);
        ++i;
        continue;
      }
      // in[i] == '$'
      if (i + 1 < in.size() && in[i + 1] == '{') {
        const std::size_t close = in.find('}', i + 2);
        if (close != std::string_view::npos) {
          const std::string name(in.substr(i + 2, close - (i + 2)));
          if (const char* val = std::getenv(name.c_str()); val != nullptr) {
            out.append(val);
          }
          i = close + 1;
          continue;
        }
        // No closing brace — emit '$' verbatim and continue.
        out.push_back('$');
        ++i;
        continue;
      }
      if (i + 1 < in.size() && isNameStart(in[i + 1])) {
        std::size_t j = i + 1;
        while (j < in.size() && isNameChar(in[j])) {
          ++j;
        }
        const std::string name(in.substr(i + 1, j - (i + 1)));
        if (const char* val = std::getenv(name.c_str()); val != nullptr) {
          out.append(val);
        }
        i = j;
        continue;
      }
      // Lone '$' — leave verbatim.
      out.push_back('$');
      ++i;
    }
    return out;
  }


  // Expand XDG base-directory tokens ($XDG_CONFIG_HOME, etc.) to their known
  // defaults under ~ for user-friendly display. Paths that already start with
  // $HOME are abbreviated to ~. Dynamic (script-resolved) paths are left as-is.
  [[nodiscard]] inline std::string xdgPathForDisplay(std::string_view raw) {
    struct Mapping {
      std::string_view token;
      std::string_view homeDefault;
    };
    static constexpr std::array<Mapping, 4> kMappings = {{
        {"$XDG_CONFIG_HOME", ".config"},
        {"$XDG_DATA_HOME", ".local/share"},
        {"$XDG_STATE_HOME", ".local/state"},
        {"$XDG_CACHE_HOME", ".cache"},
    }};
    std::string expanded(raw);
    for (const auto& m : kMappings) {
      if (!expanded.starts_with(m.token))
        continue;
      if (expanded.size() != m.token.size() && expanded[m.token.size()] != '/')
        continue;
      expanded = "~/" + std::string(m.homeDefault) + expanded.substr(m.token.size());
      break;
    }
    if (!expanded.starts_with("~/")) {
      const char* home = std::getenv("HOME");
      if (home != nullptr && home[0] != '\0') {
        const std::string homeStr(home);
        if (expanded.starts_with(homeStr) && (expanded.size() == homeStr.size() || expanded[homeStr.size()] == '/')) {
          expanded = "~" + expanded.substr(homeStr.size());
        }
      }
    }
    return expanded;
  }

  [[nodiscard]] inline bool
  containsPath(const std::vector<std::filesystem::path>& paths, const std::filesystem::path& path) {
    return std::ranges::contains(paths, path);
  }

  [[nodiscard]] inline std::optional<std::string> readSmallTextFile(const std::filesystem::path& path) {
    std::ifstream file{path};
    if (!file.is_open()) {
      return std::nullopt;
    }

    std::string text;
    std::getline(file, text);
    if (text.empty()) {
      return std::nullopt;
    }

    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) {
      text.pop_back();
    }
    return text;
  }

  [[nodiscard]] inline std::string configDir() {
    const char* gnil = std::getenv("GNIL_CONFIG_HOME");
    if (gnil != nullptr && gnil[0] != '\0') {
      return std::string(gnil) + "/gnil";
    }
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] != '\0') {
      return std::string(xdg) + "/gnil";
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
      return std::string(home) + "/.config/gnil";
    }
    return {};
  }

  [[nodiscard]] inline std::string stateDir() {
    const char* gnil = std::getenv("GNIL_STATE_HOME");
    if (gnil != nullptr && gnil[0] != '\0') {
      return std::string(gnil) + "/gnil";
    }
    const char* xdg = std::getenv("XDG_STATE_HOME");
    if (xdg != nullptr && xdg[0] != '\0') {
      return std::string(xdg) + "/gnil";
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
      return std::string(home) + "/.local/state/gnil";
    }
    return {};
  }

  [[nodiscard]] inline std::string dataDir() {
    const char* gnil = std::getenv("GNIL_DATA_HOME");
    if (gnil != nullptr && gnil[0] != '\0') {
      return std::string(gnil) + "/gnil";
    }
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg != nullptr && xdg[0] != '\0') {
      return std::string(xdg) + "/gnil";
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
      return std::string(home) + "/.local/share/gnil";
    }
    return {};
  }

  [[nodiscard]] inline std::string cacheDir() {
    const char* gnil = std::getenv("GNIL_CACHE_HOME");
    if (gnil != nullptr && gnil[0] != '\0') {
      return std::string(gnil) + "/gnil";
    }
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg != nullptr && xdg[0] != '\0') {
      return std::string(xdg) + "/gnil";
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
      return std::string(home) + "/.cache/gnil";
    }
    return {};
  }

  // Git-source repo caches. Host-managed, re-fetchable, so they live under the
  // state dir — never config.
  [[nodiscard]] inline std::string pluginSourcesDir() {
    const std::string base = stateDir();
    if (base.empty()) {
      return {};
    }
    return base + "/plugins/sources";
  }

  // Exported runtime files for enabled git-source plugins. Re-derivable from
  // source repos; path sources and local dev plugins do not use this directory.
  [[nodiscard]] inline std::string pluginMaterializedDir() {
    const std::string base = stateDir();
    if (base.empty()) {
      return {};
    }
    return base + "/plugins/materialized";
  }

  [[nodiscard]] inline std::vector<std::uint8_t> readBinaryFile(const std::string& path) {
    if (path.empty()) {
      return {};
    }

    std::error_code ec;
    const std::filesystem::path fsPath = expandUserPath(path);
    if (!std::filesystem::is_regular_file(fsPath, ec) || ec) {
      return {};
    }

    const std::uintmax_t fileSize = std::filesystem::file_size(fsPath, ec);
    if (ec || fileSize == 0) {
      return {};
    }

    constexpr std::uintmax_t kMaxBinaryReadBytes = 256ULL * 1024ULL * 1024ULL;
    if (fileSize > kMaxBinaryReadBytes) {
      return {};
    }

    std::ifstream file(fsPath, std::ios::binary);
    if (!file) {
      return {};
    }

    std::vector<std::uint8_t> data(static_cast<std::size_t>(fileSize));
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(fileSize));
    if (!file) {
      return {};
    }
    return data;
  }

  // Expands ~ and resolves relative paths against baseDir when provided; otherwise uses the
  // process working directory for relative paths.
  [[nodiscard]] inline std::filesystem::path
  resolvePath(std::string_view path, std::optional<std::string_view> baseDir = std::nullopt) {
    if (path.empty() || path.starts_with("color:")) {
      return std::filesystem::path(path);
    }

    std::filesystem::path resolved = expandUserPath(std::string(path));
    if (!resolved.is_absolute()) {
      if (baseDir.has_value() && !baseDir->empty()) {
        resolved = std::filesystem::path(*baseDir) / resolved;
      } else {
        std::error_code ec;
        resolved = std::filesystem::absolute(resolved, ec);
        if (ec) {
          return std::filesystem::path(path);
        }
      }
    }

    std::error_code ec;
    return resolved.lexically_normal();
  }

  [[nodiscard]] inline std::string normalizeWallpaperPath(std::string_view path) {
    if (path.empty() || path.starts_with("color:")) {
      return std::string(path);
    }

    std::error_code ec;
    auto absolute = std::filesystem::absolute(std::filesystem::path(path), ec);
    if (ec) {
      absolute = std::filesystem::path(path);
    }
    return expandUserPath(absolute.lexically_normal().string()).string();
  }

} // namespace FileUtils
