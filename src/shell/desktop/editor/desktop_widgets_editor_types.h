#pragma once

#include "config/config_types.h"

#include <string_view>

// Shared layout snapshot for the desktop widget editor.
using DesktopWidgetsEditorSnapshot = DesktopWidgetsConfig;

struct DesktopWidgetsEditorProfile {
  std::string_view logSection;
  std::string_view layerNamespace;
  std::string_view widgetIdPrefix;
  bool showLockscreenLoginPreview = false;

  [[nodiscard]] static DesktopWidgetsEditorProfile desktop();
};
