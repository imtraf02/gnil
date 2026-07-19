#pragma once

#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "core/timer_manager.h"
#include "shell/chrome/chrome_geometry.h"
#include "shell/panel/panel_click_shield.h"
#include "shell/panel/panel_keyboard_focus.h"
#include "shell/panel/attached_panel_context.h"
#include "ui/dialogs/layer_popup_host.h"
#include "wayland/hyprland/popup_grab_host.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ConfigService;
class CompositorPlatform;
class ContextMenuPopup;
class ChromeBlobNode;
class SelectDropdownPopup;
class IpcService;
class LayerSurface;
class Node;
class Panel;
class RenderContext;
class Renderer;
class Surface;
class WaylandConnection;
enum class LayerShellLayer : std::uint32_t;
struct KeyboardEvent;
struct PointerEvent;
struct wl_output;
struct wl_surface;

struct PanelOpenRequest {
  wl_output* output = nullptr;
  // Surface that delivered the opening click. Niri can report keyboard focus
  // entering this bar surface before it transfers focus to the new drawer;
  // treating it as shell-owned prevents that same click from closing the panel.
  wl_surface* triggerSurface = nullptr;
  float anchorX = 0.0f;
  float anchorY = 0.0f;
  bool hasExplicitAnchor = false;
  bool hasAnchorPosition = false;
  std::string_view context;
  std::string_view sourceBarName;
};

class PanelManager : public PopupGrabHost {
public:
  PanelManager();
  ~PanelManager();

  PanelManager(const PanelManager&) = delete;
  PanelManager& operator=(const PanelManager&) = delete;

  static PanelManager& instance();
  static PanelManager* current() noexcept;

  void initialize(CompositorPlatform& platform, ConfigService* config, RenderContext* renderContext);

  // Optional: invoked from shell UI (e.g. control center) to spawn the standalone settings toplevel.
  void setOpenSettingsWindowCallback(std::function<void(std::string)> callback);
  void setCloseSettingsWindowCallback(std::function<void()> callback);
  void setToggleSettingsWindowCallback(std::function<void(std::string)> callback);
  void setCloseDesktopWidgetsEditorCallback(std::function<void()> callback);
  void openSettingsWindow(std::string context = "");
  void closeSettingsWindow();
  void toggleSettingsWindow(std::string context = "");
  void setChromePanelStateCallback(
      std::function<void(wl_output*, std::string_view, std::optional<ChromePanelState>)> callback
  );
  void setPanelClosedCallback(std::function<void()> callback);
  // Run work only after the current panel surface and its chrome have fully
  // completed their close animation. If no panel is active, the work is
  // deferred to the next dispatch turn immediately.
  void runAfterPanelClosed(std::function<void()> callback);
  void setPanelOpenedCallback(std::function<void()> callback);
  void setAttachedPanelAvailabilityCallback(std::function<bool(wl_output*, std::string_view)> callback);
  void setAttachedPanelLayerProvider(std::function<std::optional<std::string>(wl_output*, std::string_view)> provider);
  void setAttachedPanelBarSettledCallback(std::function<bool(wl_output*, std::string_view)> callback);
  void setClickShieldExcludeRectsProvider(PanelClickShield::ExcludeProvider provider);
  // Called when an auto-hide bar finishes revealing for an attached panel open.
  void onAttachedBarRevealSettled(wl_output* output, std::string_view barName);

  void registerPanel(const std::string& id, std::unique_ptr<Panel> content);
  // Drops a previously registered panel, closing it first if it is open.
  void unregisterPanel(const std::string& id);

  void openPanel(const std::string& panelId, PanelOpenRequest request = {});
  void closePanel(bool animateClose = true);
  void togglePanel(const std::string& panelId, PanelOpenRequest request);
  // IPC-friendly overload: asks CompositorPlatform for preferred interactive output.
  void togglePanel(const std::string& panelId);
  void clearClipboardHistory();

  bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);

  [[nodiscard]] bool isOpen() const noexcept;
  [[nodiscard]] bool isOpenPanel(std::string_view panelId) const noexcept;
  // True only for the panel content surface and its child popups.  The bar
  // surface that originally triggered a panel is deliberately excluded.
  [[nodiscard]] bool ownsPanelSurface(wl_surface* surface) const noexcept;
  [[nodiscard]] bool isPanelTransitionActive() const noexcept;
  [[nodiscard]] bool isAttachedOpen() const noexcept;
  // Output the active panel is on; null when none is open.
  [[nodiscard]] wl_output* attachedPanelOutput() const noexcept;
  // Bar that opened the active panel; empty when none was recorded.
  [[nodiscard]] std::string_view attachedSourceBarName() const noexcept;
  [[nodiscard]] const std::string& activePanelId() const noexcept;
  // True when a panel is open and it reports the given context as active (e.g. control-center tab).
  [[nodiscard]] bool isActivePanelContext(std::string_view context) const noexcept;
  [[nodiscard]] std::optional<LayerPopupParentContext> popupParentContextForSurface(wl_surface* surface) const noexcept;
  [[nodiscard]] std::optional<LayerPopupParentContext> fallbackPopupParentContext() const noexcept;

  [[nodiscard]] RenderContext* renderContext() const noexcept { return m_renderContext; }
  [[nodiscard]] WaylandConnection* wayland() const noexcept;

  void setActivePopup(ContextMenuPopup* popup);
  void clearActivePopup();

  void refresh();
  // Reacts to a ConfigService reload while a panel is open: re-pulls the host bar's
  // per-panel-relevant config (attached background opacity), styling, and compositor
  // blur region. No-op when no panel is open.
  void onConfigReloaded();
  void onIconThemeChanged();
  void onOutputChange();
  void focusArea(InputArea* area);
  [[nodiscard]] InputDispatcher& inputDispatcher() noexcept { return m_inputDispatcher; }
  [[nodiscard]] const InputDispatcher& inputDispatcher() const noexcept { return m_inputDispatcher; }
  void requestUpdateOnly();
  void requestLayout();
  // Resize only the visible body of a dynamic panel. The layer-shell surface
  // remains compositor-sized and click-through outside the body.
  void requestActivePanelVisualSize(float width, float height, bool animate = true);
  // Requests a redraw on the active panel surface without re-running panel
  // update/layout. Used for reactive palette restyling.
  void requestRedraw();
  void requestFrameTick();
  void close();
  void beginAttachedPopup(wl_surface* surface);
  void endAttachedPopup(wl_surface* surface);

  // PopupSurface enrollment lets the Niri keyboard-focus tracker distinguish
  // a child popup from a real focus leave to another application.
  void registerPopupSurface(wl_surface* surface) override;
  void unregisterPopupSurface(wl_surface* surface) override;

  void registerIpc(IpcService& ipc);

private:
  static PanelManager* s_instance;

  void buildScene(std::uint32_t width, std::uint32_t height);
  void prepareFrame(bool needsUpdate, bool needsLayout);
  void applyPendingPanelFocus();
  [[nodiscard]] bool ownsKeyboardSurface(wl_surface* surface) const noexcept;
  void scheduleKeyboardFocusBootstrap();
  void continueKeyboardFocusBootstrapAfterMap();
  void finishKeyboardFocusBootstrap(wl_surface* surface);
  void cancelKeyboardFocusBootstrap();
  void deferKeyboardFocusCloseCheck();
  void activateClickShield(LayerShellLayer layer);
  void syncClickShieldOutputs();
  void destroyPanel();
  void applyAttachedReveal(float progress);
  void applyDetachedReveal(float progress);
  [[nodiscard]] std::optional<ChromePanelState> sampleChromeRevealState(float revealProgress) const;
  void startAttachedOpenAnimation();
  bool switchAttachedPanel(const std::string& panelId, PanelOpenRequest request);
  bool retargetAttachedPanel(const PanelOpenRequest& request);
  void applyAttachedMorphState(ChromePanelState displayed);
  void positionAttachedContentLayers(const ChromePanelState& displayed);
  void clearRetainedContentLayers();
  void syncActivePanelIntrinsicHeight(Renderer& renderer, bool animate);
  void stabilizeActivePanelIntrinsicHeight(Renderer& renderer);
  void syncAttachedChromeGeometry();
  void publishChromePanelState(float revealProgress);
  // Submit a wl_region matching the panel body after applying the current reveal clip.
  void applyPanelCompositorBlur(int bodyX, int bodyY, int bodyW, int bodyH, int clipX, int clipY, int clipW, int clipH);
  void syncDetachedVisualGeometry(std::uint32_t surfaceWidth, std::uint32_t surfaceHeight);

  CompositorPlatform* m_platform = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  std::function<void(std::string)> m_openSettingsWindow;
  std::function<void()> m_closeSettingsWindow;
  std::function<void(std::string)> m_toggleSettingsWindow;
  std::function<void()> m_closeDesktopWidgetsEditor;
  std::function<void(wl_output*, std::string_view, std::optional<ChromePanelState>)> m_chromePanelStateCallback;
  std::function<void()> m_panelClosedCallback;
  std::vector<std::function<void()>> m_afterPanelClosed;
  std::function<void()> m_panelOpenedCallback;
  std::function<bool(wl_output*, std::string_view)> m_attachedPanelAvailabilityCallback;
  std::function<std::optional<std::string>(wl_output*, std::string_view)> m_attachedPanelLayerProvider;
  std::function<bool(wl_output*, std::string_view)> m_attachedPanelBarSettledCallback;
  PanelClickShield::ExcludeProvider m_clickShieldExcludeRectsProvider;
  PanelClickShield m_clickShield;
  std::unique_ptr<Surface> m_surface;
  LayerSurface* m_layerSurface = nullptr;
  // m_sceneRoot must be destroyed before m_animations — ~Node() calls cancelForOwner().
  // Also m_panels (which own their own Nodes parented under m_sceneRoot) must be destroyed
  // before m_animations for the same reason.
  AnimationManager m_animations;
  std::unique_ptr<Node> m_sceneRoot;
  ChromeBlobNode* m_detachedChromeNode = nullptr;
  Node* m_contentNode = nullptr;
  Node* m_detachedRevealClipNode = nullptr;
  Node* m_detachedRevealContentNode = nullptr;
  Node* m_attachedRevealClipNode = nullptr;
  Node* m_attachedRevealContentNode = nullptr;
  InputDispatcher m_inputDispatcher;

  std::unordered_map<std::string, std::unique_ptr<Panel>> m_panels;
  Panel* m_activePanel = nullptr;
  std::string m_activePanelId;
  std::string m_pendingOpenContext;
  struct QueuedPanelOpen {
    std::string panelId;
    wl_output* output = nullptr;
    wl_surface* triggerSurface = nullptr;
    float anchorX = 0.0f;
    float anchorY = 0.0f;
    bool hasExplicitAnchor = false;
    bool hasAnchorPosition = false;
    std::string context;
    std::string sourceBarName;
  };
  std::optional<QueuedPanelOpen> m_queuedPanelOpen;

  wl_output* m_output = nullptr;
  wl_surface* m_wlSurface = nullptr;
  float m_contentWidth = 0.0f;
  float m_contentHeight = 0.0f;
  std::int32_t m_panelInsetX = 0;
  std::int32_t m_panelInsetY = 0;
  std::uint32_t m_panelVisualWidth = 0;
  std::uint32_t m_panelVisualHeight = 0;
  float m_panelVisualWidthExact = 0.0f;
  float m_panelVisualHeightExact = 0.0f;
  // Fill axes derive their visual size from the compositor-configured surface
  // size in buildScene; that math also needs the trailing shadow bleed.
  bool m_panelFillWidth = false;
  bool m_panelFillHeight = false;
  bool m_panelCustomWidth = false;
  bool m_panelDynamicVisualSize = false;
  bool m_intrinsicVisualHeightResolved = false;
  bool m_panelGeometryDirty = false;
  std::string m_panelScreenPosition;
  std::int32_t m_detachedBleedLeft = 0;
  std::int32_t m_detachedBleedTop = 0;
  std::int32_t m_detachedBleedRight = 0;
  std::int32_t m_detachedBleedBottom = 0;
  AnimationManager::Id m_panelResizeAnimation = 0;
  AnimationManager::Id m_panelMorphAnimation = 0;
  AnimationManager::Id m_attachedRevealAnimation = 0;
  AnimationManager::Id m_panelContentAnimation = 0;
  struct RetainedContentLayer {
    Panel* panel = nullptr;
    Node* node = nullptr;
    float visualWidth = 1.0f;
    float visualHeight = 1.0f;
    float padding = 0.0f;
    float startOpacity = 0.0f;
  };
  std::vector<RetainedContentLayer> m_retainedContentLayers;
  float m_activeContentVisualWidth = 1.0f;
  float m_activeContentVisualHeight = 1.0f;
  float m_activeContentPadding = 0.0f;
  float m_pendingVisualWidth = 0.0f;
  float m_pendingVisualHeight = 0.0f;
  bool m_pendingVisualSize = false;
  bool m_pendingVisualSizeAnimated = true;
  float m_attachedBackgroundOpacity = 1.0f;
  bool m_attachedContactShadow = false;
  float m_attachedRevealProgress = 1.0f;
  float m_detachedRevealProgress = 1.0f;
  AttachedRevealDirection m_attachedRevealDirection = AttachedRevealDirection::Down;
  AttachedRevealDirection m_detachedRevealDirection = AttachedRevealDirection::Down;
  std::string m_attachedBarPosition; // "top" / "bottom" / "left" / "right" while attached, empty otherwise
  std::string m_sourceBarName;       // name of the bar that opened the current panel
  std::optional<ChromePanelState> m_chromePanelState;
  ChromeTransitionState m_chromeTransition;
  bool m_pointerInside = false;
  bool m_inTransition = false;
  bool m_closing = false;
  bool m_attachedToBar = false;
  // True for any panel whose material is painted by ChromeOutputHost.  This
  // includes icon-attached popouts as well as the launcher/session fixed frame
  // slots, even though the latter keep a separate fullscreen content surface.
  bool m_chromeHosted = false;
  bool m_attachedOpenAnimationPending = false;
  bool m_attachedInitialGeometryReady = false;
  std::size_t m_attachedPopupCount = 0;
  std::unordered_set<wl_surface*> m_ownedPopupSurfaces;
  wl_surface* m_openTriggerSurface = nullptr;
  std::uint32_t m_openTriggerSerial = 0;
  PanelKeyboardFocusTracker m_keyboardFocus;
  bool m_keyboardFocusBootstrapScheduled = false;
  bool m_keyboardFocusBootstrapActive = false;
  bool m_clickShieldRequested = false;
  Timer m_keyboardFocusRelaxTimer;
  ContextMenuPopup* m_activePopup = nullptr;
  std::unique_ptr<SelectDropdownPopup> m_selectPopup;
  std::uint64_t m_destroyGeneration = 0; // invalidates stale deferred destroyPanel calls
};
