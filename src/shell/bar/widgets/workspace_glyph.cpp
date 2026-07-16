#include "shell/bar/widgets/workspace_glyph.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>

namespace workspace_glyph {
  namespace {
    std::string lower(std::string_view value) {
      std::string result(value);
      std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return result;
    }

    bool containsAny(std::string_view value, std::initializer_list<std::string_view> needles) {
      return std::ranges::any_of(needles, [value](std::string_view needle) { return value.contains(needle); });
    }
  } // namespace

  std::string forApp(std::string_view appId, std::string_view desktopCategories) {
    const std::string id = lower(appId);
    const std::string categories = lower(desktopCategories);

    if (containsAny(id, {"terminal", "alacritty", "kitty", "foot", "wezterm", "konsole", "gnome-console"})
        || categories.contains("terminalemulator")) {
      return "terminal";
    }
    if (containsAny(id, {"firefox", "chrom", "brave", "vivaldi", "opera", "browser", "zen"})
        || containsAny(categories, {"webbrowser", "network"})) {
      return "web";
    }
    if (containsAny(id, {"code", "vscodium", "zed", "idea", "clion", "pycharm", "neovim", "emacs"})
        || categories.contains("development")) {
      return "code";
    }
    if (containsAny(id, {"vlc", "mpv", "spotify", "music", "video", "player"})
        || containsAny(categories, {"audio", "video", "multimedia"})) {
      return "movie";
    }
    if (containsAny(id, {"gimp", "inkscape", "krita", "blender", "photo", "image"})
        || categories.contains("graphics")) {
      return "image";
    }
    if (containsAny(id, {"steam", "lutris", "heroic", "game"}) || categories.contains("game")) {
      return "sports_esports";
    }
    if (containsAny(id, {"libreoffice", "writer", "calc", "onlyoffice"}) || categories.contains("office")) {
      return "description";
    }
    if (containsAny(id, {"nautilus", "dolphin", "thunar", "nemo", "pcmanfm", "filemanager"})
        || categories.contains("filemanager")) {
      return "folder";
    }
    if (containsAny(id, {"settings", "control-center", "systemsettings"})
        || containsAny(categories, {"settings", "system"})) {
      return "settings";
    }
    return "app-window";
  }

  std::vector<std::string> limit(std::vector<std::string> glyphs, std::size_t maximum) {
    if (glyphs.size() > maximum) {
      glyphs.resize(maximum);
    }
    return glyphs;
  }

  StateTone stateTone(bool active, bool urgent, bool occupied) noexcept {
    if (urgent) {
      return StateTone::Urgent;
    }
    if (active) {
      return StateTone::Active;
    }
    return occupied ? StateTone::Occupied : StateTone::Empty;
  }

} // namespace workspace_glyph
