#include "shell/desktop/editor/desktop_widgets_editor_types.h"

DesktopWidgetsEditorProfile DesktopWidgetsEditorProfile::desktop() {
  return DesktopWidgetsEditorProfile{
      .logSection = "desktop",
      .layerNamespace = "gnil-desktop-widgets-editor",
      .widgetIdPrefix = "desktop-widget-",
      .showLockscreenLoginPreview = false,
  };
}
