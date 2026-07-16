#pragma once

#include "core/timer_manager.h"
#include "dbus/tray/tray_service.h"
#include "shell/panel/panel.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class ConfigService;
class ContextMenuControl;
class Node;
class Renderer;
class ScrollView;

// DBus tray menus live in the same persistent bar-panel host as every other
// rail popout. Submenus replace content inside this panel instead of creating
// cascading xdg-popup surfaces.
class TrayMenu final : public Panel {
public:
  TrayMenu(ConfigService* config, TrayService* tray);

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  void onConfigReloaded() override;
  [[nodiscard]] bool isContextActive(std::string_view context) const override;
  [[nodiscard]] bool handleGlobalKey(
      std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit
  ) override;
  [[nodiscard]] bool dismissTransientUi() override;
  [[nodiscard]] float preferredWidth() const override { return scaled(300.0f); }
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] bool usesDynamicVisualSize() const noexcept override { return true; }
  [[nodiscard]] float initialVisualHeight() const override { return scaled(160.0f); }
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override { return PanelPlacement::Attached; }
  [[nodiscard]] LayerShellLayer layer() const override { return LayerShellLayer::Overlay; }

  void onTrayChanged();

private:
  struct Level {
    std::int32_t parentId = 0;
    std::vector<TrayMenuEntry> entries;
  };

  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void refreshCurrentLevel();
  void rebuildMenu(bool forward = true, bool animate = true);
  void openSubmenu(std::int32_t parentId);
  bool goBack();
  void activate(std::int32_t entryId);
  void scheduleEntryRetry(int attempt);
  void requestIntrinsicHeight(bool animate = true);
  [[nodiscard]] std::vector<TrayMenuEntry> rootEntries();
  [[nodiscard]] std::vector<TrayMenuEntry> currentEntries() const;
  [[nodiscard]] std::optional<TrayItemInfo> activeTrayItem() const;
  [[nodiscard]] bool activeItemPinned() const;
  bool toggleActiveItemPinned();

  ConfigService* m_config = nullptr;
  TrayService* m_tray = nullptr;
  std::string m_activeItemId;
  std::vector<Level> m_levels;
  ScrollView* m_scrollView = nullptr;
  Node* m_menuTransition = nullptr;
  ContextMenuControl* m_menu = nullptr;
  float m_lastRequestedHeight = 0.0f;
  bool m_needsRefresh = false;
  Timer m_retryTimer;
};
