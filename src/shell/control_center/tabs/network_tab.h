#pragma once

#include "core/timer_manager.h"
#include "dbus/network/network_secret_agent.h"
#include "dbus/network/network_types.h"
#include "render/animation/animation_manager.h"
#include "shell/control_center/tab.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class AccessPointRow;
class Button;
class ConfigService;
class ContextMenuPopup;
class Flex;
class Glyph;
class Input;
class InputArea;
class Label;
class RenderContext;
class ScrollView;
class Spinner;
class Toggle;
class INetworkService;
class WaylandConnection;
struct ContextMenuControlEntry;

enum class NetworkTabPresentation : std::uint8_t {
  ControlCenter,
  ReferencePanel,
};

class NetworkTab : public Tab {
public:
  NetworkTab(
      INetworkService* network, NetworkSecretAgent* secrets, ConfigService* config = nullptr,
      WaylandConnection* wayland = nullptr, RenderContext* renderContext = nullptr,
      NetworkTabPresentation presentation = NetworkTabPresentation::ControlCenter
  );
  ~NetworkTab() override;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void setActive(bool active) override;
  void onClose() override;
  bool dismissTransientUi() override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doPrepareIntrinsicLayout(Renderer& renderer, float contentWidth, float maxBodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;

  void syncCurrentCard();
  void beginPendingAction(bool wasConnected);
  void rebuildApList(Renderer& renderer);
  // Pushes live signal values into the existing rows. Returns true if any changed.
  bool syncApRows();
  void syncPasswordCard();
  void showPasswordPrompt(const NetworkSecretAgent::SecretRequest& request);
  void showPasswordPrompt(const AccessPointInfo& ap);
  void submitPasswordPrompt(const std::string& value);
  void cancelPasswordPrompt();
  void clearPasswordPrompt();
  void openVpnMenu();
  void openAccessPointMenu(const AccessPointInfo& ap, Button* anchor);
  void openContextMenu(
      Button* anchor, std::vector<ContextMenuControlEntry> entries,
      std::function<void(const ContextMenuControlEntry&)> onActivate
  );
  void closeContextMenu();
  void openPasswordSheet();
  void closePasswordSheet(bool animated);
  void setPasswordSheetProgress(float progress);
  void layoutPasswordSheet();
  [[nodiscard]] std::string
  structureKey(const std::vector<AccessPointInfo>& aps, const std::vector<VpnConnectionInfo>& vpns) const;

  INetworkService* m_network = nullptr;
  NetworkSecretAgent* m_secrets = nullptr;
  ConfigService* m_config = nullptr;
  WaylandConnection* m_wayland = nullptr;
  RenderContext* m_renderContext = nullptr;
  NetworkTabPresentation m_presentation = NetworkTabPresentation::ControlCenter;

  Flex* m_rootLayout = nullptr;
  Flex* m_currentCard = nullptr;
  Flex* m_currentIconBubble = nullptr;
  Glyph* m_currentGlyph = nullptr;
  Label* m_currentTitle = nullptr;
  Label* m_currentDetail = nullptr;
  Flex* m_connectedBadge = nullptr;
  Flex* m_passwordCard = nullptr;
  Flex* m_passwordOverlay = nullptr;
  InputArea* m_passwordScrim = nullptr;
  Label* m_passwordTitle = nullptr;
  Input* m_passwordInput = nullptr;
  Button* m_passwordRevealButton = nullptr;
  bool m_passwordRevealed = false;
  ScrollView* m_listScroll = nullptr;
  Flex* m_list = nullptr;

  Button* m_rescanButton = nullptr;
  Button* m_vpnMenuButton = nullptr;
  Toggle* m_wifiToggle = nullptr;
  Flex* m_currentRow = nullptr;
  Button* m_disconnectButton = nullptr;
  Spinner* m_scanSpinner = nullptr;
  bool m_vpnVisible = true;

  std::unique_ptr<ContextMenuPopup> m_contextMenuPopup;
  bool m_contextMenuOpen = false;
  bool m_passwordSheetOpen = false;
  float m_passwordSheetProgress = 0.0f;
  AnimationManager::Id m_passwordSheetAnimation = 0;
  float m_layoutWidth = 0.0f;
  float m_layoutHeight = 0.0f;

  std::unordered_map<std::string, AccessPointRow*> m_apRows;

  std::string m_lastStructureKey;
  float m_lastListWidth = -1.0f;

  bool m_hasPendingSecret = false;
  std::string m_pendingSsid;
  std::optional<AccessPointInfo> m_pendingAccessPoint;
  bool m_active = false;

  // Connect/disconnect stays disabled from click until the state flips (or a
  // timeout), so a click on stale state cannot fire the inverse action.
  bool m_actionPending = false;
  bool m_actionPendingConnected = false;
  std::chrono::steady_clock::time_point m_actionPendingSince;

};
