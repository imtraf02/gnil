#pragma once

#include "shell/settings/nexus_route.h"
#include "shell/settings/nexus_services.h"
#include "core/timer_manager.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Button;
class Flex;
class Input;
class InputArea;
class Label;
class Node;
class Renderer;
class ScrollView;
class Slider;
class Toggle;

enum class NexusNavigationMode {
  CompactIcons,
  LabeledSidebar,
};

class NexusView {
public:
  NexusView(NexusRoute& route, NexusServices services);

  [[nodiscard]] std::unique_ptr<Node>
  build(
      float scale, NexusNavigationMode navigationMode, std::function<void()> requestPip,
      std::function<void()> requestRefresh = {}, std::function<void()> requestClose = {}
  );
  void layout(Renderer& renderer, float width, float height);
  void update();
  void refresh();
  void setActive(bool active);
  void cancelTransientPrompts();
  [[nodiscard]] bool escape();
  [[nodiscard]] bool contentRefreshPending() const noexcept { return m_contentRefreshPending; }
  [[nodiscard]] Input* searchInput() const noexcept { return m_search; }
  [[nodiscard]] InputArea* initialFocusArea() const noexcept;

private:
  void selectPage(NexusPage page);
  void selectSearchResult(NexusPage page, std::string controlId);
  void deferContentMutation(std::function<void()> mutation);
  void requestContentRefresh();
  void rebuildContent();
  void search(std::string_view query);
  void addSearchResults(std::string_view query);
  void addConfigPage();
  void addConfigGroupIndex();
  void addWallpaperBrowserCard();
  void addPageActions();
  void addNetworkPasswordPrompt();
  void addBluetoothPairingPrompt();
  void addDefaultAppRows();
  void addDefaultAppPicker();
  void addAboutInfo();
  void saveSupportReport();
  void resetCurrentPage();
  [[nodiscard]] std::vector<std::vector<std::string>> currentPageResetPaths() const;
  void animateContentReveal();
  void syncNetworkRescan();
  [[nodiscard]] bool contentHasActiveSliderDrag() const;
  void showError(std::string message);
  [[nodiscard]] std::string liveStructureKey() const;
  void addControlRow(std::string title, std::string description, bool available, std::string controlId = {});
  [[nodiscard]] bool pageAvailable(NexusPage page) const noexcept;

  NexusRoute& m_route;
  NexusServices m_services;
  float m_scale = 1.0f;
  NexusNavigationMode m_navigationMode = NexusNavigationMode::CompactIcons;
  Flex* m_root = nullptr;
  Flex* m_content = nullptr;
  Label* m_title = nullptr;
  Label* m_errorLabel = nullptr;
  Input* m_search = nullptr;
  Button* m_backButton = nullptr;
  Button* m_resetButton = nullptr;
  Input* m_credentialInput = nullptr;
  Input* m_pairingInput = nullptr;
  InputArea* m_navigationFocus = nullptr;
  ScrollView* m_scroll = nullptr;
  Node* m_pendingScrollTarget = nullptr;
  std::vector<Button*> m_navButtons;
  struct LiveRow {
    Label* detail = nullptr;
    Button* action = nullptr;
    Button* secondaryAction = nullptr;
    Slider* slider = nullptr;
  };
  std::unordered_map<std::string, LiveRow> m_liveRows;
  Toggle* m_wifiToggle = nullptr;
  Toggle* m_bluetoothToggle = nullptr;
  Toggle* m_discoverableToggle = nullptr;
  Slider* m_outputSlider = nullptr;
  Slider* m_inputSlider = nullptr;
  std::string m_liveStructure;
  std::function<void()> m_requestRefresh;
  std::function<void()> m_requestPip;
  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);
  bool m_contentMutationPending = false;
  bool m_contentRefreshPending = false;
  bool m_resetConfirmationPending = false;
  bool m_clipboardClearConfirmationPending = false;
  bool m_launcherResetConfirmationPending = false;
  std::string m_pendingForgetEntity;
  bool m_active = false;
  std::string m_errorMessage;
  Timer m_networkRescanTimer;
  Timer m_deferredRefreshTimer;

  // Stable backing storage for the generic settings-control factory. Entity
  // editors are intentionally hosted as sheets by the window/panel adapters;
  // these values preserve selection while the main page is rebuilt.
  std::string m_editingWidgetName;
  std::string m_editingCapsuleGroupId;
  std::vector<std::string> m_selectedLaneWidgets;
  std::string m_pendingDeleteWidgetName;
  std::string m_pendingDeleteWidgetSettingPath;
  std::string m_renamingWidgetName;
};
