#include "shell/bar/widgets/workspace_glyph.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
  void expect(bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      std::exit(1);
    }
  }
}

int main() {
  expect(workspace_glyph::forApp("org.wezfurlong.wezterm", "System;TerminalEmulator;") == "terminal", "terminal glyph");
  expect(workspace_glyph::forApp("firefox", "Network;WebBrowser;") == "web", "browser glyph");
  expect(workspace_glyph::forApp("dev.zed.Zed", "Development;") == "code", "development glyph");
  expect(workspace_glyph::forApp("org.videolan.VLC", "AudioVideo;Video;") == "movie", "media glyph");
  expect(workspace_glyph::forApp("org.kde.krita", "Graphics;") == "image", "graphics glyph");
  expect(workspace_glyph::forApp("steam", "Game;") == "sports_esports", "game glyph");
  expect(workspace_glyph::forApp("libreoffice-writer", "Office;") == "description", "office glyph");
  expect(workspace_glyph::forApp("thunar", "FileManager;") == "folder", "file manager glyph");
  expect(workspace_glyph::forApp("systemsettings", "Settings;System;") == "settings", "settings glyph");
  expect(workspace_glyph::forApp("unknown-app") == "app-window", "fallback glyph");

  std::vector<std::string> sameAppWindows{"web", "web", "web", "web", "web", "web"};
  const auto limited = workspace_glyph::limit(sameAppWindows, 5);
  expect(limited.size() == 5, "window glyph limit failed");
  expect(limited[0] == "web" && limited[4] == "web", "same-app windows were sorted or deduplicated");

  using workspace_glyph::StateTone;
  expect(workspace_glyph::stateTone(true, false, true) == StateTone::Active, "active workspace state");
  expect(workspace_glyph::stateTone(false, true, true) == StateTone::Urgent, "urgent workspace state");
  expect(workspace_glyph::stateTone(true, true, true) == StateTone::Urgent, "urgent state must remain visible when active");
  expect(workspace_glyph::stateTone(false, false, true) == StateTone::Occupied, "occupied workspace state");
  expect(workspace_glyph::stateTone(false, false, false) == StateTone::Empty, "empty workspace state");

  std::ifstream source(std::string(GNIL_SOURCE_ROOT) + "/src/shell/bar/widgets/workspaces_widget.cpp");
  const std::string text((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
  expect(text.find("ui::image") == std::string::npos, "workspace widget still creates an Image node");
  expect(text.find("IconResolver") == std::string::npos, "workspace widget still uses IconResolver");
  expect(text.find("m_activeIndicator") != std::string::npos, "workspace widget lost its shared active indicator");
  return 0;
}
