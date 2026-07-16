#pragma once

#include "config/config_types.h"
#include "shell/panel/panel.h"
#include "ui/style.h"

#include <cmath>
#include <optional>
#include <string>
#include <vector>

class Box;
class Button;
class Flex;
class GridView;
class InputArea;
class CountdownRing;
class Renderer;
class ConfigService;
class SessionActionRunner;

class SessionPanel : public Panel {
public:
  explicit SessionPanel(ConfigService* config, SessionActionRunner& actionRunner)
      : m_config(config), m_actionRunner(&actionRunner) {}

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  void onFrameTick(float deltaMs) override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

  [[nodiscard]] float preferredWidth() const override;
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] bool hasDecoration() const override { return true; }
  [[nodiscard]] LayerShellLayer layer() const override { return LayerShellLayer::Overlay; }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::OnDemand; }
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override;
  // This is a centred action rail attached to the right frame, not a
  // full-height sidebar.  Keeping the body intrinsic is what leaves the
  // frame visible above and below it, matching Caelestia's session drawer.
  [[nodiscard]] bool fillsHeight() const noexcept override { return false; }
  [[nodiscard]] bool usesDynamicVisualSize() const noexcept override { return true; }
  [[nodiscard]] float initialVisualWidth() const override { return preferredWidth(); }
  [[nodiscard]] float initialVisualHeight() const override { return preferredHeight(); }
  [[nodiscard]] ChromeEdge chromeEdge() const noexcept override { return ChromeEdge::Right; }

private:
  struct ActionCountdownOverlay {
    Flex* root = nullptr;
    Box* scrim = nullptr;
    CountdownRing* ring = nullptr;
  };

  struct PendingCountdown {
    std::size_t index = 0;
    double remainingMs = 0.0;
    double totalMs = 0.0;
  };

  // Caelestia Tokens.sizes.session.button is 80 px.
  static constexpr float kActionButtonMinHeight = 80.0f;
  static constexpr float kButtonMinWidth = 80.0f;
  static constexpr float kPanelMinWidth = 108.0f;

  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void armEntry(std::size_t index);
  void executeEntry(std::size_t index);
  void cancelCountdown();
  void focusButton(std::size_t index);
  [[nodiscard]] std::optional<std::size_t> focusedButtonIndex() const;
  void updateSelectionVisuals();
  void updateCountdownVisuals();
  void layoutCountdownOverlays(Renderer& renderer);
  void restoreEntryBadges();
  void hideCountdownOverlays();
  void attachCountdownOverlay(Button& button, ActionCountdownOverlay& overlay, float scale);
  void syncCountdownOverlayColors(std::size_t index);
  void invokeEntry(const SessionPanelActionConfig& cfg);
  [[nodiscard]] std::vector<SessionPanelActionConfig> effectiveActions() const;
  [[nodiscard]] Button* createActionButton(const SessionPanelActionConfig& cfg, std::size_t index, float scale);
  [[nodiscard]] std::size_t entryCountForLayout() const;
  [[nodiscard]] std::size_t visibleColumnCount() const;
  [[nodiscard]] std::size_t visibleRowCount() const;

  GridView* m_rootLayout = nullptr;
  std::vector<SessionPanelActionConfig> m_visibleEntries;
  std::vector<Button*> m_visibleButtons;
  std::vector<ActionCountdownOverlay> m_countdownOverlays;
  std::vector<std::optional<std::string>> m_entryShortcutBadges;
  std::optional<PendingCountdown> m_pendingCountdown;
  ConfigService* m_config = nullptr;
  SessionActionRunner* m_actionRunner = nullptr;
};
