#include "theme/cli.h"

#include "core/toml.h" // IWYU pragma: keep
#include "theme/color.h"
#include "theme/fixed_palette.h"
#include "theme/image_loader.h"
#include "theme/json_output.h"
#include "theme/palette_generator.h"
#include "theme/palette_transform.h"
#include "theme/scheme.h"
#include "util/file_utils.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <print>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace gnil::theme {

  namespace {

    constexpr const char* kHelpText =
        "Usage: gnil theme <image> [options]\n"
        "\n"
        "Generate a color palette from an image.\n"
        "\n"
        "Options:\n"
        "  --scheme <name>   Palette generation scheme\n"
        "  --dark|--light|--both  Select emitted palette variant\n"
        "  --pure-black      Re-anchor the dark surface ramp to true black\n"
        "  --theme-json <f>  Load precomputed dark/light token maps from JSON\n"
        "  -o <file>         Write JSON to file instead of stdout\n";

    using TokenMap = std::unordered_map<std::string, uint32_t>;


    std::optional<Color> loadHexColor(const nlohmann::json& src, const char* key) {
      if (!src.contains(key) || !src[key].is_string())
        return std::nullopt;
      try {
        return Color::fromHex(src[key].get<std::string>());
      } catch (...) {
        return std::nullopt;
      }
    }

    void setToken(TokenMap& dst, std::string_view key, std::string_view hex) {
      dst[std::string(key)] = Color::fromHex(hex).toArgb();
    }

    std::optional<::Palette> parseFixedPaletteJson(const nlohmann::json& src, std::string& err) {
      const auto primary = loadHexColor(src, "mPrimary");
      const auto onPrimary = loadHexColor(src, "mOnPrimary");
      const auto secondary = loadHexColor(src, "mSecondary");
      const auto onSecondary = loadHexColor(src, "mOnSecondary");
      const auto tertiary = loadHexColor(src, "mTertiary");
      const auto onTertiary = loadHexColor(src, "mOnTertiary");
      const auto error = loadHexColor(src, "mError");
      const auto onError = loadHexColor(src, "mOnError");
      const auto surface = loadHexColor(src, "mSurface");
      const auto onSurface = loadHexColor(src, "mOnSurface");
      const auto surfaceVariant = loadHexColor(src, "mSurfaceVariant");
      const auto onSurfaceVariant = loadHexColor(src, "mOnSurfaceVariant");
      const auto outlineRaw = loadHexColor(src, "mOutline");
      const auto shadow = loadHexColor(src, "mShadow").value_or(surface.value_or(Color{}));

      if (!primary
          || !onPrimary
          || !secondary
          || !onSecondary
          || !tertiary
          || !onTertiary
          || !error
          || !onError
          || !surface
          || !onSurface
          || !surfaceVariant
          || !onSurfaceVariant
          || !outlineRaw) {
        err = "fixed palette json is missing required colors";
        return std::nullopt;
      }
      return ::Palette{
          .primary = rgbHex(primary->toArgb() & 0x00FFFFFFU),
          .onPrimary = rgbHex(onPrimary->toArgb() & 0x00FFFFFFU),
          .secondary = rgbHex(secondary->toArgb() & 0x00FFFFFFU),
          .onSecondary = rgbHex(onSecondary->toArgb() & 0x00FFFFFFU),
          .tertiary = rgbHex(tertiary->toArgb() & 0x00FFFFFFU),
          .onTertiary = rgbHex(onTertiary->toArgb() & 0x00FFFFFFU),
          .error = rgbHex(error->toArgb() & 0x00FFFFFFU),
          .onError = rgbHex(onError->toArgb() & 0x00FFFFFFU),
          .surface = rgbHex(surface->toArgb() & 0x00FFFFFFU),
          .onSurface = rgbHex(onSurface->toArgb() & 0x00FFFFFFU),
          .surfaceVariant = rgbHex(surfaceVariant->toArgb() & 0x00FFFFFFU),
          .onSurfaceVariant = rgbHex(onSurfaceVariant->toArgb() & 0x00FFFFFFU),
          .outline = rgbHex(outlineRaw->toArgb() & 0x00FFFFFFU),
          .shadow = rgbHex(shadow.toArgb() & 0x00FFFFFFU),
          .hover = rgbHex(tertiary->toArgb() & 0x00FFFFFFU),
          .onHover = rgbHex(onTertiary->toArgb() & 0x00FFFFFFU),
      };
    }

    void injectTerminalColors(TokenMap& dst, const nlohmann::json& modeJson) {
      if (!modeJson.contains(kTerminalJsonKey) || !modeJson[kTerminalJsonKey].is_object())
        return;
      const auto& terminal = modeJson[kTerminalJsonKey];
      for (const auto& [jsonKey, flatKey] : kTerminalDirectColorTokenKeys) {
        if (terminal.contains(jsonKey) && terminal[jsonKey].is_string())
          setToken(dst, flatKey, terminal[jsonKey].get<std::string>());
      }
      for (const auto& group : kTerminalAnsiGroupTokenKeys) {
        if (!terminal.contains(group.jsonKey) || !terminal[group.jsonKey].is_object())
          continue;
        for (const auto key : kTerminalAnsiColorJsonKeys) {
          const auto& groupJson = terminal[group.jsonKey];
          if (!groupJson.contains(key) || !groupJson[key].is_string())
            continue;
          setToken(dst, std::string(group.tokenPrefix) + "_" + std::string(key), groupJson[key].get<std::string>());
        }
      }
    }

    bool loadThemeJson(const std::filesystem::path& path, GeneratedPalette& palette, std::string& err) {
      std::ifstream f(path);
      if (!f) {
        err = "cannot open theme json";
        return false;
      }

      nlohmann::json root;
      try {
        f >> root;
      } catch (const std::exception& e) {
        err = e.what();
        return false;
      }

      auto loadTokenMode = [](const nlohmann::json& src, TokenMap& dst) {
        if (!src.is_object())
          return;
        for (auto it = src.begin(); it != src.end(); ++it) {
          if (!it.value().is_string())
            continue;
          try {
            dst[it.key()] = Color::fromHex(it.value().get<std::string>()).toArgb();
          } catch (...) {
          }
        }
      };

      auto loadFixedPalette = [&](const nlohmann::json& src, std::string_view mode, TokenMap& dst) -> bool {
        auto parsed = parseFixedPaletteJson(src, err);
        if (!parsed)
          return false;
        dst = expandFixedPaletteMode(*parsed, mode == "dark");
        injectTerminalColors(dst, src);
        return true;
      };

      auto isFixedPaletteMode = [](const nlohmann::json& src) { return src.is_object() && src.contains("mPrimary"); };

      if (root.contains("dark") || root.contains("light")) {
        if (root.contains("dark")) {
          if (isFixedPaletteMode(root["dark"])) {
            if (!loadFixedPalette(root["dark"], "dark", palette.dark))
              return false;
          } else {
            loadTokenMode(root["dark"], palette.dark);
          }
        }
        if (root.contains("light")) {
          if (isFixedPaletteMode(root["light"])) {
            if (!loadFixedPalette(root["light"], "light", palette.light))
              return false;
          } else {
            loadTokenMode(root["light"], palette.light);
          }
        }
      } else if (isFixedPaletteMode(root)) {
        if (!loadFixedPalette(root, "dark", palette.dark) || !loadFixedPalette(root, "light", palette.light))
          return false;
      } else {
        loadTokenMode(root, palette.dark);
      }

      if (palette.dark.empty() && palette.light.empty()) {
        err = "theme json contained no token maps";
        return false;
      }
      synthesizeTerminalPaletteTokens(palette);
      return true;
    }

  } // namespace

  int runCli(int argc, char* argv[]) {
    const char* imagePath = nullptr;
    const char* themeJsonPath = nullptr;
    std::string schemeName = "m3-tonal-spot";
    Variant variant = Variant::Dark;
    bool pureBlack = false;
    const char* outPath = nullptr;

    for (int i = 2; i < argc; ++i) {
      const char* a = argv[i];
      if (std::strcmp(a, "--help") == 0) {
        std::println("{}", kHelpText);
        return 0;
      }
      if (std::strcmp(a, "--scheme") == 0 && i + 1 < argc) {
        schemeName = argv[++i];
        continue;
      }
      if (std::strcmp(a, "--theme-json") == 0 && i + 1 < argc) {
        themeJsonPath = argv[++i];
        continue;
      }
      if (std::strcmp(a, "--dark") == 0) {
        variant = Variant::Dark;
        continue;
      }
      if (std::strcmp(a, "--light") == 0) {
        variant = Variant::Light;
        continue;
      }
      if (std::strcmp(a, "--both") == 0) {
        variant = Variant::Both;
        continue;
      }
      if (std::strcmp(a, "--pure-black") == 0) {
        pureBlack = true;
        continue;
      }
      if (std::strcmp(a, "-o") == 0 && i + 1 < argc) {
        outPath = argv[++i];
        continue;
      }
      if (!imagePath && a[0] != '-') {
        imagePath = a;
        continue;
      }
      std::println(stderr, "error: unknown theme argument: {}", a);
      return 1;
    }

    if (!imagePath && !themeJsonPath) {
      std::println(stderr, "error: theme requires an image path or --theme-json (try: gnil theme --help)");
      return 1;
    }

    auto schemeOpt = schemeFromString(schemeName);
    if (!schemeOpt) {
      std::println(stderr, "error: unknown scheme '{}'", schemeName);
      return 1;
    }

    std::string err;
    GeneratedPalette palette;
    if (themeJsonPath) {
      if (!loadThemeJson(FileUtils::expandUserPath(themeJsonPath), palette, err)) {
        std::println(stderr, "error: failed to load theme json: {}", err);
        return 1;
      }
    } else {
      auto loaded = loadAndResize(imagePath, *schemeOpt);
      if (!loaded) {
        std::println(stderr, "error: failed to load image: {}", loaded.error());
        return 1;
      }

      auto generated = generate(loaded->rgb, *schemeOpt);
      if (!generated) {
        std::println(stderr, "error: palette generation failed: {}", generated.error());
        return 1;
      }
      palette = std::move(*generated);
    }

    if (pureBlack) {
      applyPureBlackDark(palette);
    }

    const std::string json = toJson(palette, *schemeOpt, variant);
    if (outPath) {
      std::ofstream f(outPath);
      if (!f) {
        std::println(stderr, "error: cannot open output file: {}", outPath);
        return 1;
      }
      f << json << '\n';
    } else {
      std::fwrite(json.data(), 1, json.size(), stdout);
      std::fputc('\n', stdout);
    }

    return 0;
  }

} // namespace gnil::theme
