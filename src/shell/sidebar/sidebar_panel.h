#pragma once

#include "shell/notification/notification_card_model.h"
#include "shell/panel/panel.h"
#include "ui/controls/scroll_view.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>

class Button;
class ConfigService;
class Flex;
class NotificationManager;
class Renderer;

// Full-height notification dock attached to the right output frame. `sidebar`
// is its canonical public id; `notifications` remains an IPC compatibility alias.
class SidebarPanel final : public Panel {
public:
  SidebarPanel(ConfigService* config, NotificationManager* notifications);

  void create() override;
  void onClose() override;
  [[nodiscard]] bool dismissTransientUi() override;

  [[nodiscard]] float preferredWidth() const override { return scaled(420.0f); }
  [[nodiscard]] float preferredHeight() const override { return scaled(900.0f); }
  [[nodiscard]] bool fillsHeight() const noexcept override { return true; }
  [[nodiscard]] LayerShellLayer layer() const override { return LayerShellLayer::Top; }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::OnDemand; }
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override { return PanelPlacement::Floating; }
  [[nodiscard]] std::string panelScreenPosition() const override { return "top_right"; }
  [[nodiscard]] ChromeEdge chromeEdge() const noexcept override { return ChromeEdge::Right; }
  [[nodiscard]] bool isContextActive(std::string_view context) const override {
    return context.empty() || context == "sidebar" || context == "notifications";
  }

private:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void rebuildContent(Renderer& renderer);
  std::unique_ptr<Flex> buildGroup(Renderer& renderer, const notification_card::Group& group);
  std::unique_ptr<Flex> buildCard(Renderer& renderer, const Notification& notification);
  void clearAll();
  void clearGroup(std::string key);
  void toggleGroup(std::string key);
  void toggleNotification(uint32_t id);
  void beginReply(uint32_t id);
  void endReply(bool resumeTimeout);
  void submitReply(uint32_t id, const std::string& text);
  void markContentDirty();

  ConfigService* m_config = nullptr;
  NotificationManager* m_notifications = nullptr;
  Flex* m_root = nullptr;
  Flex* m_content = nullptr;
  Button* m_dndButton = nullptr;
  Button* m_clearAllButton = nullptr;
  ScrollView* m_scroll = nullptr;
  ScrollViewState m_scrollState;
  std::unordered_set<std::string> m_expandedGroups;
  std::unordered_set<uint32_t> m_expandedNotifications;
  std::optional<uint32_t> m_replyNotificationId;
  std::uint64_t m_lastSerial = 0;
  bool m_contentDirty = true;
};
