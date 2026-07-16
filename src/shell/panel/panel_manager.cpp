#include "shell/panel/panel_manager.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/key_chord.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "ipc/ipc_service.h"
#include "render/render_context.h"
#include "render/scene/chrome_blob_node.h"
#include "render/scene/input_area.h"
#include "shell/bar/bar_reserved_zone.h"
#include "shell/chrome/chrome_output_host.h"
#include "shell/clipboard/clipboard_panel.h"
#include "shell/screen_position.h"
#include "shell/surface/shadow.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

PanelManager* PanelManager::s_instance = nullptr;

namespace {

  constexpr Logger kLog("panel");
  constexpr std::int32_t kDetachedPanelShadowSafetyPadding = 2;

  [[nodiscard]] bool isStandaloneContentPanel(std::string_view id) noexcept {
    return id == "media" || id == "audio" || id == "brightness" || id == "system" || id == "battery"
        || id == "network" || id == "bluetooth" || id == "weather" || id == "calendar" || id == "screen-time";
  }

  [[nodiscard]] std::string_view legacyControlCenterTarget(std::string_view context) noexcept {
    if (context == "media" || context == "audio" || context == "system" || context == "network"
        || context == "bluetooth" || context == "weather" || context == "calendar" || context == "screen-time") {
      return context;
    }
    if (context == "monitor") return "brightness";
    if (context == "power") return "battery";
    if (context == "notifications") return "sidebar";
    return {};
  }

  [[nodiscard]] const ShellConfig::PanelConfig::SizeOverride*
  panelSizeOverride(const ConfigService* config, std::string_view panelId) noexcept {
    if (config == nullptr) {
      return nullptr;
    }
    const auto& sizes = config->config().shell.panel.sizes;
    const auto it = std::ranges::find(sizes, panelId, &ShellConfig::PanelConfig::SizeOverride::id);
    return it == sizes.end() ? nullptr : &*it;
  }

  [[nodiscard]] float scaledPanelOverride(std::int32_t logical, float scale) noexcept {
    return static_cast<float>(std::clamp(logical, 1, 8192)) * std::max(0.1f, scale);
  }

  shell::surface_shadow::Bleed
  detachedPanelSurfaceBleed(bool hasDecoration, const ShellConfig::ShadowConfig& shadow) noexcept {
    auto bleed = shell::surface_shadow::bleed(hasDecoration, shadow);
    if (shell::surface_shadow::enabled(hasDecoration, shadow)) {
      bleed.left += kDetachedPanelShadowSafetyPadding;
      bleed.right += kDetachedPanelShadowSafetyPadding;
      bleed.up += kDetachedPanelShadowSafetyPadding;
      bleed.down += kDetachedPanelShadowSafetyPadding;
    }
    return bleed;
  }

  std::uint32_t panelSurfaceExtent(std::uint32_t contentSize, std::int32_t before, std::int32_t after) noexcept {
    const auto total =
        static_cast<std::int64_t>(contentSize) + static_cast<std::int64_t>(before) + static_cast<std::int64_t>(after);
    return static_cast<std::uint32_t>(std::max<std::int64_t>(1, total));
  }

  BarConfig resolvePanelBarConfig(
      ConfigService* configService, CompositorPlatform* platform, wl_output* output, std::string_view barName = {}
  ) {
    BarConfig barConfig;
    if (configService == nullptr || configService->config().bars.empty()) {
      return barConfig;
    }

    const auto& bars = configService->config().bars;
    bool found = false;
    if (!barName.empty()) {
      for (const auto& bar : bars) {
        if (bar.name == barName) {
          barConfig = bar;
          found = true;
          break;
        }
      }
    }
    if (!found) {
      barConfig = bars.front();
    }

    if (platform == nullptr || output == nullptr) {
      return barConfig;
    }

    if (const auto* wlOutput = platform->findOutputByWl(output); wlOutput != nullptr) {
      return ConfigService::resolveForOutput(barConfig, *wlOutput);
    }

    return barConfig;
  }

  bool hasMultipleEnabledBarsOnEdge(
      ConfigService* configService, CompositorPlatform* platform, wl_output* output, std::string_view position
  ) {
    if (configService == nullptr || position.empty()) {
      return false;
    }

    const WaylandOutput* wlOutput = nullptr;
    if (platform != nullptr && output != nullptr) {
      wlOutput = platform->findOutputByWl(output);
    }

    std::size_t count = 0;
    for (const auto& bar : configService->config().bars) {
      const BarConfig resolved = wlOutput != nullptr ? ConfigService::resolveForOutput(bar, *wlOutput) : bar;
      if (!resolved.enabled || resolved.position != position) {
        continue;
      }
      ++count;
      if (count > 1) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] float panelRevealContentOpacity(float reveal) {
    const float v = std::clamp(reveal, 0.0f, 1.0f);
    if (v <= 0.15f) {
      return 0.0f;
    }
    return std::clamp((v - 0.15f) / 0.85f, 0.0f, 1.0f);
  }

  [[nodiscard]] ChromePanelState hiddenChromeState(
      const ChromePanelState& open, AttachedRevealDirection direction
  ) noexcept {
    ChromePanelState hidden = open;
    ChromeEdge contactEdge = ChromeEdge::None;
    switch (direction) {
    case AttachedRevealDirection::Down:
      contactEdge = ChromeEdge::Top;
      break;
    case AttachedRevealDirection::Up:
      contactEdge = ChromeEdge::Bottom;
      break;
    case AttachedRevealDirection::Right:
      contactEdge = ChromeEdge::Left;
      break;
    case AttachedRevealDirection::Left:
      contactEdge = ChromeEdge::Right;
      break;
    }
    hidden.rect = chromeAnchoredRevealRect(open.rect, contactEdge, 0.0f);
    if (open.connectorVisible) {
      hidden.connector = chromeAnchoredRevealRect(open.connector, contactEdge, 0.0f);
    }
    hidden.opacity = 0.0f;
    hidden.progress = 0.0f;
    hidden.inputEnabled = false;
    return hidden;
  }

  [[nodiscard]] AttachedRevealDirection
  detachedRevealDirection(std::string_view panelPosition, std::string_view barPosition) {
    if (panelPosition == "top_left" || panelPosition == "top_center" || panelPosition == "top_right") {
      return AttachedRevealDirection::Down;
    }
    if (panelPosition == "bottom_left" || panelPosition == "bottom_center" || panelPosition == "bottom_right") {
      return AttachedRevealDirection::Up;
    }
    if (panelPosition == "center_left") {
      return AttachedRevealDirection::Right;
    }
    if (panelPosition == "center_right") {
      return AttachedRevealDirection::Left;
    }
    if (panelPosition == "center") {
      return AttachedRevealDirection::Down;
    }
    return attached_panel::revealDirection(barPosition);
  }

  float resolvePanelContentScale(ConfigService* configService) {
    if (configService == nullptr) {
      return 1.0f;
    }
    return std::max(0.1f, configService->config().accessibility.uiScale);
  }

  float resolvePanelCardOpacity(ConfigService* configService, float panelBackgroundOpacity) {
    (void)configService;
    (void)panelBackgroundOpacity;
    return 1.0f;
  }

  float resolveDetachedPanelBackgroundOpacity(ConfigService* configService) {
    (void)configService;
    return 1.0f;
  }

  [[nodiscard]] std::string_view detachedFrameJoinEdge(const Panel* panel, std::string_view /*panelId*/) noexcept {
    if (panel == nullptr) {
      return {};
    }
    switch (panel->chromeEdge()) {
    case ChromeEdge::Top:
      return "top";
    case ChromeEdge::Right:
      return "right";
    case ChromeEdge::Bottom:
      return "bottom";
    case ChromeEdge::Left:
      return "left";
    case ChromeEdge::None:
      return {};
    }
    return {};
  }

  [[nodiscard]] float
  detachedPanelBackgroundOpacity(const Panel* panel, std::string_view panelId, ConfigService* configService) {
    // Frame-joined pieces must use the exact same surface colour and alpha as
    // the frame. Applying detached transparency here creates a visible seam.
    return detachedFrameJoinEdge(panel, panelId).empty() ? resolveDetachedPanelBackgroundOpacity(configService) : 1.0f;
  }

  // Floating screen position for a built-in panel (one of kPanelPositions).
  // "auto" = bar-relative (and the default for any non-built-in panel).
  [[nodiscard]] std::string resolvePanelPosition(const ConfigService* configService, std::string_view panelId) {
    // These are part of GNIL's fixed Caelestia-style spatial model rather than
    // user-selectable drawer positions.
    if (panelId == "launcher") {
      return "bottom_center";
    }
    if (panelId == "session") {
      return "center_right";
    }
    if (panelId == "control-center") {
      return "top_center";
    }
    if (panelId == "settings") {
      return "top_center";
    }
    if (panelId == "notifications" || panelId == "sidebar") {
      return "top_right";
    }
    (void)configService;
    if (panelId == "clipboard") {
      return "center";
    }
    if (panelId == "wallpaper") {
      return "auto";
    }
    if (panelId == "polkit") {
      return "center";
    }
    return "auto";
  }

  [[nodiscard]] bool openNearClickEnabledForPanel(const ConfigService* configService, std::string_view panelId) {
    if (panelId == "tray-drawer") {
      return true;
    }
    (void)configService;
    (void)panelId;
    return false;
  }

  [[nodiscard]] bool
  openNearClickEnabled(const Panel* panel, std::string_view panelId, const ConfigService* configService) {
    if (panelId.contains(':')) {
      if (panel == nullptr) {
        return false;
      }
      const bool pinned = panel->panelPlacement() == PanelPlacement::Floating
          && panel->panelScreenPosition() != "auto"
          && panel->panelScreenPosition() != "center";
      return !pinned && panel->panelOpenNearClick();
    }
    return openNearClickEnabledForPanel(configService, panelId);
  }

  [[nodiscard]] bool keepsFixedFrameSlot(std::string_view panelId) noexcept {
    // These surfaces are continuations of the global frame, so moving them
    // to an icon would break their Caelestia-style frame joins.
    return panelId == "launcher" || panelId == "session" || panelId == "settings" || panelId == "sidebar";
  }

  [[nodiscard]] bool needsDismissFocusBootstrap(std::string_view panelId) noexcept {
    // Launcher and session are normally opened from a keyboard-inert rail and
    // must observe one real focus enter before focus-leave can dismiss them.
    // Notifications deliberately remain non-focus-stealing.
    return panelId == "launcher" || panelId == "session";
  }

  [[nodiscard]] bool requestUsesTriggerAnchor(
      const PanelOpenRequest& request, const Panel* panel, std::string_view panelId, const ConfigService* configService
  ) {
    if (!request.hasAnchorPosition) {
      return false;
    }
    // An explicit anchor comes from the center of a rail icon. It is the
    // default spatial model for popouts; the legacy setting remains useful for
    // non-widget callers that only supply a pointer position.
    return (request.hasExplicitAnchor && !keepsFixedFrameSlot(panelId))
        || openNearClickEnabled(panel, panelId, configService);
  }

} // namespace

PanelManager::PanelManager() { s_instance = this; }

PanelManager::~PanelManager() {
  if (m_platform != nullptr) {
    m_platform->wayland().setShellKeyboardFocusCallback({});
  }
  if (s_instance == this) {
    s_instance = nullptr;
  }
}

PanelManager& PanelManager::instance() { return *s_instance; }

PanelManager* PanelManager::current() noexcept { return s_instance; }

WaylandConnection* PanelManager::wayland() const noexcept {
  return m_platform != nullptr ? &m_platform->wayland() : nullptr;
}

void PanelManager::initialize(CompositorPlatform& platform, ConfigService* config, RenderContext* renderContext) {
  m_platform = &platform;
  m_config = config;
  m_renderContext = renderContext;
  m_clickShield.initialize(platform.wayland());
  platform.wayland().setShellKeyboardFocusCallback([this](wl_surface* surface, bool entered) {
    const bool owned = ownsKeyboardSurface(surface);
    const auto action = m_keyboardFocus.observe(owned, entered);
    kLog.debug(
        "keyboard focus {} surface={} owned={} bootstrap_scheduled={} bootstrap_active={}",
        entered ? "enter" : "leave", static_cast<const void*>(surface), owned,
        m_keyboardFocusBootstrapScheduled, m_keyboardFocusBootstrapActive
    );
    if (entered && surface == m_wlSurface && m_keyboardFocusBootstrapActive) {
      finishKeyboardFocusBootstrap(surface);
    }
    if (!m_clickShield.isActive()
        && action == PanelKeyboardFocusTracker::Action::CheckAfterDispatch
        && !m_keyboardFocusBootstrapScheduled && !m_keyboardFocusBootstrapActive) {
      deferKeyboardFocusCloseCheck();
    }
  });
}

bool PanelManager::ownsKeyboardSurface(wl_surface* surface) const noexcept {
  return surface != nullptr
      && (surface == m_wlSurface || surface == m_openTriggerSurface || m_ownedPopupSurfaces.contains(surface));
}

void PanelManager::scheduleKeyboardFocusBootstrap() {
  if (!isOpen() || m_closing || m_layerSurface == nullptr || m_wlSurface == nullptr
      || m_layerSurface->keyboardInteractivity() != LayerShellKeyboard::OnDemand
      || !needsDismissFocusBootstrap(m_activePanelId)) {
    return;
  }

  // Promotion is continued from the first wl_surface.frame callback. A timer
  // is insufficient here because the initial scene build can itself take
  // longer than the delay; frame-done is the compositor's proof that the
  // OnDemand buffer was actually mapped.
  m_keyboardFocusBootstrapScheduled = true;
}

void PanelManager::continueKeyboardFocusBootstrapAfterMap() {
  if (!m_keyboardFocusBootstrapScheduled || !isOpen() || m_closing || m_layerSurface == nullptr) {
    return;
  }
  m_keyboardFocusBootstrapScheduled = false;
  const bool panelStillOwnsFocus = m_platform != nullptr
      && ownsKeyboardSurface(m_platform->lastKeyboardSurface());
  if (!m_keyboardFocus.needsBootstrap() && panelStillOwnsFocus) {
    kLog.debug("keyboard focus bootstrap not needed for \"{}\"", m_activePanelId);
    return;
  }

  // OnDemand can emit a transient enter/leave while the first configure is
  // settling. That enter must not arm outside-dismiss permanently; acquire a
  // fresh, stable enter from the bootstrap promotion below.
  m_keyboardFocus.beginOpen();
  // The first frame was mapped OnDemand, so Niri has created its focus token.
  // Promote only long enough to receive wl_keyboard.enter, then hand it back.
  m_keyboardFocusBootstrapActive = true;
  kLog.debug("keyboard focus bootstrap promoting \"{}\" to exclusive after first frame", m_activePanelId);
  m_layerSurface->setKeyboardInteractivity(LayerShellKeyboard::Exclusive);
}

void PanelManager::finishKeyboardFocusBootstrap(wl_surface* surface) {
  if (!m_keyboardFocusBootstrapActive || surface == nullptr || surface != m_wlSurface) {
    return;
  }

  wl_surface* const expectedSurface = surface;
  // Let Niri process the configure/focus token before relaxing the layer. An
  // immediate Exclusive -> OnDemand commit is observed as a synthetic leave
  // by keyboard-inert panels such as session.
  m_keyboardFocusRelaxTimer.start(std::chrono::milliseconds(100), [this, expectedSurface]() {
    if (!isOpen() || m_closing || m_layerSurface == nullptr || m_wlSurface != expectedSurface) {
      return;
    }
    // The panel now owns an OnDemand token. This tracker remains the fallback
    // when Niri lacks viewporter/SHM support and distinguishes owned popups
    // from a genuine focus transfer while the click shield is active.
    m_keyboardFocusBootstrapActive = false;
    kLog.debug("keyboard focus bootstrap relaxing \"{}\" to on-demand", m_activePanelId);
    m_layerSurface->setKeyboardInteractivity(LayerShellKeyboard::OnDemand);
  });
}

void PanelManager::cancelKeyboardFocusBootstrap() {
  m_keyboardFocusBootstrapScheduled = false;
  m_keyboardFocusRelaxTimer.stop();
  if (m_layerSurface != nullptr && needsDismissFocusBootstrap(m_activePanelId)
      && m_layerSurface->keyboardInteractivity() == LayerShellKeyboard::Exclusive) {
    m_layerSurface->setKeyboardInteractivity(LayerShellKeyboard::OnDemand);
  }
  m_keyboardFocusBootstrapActive = false;
}

void PanelManager::deferKeyboardFocusCloseCheck() {
  const std::uint64_t generation = m_destroyGeneration;
  DeferredCall::callLater([this, generation]() {
    if (generation != m_destroyGeneration || !isOpen() || m_closing || m_platform == nullptr) {
      return;
    }
    // wl_keyboard.leave and enter are separate events. Query the seat after
    // the dispatch batch so panel -> popup -> panel never looks like an
    // outside click, while a real application focus closes exactly once.
    const auto& wayland = m_platform->wayland();
    const bool triggerPressedAgain = shouldDeferFocusDismissToTriggerToggle(
        m_openTriggerSurface != nullptr && wayland.lastPointerSurface() == m_openTriggerSurface,
        wayland.lastInputSource() == WaylandSeat::InputSource::Pointer, m_openTriggerSerial,
        wayland.lastInputSerial()
    );
    // On Niri, pressing the trigger can move keyboard focus away before the
    // widget's button-up callback runs. Let that callback perform the toggle;
    // otherwise close starts here and button-up reverses the same surface.
    if (!triggerPressedAgain
        && m_keyboardFocus.shouldClose(ownsKeyboardSurface(m_platform->lastKeyboardSurface()))) {
      closePanel();
    }
  });
}

void PanelManager::setOpenSettingsWindowCallback(std::function<void(std::string)> callback) {
  m_openSettingsWindow = std::move(callback);
}

void PanelManager::setCloseSettingsWindowCallback(std::function<void()> callback) {
  m_closeSettingsWindow = std::move(callback);
}

void PanelManager::setToggleSettingsWindowCallback(std::function<void(std::string)> callback) {
  m_toggleSettingsWindow = std::move(callback);
}

void PanelManager::setCloseDesktopWidgetsEditorCallback(std::function<void()> callback) {
  m_closeDesktopWidgetsEditor = std::move(callback);
}

void PanelManager::openSettingsWindow(std::string context) {
  if (isOpen() && !m_closing) {
    closePanel();
  }
  if (m_openSettingsWindow) {
    m_openSettingsWindow(std::move(context));
  }
}

void PanelManager::closeSettingsWindow() {
  if (m_closeSettingsWindow) {
    m_closeSettingsWindow();
  }
}

void PanelManager::toggleSettingsWindow(std::string context) {
  if (isOpen() && !m_closing) {
    closePanel();
  }
  if (m_toggleSettingsWindow) {
    m_toggleSettingsWindow(std::move(context));
    return;
  }
  if (m_openSettingsWindow) {
    m_openSettingsWindow(std::move(context));
  }
}

void PanelManager::setChromePanelStateCallback(
    std::function<void(wl_output*, std::string_view, std::optional<ChromePanelState>)> callback
) {
  m_chromePanelStateCallback = std::move(callback);
}

void PanelManager::setPanelClosedCallback(std::function<void()> callback) {
  m_panelClosedCallback = std::move(callback);
}

void PanelManager::runAfterPanelClosed(std::function<void()> callback) {
  if (!callback) {
    return;
  }
  if (!isOpen() && !m_closing) {
    DeferredCall::callLater(std::move(callback));
    return;
  }
  m_afterPanelClosed.push_back(std::move(callback));
}

void PanelManager::setPanelOpenedCallback(std::function<void()> callback) {
  m_panelOpenedCallback = std::move(callback);
}

void PanelManager::setAttachedPanelAvailabilityCallback(std::function<bool(wl_output*, std::string_view)> callback) {
  m_attachedPanelAvailabilityCallback = std::move(callback);
}

void PanelManager::setAttachedPanelLayerProvider(
    std::function<std::optional<std::string>(wl_output*, std::string_view)> provider
) {
  m_attachedPanelLayerProvider = std::move(provider);
}

void PanelManager::setAttachedPanelBarSettledCallback(std::function<bool(wl_output*, std::string_view)> callback) {
  m_attachedPanelBarSettledCallback = std::move(callback);
}

void PanelManager::setClickShieldExcludeRectsProvider(PanelClickShield::ExcludeProvider provider) {
  m_clickShieldExcludeRectsProvider = std::move(provider);
}

void PanelManager::onAttachedBarRevealSettled(wl_output* output, std::string_view barName) {
  if (!m_attachedOpenAnimationPending || !isAttachedOpen() || m_output != output) {
    return;
  }
  if (!m_sourceBarName.empty() && !barName.empty() && m_sourceBarName != barName) {
    return;
  }
  startAttachedOpenAnimation();
  requestFrameTick();
}

void PanelManager::registerPanel(const std::string& id, std::unique_ptr<Panel> content) {
  m_panels[id] = std::move(content);
}

void PanelManager::unregisterPanel(const std::string& id) {
  auto it = m_panels.find(id);
  if (it == m_panels.end()) {
    return;
  }
  if (isOpenPanel(id)) {
    closePanel(/*animateClose=*/false);
  }
  m_panels.erase(it);
}

void PanelManager::openPanel(const std::string& panelId, PanelOpenRequest request) {
  if (panelId == "control-center") {
    const std::string_view target = legacyControlCenterTarget(request.context);
    if (target.empty()) {
      kLog.warn("control-center dashboard was removed; open a destination panel directly");
      return;
    }
    request.context = {};
    openPanel(std::string(target), request);
    return;
  }
  if (panelId == "sidebar" && m_config != nullptr && !m_config->config().sidebar.enabled) {
    return;
  }
  if (isOpen() && !m_closing && m_attachedToBar && panelId != m_activePanelId
      && switchAttachedPanel(panelId, request)) {
    return;
  }
  if (m_inTransition) {
    return;
  }

  // SmartPanel switching is a committed close followed by a fresh open.  Do
  // not tear down the active scene in the click handler: that was the source
  // of square flashes, stale popup grabs and tray/panel overlap.
  if (isOpen() || m_closing) {
    m_queuedPanelOpen = QueuedPanelOpen{
        .panelId = panelId,
        .output = request.output,
        .triggerSurface = request.triggerSurface,
        .anchorX = request.anchorX,
        .anchorY = request.anchorY,
        .hasExplicitAnchor = request.hasExplicitAnchor,
        .hasAnchorPosition = request.hasAnchorPosition,
        .context = std::string(request.context),
        .sourceBarName = std::string(request.sourceBarName),
    };
    if (!m_closing) {
      closePanel();
    }
    return;
  }

  m_keyboardFocus.beginOpen();
  m_openTriggerSurface = request.triggerSurface;
  m_openTriggerSerial = m_platform != nullptr ? m_platform->lastInputSerial() : 0;
  m_ownedPopupSurfaces.clear();

  if (request.output == nullptr && m_platform != nullptr) {
    request.output = m_platform->focusedInteractiveOutput(std::chrono::milliseconds(1200));
    if (request.output == nullptr) {
      // No focus source resolved an output (e.g. a compositor with no focus
      // IPC/backend). Ask the compositor which output an unpinned surface lands
      // on — the focused one — then reopen with that concrete output so all the
      // normal placement (attached, bar-relative, per-output config) applies.
      // Falls back to the arbitrary first output if the probe times out.
      //
      // The open is deferred past this call, so the request's string_view fields
      // are copied into owned storage the continuation keeps alive.
      m_platform->probeFocusedOutput(
          [this, panelId, request, context = std::string(request.context),
           sourceBarName = std::string(request.sourceBarName)](wl_output* probed) mutable {
            request.output =
                probed != nullptr ? probed : m_platform->preferredInteractiveOutput(std::chrono::milliseconds(1200));
            if (request.output == nullptr) {
              return; // no usable output at all — nothing to open on.
            }
            request.context = context;
            request.sourceBarName = sourceBarName;
            openPanel(panelId, request);
          },
          std::chrono::milliseconds(250)
      );
      return;
    }
  }

  if (m_closeDesktopWidgetsEditor) {
    m_closeDesktopWidgetsEditor();
  }

  auto it = m_panels.find(panelId);
  if (it == m_panels.end()) {
    kLog.warn("panel manager: unknown panel \"{}\"", panelId);
    return;
  }

  m_activePanel = it->second.get();
  m_activePanelId = panelId;
  m_clickShieldRequested = panel_dismissal::usesClickShield(
      m_activePanelId, request.triggerSurface != nullptr || request.hasExplicitAnchor
  );
  m_activePanel->setContentScale(resolvePanelContentScale(m_config));
  m_pendingOpenContext = std::string(request.context);
  m_activePanel->setPendingOpenContext(request.context);

  auto panelWidth = static_cast<std::uint32_t>(m_activePanel->preferredWidth());
  auto panelHeight = static_cast<std::uint32_t>(m_activePanel->preferredHeight());
  const auto* sizeOverride = panelSizeOverride(m_config, panelId);
  m_panelCustomWidth = sizeOverride != nullptr && sizeOverride->width.has_value();
  if (m_panelCustomWidth) {
    panelWidth = static_cast<std::uint32_t>(std::lround(
        scaledPanelOverride(*sizeOverride->width, m_activePanel->contentScale())
    ));
  }
  auto barConfig = resolvePanelBarConfig(m_config, m_platform, request.output, request.sourceBarName);
  m_sourceBarName = request.sourceBarName.empty() ? barConfig.name : std::string(request.sourceBarName);
  if (m_attachedPanelLayerProvider != nullptr) {
    if (auto layer = m_attachedPanelLayerProvider(request.output, m_sourceBarName); layer.has_value()) {
      barConfig.layer = *layer;
    }
  }
  const bool isBottom = barConfig.position == "bottom";
  const bool isLeft = barConfig.position == "left";
  const bool isRight = barConfig.position == "right";
  constexpr std::int32_t panelGap = 12;
  const auto screenPadding = static_cast<std::int32_t>(Style::spaceSm);

  std::int32_t resolvedOutputWidth = 0;
  std::int32_t resolvedOutputHeight = 0;
  if (m_platform != nullptr) {
    const auto* wlOutput = m_platform->findOutputByWl(request.output);
    if (wlOutput != nullptr && wlOutput->effectiveLogicalWidth() > 0) {
      resolvedOutputWidth = wlOutput->effectiveLogicalWidth();
    }
    if (wlOutput != nullptr && wlOutput->effectiveLogicalHeight() > 0) {
      resolvedOutputHeight = wlOutput->effectiveLogicalHeight();
    }
  }
  // Backstop clamp: never request a surface larger than the output — the
  // compositor renders such a surface broken. This is sanity capping, not
  // work-area layout; if the compositor still configures smaller (exclusive
  // zones), buildScene lays out at the configured size.
  if (resolvedOutputWidth > 0) {
    panelWidth = std::min(panelWidth, static_cast<std::uint32_t>(std::max(1, resolvedOutputWidth - screenPadding * 2)));
  }
  if (resolvedOutputHeight > 0) {
    panelHeight =
        std::min(panelHeight, static_cast<std::uint32_t>(std::max(1, resolvedOutputHeight - screenPadding * 2)));
  }
  const std::int32_t outputWidth =
      resolvedOutputWidth > 0 ? resolvedOutputWidth : static_cast<std::int32_t>(panelWidth);
  const std::int32_t outputHeight =
      resolvedOutputHeight > 0 ? resolvedOutputHeight : static_cast<std::int32_t>(panelHeight);

  const auto clampMargin = [](float desired, std::int32_t panelSize, std::int32_t outputSize,
                              std::int32_t padding) -> std::int32_t {
    const std::int32_t maxValue = std::max(padding, outputSize - panelSize - padding);
    return static_cast<std::int32_t>(std::clamp(desired, static_cast<float>(padding), static_cast<float>(maxValue)));
  };

  PanelPlacement activePlacement = m_activePanel->panelPlacement();
  const bool fillWidth = m_activePanel->fillsWidth() && !m_panelCustomWidth;
  const bool fillHeight = m_activePanel->fillsHeight();
  if ((fillWidth || fillHeight) && activePlacement != PanelPlacement::Floating) {
    kLog.warn("panel manager: \"{}\" uses fill sizing, which requires floating placement — opening floating", panelId);
    activePlacement = PanelPlacement::Floating;
  }
  // A panel opened directly from a rail icon is a bar popout. Caelestia keeps
  // these in the shared frame/blob instead of honoring a legacy floating
  // placement. Launcher and session deliberately retain their fixed frame
  // slots, and fill-sized destinations remain standalone.
  if (request.hasExplicitAnchor && !keepsFixedFrameSlot(m_activePanelId) && !fillWidth && !fillHeight) {
    activePlacement = PanelPlacement::Attached;
  }
  m_panelFillWidth = fillWidth;
  m_panelFillHeight = fillHeight;
  m_panelDynamicVisualSize = m_activePanel->usesDynamicVisualSize();
  m_intrinsicVisualHeightResolved = false;
  const bool pluginPanel = m_activePanelId.contains(':');
  const std::string panelPosition =
      pluginPanel ? m_activePanel->panelScreenPosition() : resolvePanelPosition(m_config, m_activePanelId);
  m_panelScreenPosition = panelPosition;
  const AttachedRevealDirection detachedDirection = detachedRevealDirection(panelPosition, barConfig.position);
  const bool useTriggerAnchor = requestUsesTriggerAnchor(request, m_activePanel, m_activePanelId, m_config);
  const bool useScreenPosition = !useTriggerAnchor
      && activePlacement == PanelPlacement::Floating
      && panelPosition != "auto"
      && panelPosition != "center";
  const bool useCenteredPlacement = !useTriggerAnchor
      && ((activePlacement == PanelPlacement::Floating && panelPosition == "center")
          || (activePlacement == PanelPlacement::Attached
              && m_attachedPanelAvailabilityCallback != nullptr
              && !m_attachedPanelAvailabilityCallback(request.output, m_sourceBarName)));
  const bool useFloatingAnchor = !useCenteredPlacement && useTriggerAnchor;
  const auto detachedShadowBleed =
      detachedPanelSurfaceBleed(m_activePanel->hasDecoration(), m_config->config().shell.shadow);
  const std::uint32_t detachedSurfaceWidth =
      panelSurfaceExtent(panelWidth, detachedShadowBleed.left, detachedShadowBleed.right);
  const std::uint32_t detachedSurfaceHeight =
      panelSurfaceExtent(panelHeight, detachedShadowBleed.up, detachedShadowBleed.down);
  const auto chromeLayout =
      resolveChromeLayoutContext(barConfig, m_config->config().shell, outputWidth, outputHeight);
  const auto& barRect = chromeLayout.barRect;
  const bool multipleBarsOnEdge =
      hasMultipleEnabledBarsOnEdge(m_config, m_platform, request.output, barConfig.position);
  const bool useReservedEdgePlacement =
      !useCenteredPlacement && !useScreenPosition && multipleBarsOnEdge && barConfig.thickness > 0;
  const auto marginLeftFromAnchor = clampMargin(
      request.anchorX - static_cast<float>(panelWidth) * 0.5f, static_cast<std::int32_t>(panelWidth), outputWidth,
      screenPadding
  );
  const auto marginTopFromAnchor = clampMargin(
      request.anchorY - static_cast<float>(panelHeight) * 0.5f, static_cast<std::int32_t>(panelHeight), outputHeight,
      screenPadding
  );

  std::uint32_t standaloneAnchor = 0;
  std::int32_t standaloneMarginTop = 0;
  std::int32_t standaloneMarginRight = 0;
  std::int32_t standaloneMarginBottom = 0;
  std::int32_t standaloneMarginLeft = 0;
  if (!useCenteredPlacement) {
    const std::int32_t barWidth = std::max(0, barRect.right - barRect.left);
    const std::int32_t barHeight = std::max(0, barRect.bottom - barRect.top);
    const auto centeredAlongBarX = clampMargin(
        static_cast<float>(barRect.left) + (static_cast<float>(barWidth) - static_cast<float>(panelWidth)) * 0.5f,
        static_cast<std::int32_t>(panelWidth), outputWidth, screenPadding
    );
    const auto centeredAlongBarY = clampMargin(
        static_cast<float>(barRect.top) + (static_cast<float>(barHeight) - static_cast<float>(panelHeight)) * 0.5f,
        static_cast<std::int32_t>(panelHeight), outputHeight, screenPadding
    );

    if (useScreenPosition) {
      // Pinned to a screen edge/corner, independent of the bar.
      const auto sp = shell::screenPositionAnchor(panelPosition, panelGap);
      standaloneAnchor = sp.anchor;
      standaloneMarginTop = sp.marginTop;
      standaloneMarginRight = sp.marginRight;
      standaloneMarginBottom = sp.marginBottom;
      standaloneMarginLeft = sp.marginLeft;
    } else if (useReservedEdgePlacement) {
      if (isLeft) {
        standaloneAnchor = LayerShellAnchor::Left | LayerShellAnchor::Top;
        standaloneMarginLeft = panelGap;
        standaloneMarginTop = useFloatingAnchor ? marginTopFromAnchor : centeredAlongBarY;
      } else if (isRight) {
        standaloneAnchor = LayerShellAnchor::Right | LayerShellAnchor::Top;
        standaloneMarginRight = panelGap;
        standaloneMarginTop = useFloatingAnchor ? marginTopFromAnchor : centeredAlongBarY;
      } else if (isBottom) {
        standaloneAnchor = LayerShellAnchor::Bottom | LayerShellAnchor::Left;
        standaloneMarginBottom = panelGap;
        standaloneMarginLeft = useFloatingAnchor ? marginLeftFromAnchor : centeredAlongBarX;
      } else {
        standaloneAnchor = LayerShellAnchor::Top | LayerShellAnchor::Left;
        standaloneMarginTop = panelGap;
        standaloneMarginLeft = useFloatingAnchor ? marginLeftFromAnchor : centeredAlongBarX;
      }
    } else {
      standaloneAnchor = LayerShellAnchor::Top | LayerShellAnchor::Left;
      if (isLeft) {
        standaloneMarginLeft = clampMargin(
            static_cast<float>(barRect.right + panelGap), static_cast<std::int32_t>(panelWidth), outputWidth,
            screenPadding
        );
        standaloneMarginTop = useFloatingAnchor ? marginTopFromAnchor : centeredAlongBarY;
      } else if (isRight) {
        standaloneMarginLeft = clampMargin(
            static_cast<float>(barRect.left - static_cast<std::int32_t>(panelWidth) - panelGap),
            static_cast<std::int32_t>(panelWidth), outputWidth, screenPadding
        );
        standaloneMarginTop = useFloatingAnchor ? marginTopFromAnchor : centeredAlongBarY;
      } else if (isBottom) {
        standaloneMarginTop = clampMargin(
            static_cast<float>(barRect.top - static_cast<std::int32_t>(panelHeight) - panelGap),
            static_cast<std::int32_t>(panelHeight), outputHeight, screenPadding
        );
        standaloneMarginLeft = useFloatingAnchor ? marginLeftFromAnchor : centeredAlongBarX;
      } else {
        standaloneMarginTop = clampMargin(
            static_cast<float>(barRect.bottom + panelGap), static_cast<std::int32_t>(panelHeight), outputHeight,
            screenPadding
        );
        standaloneMarginLeft = useFloatingAnchor ? marginLeftFromAnchor : centeredAlongBarX;
      }
    }
  }

  if (useCenteredPlacement) {
    standaloneAnchor = LayerShellAnchor::Top | LayerShellAnchor::Left;
    standaloneMarginLeft = (outputWidth - static_cast<std::int32_t>(panelWidth)) / 2 - detachedShadowBleed.left;
    standaloneMarginTop = (outputHeight - static_cast<std::int32_t>(panelHeight)) / 2 - detachedShadowBleed.up;
  } else {
    if ((standaloneAnchor & LayerShellAnchor::Left) != 0) {
      standaloneMarginLeft -= detachedShadowBleed.left;
    } else if ((standaloneAnchor & LayerShellAnchor::Right) != 0) {
      standaloneMarginRight -= detachedShadowBleed.right;
    }
    if ((standaloneAnchor & LayerShellAnchor::Top) != 0) {
      standaloneMarginTop -= detachedShadowBleed.up;
    } else if ((standaloneAnchor & LayerShellAnchor::Bottom) != 0) {
      standaloneMarginBottom -= detachedShadowBleed.down;
    }
  }

  // Single-bar detached panels are placed relative to the bar's config edge. Honor
  // other surfaces' exclusive zones (exclusive_zone = 0 below) and anchor to the
  // bar's reserved edge so the panel tracks the bar's real on-screen position;
  // subtract the bar's own reservation on the main axis to avoid double-counting.
  // Reproduces the prior absolute placement when nothing else reserves space.
  const bool useBarRelativeDetached = !useCenteredPlacement && !useScreenPosition && !useReservedEdgePlacement;
  if (useBarRelativeDetached) {
    const std::int32_t barReserved = reservedBarEdgeDistance(barConfig, m_config->config().shell);
    const auto sw = static_cast<std::int32_t>(detachedSurfaceWidth);
    const auto sh = static_cast<std::int32_t>(detachedSurfaceHeight);
    if (isBottom) {
      standaloneAnchor = LayerShellAnchor::Bottom | LayerShellAnchor::Left;
      standaloneMarginBottom = outputHeight - sh - standaloneMarginTop - barReserved;
      standaloneMarginTop = 0;
    } else if (isRight) {
      standaloneAnchor = LayerShellAnchor::Top | LayerShellAnchor::Right;
      standaloneMarginRight = outputWidth - sw - standaloneMarginLeft - barReserved;
      standaloneMarginLeft = 0;
    } else if (isLeft) {
      standaloneMarginLeft -= barReserved;
    } else {
      standaloneMarginTop -= barReserved;
    }
  }

  // A filled axis dual-anchors the surface with a requested size of 0: the
  // compositor assigns the extent, subtracting every exclusive zone on the
  // output (all bars and any third-party client) — the shell never computes
  // the work area itself. Margins keep the screen padding around the visible
  // body (the shadow bleed sits outside the padding); they override whatever
  // the placement branches above computed on that axis. The default size is
  // only the fallback if the compositor assigns nothing.
  std::uint32_t requestedSurfaceWidth = detachedSurfaceWidth;
  std::uint32_t requestedSurfaceHeight = detachedSurfaceHeight;
  std::uint32_t fallbackSurfaceWidth = detachedSurfaceWidth;
  std::uint32_t fallbackSurfaceHeight = detachedSurfaceHeight;
  if (fillWidth) {
    standaloneAnchor |= LayerShellAnchor::Left | LayerShellAnchor::Right;
    standaloneMarginLeft = 0 - detachedShadowBleed.left;
    standaloneMarginRight = 0 - detachedShadowBleed.right;
    requestedSurfaceWidth = 0;
    fallbackSurfaceWidth =
        static_cast<std::uint32_t>(std::max(1, outputWidth - standaloneMarginLeft - standaloneMarginRight));
  }
  if (fillHeight) {
    standaloneAnchor |= LayerShellAnchor::Top | LayerShellAnchor::Bottom;
    standaloneMarginTop = 0 - detachedShadowBleed.up;
    standaloneMarginBottom = 0 - detachedShadowBleed.down;
    requestedSurfaceHeight = 0;
    fallbackSurfaceHeight =
        static_cast<std::uint32_t>(std::max(1, outputHeight - standaloneMarginTop - standaloneMarginBottom));
  }
  if (detachedFrameJoinEdge(m_activePanel, m_activePanelId) == "right") {
    // Put the visible session rail flush against the inner edge of the shared
    // screen frame. Its filled vertical surface only supplies room for centring
    // and concave corners; the rest stays input-transparent.
    standaloneMarginRight = chromeFrameThickness(m_config->config().shell) - detachedShadowBleed.right;
  }
  if (detachedFrameJoinEdge(m_activePanel, m_activePanelId) == "top") {
    // Keep the dashboard body flush with the inner edge of the structural
    // frame. Its concave top corners are then continuous with the frame.
    standaloneMarginTop = chromeFrameThickness(m_config->config().shell) - detachedShadowBleed.up;
  }

  const bool useFixedFrameChrome = keepsFixedFrameSlot(m_activePanelId)
      && outputWidth > 0
      && outputHeight > 0
      && (m_attachedPanelAvailabilityCallback == nullptr
          || m_attachedPanelAvailabilityCallback(request.output, m_sourceBarName));
  if (useFixedFrameChrome) {
    // The content needs the whole output as its animation canvas, but remains
    // input-transparent outside the visible body.  The material itself is
    // published to the bar's ChromeOutputHost below.
    standaloneAnchor =
        LayerShellAnchor::Top | LayerShellAnchor::Right | LayerShellAnchor::Bottom | LayerShellAnchor::Left;
    standaloneMarginTop = 0;
    standaloneMarginRight = 0;
    standaloneMarginBottom = 0;
    standaloneMarginLeft = 0;
    requestedSurfaceWidth = 0;
    requestedSurfaceHeight = 0;
    fallbackSurfaceWidth = static_cast<std::uint32_t>(outputWidth);
    fallbackSurfaceHeight = static_cast<std::uint32_t>(outputHeight);
  }

  const bool useAttachedPlacement = activePlacement == PanelPlacement::Attached
      && (m_attachedPanelAvailabilityCallback == nullptr
          || m_attachedPanelAvailabilityCallback(request.output, m_sourceBarName))
      && barConfig.thickness > 0
      && outputWidth > 0
      && outputHeight > 0;
  const LayerShellLayer panelLayer =
      useAttachedPlacement ? layerShellLayerFromConfig(barConfig.layer) : m_activePanel->layer();
  // Niri assigns its internal on-demand focus token only when a surface is
  // first mapped as OnDemand. Mapping as Exclusive and changing the mode
  // later drops focus immediately because that token was never created.
  // Preserve the panel's requested mode from the initial layer-shell commit;
  // this gives launchers their Niri focus token without making the pointer
  // shield keyboard-interactive.
  const LayerShellKeyboard openingKeyboardMode = m_activePanel->keyboardMode();

  auto surfaceConfig = LayerSurfaceConfig{
      .nameSpace = "gnil-drawer",
      .layer = m_activePanel->layer(),
      .anchor = standaloneAnchor,
      .width = requestedSurfaceWidth,
      .height = requestedSurfaceHeight,
      // Centered panels ignore exclusive zones; filled axes must respect them
      // (that is what makes the compositor subtract bars and other clients).
      .exclusiveZone = (useFixedFrameChrome || (useCenteredPlacement && !fillWidth && !fillHeight)) ? -1 : 0,
      .marginTop = standaloneMarginTop,
      .marginRight = standaloneMarginRight,
      .marginBottom = standaloneMarginBottom,
      .marginLeft = standaloneMarginLeft,
      .keyboard = openingKeyboardMode,
      .defaultWidth = fallbackSurfaceWidth,
      .defaultHeight = fallbackSurfaceHeight,
      .prewarmBlur = true,
  };

  const auto configureSurfaceCallbacks = [this](Surface& surface) {
    surface.setRenderContext(m_renderContext);
    surface.setConfigureCallback([this](std::uint32_t /*width*/, std::uint32_t /*height*/) {
      if (m_surface != nullptr) {
        m_surface->requestLayout();
      }
    });
    surface.setPrepareFrameCallback([this](bool needsUpdate, bool needsLayout) {
      prepareFrame(needsUpdate, needsLayout);
    });
    surface.setFrameTickCallback([this](float deltaMs) {
      continueKeyboardFocusBootstrapAfterMap();
      startAttachedOpenAnimation();
      if (m_activePanel != nullptr) {
        m_activePanel->onFrameTick(deltaMs);
      }
    });
    surface.setAnimationManager(&m_animations);
  };

  const auto resetPanelOpenState = [this]() {
    m_clickShield.deactivate();
    m_surface.reset();
    m_layerSurface = nullptr;
    m_output = nullptr;
    m_wlSurface = nullptr;
    m_activePanel = nullptr;
    m_activePanelId.clear();
    m_pendingOpenContext.clear();
    m_panelInsetX = 0;
    m_panelInsetY = 0;
    m_panelVisualWidth = 0;
    m_panelVisualHeight = 0;
    m_panelVisualWidthExact = 0.0f;
    m_panelVisualHeightExact = 0.0f;
    m_panelFillWidth = false;
    m_panelFillHeight = false;
    m_panelCustomWidth = false;
    m_panelDynamicVisualSize = false;
    m_intrinsicVisualHeightResolved = false;
    m_panelGeometryDirty = false;
    m_panelResizeAnimation = 0;
    m_panelMorphAnimation = 0;
    m_panelContentAnimation = 0;
    m_retainedContentLayers.clear();
    m_activeContentVisualWidth = 1.0f;
    m_activeContentVisualHeight = 1.0f;
    m_activeContentPadding = 0.0f;
    m_pendingVisualSize = false;
    m_panelScreenPosition.clear();
    m_detachedBleedLeft = 0;
    m_detachedBleedTop = 0;
    m_detachedBleedRight = 0;
    m_detachedBleedBottom = 0;
    m_attachedBackgroundOpacity = 1.0f;
    m_attachedContactShadow = false;
    m_attachedRevealProgress = 1.0f;
    m_detachedRevealProgress = 1.0f;
    m_attachedRevealDirection = AttachedRevealDirection::Down;
    m_detachedRevealDirection = AttachedRevealDirection::Down;
    m_attachedBarPosition.clear();
    m_sourceBarName.clear();
    m_chromePanelState.reset();
    m_attachedToBar = false;
    m_chromeHosted = false;
    m_attachedOpenAnimationPending = false;
    m_openTriggerSurface = nullptr;
    m_openTriggerSerial = 0;
    m_keyboardFocusBootstrapScheduled = false;
    m_keyboardFocusBootstrapActive = false;
    m_keyboardFocusRelaxTimer.stop();
    m_keyboardFocus.endOpen();
    m_clickShieldRequested = false;
  };

  if (useAttachedPlacement) {
    const std::string_view barPosition = barConfig.position;
    const bool barIsBottom = barPosition == "bottom";
    const bool barIsLeft = barPosition == "left";
    const bool barIsRight = barPosition == "right";
    const bool barIsVertical = barIsLeft || barIsRight;

    // Like Caelestia's ContentWindow, attached popouts use an output-sized
    // transparent surface. This gives the concave SDF enough room at screen
    // ends and prevents layer-shell margins/exclusive zones from displacing a
    // popout independently from its bar.
    const std::uint32_t surfaceWidth = static_cast<std::uint32_t>(outputWidth);
    const std::uint32_t surfaceHeight = static_cast<std::uint32_t>(outputHeight);

    // Bar visible rect in screen coords, derived from BarConfig + output dimensions.
    const std::int32_t barLeft = barRect.left;
    const std::int32_t barTop = barRect.top;
    const std::int32_t barRight = barRect.right;
    const std::int32_t barBottom = barRect.bottom;

    const std::int32_t frameThickness = chromeFrameThickness(m_config->config().shell);
    const auto edge = chromeLayout.barEdge;
    std::int32_t visualX = 0;
    std::int32_t visualY = 0;
    const bool useAnchorForAttached = requestUsesTriggerAnchor(request, m_activePanel, m_activePanelId, m_config);
    if (barIsVertical) {
      const auto minY = frameThickness;
      const auto maxY = std::max(minY, outputHeight - frameThickness - static_cast<std::int32_t>(panelHeight));
      const auto centeredY = barTop + (barBottom - barTop - static_cast<std::int32_t>(panelHeight)) / 2;
      const auto desiredY =
          static_cast<std::int32_t>(std::lround(request.anchorY - static_cast<float>(panelHeight) * 0.5f));
      visualY = useAnchorForAttached ? std::clamp(desiredY, minY, maxY) : centeredY;
    } else {
      const auto minX = frameThickness;
      const auto maxX = std::max(minX, outputWidth - frameThickness - static_cast<std::int32_t>(panelWidth));
      const auto centeredX = barLeft + (barRight - barLeft - static_cast<std::int32_t>(panelWidth)) / 2;
      const auto desiredX =
          static_cast<std::int32_t>(std::lround(request.anchorX - static_cast<float>(panelWidth) * 0.5f));
      visualX = useAnchorForAttached ? std::clamp(desiredX, minX, maxX) : centeredX;
    }

    // Keep the logical body (content and input) completely outside the bar.
    // The host unions this unmodified rounded body with the inverse aperture,
    // so the touching corners remain concave without a second render rect.
    const ChromeRect visibleBar{
        .x = static_cast<float>(barLeft),
        .y = static_cast<float>(barTop),
        .width = static_cast<float>(barRight - barLeft),
        .height = static_cast<float>(barBottom - barTop),
    };
    const auto logicalBody = chromePlaceAttachedBody(
        ChromeRect{
            .x = static_cast<float>(visualX),
            .y = static_cast<float>(visualY),
            .width = static_cast<float>(panelWidth),
            .height = static_cast<float>(panelHeight),
        },
        visibleBar, edge
    );
    visualX = static_cast<std::int32_t>(std::lround(logicalBody.x));
    visualY = static_cast<std::int32_t>(std::lround(logicalBody.y));

    m_panelInsetX = visualX;
    m_panelInsetY = visualY;
    m_panelVisualWidth = panelWidth;
    m_panelVisualHeight = panelHeight;
    m_panelVisualWidthExact = static_cast<float>(panelWidth);
    m_panelVisualHeightExact = static_cast<float>(panelHeight);
    // Caelestia renders the rail, global frame and attached drawer as one
    // BlobGroup.  GNIL has separate layer surfaces, therefore the drawer must
    // use the structural frame's opaque Surface fill rather than the bar's
    // optional translucency; otherwise the wallpaper is composited twice at
    // the seam and the drawer reads as a detached glass card.
    m_attachedBackgroundOpacity = 1.0f;
    // The shared chrome backdrop already makes the join continuous.  A second
    // contact gradient looks like a glass seam, so legacy contact_shadow is
    // intentionally ignored.
    m_attachedContactShadow = false;
    m_attachedRevealProgress = 0.0f;
    m_attachedRevealDirection = attached_panel::revealDirection(barPosition);
    m_attachedBarPosition = std::string(barPosition);
    m_attachedToBar = true;
    m_chromeHosted = true;

    // Chrome uses output-local body geometry directly. Smooth union replaces
    // the previous manually enlarged concave-corner rectangle.
    m_chromePanelState = ChromePanelState{
        .rect =
            ChromeRect{
                .x = static_cast<float>(visualX),
                .y = static_cast<float>(visualY),
                .width = static_cast<float>(panelWidth),
                .height = static_cast<float>(panelHeight),
            },
        .radius = std::max(1.0f, m_config->config().shell.chrome.rounding),
        .opacity = 1.0f,
        .progress = 1.0f,
        .edge = edge,
        .hasTriggerAnchor = useAnchorForAttached,
        .attached = true,
        .visible = true,
        .inputEnabled = true,
    };
    const ChromeGeometryModel geometry(chromeLayout.geometry);
    if (useAnchorForAttached) {
      m_chromePanelState->triggerAnchor = ChromePoint{.x = request.anchorX, .y = request.anchorY};
      *m_chromePanelState = geometry.resolveAnchoredPanel(
          *m_chromePanelState, visibleBar,
          static_cast<float>(outputWidth), static_cast<float>(outputHeight)
      );
      m_panelInsetX = static_cast<std::int32_t>(std::lround(m_chromePanelState->rect.x));
      m_panelInsetY = static_cast<std::int32_t>(std::lround(m_chromePanelState->rect.y));
    } else {
      m_chromePanelState->rect = geometry.clampPanel(
          m_chromePanelState->rect, edge, static_cast<float>(outputWidth), static_cast<float>(outputHeight)
      );
    }
    // Start as a collapsed body on the contact edge. Translating a complete
    // body off-screen lets one frame of wallpaper show between the bar and the
    // panel before the two SDFs meet.
    const auto initialChromeState = hiddenChromeState(*m_chromePanelState, m_attachedRevealDirection);
    m_chromeTransition.reset(initialChromeState);
    m_chromeTransition.setTarget(*m_chromePanelState);

    auto attachedConfig = LayerSurfaceConfig{
        .nameSpace = "gnil-drawer-attached",
        .layer = panelLayer,
        .anchor = LayerShellAnchor::Top | LayerShellAnchor::Right | LayerShellAnchor::Bottom | LayerShellAnchor::Left,
        .width = 0,
        .height = 0,
        .exclusiveZone = -1,
        .keyboard = openingKeyboardMode,
        .defaultWidth = surfaceWidth,
        .defaultHeight = surfaceHeight,
        .prewarmBlur = true,
    };

    activateClickShield(panelLayer);
    auto layerSurfaceUnique = std::make_unique<LayerSurface>(m_platform->wayland(), std::move(attachedConfig));
    m_layerSurface = layerSurfaceUnique.get();
    m_surface = std::move(layerSurfaceUnique);
    configureSurfaceCallbacks(*m_surface);

    m_inTransition = true;
    const bool ok = m_layerSurface->initialize(request.output);
    m_inTransition = false;

    if (ok) {
      m_output = request.output;
      m_wlSurface = m_surface->wlSurface();
      m_surface->setInputRegion(
          {InputRect{m_panelInsetX, m_panelInsetY, static_cast<int>(panelWidth), static_cast<int>(panelHeight)}}
      );
      m_surface->setBlurRegion({});
      publishChromePanelState(m_attachedRevealProgress);
      m_surface->requestRedraw();
      scheduleKeyboardFocusBootstrap();
      kLog.debug("panel manager: opened \"{}\" as attached layer-shell", panelId);
      if (m_panelOpenedCallback) {
        m_panelOpenedCallback();
      }
      return;
    }

    if (m_chromePanelStateCallback) {
      m_chromePanelStateCallback(request.output, m_sourceBarName, std::nullopt);
    }
    m_surface.reset();
    m_layerSurface = nullptr;
    m_attachedToBar = false;
    m_chromeHosted = false;
    m_panelInsetX = 0;
    m_panelInsetY = 0;
    m_panelVisualWidth = 0;
    m_panelVisualHeight = 0;
    m_attachedBackgroundOpacity = 1.0f;
    m_attachedContactShadow = false;
    m_attachedRevealProgress = 1.0f;
    m_detachedRevealProgress = 1.0f;
    m_attachedRevealDirection = AttachedRevealDirection::Down;
    m_detachedRevealDirection = AttachedRevealDirection::Down;
    m_attachedBarPosition.clear();
    m_chromePanelState.reset();
    m_attachedOpenAnimationPending = false;
    m_clickShield.deactivate();
    kLog.warn("panel manager: attached layer-shell failed for \"{}\", falling back to standalone", panelId);
  }

  auto layerSurface = std::make_unique<LayerSurface>(m_platform->wayland(), std::move(surfaceConfig));
  m_layerSurface = layerSurface.get();
  m_surface = std::move(layerSurface);
  m_panelInsetX = detachedShadowBleed.left;
  m_panelInsetY = detachedShadowBleed.up;
  const float initialVisualWidth =
      m_panelDynamicVisualSize && !m_panelCustomWidth ? m_activePanel->initialVisualWidth() : static_cast<float>(panelWidth);
  const float initialVisualHeight =
      m_panelDynamicVisualSize ? m_activePanel->initialVisualHeight() : static_cast<float>(panelHeight);
  m_panelVisualWidthExact = std::max(1.0f, initialVisualWidth);
  m_panelVisualHeightExact = std::max(1.0f, initialVisualHeight);
  m_panelVisualWidth = static_cast<std::uint32_t>(std::lround(m_panelVisualWidthExact));
  m_panelVisualHeight = static_cast<std::uint32_t>(std::lround(m_panelVisualHeightExact));
  m_detachedBleedLeft = useFixedFrameChrome ? 0 : detachedShadowBleed.left;
  m_detachedBleedTop = useFixedFrameChrome ? 0 : detachedShadowBleed.up;
  m_detachedBleedRight = useFixedFrameChrome ? 0 : detachedShadowBleed.right;
  m_detachedBleedBottom = useFixedFrameChrome ? 0 : detachedShadowBleed.down;
  m_attachedBackgroundOpacity = 1.0f;
  m_attachedContactShadow = false;
  m_attachedRevealProgress = 1.0f;
  // This path publishes the compositor blur region before the first scene build.
  // Keep detached panels hidden until buildScene applies the opening reveal.
  m_detachedRevealProgress = 0.0f;
  m_attachedRevealDirection = AttachedRevealDirection::Down;
  m_detachedRevealDirection = detachedDirection;
  m_chromePanelState.reset();
  m_attachedToBar = false;
  m_chromeHosted = useFixedFrameChrome;
  if (useFixedFrameChrome) {
    const auto& chrome = m_config->config().shell.chrome;
    ChromeGeometryModel geometry(chromeLayout.geometry);
    ChromeRect rect{
        .x = (static_cast<float>(outputWidth) - static_cast<float>(m_panelVisualWidth)) * 0.5f,
        .y = (static_cast<float>(outputHeight) - static_cast<float>(m_panelVisualHeight)) * 0.5f,
        .width = static_cast<float>(m_panelVisualWidth),
        .height = static_cast<float>(m_panelVisualHeight),
    };
    if (m_panelScreenPosition.ends_with("_right")) {
      rect.x = static_cast<float>(outputWidth) - rect.width;
    } else if (m_panelScreenPosition.ends_with("_left")) {
      rect.x = 0.0f;
    }
    if (m_panelScreenPosition.starts_with("top_")) {
      rect.y = 0.0f;
    } else if (m_panelScreenPosition.starts_with("bottom_")) {
      rect.y = static_cast<float>(outputHeight) - rect.height;
    }
    rect = geometry.clampPanel(
        rect, m_activePanel->chromeEdge(), static_cast<float>(outputWidth), static_cast<float>(outputHeight)
    );
    m_panelInsetX = static_cast<std::int32_t>(std::lround(rect.x));
    m_panelInsetY = static_cast<std::int32_t>(std::lround(rect.y));
    m_chromePanelState = ChromePanelState{
        .rect = rect,
        .radius = std::max(1.0f, chrome.rounding),
        .opacity = 1.0f,
        .progress = 1.0f,
        .edge = m_activePanel->chromeEdge(),
        .attached = true,
        .visible = true,
        .inputEnabled = true,
    };
    const auto initialChromeState = hiddenChromeState(*m_chromePanelState, m_detachedRevealDirection);
    m_chromeTransition.reset(initialChromeState);
    m_chromeTransition.setTarget(*m_chromePanelState);
  }
  configureSurfaceCallbacks(*m_surface);

  // Guard against re-entrancy: initialize can process queued Wayland events.
  activateClickShield(m_activePanel->layer());
  m_inTransition = true;
  bool ok = m_layerSurface->initialize(request.output);
  m_inTransition = false;

  if (!ok) {
    kLog.warn("panel manager: failed to initialize surface for panel \"{}\"", panelId);
    resetPanelOpenState();
    return;
  }

  m_output = request.output;
  m_wlSurface = m_surface->wlSurface();
  m_surface->setInputRegion({InputRect{
      m_panelInsetX, m_panelInsetY, static_cast<int>(m_panelVisualWidth), static_cast<int>(m_panelVisualHeight)
  }});
  m_surface->setBlurRegion({});
  if (m_chromeHosted) {
    publishChromePanelState(m_detachedRevealProgress);
  }
  scheduleKeyboardFocusBootstrap();
  kLog.debug("panel manager: opened \"{}\"", panelId);
  if (m_panelOpenedCallback) {
    m_panelOpenedCallback();
  }
}

void PanelManager::positionAttachedContentLayers(const ChromePanelState& displayed) {
  const float centerX = displayed.rect.x + displayed.rect.width * 0.5f;
  const float centerY = displayed.rect.y + displayed.rect.height * 0.5f;
  const auto positionLayer = [centerX, centerY](
                                 Node* node, float visualWidth, float visualHeight, float padding
                             ) {
    if (node == nullptr) {
      return;
    }
    const float contentWidth = std::max(0.0f, visualWidth - padding * 2.0f);
    const float contentHeight = std::max(0.0f, visualHeight - padding * 2.0f);
    node->setPosition(
        centerX - visualWidth * 0.5f + padding,
        centerY - visualHeight * 0.5f + padding
    );
    node->setFrameSize(contentWidth, contentHeight);
  };

  positionLayer(
      m_contentNode, m_activeContentVisualWidth, m_activeContentVisualHeight, m_activeContentPadding
  );
  for (const auto& layer : m_retainedContentLayers) {
    positionLayer(layer.node, layer.visualWidth, layer.visualHeight, layer.padding);
  }
}

void PanelManager::clearRetainedContentLayers() {
  for (auto& layer : m_retainedContentLayers) {
    if (layer.panel != nullptr && layer.panel != m_activePanel) {
      layer.panel->onClose();
    }
    if (layer.node != nullptr && layer.node->parent() != nullptr) {
      (void)layer.node->parent()->removeChild(layer.node);
    }
  }
  m_retainedContentLayers.clear();
}

void PanelManager::applyAttachedMorphState(ChromePanelState displayed) {
  if (!m_attachedToBar || !m_chromePanelState.has_value() || m_sceneRoot == nullptr) {
    return;
  }
  displayed.opacity = 1.0f;
  displayed.progress = 1.0f;
  displayed.visible = true;
  displayed.inputEnabled = !m_closing;

  const auto visibleBody = chromeAttachedRevealClip(displayed.rect, m_chromePanelState->rect, displayed.edge);
  if (m_attachedRevealClipNode != nullptr && m_attachedRevealContentNode != nullptr) {
    m_attachedRevealClipNode->setPosition(visibleBody.x, visibleBody.y);
    m_attachedRevealClipNode->setFrameSize(visibleBody.width, visibleBody.height);
    // Children use output-local coordinates. The clip and its child cancel
    // each other's translation so natural-size content only follows the
    // shared panel centre; it is never reflowed at an intermediate width.
    m_attachedRevealContentNode->setPosition(-visibleBody.x, -visibleBody.y);
    m_attachedRevealContentNode->setFrameSize(m_sceneRoot->width(), m_sceneRoot->height());
  }
  positionAttachedContentLayers(displayed);

  if (m_surface != nullptr) {
    const int inputWidth = std::max(0, static_cast<int>(std::lround(visibleBody.width)));
    const int inputHeight = std::max(0, static_cast<int>(std::lround(visibleBody.height)));
    if (!m_closing && inputWidth > 0 && inputHeight > 0) {
      m_surface->setInputRegion({InputRect{
          static_cast<int>(std::lround(visibleBody.x)), static_cast<int>(std::lround(visibleBody.y)),
          inputWidth, inputHeight
      }});
    } else {
      m_surface->setInputRegion({});
    }
    m_surface->requestRedraw();
  }
  if (m_chromePanelStateCallback) {
    m_chromePanelStateCallback(m_output, m_sourceBarName, displayed);
  }
}

bool PanelManager::switchAttachedPanel(const std::string& panelId, PanelOpenRequest request) {
  if (!m_attachedToBar || m_sceneRoot == nullptr || m_attachedRevealContentNode == nullptr
      || m_activePanel == nullptr || m_config == nullptr || m_platform == nullptr) {
    return false;
  }
  const auto nextIt = m_panels.find(panelId);
  if (nextIt == m_panels.end() || nextIt->second == nullptr || keepsFixedFrameSlot(panelId)
      || panelId == "sidebar") {
    return false;
  }
  Panel* const nextPanel = nextIt->second.get();
  if (nextPanel->fillsWidth() || nextPanel->fillsHeight()) {
    return false;
  }
  if (!request.hasExplicitAnchor && request.sourceBarName.empty()) {
    return false;
  }
  if (request.output == nullptr) {
    request.output = m_output;
  }
  if (request.output != m_output) {
    return false;
  }
  if (request.sourceBarName.empty()) {
    request.sourceBarName = m_sourceBarName;
  }
  if (request.sourceBarName != m_sourceBarName) {
    return false;
  }

  // Keep every still-visible outgoing layer alive across a rapid retarget.
  // Caelestia's individual loaders behave the same way: a new destination is
  // measured immediately while prior content finishes its effects fade.
  if (m_panelContentAnimation != 0) {
    m_animations.cancel(m_panelContentAnimation);
    m_panelContentAnimation = 0;
  }
  // A panel object cannot own two live UI trees. If a very fast reversal
  // targets a retained instance, retire only that oldest copy before create().
  for (auto it = m_retainedContentLayers.begin(); it != m_retainedContentLayers.end();) {
    if (it->panel != nextPanel) {
      ++it;
      continue;
    }
    if (it->panel != nullptr) {
      it->panel->onClose();
    }
    if (it->node != nullptr && it->node->parent() != nullptr) {
      (void)it->node->parent()->removeChild(it->node);
    }
    it = m_retainedContentLayers.erase(it);
  }

  Panel* const previousPanel = m_activePanel;
  Node* const previousContent = m_contentNode;
  const float previousVisualWidth = m_activeContentVisualWidth;
  const float previousVisualHeight = m_activeContentVisualHeight;
  const float previousPadding = m_activeContentPadding;
  m_inputDispatcher.setFocus(nullptr);
  if (previousContent != nullptr) {
    previousContent->setHitTestVisible(false);
  }

  m_activePanel = nextPanel;
  m_activePanelId = panelId;
  m_openTriggerSurface = request.triggerSurface;
  m_openTriggerSerial = m_platform->lastInputSerial();
  m_pendingOpenContext = std::string(request.context);
  nextPanel->setPendingOpenContext(request.context);
  nextPanel->setContentScale(resolvePanelContentScale(m_config));
  nextPanel->setAnimationManager(&m_animations);

  const auto* sizeOverride = panelSizeOverride(m_config, panelId);
  m_panelCustomWidth = sizeOverride != nullptr && sizeOverride->width.has_value();
  const float nextWidth = m_panelCustomWidth
      ? scaledPanelOverride(*sizeOverride->width, nextPanel->contentScale())
      : (nextPanel->usesDynamicVisualSize() ? nextPanel->initialVisualWidth() : nextPanel->preferredWidth());
  const float nextHeight = nextPanel->usesDynamicVisualSize()
      ? nextPanel->initialVisualHeight()
      : nextPanel->preferredHeight();
  m_panelDynamicVisualSize = nextPanel->usesDynamicVisualSize();
  m_intrinsicVisualHeightResolved = false;
  m_panelFillWidth = false;
  m_panelFillHeight = false;

  auto contentWrapper = std::make_unique<Node>();
  contentWrapper->setOpacity(0.0f);
  contentWrapper->setClipChildren(true);
  m_contentNode = contentWrapper.get();
  const float panelBackgroundOpacity = m_attachedBackgroundOpacity;
  nextPanel->setPanelCardOpacity(resolvePanelCardOpacity(m_config, panelBackgroundOpacity));
  nextPanel->setPanelBordersEnabled(m_config->config().shell.panel.borders);
  nextPanel->create();
  nextPanel->onOpen(request.context);
  m_pendingOpenContext.clear();
  if (nextPanel->root() != nullptr) {
    contentWrapper->addChild(nextPanel->releaseRoot());
  }
  m_attachedRevealContentNode->addChild(std::move(contentWrapper));

  // Resolve the destination's intrinsic height before retargeting the shared
  // chrome. Position, size and effective corners then travel as one morph
  // instead of completing the horizontal move at the 360px fallback height
  // and resizing afterwards.
  m_renderContext->makeCurrent(m_surface->renderTarget());
  {
    UiPhaseScope updatePhase(UiPhase::Update);
    nextPanel->update(*m_renderContext);
  }
  float resolvedHeight = std::max(1.0f, std::min(nextHeight, 720.0f * nextPanel->contentScale()));
  if (m_panelDynamicVisualSize) {
    const auto desired = nextPanel->desiredVisualHeight(*m_renderContext, std::max(1.0f, nextWidth));
    if (desired.has_value()) {
      resolvedHeight = std::clamp(*desired, 1.0f, std::max(1.0f, 720.0f * nextPanel->contentScale()));
      m_intrinsicVisualHeightResolved = true;
    }
  }
  m_panelVisualWidthExact = std::max(1.0f, nextWidth);
  m_panelVisualHeightExact = resolvedHeight;
  m_panelVisualWidth = static_cast<std::uint32_t>(std::lround(m_panelVisualWidthExact));
  m_panelVisualHeight = static_cast<std::uint32_t>(std::lround(m_panelVisualHeightExact));
  m_activeContentVisualWidth = m_panelVisualWidthExact;
  m_activeContentVisualHeight = m_panelVisualHeightExact;
  m_activeContentPadding = nextPanel->hasDecoration()
      ? nextPanel->contentScale() * Style::panelPadding
      : 0.0f;

  // Layout the destination at its final natural dimensions before the first
  // morph sample. Only its wrapper moves and clips during the transition.
  m_contentWidth = std::max(0.0f, m_activeContentVisualWidth - m_activeContentPadding * 2.0f);
  m_contentHeight = std::max(0.0f, m_activeContentVisualHeight - m_activeContentPadding * 2.0f);
  {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    nextPanel->layout(*m_renderContext, m_contentWidth, m_contentHeight);
  }
  if (m_contentNode != nullptr) {
    m_contentNode->setFrameSize(m_contentWidth, m_contentHeight);
  }

  if (!retargetAttachedPanel(request)) {
    return false;
  }

  if (previousContent != nullptr) {
    previousContent->setClipChildren(true);
    m_retainedContentLayers.push_back(RetainedContentLayer{
        .panel = previousPanel,
        .node = previousContent,
        .visualWidth = previousVisualWidth,
        .visualHeight = previousVisualHeight,
        .padding = previousPadding,
        .startOpacity = previousContent->opacity(),
    });
  }
  // Bound pathological click-spam without abruptly removing the most visible
  // recent layers.
  while (m_retainedContentLayers.size() > 4) {
    auto& oldest = m_retainedContentLayers.front();
    if (oldest.panel != nullptr && oldest.panel != m_activePanel) oldest.panel->onClose();
    if (oldest.node != nullptr && oldest.node->parent() != nullptr) {
      (void)oldest.node->parent()->removeChild(oldest.node);
    }
    m_retainedContentLayers.erase(m_retainedContentLayers.begin());
  }
  applyAttachedMorphState(m_chromeTransition.displayed());
  const float incomingOpacity = m_contentNode != nullptr ? m_contentNode->opacity() : 0.0f;
  for (auto& layer : m_retainedContentLayers) {
    layer.startOpacity = layer.node != nullptr ? layer.node->opacity() : 0.0f;
  }
  Node* const incomingContent = m_contentNode;
  m_panelContentAnimation = m_animations.animate(
      0.0f, 1.0f, Style::animBarPopoutContentIn, Easing::Linear,
      [this, incomingContent, incomingOpacity](float progress) {
        const float incomingProgress = applyEasing(Easing::CaelestiaSlowEffects, progress);
        const float outgoingLinear = std::clamp(
            progress * static_cast<float>(Style::animBarPopoutContentIn)
                / static_cast<float>(Style::animBarPopoutContentOut),
            0.0f, 1.0f
        );
        const float outgoingProgress = applyEasing(Easing::CaelestiaDefaultEffects, outgoingLinear);
        if (incomingContent != nullptr) {
          incomingContent->setOpacity(std::lerp(incomingOpacity, 1.0f, incomingProgress));
        }
        for (auto& layer : m_retainedContentLayers) {
          if (layer.node != nullptr) {
            layer.node->setOpacity(std::lerp(layer.startOpacity, 0.0f, outgoingProgress));
          }
        }
        if (m_surface != nullptr) {
          m_surface->requestRedraw();
        }
      },
      [this, incomingContent]() {
        m_panelContentAnimation = 0;
        // A newer retarget may have replaced m_contentNode while this callback
        // was queued. Only the current transition owns the retained stack.
        if (m_contentNode == incomingContent) {
          clearRetainedContentLayers();
        }
      },
      m_sceneRoot.get()
  );
  m_panelGeometryDirty = true;
  m_surface->requestLayout();
  m_surface->requestFrameTick();
  if (m_panelOpenedCallback) {
    m_panelOpenedCallback();
  }
  return true;
}

void PanelManager::activateClickShield(LayerShellLayer layer) {
  if (!m_clickShieldRequested || m_platform == nullptr) {
    return;
  }
  std::vector<wl_output*> outputs;
  outputs.reserve(m_platform->outputs().size());
  for (const auto& output : m_platform->outputs()) {
    if (output.output != nullptr) {
      outputs.push_back(output.output);
    }
  }
  m_clickShield.activate(outputs, layer, m_clickShieldExcludeRectsProvider);
}

void PanelManager::syncClickShieldOutputs() {
  if (!m_clickShieldRequested || !m_clickShield.isArmed() || m_platform == nullptr) {
    return;
  }
  std::vector<wl_output*> outputs;
  outputs.reserve(m_platform->outputs().size());
  for (const auto& output : m_platform->outputs()) {
    if (output.output != nullptr) {
      outputs.push_back(output.output);
    }
  }
  m_clickShield.syncOutputs(outputs);
}

void PanelManager::closePanel(bool animateClose) {
  if (!isOpen() || m_inTransition || m_closing) {
    return;
  }

  kLog.debug("panel manager: closing \"{}\"", m_activePanelId);

  // Release application and other-output input before the visual exit. The
  // click that initiated dismissal stays consumed by the now-destroyed shield;
  // the next click reaches the client normally.
  m_clickShield.deactivate();

  // Disable input during close animation
  m_inputDispatcher.setSceneRoot(nullptr);
  m_closing = true;
  m_attachedOpenAnimationPending = false;
  cancelKeyboardFocusBootstrap();
  // Input disappears at close start, not at the end of the visual animation;
  // transparent pixels can therefore never block the application beneath.
  if (m_surface != nullptr) {
    m_surface->setInputRegion({});
  }
  // Freeze the geometry currently on screen before the exit. Resize and
  // anchor morphs must not keep writing panel bounds while reveal is closing.
  if (m_panelResizeAnimation != 0) {
    m_animations.cancel(m_panelResizeAnimation);
    m_panelResizeAnimation = 0;
  }
  if (m_panelMorphAnimation != 0) {
    m_animations.cancel(m_panelMorphAnimation);
    m_panelMorphAnimation = 0;
    if (m_chromePanelState.has_value()) {
      auto displayed = m_chromeTransition.displayed();
      displayed.opacity = 1.0f;
      displayed.progress = 1.0f;
      displayed.visible = true;
      displayed.inputEnabled = false;
      m_chromePanelState = displayed;
      m_panelInsetX = static_cast<std::int32_t>(std::lround(displayed.rect.x));
      m_panelInsetY = static_cast<std::int32_t>(std::lround(displayed.rect.y));
    }
  }
  if (m_chromeHosted) {
    publishChromePanelState(m_attachedToBar ? m_attachedRevealProgress : m_detachedRevealProgress);
  }

  if (animateClose && m_sceneRoot != nullptr && m_activePanel != nullptr && m_activePanel->wantsCloseAnimation()) {
    const std::uint64_t gen = ++m_destroyGeneration;
    if (m_attachedToBar && m_attachedRevealClipNode != nullptr) {
      m_animations.cancelForOwner(m_attachedRevealClipNode);
      m_animations.animate(
          m_attachedRevealProgress, 0.0f, Style::animChromeExit, Easing::EaseInCubic,
          [this](float v) { applyAttachedReveal(v); },
          [this, gen]() {
            DeferredCall::callLater([this, gen]() {
              if (m_destroyGeneration == gen) {
                destroyPanel();
              }
            });
          },
          m_attachedRevealClipNode
      );
    } else {
      m_animations.cancelForOwner(m_sceneRoot.get());
      m_animations.animate(
          m_detachedRevealProgress, 0.0f, Style::animChromeExit, Easing::EaseInCubic,
          [this](float v) { applyDetachedReveal(v); },
          [this, gen]() {
            DeferredCall::callLater([this, gen]() {
              if (m_destroyGeneration == gen) {
                destroyPanel();
              }
            });
          },
          m_sceneRoot.get()
      );
    }
    m_surface->requestRedraw();
  } else {
    destroyPanel();
  }
}

void PanelManager::destroyPanel() {
  auto queuedOpen = std::move(m_queuedPanelOpen);
  m_queuedPanelOpen.reset();
  m_clickShield.deactivate();
  if (m_chromePanelStateCallback && m_output != nullptr) {
    m_chromePanelStateCallback(m_output, m_sourceBarName, std::nullopt);
  }
  m_animations.cancelAll();
  m_closing = false;
  m_pointerInside = false;
  m_attachedPopupCount = 0;
  m_keyboardFocusBootstrapScheduled = false;
  m_keyboardFocusBootstrapActive = false;
  m_keyboardFocusRelaxTimer.stop();
  m_keyboardFocus.endOpen();
  m_clickShieldRequested = false;
  m_openTriggerSurface = nullptr;
  m_openTriggerSerial = 0;
  m_ownedPopupSurfaces.clear();
  m_inputDispatcher.setSceneRoot(nullptr);
  if (m_activePanel != nullptr) {
    m_activePanel->onClose();
  }
  clearRetainedContentLayers();
  m_detachedChromeNode = nullptr;
  m_contentNode = nullptr;
  m_detachedRevealClipNode = nullptr;
  m_detachedRevealContentNode = nullptr;
  m_attachedRevealClipNode = nullptr;
  m_attachedRevealContentNode = nullptr;
  m_selectPopup.reset();
  m_sceneRoot.reset();
  m_surface.reset();
  m_layerSurface = nullptr;
  m_output = nullptr;
  m_wlSurface = nullptr;
  m_activePanel = nullptr;
  m_activePanelId.clear();
  m_pendingOpenContext.clear();
  m_panelInsetX = 0;
  m_panelInsetY = 0;
  m_panelVisualWidth = 0;
  m_panelVisualHeight = 0;
  m_panelVisualWidthExact = 0.0f;
  m_panelVisualHeightExact = 0.0f;
  m_panelFillWidth = false;
  m_panelFillHeight = false;
  m_panelCustomWidth = false;
  m_panelDynamicVisualSize = false;
  m_intrinsicVisualHeightResolved = false;
  m_panelGeometryDirty = false;
  m_panelResizeAnimation = 0;
  m_panelMorphAnimation = 0;
  m_panelContentAnimation = 0;
  m_activeContentVisualWidth = 1.0f;
  m_activeContentVisualHeight = 1.0f;
  m_activeContentPadding = 0.0f;
  m_pendingVisualSize = false;
  m_panelScreenPosition.clear();
  m_detachedBleedLeft = 0;
  m_detachedBleedTop = 0;
  m_detachedBleedRight = 0;
  m_detachedBleedBottom = 0;
  m_attachedBackgroundOpacity = 1.0f;
  m_attachedContactShadow = false;
  m_attachedRevealProgress = 1.0f;
  m_detachedRevealProgress = 1.0f;
  m_attachedRevealDirection = AttachedRevealDirection::Down;
  m_detachedRevealDirection = AttachedRevealDirection::Down;
  m_attachedBarPosition.clear();
  m_sourceBarName.clear();
  m_chromePanelState.reset();
  m_attachedToBar = false;
  m_chromeHosted = false;
  m_attachedOpenAnimationPending = false;
  if (m_platform != nullptr) {
    m_platform->stopKeyRepeat();
  }
  if (m_panelClosedCallback) {
    m_panelClosedCallback();
  }
  auto afterClosed = std::move(m_afterPanelClosed);
  m_afterPanelClosed.clear();
  for (auto& callback : afterClosed) {
    if (callback) {
      callback();
    }
  }
  if (queuedOpen.has_value()) {
    DeferredCall::callLater([this, queued = std::move(*queuedOpen)]() mutable {
      openPanel(
          queued.panelId,
          PanelOpenRequest{
              .output = queued.output,
              .triggerSurface = queued.triggerSurface,
              .anchorX = queued.anchorX,
              .anchorY = queued.anchorY,
              .hasExplicitAnchor = queued.hasExplicitAnchor,
              .hasAnchorPosition = queued.hasAnchorPosition,
              .context = queued.context,
              .sourceBarName = queued.sourceBarName,
          }
      );
    });
  }
}

void PanelManager::togglePanel(const std::string& panelId, PanelOpenRequest request) {
  if (panelId == "control-center") {
    const std::string_view target = legacyControlCenterTarget(request.context);
    if (target.empty()) {
      kLog.warn("control-center dashboard was removed; toggle a destination panel directly");
      return;
    }
    request.context = {};
    togglePanel(std::string(target), request);
    return;
  }
  if (shouldKeepClosingOnToggle(isOpen(), m_closing, m_activePanelId == panelId)) {
    // Closing is a committed toggle state. Reviving the already-unfocused
    // layer surface leaves no future focus-leave event for outside dismissal.
    return;
  }

  // A fully open instance toggles normally. A closing instance was handled
  // above and remains committed to closing until its surface is destroyed.
  if (isOpen() && !m_closing && m_activePanelId == panelId) {
    if (!request.context.empty() && m_activePanel != nullptr) {
      if (m_activePanel->isContextActive(request.context)) {
        closePanel();
        return;
      }
      // Panels placed near the clicked widget must fully reopen so geometry
      // and bar decoration track the new anchor. Once the panel already lives
      // on a fullscreen attached surface, keep that surface and morph from the
      // geometry currently on screen instead of remapping it.
      if (requestUsesTriggerAnchor(request, m_activePanel, panelId, m_config)) {
        if (m_attachedToBar && retargetAttachedPanel(request)) {
          m_activePanel->onOpen(request.context);
          refresh();
          return;
        }
        openPanel(panelId, request);
        return;
      }
      m_activePanel->onOpen(request.context);
      refresh();
      return;
    }
    closePanel();
  } else {
    openPanel(panelId, request);
  }
}

void PanelManager::togglePanel(const std::string& panelId) {
  // Keep shortcut/plugin/IPC toggles on the same state machine as bar and
  // tray triggers. In particular, a repeated event during the exit must not
  // resurrect the already-unfocused layer surface.
  togglePanel(panelId, PanelOpenRequest{});
}

void PanelManager::clearClipboardHistory() {
  const auto it = m_panels.find("clipboard");
  if (it == m_panels.end()) {
    return;
  }
  if (auto* clipboardPanel = dynamic_cast<ClipboardPanel*>(it->second.get())) {
    clipboardPanel->clearHistoryFromIpc();
  }
}

bool PanelManager::onPointerEvent(const PointerEvent& event) {
  if (!isOpen() || m_inTransition) {
    return false;
  }

  if (m_clickShield.ownsSurface(event.surface)) {
    if (event.type == PointerEvent::Type::Button && event.state == 1) {
      // Pointer dismissal closes the entire group. Escape deliberately takes
      // the more gradual transient-first path below in onKeyboardEvent().
      closePanel();
    }
    return true;
  }

  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    if (m_selectPopup->onPointerEvent(event)) {
      return true;
    }
    if (event.type == PointerEvent::Type::Button && event.state == 1) {
      m_selectPopup->closeSelectDropdown();
      return true;
    }
  }

  if (m_activePopup != nullptr) {
    if (m_activePopup->onPointerEvent(event)) {
      return true;
    }
    if (event.type == PointerEvent::Type::Button && event.state == 1) {
      m_activePopup->close();
      return true;
    }
  }

  if (m_attachedPopupCount > 0) {
    if (event.surface == m_wlSurface) {
      if (event.type == PointerEvent::Type::Enter) {
        m_pointerInside = true;
      } else if (event.type == PointerEvent::Type::Leave) {
        m_pointerInside = false;
      }
    }
    return false;
  }

  switch (event.type) {
  case PointerEvent::Type::Enter: {
    if (event.surface == m_wlSurface) {
      m_pointerInside = true;
      m_inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    }
    break;
  }
  case PointerEvent::Type::Leave: {
    if (event.surface == m_wlSurface) {
      m_pointerInside = false;
      m_inputDispatcher.pointerLeave();
    }
    break;
  }
  case PointerEvent::Type::Motion: {
    if (!m_pointerInside) {
      return false;
    }
    m_inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
    break;
  }
  case PointerEvent::Type::Button: {
    bool pressed = (event.state == 1);

    // Click outside panel closes it.
    if (pressed && !m_pointerInside) {
      closePanel();
      return false;
    }

    if (m_pointerInside) {
      if (pressed && event.surface == m_wlSurface && m_inputDispatcher.hoveredArea() == nullptr) {
        if (m_activePanel != nullptr && m_activePanel->dismissTransientUi()) {
          refresh();
          return true;
        }
      }
      m_inputDispatcher.pointerButton(
          static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, pressed
      );
    }
    break;
  }
  case PointerEvent::Type::Axis: {
    if (!m_pointerInside) {
      return false;
    }
    m_inputDispatcher.pointerAxis(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
        event.axisDiscrete, event.axisValue120, event.axisLines
    );
    break;
  }
  }

  // Pointer interactions often only affect visual state.
  // Relayout only when the scene explicitly accumulated layout invalidation.
  if (m_surface != nullptr && m_sceneRoot != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty())) {
    if (m_sceneRoot->layoutDirty() && m_activePanel != nullptr && !m_activePanel->deferPointerRelayout()) {
      m_surface->requestLayout();
    } else {
      m_surface->requestRedraw();
    }
  }

  return m_pointerInside;
}

bool PanelManager::isOpen() const noexcept { return m_surface != nullptr && m_activePanel != nullptr; }

bool PanelManager::isOpenPanel(std::string_view panelId) const noexcept {
  if (!isOpen()) {
    return false;
  }
  if (panelId == "control-center") {
    return isStandaloneContentPanel(m_activePanelId);
  }
  return m_activePanelId == panelId;
}

bool PanelManager::ownsPanelSurface(wl_surface* surface) const noexcept {
  return surface != nullptr && isOpen()
      && (surface == m_wlSurface || m_ownedPopupSurfaces.contains(surface));
}

bool PanelManager::isPanelTransitionActive() const noexcept {
  if (!isOpen() && !m_closing) {
    return false;
  }
  if (m_closing || m_attachedOpenAnimationPending) {
    return true;
  }
  if (m_attachedToBar) {
    return m_attachedRevealProgress < 0.999f;
  }
  return m_detachedRevealProgress < 0.999f;
}

bool PanelManager::isAttachedOpen() const noexcept { return isOpen() && m_attachedToBar; }

wl_output* PanelManager::attachedPanelOutput() const noexcept { return m_output; }

std::string_view PanelManager::attachedSourceBarName() const noexcept { return m_sourceBarName; }

const std::string& PanelManager::activePanelId() const noexcept { return m_activePanelId; }

bool PanelManager::isActivePanelContext(std::string_view context) const noexcept {
  if (!isOpen() || m_activePanel == nullptr) {
    return false;
  }
  return m_activePanel->isContextActive(context);
}

void PanelManager::refresh() {
  if (!isOpen() || m_renderContext == nullptr || m_activePanel == nullptr || m_surface == nullptr) {
    return;
  }
  if (m_activePanel->deferExternalRefresh()) {
    return;
  }

  m_surface->requestUpdate();
}

void PanelManager::onIconThemeChanged() {
  if (!isOpen() || m_activePanel == nullptr || m_surface == nullptr) {
    return;
  }

  m_activePanel->onIconThemeChanged();
  m_surface->requestUpdate();
}

void PanelManager::onOutputChange() {
  if (!isOpen() || m_platform == nullptr) {
    return;
  }
  const bool sourceStillPresent = std::ranges::any_of(
      m_platform->outputs(), [this](const WaylandOutput& output) { return output.output == m_output; }
  );
  if (!sourceStillPresent) {
    closePanel(/*animateClose=*/false);
    return;
  }
  syncClickShieldOutputs();
}

void PanelManager::focusArea(InputArea* area) {
  if (!isOpen() || m_sceneRoot == nullptr) {
    return;
  }
  m_inputDispatcher.setFocus(area);
}

void PanelManager::requestUpdateOnly() {
  if (!isOpen() || m_surface == nullptr) {
    return;
  }
  m_surface->requestUpdateOnly();
}

void PanelManager::requestLayout() {
  if (!isOpen() || m_surface == nullptr) {
    return;
  }
  m_surface->requestLayout();
}

void PanelManager::requestActivePanelVisualSize(float width, float height, bool animate) {
  if (!isOpen() || m_surface == nullptr || m_activePanel == nullptr || !m_panelDynamicVisualSize) {
    return;
  }
  const float maxWidth = m_surface->width() > 0
      ? std::max(
            1.0f,
            static_cast<float>(m_surface->width()) - static_cast<float>(m_detachedBleedLeft + m_detachedBleedRight)
        )
      : std::max(1.0f, width);
  const float maxHeight = m_surface->height() > 0
      ? std::max(
            1.0f,
            static_cast<float>(m_surface->height()) - static_cast<float>(m_detachedBleedTop + m_detachedBleedBottom)
        )
      : std::max(1.0f, height);
  const float targetWidth = m_panelCustomWidth ? m_panelVisualWidthExact : std::clamp(width, 1.0f, maxWidth);
  const float targetHeight = std::clamp(height, 1.0f, maxHeight);

  if (m_attachedToBar) {
    if (m_chromePanelState.has_value()
        && std::abs(m_chromePanelState->rect.width - targetWidth) < 0.5f
        && std::abs(m_chromePanelState->rect.height - targetHeight) < 0.5f) {
      return;
    }
    m_panelVisualWidthExact = targetWidth;
    m_panelVisualHeightExact = targetHeight;
    m_panelVisualWidth = static_cast<std::uint32_t>(std::lround(targetWidth));
    m_panelVisualHeight = static_cast<std::uint32_t>(std::lround(targetHeight));
    m_activeContentVisualWidth = targetWidth;
    m_activeContentVisualHeight = targetHeight;
    PanelOpenRequest retarget{
        .output = m_output,
        .triggerSurface = m_openTriggerSurface,
        .anchorX = m_chromePanelState.has_value() ? m_chromePanelState->triggerAnchor.x : 0.0f,
        .anchorY = m_chromePanelState.has_value() ? m_chromePanelState->triggerAnchor.y : 0.0f,
        .hasExplicitAnchor = true,
        .hasAnchorPosition = true,
        .sourceBarName = m_sourceBarName,
    };
    // A non-animated intrinsic measurement must not cancel an anchor morph
    // that is already in flight (tray-menu switches used to snap here).
    // Retarget from the displayed state and fold the new natural size into
    // the same spatial transition.
    if (animate || m_panelMorphAnimation != 0) {
      (void)retargetAttachedPanel(retarget);
    } else {
      syncAttachedChromeGeometry();
      if (m_chromePanelState.has_value()) {
        applyAttachedMorphState(*m_chromePanelState);
      }
    }
    m_panelGeometryDirty = true;
    m_surface->requestLayout();
    m_surface->requestFrameTick();
    return;
  }
  const float startWidth = m_panelVisualWidthExact > 0.0f ? m_panelVisualWidthExact : targetWidth;
  const float startHeight = m_panelVisualHeightExact > 0.0f ? m_panelVisualHeightExact : targetHeight;

  if (m_panelResizeAnimation != 0) {
    m_animations.cancel(m_panelResizeAnimation);
    m_panelResizeAnimation = 0;
  }

  const auto applySize = [this, startWidth, startHeight, targetWidth, targetHeight](float progress) {
    m_panelVisualWidthExact = std::lerp(startWidth, targetWidth, progress);
    m_panelVisualHeightExact = std::lerp(startHeight, targetHeight, progress);
    m_panelVisualWidth = static_cast<std::uint32_t>(std::lround(m_panelVisualWidthExact));
    m_panelVisualHeight = static_cast<std::uint32_t>(std::lround(m_panelVisualHeightExact));
    if (m_attachedToBar) {
      syncAttachedChromeGeometry();
    }
    m_panelGeometryDirty = true;
    if (m_surface != nullptr) {
      m_surface->requestLayout();
      m_surface->requestRedraw();
    }
  };

  if (!animate
      || m_sceneRoot == nullptr
      || (std::abs(startWidth - targetWidth) < 0.5f && std::abs(startHeight - targetHeight) < 0.5f)) {
    applySize(1.0f);
    return;
  }

  m_panelResizeAnimation = m_animations.animate(
      0.0f, 1.0f, Style::animChromeSpatial, Easing::FluidSpatial, applySize,
      [this]() { m_panelResizeAnimation = 0; }, m_sceneRoot.get()
  );
  m_surface->requestFrameTick();
}

void PanelManager::syncActivePanelIntrinsicHeight(Renderer& renderer, bool animate) {
  if (m_activePanel == nullptr || !m_panelDynamicVisualSize) {
    return;
  }
  // A switching panel is laid out toward its destination width. Measuring at
  // an intermediate morph width can reflow text and enqueue a second height
  // change immediately after the shared-element transition settles.
  const float visualWidth = m_attachedToBar && m_panelMorphAnimation != 0 && m_chromePanelState.has_value()
      ? m_chromePanelState->rect.width
      : (m_panelVisualWidthExact > 0.0f
            ? m_panelVisualWidthExact
            : std::max(1.0f, m_activePanel->preferredWidth()));
  const auto desired = m_activePanel->desiredVisualHeight(renderer, visualWidth);
  if (!desired.has_value()) {
    return;
  }
  const float cap = 720.0f * m_activePanel->contentScale();
  const float target = std::clamp(*desired, 1.0f, std::max(1.0f, cap));
  const bool shouldAnimate = animate && m_intrinsicVisualHeightResolved;
  m_intrinsicVisualHeightResolved = true;
  requestActivePanelVisualSize(visualWidth, target, shouldAnimate);
}

void PanelManager::requestRedraw() {
  if (!isOpen() || m_surface == nullptr) {
    return;
  }
  m_surface->requestRedraw();
}

void PanelManager::requestFrameTick() {
  if (!isOpen() || m_surface == nullptr) {
    return;
  }
  m_surface->requestFrameTick();
}

void PanelManager::close() { closePanel(); }

void PanelManager::setActivePopup(ContextMenuPopup* popup) { m_activePopup = popup; }

void PanelManager::clearActivePopup() { m_activePopup = nullptr; }

void PanelManager::registerPopupSurface(wl_surface* surface) {
  if (surface == nullptr) {
    return;
  }
  m_ownedPopupSurfaces.insert(surface);
}

void PanelManager::unregisterPopupSurface(wl_surface* surface) {
  if (surface == nullptr) {
    return;
  }
  m_ownedPopupSurfaces.erase(surface);
}

void PanelManager::beginAttachedPopup(wl_surface* surface) {
  if (surface == nullptr || surface != m_wlSurface) {
    return;
  }
  ++m_attachedPopupCount;
}

void PanelManager::endAttachedPopup(wl_surface* surface) {
  if (surface == nullptr || surface != m_wlSurface) {
    return;
  }
  if (m_attachedPopupCount > 0) {
    --m_attachedPopupCount;
  }
  if (m_attachedPopupCount > 0) {
    return;
  }
  m_pointerInside =
      m_platform != nullptr && m_platform->hasPointerPosition() && m_platform->lastPointerSurface() == m_wlSurface;
  if (m_pointerInside) {
    m_inputDispatcher.pointerEnter(
        static_cast<float>(m_platform->lastPointerX()), static_cast<float>(m_platform->lastPointerY()),
        m_platform->lastInputSerial()
    );
  } else {
    m_inputDispatcher.pointerLeave();
  }
  requestRedraw();
}

std::optional<LayerPopupParentContext> PanelManager::popupParentContextForSurface(wl_surface* surface) const noexcept {
  if (surface == nullptr || surface != m_wlSurface) {
    return std::nullopt;
  }
  return fallbackPopupParentContext();
}

std::optional<LayerPopupParentContext> PanelManager::fallbackPopupParentContext() const noexcept {
  if (!isOpen() || m_surface == nullptr || m_wlSurface == nullptr || m_layerSurface == nullptr) {
    return std::nullopt;
  }

  LayerPopupParentContext context;
  context.surface = m_wlSurface;
  context.layerSurface = m_layerSurface->layerSurface();
  context.output = m_output;
  context.width = m_surface->width();
  context.height = m_surface->height();
  if (context.layerSurface == nullptr || context.width == 0 || context.height == 0) {
    return std::nullopt;
  }
  return context;
}

void PanelManager::onKeyboardEvent(const KeyboardEvent& event) {
  // m_inTransition means the surface is still initializing.
  // Keyboard events during this window must be ignored.
  if (!isOpen() || m_inTransition) {
    return;
  }

  // Gate on compositor focus: route keys only when the surface owning this panel
  // input is the one the compositor reports as keyboard-focused.
  if (m_platform != nullptr) {
    wl_surface* const kbSurface = m_platform->lastKeyboardSurface();
    const bool onPanel = (m_wlSurface != nullptr && kbSurface == m_wlSurface);
    const bool onSelectPopup =
        (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen() && kbSurface == m_selectPopup->wlSurface());
    if (!onPanel && !onSelectPopup) {
      return;
    }
  }

  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    m_selectPopup->onKeyboardEvent(event);
    return;
  }

  if (event.pressed && KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
    if (m_activePanel != nullptr
        && m_activePanel->handleGlobalKey(event.sym, event.modifiers, event.pressed, event.preedit)) {
      if (m_surface != nullptr && m_sceneRoot != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty())) {
        if (m_sceneRoot->layoutDirty()) {
          m_surface->requestLayout();
        } else {
          m_surface->requestRedraw();
        }
      }
      return;
    }
    if (m_activePanel != nullptr && m_activePanel->dismissTransientUi()) {
      if (m_surface != nullptr && m_sceneRoot != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty())) {
        if (m_sceneRoot->layoutDirty()) {
          m_surface->requestLayout();
        } else {
          m_surface->requestRedraw();
        }
      }
      return;
    }
    closePanel();
    return;
  }

  // A focused text input owns plain printable keys; the panel's global key
  // handler must not claim them (Space is a Validate chord but must type a space).
  const InputArea* const focusedArea = m_inputDispatcher.focusedArea();
  const bool textInputFocused = focusedArea != nullptr && focusedArea->textInputClient() != nullptr;
  const bool reserveForTextInput =
      event.pressed && textInputFocused && isPlainPrintableKey(event.utf32, event.modifiers, event.preedit);

  if (!reserveForTextInput
      && m_activePanel != nullptr
      && m_activePanel->handleGlobalKey(event.sym, event.modifiers, event.pressed, event.preedit)) {
    if (m_surface != nullptr && m_sceneRoot != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty())) {
      if (m_sceneRoot->layoutDirty()) {
        m_surface->requestLayout();
      } else {
        m_surface->requestRedraw();
      }
    }
    return;
  }

  m_inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  if (m_surface != nullptr && m_sceneRoot != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty())) {
    if (m_sceneRoot->layoutDirty()) {
      m_surface->requestLayout();
    } else {
      m_surface->requestRedraw();
    }
  }
}

void PanelManager::applyAttachedReveal(float progress) {
  m_attachedRevealProgress = progress;
  if (!m_attachedToBar || m_attachedRevealClipNode == nullptr || m_sceneRoot == nullptr) {
    if (m_attachedToBar && m_surface != nullptr) {
      m_surface->clearBlurRegion();
    }
    return;
  }

  const float w = m_sceneRoot->width();
  const float h = m_sceneRoot->height();
  const float panelW = m_panelVisualWidth > 0 ? static_cast<float>(m_panelVisualWidth) : w;
  const float panelH = m_panelVisualHeight > 0 ? static_cast<float>(m_panelVisualHeight) : h;
  const float coverage = std::clamp(progress, 0.0f, 1.0f);
  const float travelX = (m_attachedRevealDirection == AttachedRevealDirection::Left
                         || m_attachedRevealDirection == AttachedRevealDirection::Right)
      ? panelW * (1.0f - progress)
      : 0.0f;
  const float travelY = (m_attachedRevealDirection == AttachedRevealDirection::Up
                         || m_attachedRevealDirection == AttachedRevealDirection::Down)
      ? panelH * (1.0f - progress)
      : 0.0f;

  float contentX = 0.0f;
  float contentY = 0.0f;
  switch (m_attachedRevealDirection) {
  case AttachedRevealDirection::Down:
    contentY = -travelY;
    break;
  case AttachedRevealDirection::Up:
    contentY = travelY;
    break;
  case AttachedRevealDirection::Right:
    contentX = -travelX;
    break;
  case AttachedRevealDirection::Left:
    contentX = travelX;
    break;
  }

  const auto displayed = sampleChromeRevealState(progress);
  ChromeRect visibleBody{
      .x = static_cast<float>(m_panelInsetX) + contentX,
      .y = static_cast<float>(m_panelInsetY) + contentY,
      .width = panelW,
      .height = panelH,
  };
  if (m_chromeHosted && displayed.has_value() && m_chromePanelState.has_value()) {
    contentX = displayed->rect.x - m_chromePanelState->rect.x;
    contentY = displayed->rect.y - m_chromePanelState->rect.y;
    // The SDF body is free to travel under the bar, but panel content passes
    // through a fixed portal at the bar's inner edge. The panel surface maps
    // after the bar surface on Niri, so this clip is the cross-surface
    // equivalent of keeping persistent bar content at the highest z-index.
    visibleBody = chromeAttachedRevealClip(displayed->rect, m_chromePanelState->rect, displayed->edge);
    m_attachedRevealClipNode->setPosition(visibleBody.x, visibleBody.y);
    m_attachedRevealClipNode->setFrameSize(visibleBody.width, visibleBody.height);
    if (m_attachedRevealContentNode != nullptr) {
      m_attachedRevealContentNode->setPosition(
          displayed->rect.x - visibleBody.x - m_chromePanelState->rect.x,
          displayed->rect.y - visibleBody.y - m_chromePanelState->rect.y
      );
      m_attachedRevealContentNode->setFrameSize(w, h);
    }
  } else {
    m_attachedRevealClipNode->setPosition(0.0f, 0.0f);
    m_attachedRevealClipNode->setFrameSize(w, h);
    if (m_attachedRevealContentNode != nullptr) {
      m_attachedRevealContentNode->setPosition(contentX, contentY);
      m_attachedRevealContentNode->setFrameSize(w, h);
    }
  }
  if (m_contentNode != nullptr) {
    m_contentNode->setOpacity(panelRevealContentOpacity(coverage));
  }
  publishChromePanelState(progress);
  const int bodyX = static_cast<int>(std::lround(visibleBody.x));
  const int bodyY = static_cast<int>(std::lround(visibleBody.y));
  const int bodyWidth = std::max(0, static_cast<int>(std::lround(visibleBody.width)));
  const int bodyHeight = std::max(0, static_cast<int>(std::lround(visibleBody.height)));
  if (m_surface != nullptr) {
    if (!m_closing && coverage > 0.001f && bodyWidth > 0 && bodyHeight > 0) {
      m_surface->setInputRegion({InputRect{bodyX, bodyY, bodyWidth, bodyHeight}});
    } else {
      m_surface->setInputRegion({});
    }
  }
  applyPanelCompositorBlur(
      bodyX, bodyY, bodyWidth, bodyHeight, 0, 0,
      static_cast<int>(std::lround(w)), static_cast<int>(std::lround(h))
  );
}

void PanelManager::applyDetachedReveal(float progress) {
  m_detachedRevealProgress = progress;
  if (m_attachedToBar || m_sceneRoot == nullptr) {
    if (!m_attachedToBar && m_surface != nullptr) {
      m_surface->clearBlurRegion();
    }
    return;
  }

  const float surfaceW = m_sceneRoot->width();
  const float surfaceH = m_sceneRoot->height();
  const float coverage = std::clamp(progress, 0.0f, 1.0f);
  // Keep the complete rounded silhouette and move it as one object. The
  // travel is bounded by the surface bleed so the compositor boundary cannot
  // replace a rounded edge with a rectangular clip.
  const bool horizontalTravel = m_detachedRevealDirection == AttachedRevealDirection::Left
      || m_detachedRevealDirection == AttachedRevealDirection::Right;
  const float travel = m_chromeHosted
      ? (horizontalTravel ? static_cast<float>(m_panelVisualWidth) : static_cast<float>(m_panelVisualHeight))
      : std::min(
            24.0f,
            static_cast<float>(
                std::max({m_detachedBleedLeft, m_detachedBleedTop, m_detachedBleedRight, m_detachedBleedBottom, 1})
            )
        );
  float contentX = 0.0f;
  float contentY = 0.0f;

  switch (m_detachedRevealDirection) {
  case AttachedRevealDirection::Down:
    contentY = -travel * (1.0f - progress);
    break;
  case AttachedRevealDirection::Up:
    contentY = travel * (1.0f - progress);
    break;
  case AttachedRevealDirection::Right:
    contentX = -travel * (1.0f - progress);
    break;
  case AttachedRevealDirection::Left:
    contentX = travel * (1.0f - progress);
    break;
  }

  const auto displayed = sampleChromeRevealState(progress);
  if (m_detachedRevealClipNode != nullptr && m_detachedRevealContentNode != nullptr) {
    if (m_chromeHosted && displayed.has_value() && m_chromePanelState.has_value()) {
      contentX = displayed->rect.x - m_chromePanelState->rect.x;
      contentY = displayed->rect.y - m_chromePanelState->rect.y;
      m_detachedRevealClipNode->setPosition(displayed->rect.x, displayed->rect.y);
      m_detachedRevealClipNode->setFrameSize(displayed->rect.width, displayed->rect.height);
      m_detachedRevealContentNode->setPosition(-m_chromePanelState->rect.x, -m_chromePanelState->rect.y);
    } else {
      m_detachedRevealClipNode->setPosition(0.0f, 0.0f);
      m_detachedRevealClipNode->setFrameSize(surfaceW, surfaceH);
      m_detachedRevealContentNode->setPosition(contentX, contentY);
    }
    m_detachedRevealContentNode->setFrameSize(surfaceW, surfaceH);
  }

  if (m_contentNode != nullptr) {
    m_contentNode->setOpacity(panelRevealContentOpacity(coverage));
  }
  if (m_chromeHosted) {
    publishChromePanelState(progress);
  }
  if (m_detachedChromeNode != nullptr && m_config != nullptr) {
    auto style = m_detachedChromeNode->style();
    if (style.rectCount > 0) {
      style.rects[0].deformation = std::clamp(
          std::sin(coverage * 3.14159265358979323846f) * m_config->config().shell.chrome.deformScale, -0.12f, 0.12f
      );
      style.debugInputRect = style.rects[0];
      style.debugProgress = coverage;
      style.debugInputEnabled = !m_closing && coverage > 0.001f;
      m_detachedChromeNode->setStyle(style);
    }
  }
  if (m_surface != nullptr) {
    if (!m_closing && coverage > 0.001f) {
      m_surface->setInputRegion({InputRect{
          displayed.has_value() ? static_cast<int>(std::lround(displayed->rect.x))
                                : m_panelInsetX + static_cast<int>(std::lround(contentX)),
          displayed.has_value() ? static_cast<int>(std::lround(displayed->rect.y))
                                : m_panelInsetY + static_cast<int>(std::lround(contentY)),
          static_cast<int>(m_panelVisualWidth),
          static_cast<int>(m_panelVisualHeight)
      }});
    } else {
      m_surface->setInputRegion({});
    }
  }
  applyPanelCompositorBlur(
      m_panelInsetX, m_panelInsetY, static_cast<int>(m_panelVisualWidth), static_cast<int>(m_panelVisualHeight), 0, 0,
      static_cast<int>(std::lround(surfaceW)), static_cast<int>(std::lround(surfaceH))
  );
}

void PanelManager::startAttachedOpenAnimation() {
  if (!m_attachedOpenAnimationPending || !m_attachedToBar || m_attachedRevealClipNode == nullptr || m_closing) {
    return;
  }
  if (m_attachedPanelBarSettledCallback != nullptr
      && m_output != nullptr
      && !m_attachedPanelBarSettledCallback(m_output, m_sourceBarName)) {
    return;
  }

  m_attachedOpenAnimationPending = false;
  m_animations.animate(
      m_attachedRevealProgress, 1.0f, Style::animBarPopoutSpatial, Easing::FluidSpatial,
      [this](float v) { applyAttachedReveal(v); },
      {},
      m_attachedRevealClipNode
  );
}

bool PanelManager::retargetAttachedPanel(const PanelOpenRequest& request) {
  if (!m_attachedToBar
      || !m_chromePanelState
      || m_output == nullptr
      || request.output != m_output
      || m_config == nullptr
      || m_platform == nullptr
      || m_surface == nullptr
      || m_activePanel == nullptr) {
    return false;
  }
  const auto* output = m_platform->findOutputByWl(m_output);
  if (output == nullptr || output->effectiveLogicalWidth() <= 0 || output->effectiveLogicalHeight() <= 0) {
    return false;
  }

  m_openTriggerSurface = request.triggerSurface;
  m_openTriggerSerial = m_platform->lastInputSerial();

  const int outputWidth = output->effectiveLogicalWidth();
  const int outputHeight = output->effectiveLogicalHeight();
  const BarConfig bar = resolvePanelBarConfig(m_config, m_platform, m_output, request.sourceBarName);
  const auto chromeLayout =
      resolveChromeLayoutContext(bar, m_config->config().shell, outputWidth, outputHeight);
  const BarVisibleRect& barRect = chromeLayout.barRect;
  const ChromeEdge edge = chromeLayout.barEdge;
  const ChromeRect visibleBar{
      .x = static_cast<float>(barRect.left),
      .y = static_cast<float>(barRect.top),
      .width = static_cast<float>(barRect.right - barRect.left),
      .height = static_cast<float>(barRect.bottom - barRect.top),
  };

  auto target = *m_chromePanelState;
  target.edge = edge;
  target.triggerAnchor = ChromePoint{.x = request.anchorX, .y = request.anchorY};
  target.hasTriggerAnchor = true;
  target.rect.width = static_cast<float>(m_panelVisualWidth);
  target.rect.height = static_cast<float>(m_panelVisualHeight);
  target.rect = chromePlaceAttachedBody(target.rect, visibleBar, edge);

  const ChromeGeometryModel geometry(chromeLayout.geometry);
  target = geometry.resolveAnchoredPanel(
      target, visibleBar,
      static_cast<float>(outputWidth), static_cast<float>(outputHeight)
  );
  target.opacity = 1.0f;
  target.progress = 1.0f;
  target.visible = true;
  target.inputEnabled = true;

  // During a rapid switch the transition's displayed state is the only
  // truthful starting point. Reconstructing from the previous endpoint makes
  // the shared panel teleport before the new morph begins.
  const auto current = m_panelMorphAnimation != 0
      ? m_chromeTransition.displayed()
      : sampleChromeRevealState(m_attachedRevealProgress).value_or(m_chromeTransition.displayed());
  m_chromeTransition.reset(current);
  m_chromeTransition.setTarget(target);
  m_chromePanelState = target;
  // These are target/layout values. Never replace them with an intermediate
  // morph sample: doing so causes text reflow and a second resize at settle.
  m_panelInsetX = static_cast<std::int32_t>(std::lround(target.rect.x));
  m_panelInsetY = static_cast<std::int32_t>(std::lround(target.rect.y));
  m_panelVisualWidthExact = target.rect.width;
  m_panelVisualHeightExact = target.rect.height;
  m_panelVisualWidth = static_cast<std::uint32_t>(std::lround(target.rect.width));
  m_panelVisualHeight = static_cast<std::uint32_t>(std::lround(target.rect.height));
  m_activeContentVisualWidth = target.rect.width;
  m_activeContentVisualHeight = target.rect.height;
  m_animations.cancelForOwner(&m_chromeTransition);
  if (m_panelMorphAnimation != 0) {
    m_animations.cancel(m_panelMorphAnimation);
  }
  m_panelMorphAnimation = m_animations.animate(
      0.0f, 1.0f, Style::animBarPopoutSpatial, Easing::FluidSpatial,
      [this](float progress) {
        auto displayed = m_chromeTransition.sample(progress, m_config->config().shell.chrome.deformScale);
        applyAttachedMorphState(displayed);
      },
      [this]() {
        m_panelMorphAnimation = 0;
        if (m_chromePanelState.has_value()) {
          m_chromeTransition.reset(*m_chromePanelState);
          applyAttachedMorphState(*m_chromePanelState);
        }
        if (m_surface != nullptr) {
          m_surface->requestRedraw();
        }
      },
      &m_chromeTransition
  );
  m_surface->requestFrameTick();
  return true;
}

std::optional<ChromePanelState> PanelManager::sampleChromeRevealState(float revealProgress) const {
  if (!m_chromeHosted || !m_chromePanelState || m_config == nullptr) {
    return std::nullopt;
  }

  const float coverage = std::clamp(revealProgress, 0.0f, 1.0f);
  const auto direction = m_attachedToBar ? m_attachedRevealDirection : m_detachedRevealDirection;
  ChromeTransitionState reveal;
  reveal.reset(hiddenChromeState(*m_chromePanelState, direction));
  reveal.setTarget(*m_chromePanelState);
  auto state = reveal.sample(revealProgress, m_config->config().shell.chrome.deformScale);
  state.radius = std::max(1.0f, m_config->config().shell.chrome.rounding);
  state.progress = coverage;
  state.opacity = coverage;
  state.visible = true;
  state.inputEnabled = !m_closing && coverage > 0.001f;
  return state;
}

void PanelManager::publishChromePanelState(float revealProgress) {
  if (!m_chromePanelStateCallback || m_output == nullptr) {
    return;
  }
  if (const auto state = sampleChromeRevealState(revealProgress); state.has_value()) {
    m_chromePanelStateCallback(m_output, m_sourceBarName, state);
  }
}

void PanelManager::applyPanelCompositorBlur(
    int bodyX, int bodyY, int bodyW, int bodyH, int clipX, int clipY, int clipW, int clipH
) {
  // The blur region is compositor surface state, not a scene node. Callers pass the
  // same body and clip rectangles used by the reveal animation so protocol state
  // cannot get ahead of scene rendering.
  if (m_surface == nullptr || m_activePanel == nullptr) {
    return;
  }

  // Panels are opaque Material surfaces. Clearing the region here rather than
  // conditionally publishing it prevents a one-frame blur flash during a
  // layer-surface configure or reveal transition.
  (void)bodyX;
  (void)bodyY;
  (void)bodyW;
  (void)bodyH;
  (void)clipX;
  (void)clipY;
  (void)clipW;
  (void)clipH;
  m_surface->clearBlurRegion();
}

void PanelManager::onConfigReloaded() {
  if (!isOpen() || m_config == nullptr || m_activePanel == nullptr) {
    return;
  }

  m_activePanel->onConfigReloaded();
  const bool hadCustomWidth = m_panelCustomWidth;
  const auto* sizeOverride = panelSizeOverride(m_config, m_activePanelId);
  m_panelCustomWidth = sizeOverride != nullptr && sizeOverride->width.has_value();

  const auto automaticWidth = [this]() {
    return m_panelDynamicVisualSize ? m_activePanel->initialVisualWidth() : m_activePanel->preferredWidth();
  };
  float targetWidth = m_panelVisualWidthExact;
  float targetHeight = m_panelVisualHeightExact;
  if (m_panelCustomWidth) {
    targetWidth = scaledPanelOverride(*sizeOverride->width, m_activePanel->contentScale());
  } else if (hadCustomWidth) {
    targetWidth = automaticWidth();
  }

  if (m_platform != nullptr && m_output != nullptr) {
    if (const auto* output = m_platform->findOutputByWl(m_output); output != nullptr) {
      const float scale = m_activePanel->contentScale();
      targetWidth = std::min(
          targetWidth,
          static_cast<float>(std::max(1, output->effectiveLogicalWidth())) - Style::spaceSm * scale * 2.0f
      );
      targetHeight = std::min(
          targetHeight,
          static_cast<float>(std::max(1, output->effectiveLogicalHeight())) - Style::spaceSm * scale * 2.0f
      );
    }
  }
  targetWidth = std::max(1.0f, targetWidth);
  targetHeight = std::max(1.0f, targetHeight);
  const bool panelSizeChanged = std::abs(targetWidth - m_panelVisualWidthExact) >= 0.5f
      || std::abs(targetHeight - m_panelVisualHeightExact) >= 0.5f;
  const bool sizingModeChanged = hadCustomWidth != m_panelCustomWidth;
  m_panelFillWidth = m_activePanel->fillsWidth() && !m_panelCustomWidth;
  m_panelFillHeight = m_activePanel->fillsHeight();
  if (panelSizeChanged || sizingModeChanged) {
    m_panelVisualWidthExact = targetWidth;
    m_panelVisualHeightExact = targetHeight;
    m_panelVisualWidth = static_cast<std::uint32_t>(std::lround(targetWidth));
    m_panelVisualHeight = static_cast<std::uint32_t>(std::lround(targetHeight));
    m_panelGeometryDirty = true;
    if (!m_attachedToBar && !m_chromeHosted && m_layerSurface != nullptr) {
      const auto requestedWidth = m_panelFillWidth
          ? 0u
          : panelSurfaceExtent(m_panelVisualWidth, m_detachedBleedLeft, m_detachedBleedRight);
      const auto requestedHeight = m_panelFillHeight
          ? 0u
          : panelSurfaceExtent(m_panelVisualHeight, m_detachedBleedTop, m_detachedBleedBottom);
      m_layerSurface->requestSize(requestedWidth, requestedHeight);
    }
  }
  if (m_attachedToBar) {
    syncAttachedChromeGeometry();
  } else if (m_chromeHosted) {
    // Fixed frame slots are recomputed by syncDetachedVisualGeometry during
    // the requested layout pass.
    m_panelGeometryDirty = true;
  }
  if (m_attachedToBar) {
    applyAttachedReveal(m_attachedRevealProgress);
  } else {
    applyDetachedReveal(m_detachedRevealProgress);
  }
  const float panelBackgroundOpacity = m_attachedToBar
      ? m_attachedBackgroundOpacity
      : detachedPanelBackgroundOpacity(m_activePanel, m_activePanelId, m_config);
  m_activePanel->setPanelCardOpacity(resolvePanelCardOpacity(m_config, panelBackgroundOpacity));
  m_activePanel->setPanelBordersEnabled(m_config->config().shell.panel.borders);
  if (m_surface != nullptr) {
    m_surface->requestLayout();
  }

  // The remaining work is bar-config-driven and only applies to attached panels.
  if (!isAttachedOpen() || m_output == nullptr) {
    return;
  }

  bool changed = false;
  if (m_activePanel->inheritsBarBackgroundOpacity()) {
    // Chrome is deliberately opaque even when an old bar configuration still
    // contains background_opacity from the Noctalia era.
    constexpr float newOpacity = 1.0f;
    if (std::abs(newOpacity - m_attachedBackgroundOpacity) >= 0.001f) {
      m_attachedBackgroundOpacity = newOpacity;
      m_activePanel->setPanelCardOpacity(resolvePanelCardOpacity(m_config, m_attachedBackgroundOpacity));
      changed = true;
    }
  }
  if (!changed) {
    return;
  }

  if (m_surface != nullptr) {
    m_surface->requestRedraw();
  }
}

void PanelManager::syncAttachedChromeGeometry() {
  if (!m_attachedToBar
      || !m_chromePanelState
      || m_config == nullptr
      || m_platform == nullptr
      || m_output == nullptr) {
    return;
  }
  const auto* output = m_platform->findOutputByWl(m_output);
  if (output == nullptr || output->effectiveLogicalWidth() <= 0 || output->effectiveLogicalHeight() <= 0) {
    return;
  }

  const int outputWidth = output->effectiveLogicalWidth();
  const int outputHeight = output->effectiveLogicalHeight();
  const auto bar = resolvePanelBarConfig(m_config, m_platform, m_output, m_sourceBarName);
  const auto context = resolveChromeLayoutContext(bar, m_config->config().shell, outputWidth, outputHeight);
  const ChromeRect visibleBar{
      .x = static_cast<float>(context.barRect.left),
      .y = static_cast<float>(context.barRect.top),
      .width = static_cast<float>(context.barRect.right - context.barRect.left),
      .height = static_cast<float>(context.barRect.bottom - context.barRect.top),
  };

  auto updated = *m_chromePanelState;
  updated.edge = context.barEdge;
  updated.radius = std::max(1.0f, context.geometry.rounding);
  updated.rect.width = static_cast<float>(m_panelVisualWidth);
  updated.rect.height = static_cast<float>(m_panelVisualHeight);
  updated.rect = chromePlaceAttachedBody(updated.rect, visibleBar, context.barEdge);
  const ChromeGeometryModel geometry(context.geometry);
  if (updated.hasTriggerAnchor) {
    updated = geometry.resolveAnchoredPanel(updated, visibleBar, static_cast<float>(outputWidth),
                                             static_cast<float>(outputHeight));
  } else {
    updated.rect = geometry.clampPanel(
        updated.rect, context.barEdge, static_cast<float>(outputWidth), static_cast<float>(outputHeight)
    );
    updated.connector = {};
    updated.connectorVisible = false;
  }

  m_panelInsetX = static_cast<std::int32_t>(std::lround(updated.rect.x));
  m_panelInsetY = static_cast<std::int32_t>(std::lround(updated.rect.y));
  m_panelVisualWidth = static_cast<std::uint32_t>(std::lround(updated.rect.width));
  m_panelVisualHeight = static_cast<std::uint32_t>(std::lround(updated.rect.height));
  m_panelVisualWidthExact = updated.rect.width;
  m_panelVisualHeightExact = updated.rect.height;
  m_attachedBarPosition = bar.position;
  m_chromePanelState = updated;
  m_chromeTransition.reset(updated);
  m_panelGeometryDirty = true;
}

void PanelManager::syncDetachedVisualGeometry(std::uint32_t surfaceWidth, std::uint32_t surfaceHeight) {
  if (m_attachedToBar) {
    return;
  }

  const std::int32_t innerLeft = m_detachedBleedLeft;
  const std::int32_t innerTop = m_detachedBleedTop;
  const std::int32_t innerRight =
      std::max(innerLeft + 1, static_cast<std::int32_t>(surfaceWidth) - m_detachedBleedRight);
  const std::int32_t innerBottom =
      std::max(innerTop + 1, static_cast<std::int32_t>(surfaceHeight) - m_detachedBleedBottom);
  const std::int32_t availableWidth = innerRight - innerLeft;
  const std::int32_t availableHeight = innerBottom - innerTop;

  if (!m_panelDynamicVisualSize) {
    if (m_panelFillWidth || m_panelVisualWidth > static_cast<std::uint32_t>(availableWidth)) {
      m_panelVisualWidth = static_cast<std::uint32_t>(availableWidth);
      m_panelVisualWidthExact = static_cast<float>(m_panelVisualWidth);
    }
    if (m_panelFillHeight || m_panelVisualHeight > static_cast<std::uint32_t>(availableHeight)) {
      m_panelVisualHeight = static_cast<std::uint32_t>(availableHeight);
      m_panelVisualHeightExact = static_cast<float>(m_panelVisualHeight);
    }
  } else {
    m_panelVisualWidth = std::min(m_panelVisualWidth, static_cast<std::uint32_t>(availableWidth));
    m_panelVisualHeight = std::min(m_panelVisualHeight, static_cast<std::uint32_t>(availableHeight));
    m_panelVisualWidthExact = static_cast<float>(m_panelVisualWidth);
    m_panelVisualHeightExact = static_cast<float>(m_panelVisualHeight);
  }

  if (m_chromeHosted && m_chromePanelState && m_config != nullptr && m_activePanel != nullptr) {
    const auto& chrome = m_config->config().shell.chrome;
    const auto bar = resolvePanelBarConfig(m_config, m_platform, m_output, m_sourceBarName);
    const auto chromeLayout = resolveChromeLayoutContext(
        bar, m_config->config().shell, static_cast<std::int32_t>(surfaceWidth),
        static_cast<std::int32_t>(surfaceHeight)
    );
    const ChromeGeometryModel geometry(chromeLayout.geometry);
    ChromeRect rect{
        .x = (static_cast<float>(surfaceWidth) - static_cast<float>(m_panelVisualWidth)) * 0.5f,
        .y = (static_cast<float>(surfaceHeight) - static_cast<float>(m_panelVisualHeight)) * 0.5f,
        .width = static_cast<float>(m_panelVisualWidth),
        .height = static_cast<float>(m_panelVisualHeight),
    };
    if (m_panelScreenPosition.ends_with("_right")) {
      rect.x = static_cast<float>(surfaceWidth) - rect.width;
    } else if (m_panelScreenPosition.ends_with("_left")) {
      rect.x = 0.0f;
    }
    if (m_panelScreenPosition.starts_with("top_")) {
      rect.y = 0.0f;
    } else if (m_panelScreenPosition.starts_with("bottom_")) {
      rect.y = static_cast<float>(surfaceHeight) - rect.height;
    }
    rect = geometry.clampPanel(
        rect, m_activePanel->chromeEdge(), static_cast<float>(surfaceWidth), static_cast<float>(surfaceHeight)
    );
    m_panelInsetX = static_cast<std::int32_t>(std::lround(rect.x));
    m_panelInsetY = static_cast<std::int32_t>(std::lround(rect.y));
    auto updated = *m_chromePanelState;
    updated.rect = rect;
    updated.radius = std::max(1.0f, chrome.rounding);
    m_chromePanelState = updated;
    if (m_surface != nullptr) {
      m_surface->setInputRegion({InputRect{
          m_panelInsetX, m_panelInsetY, static_cast<int>(m_panelVisualWidth), static_cast<int>(m_panelVisualHeight)
      }});
    }
    return;
  }

  const bool alignLeft = m_panelScreenPosition.ends_with("_left") || m_panelScreenPosition == "center_left";
  const bool alignRight = m_panelScreenPosition.ends_with("_right") || m_panelScreenPosition == "center_right";
  const bool alignTop = m_panelScreenPosition.starts_with("top_");
  const bool alignBottom = m_panelScreenPosition.starts_with("bottom_");

  if (alignLeft) {
    m_panelInsetX = innerLeft;
  } else if (alignRight) {
    m_panelInsetX = innerRight - static_cast<std::int32_t>(m_panelVisualWidth);
  } else {
    m_panelInsetX = innerLeft + (availableWidth - static_cast<std::int32_t>(m_panelVisualWidth)) / 2;
  }

  if (alignTop) {
    m_panelInsetY = innerTop;
  } else if (alignBottom) {
    m_panelInsetY = innerBottom - static_cast<std::int32_t>(m_panelVisualHeight);
  } else {
    m_panelInsetY = innerTop + (availableHeight - static_cast<std::int32_t>(m_panelVisualHeight)) / 2;
  }

  if (m_surface != nullptr) {
    m_surface->setInputRegion({InputRect{
        m_panelInsetX, m_panelInsetY, static_cast<int>(m_panelVisualWidth), static_cast<int>(m_panelVisualHeight)
    }});
  }
}

void PanelManager::buildScene(std::uint32_t width, std::uint32_t height) {
  uiAssertNotRendering("PanelManager::buildScene");
  if (m_renderContext == nullptr || m_activePanel == nullptr) {
    return;
  }
  auto* renderer = m_renderContext;
  const bool hasDecoration = m_activePanel->hasDecoration();

  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);

  if (m_sceneRoot == nullptr) {
    m_sceneRoot = std::make_unique<Node>();
    m_sceneRoot->setAnimationManager(&m_animations);
    if (m_layerSurface != nullptr && m_renderContext != nullptr) {
      m_selectPopup = std::make_unique<SelectDropdownPopup>(m_platform->wayland(), *m_renderContext);
      if (m_config != nullptr) {
        m_selectPopup->setShadowConfig(m_config->config().shell.shadow);
      }
      m_selectPopup->setParent(m_layerSurface->layerSurface(), m_wlSurface, m_output);
      m_sceneRoot->setPopupContext(m_selectPopup.get());
    }
    m_sceneRoot->setSize(w, h);

    Node* sceneParent = m_sceneRoot.get();
    if (m_attachedToBar) {
      auto revealClip = std::make_unique<Node>();
      revealClip->setClipChildren(m_chromeHosted);
      m_attachedRevealClipNode = m_sceneRoot->addChild(std::move(revealClip));

      auto revealContent = std::make_unique<Node>();
      m_attachedRevealContentNode = m_attachedRevealClipNode->addChild(std::move(revealContent));
      sceneParent = m_attachedRevealContentNode;
    } else {
      auto revealClip = std::make_unique<Node>();
      revealClip->setClipChildren(m_chromeHosted);
      m_detachedRevealClipNode = m_sceneRoot->addChild(std::move(revealClip));

      auto revealContent = std::make_unique<Node>();
      m_detachedRevealContentNode = m_detachedRevealClipNode->addChild(std::move(revealContent));
      sceneParent = m_detachedRevealContentNode;
    }

    // Attached panel material is drawn by Bar's fullscreen chrome surface.
    // Rendering another rounded Box here would make the frame/panel join use
    // two independently clipped shapes and causes visible edge flicker.
    if (hasDecoration && !m_chromeHosted) {
      auto chrome = std::make_unique<ChromeBlobNode>();
      chrome->setHitTestVisible(false);
      chrome->setParticipatesInLayout(false);
      m_detachedChromeNode = static_cast<ChromeBlobNode*>(sceneParent->addChild(std::move(chrome)));
    }

    // Create panel content inside a wrapper node for staggered fade-in
    auto contentWrapper = std::make_unique<Node>();
    contentWrapper->setClipChildren(true);
    m_contentNode = contentWrapper.get();
    m_activeContentVisualWidth = std::max(1.0f, m_panelVisualWidthExact);
    m_activeContentVisualHeight = std::max(1.0f, m_panelVisualHeightExact);
    m_activeContentPadding = hasDecoration ? m_activePanel->contentScale() * Style::panelPadding : 0.0f;
    m_activePanel->setAnimationManager(&m_animations);
    const float panelBackgroundOpacity = m_attachedToBar
        ? m_attachedBackgroundOpacity
        : detachedPanelBackgroundOpacity(m_activePanel, m_activePanelId, m_config);
    m_activePanel->setPanelCardOpacity(resolvePanelCardOpacity(m_config, panelBackgroundOpacity));
    m_activePanel->setPanelBordersEnabled(m_config->config().shell.panel.borders);
    m_activePanel->create();
    m_activePanel->onOpen(m_pendingOpenContext);
    m_pendingOpenContext.clear();
    if (m_activePanel->root() != nullptr) {
      contentWrapper->addChild(m_activePanel->releaseRoot());
    }
    sceneParent->addChild(std::move(contentWrapper));

    m_inputDispatcher.setSceneRoot(m_sceneRoot.get());
    m_inputDispatcher.setTextInputContext(m_wlSurface, m_platform->wayland().textInputService());
    m_inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_platform->setCursorShape(serial, shape);
    });
    m_inputDispatcher.setHoverChangeCallback([this](InputArea* /*old*/, InputArea* next) {
      if (m_layerSurface != nullptr) {
        TooltipManager::instance().onHoverChange(next, m_layerSurface->layerSurface(), m_output);
      }
    });
    m_inputDispatcher.setFocusChangeCallback([this](InputArea* /*old*/, InputArea* next) {
      if (m_activePanel != nullptr && next != nullptr) {
        m_activePanel->scrollFocusedInputIntoView(next);
      }
    });

    if (m_attachedToBar && m_attachedRevealClipNode != nullptr) {
      m_sceneRoot->setOpacity(1.0f);
      applyAttachedReveal(0.0f);
      m_attachedOpenAnimationPending = true;
    } else {
      applyDetachedReveal(0.0f);
      m_animations.animate(
          0.0f, 1.0f, Style::animChromeSpatial, Easing::CaelestiaExpressiveSpatial,
          [this](float v) { applyDetachedReveal(v); }, {}, m_sceneRoot.get()
      );
    }

    m_surface->setSceneRoot(m_sceneRoot.get());

    // Set initial keyboard focus if the panel requests it
    if (m_activePanel != nullptr) {
      if (auto* focusArea = m_activePanel->initialFocusArea(); focusArea != nullptr) {
        m_inputDispatcher.setFocus(focusArea);
      }
    }
  }

  // Content updates can add/remove rows after the previous viewport layout.
  // Measure its intrinsic body before snapshotting panel geometry so a size
  // request made here affects this same frame.
  {
    UiPhaseScope updatePhase(UiPhase::Update);
    m_activePanel->update(*renderer);
  }
  syncActivePanelIntrinsicHeight(*renderer, m_sceneRoot != nullptr && m_intrinsicVisualHeightResolved);

  m_sceneRoot->setSize(w, h);
  if (m_attachedRevealContentNode != nullptr) {
    m_attachedRevealContentNode->setFrameSize(w, h);
  }
  if (m_detachedRevealContentNode != nullptr) {
    m_detachedRevealContentNode->setFrameSize(w, h);
  }

  if (!m_attachedToBar) {
    syncDetachedVisualGeometry(width, height);
  }

  if (m_attachedToBar && m_panelMorphAnimation != 0) {
    applyAttachedMorphState(m_chromeTransition.displayed());
  } else if (m_attachedToBar) {
    applyAttachedReveal(m_attachedRevealProgress);
  } else {
    applyDetachedReveal(m_detachedRevealProgress);
  }

  const auto panelX = static_cast<float>(m_panelInsetX);
  const auto panelY = static_cast<float>(m_panelInsetY);
  const float panelW = m_panelVisualWidth > 0 ? static_cast<float>(m_panelVisualWidth) : w;
  const float panelH = m_panelVisualHeight > 0 ? static_cast<float>(m_panelVisualHeight) : h;
  if (m_detachedChromeNode != nullptr && m_config != nullptr) {
    const auto& chrome = m_config->config().shell.chrome;
    ChromeBlobStyle style{
        .fill = colorForRole(ColorRole::Surface),
        .shadow = Color{},
        .apertureInsets = RectInsets{
            chrome.frameThickness, chrome.frameThickness, chrome.frameThickness, chrome.frameThickness
        },
        .rounding = chrome.rounding,
        .smoothing = chrome.smoothing,
        .shadowRadius = 0.0f,
        .shadowOffsetX = 0.0f,
        .shadowOffsetY = 0.0f,
        .rectCount = 1,
        .drawFrame = false,
        .debug = gnilChromeDebugEnabled(),
        .debugInputEnabled = !m_closing && m_detachedRevealProgress > 0.001f,
    };
    style.rects[0] = ChromeBlobRect{
        .x = panelX,
        .y = panelY,
        .width = panelW,
        .height = panelH,
        .radius = std::max(1.0f, chrome.rounding),
        .deformation = std::clamp(
            std::sin(std::clamp(m_detachedRevealProgress, 0.0f, 1.0f) * 3.14159265358979323846f) * chrome.deformScale,
            -0.12f, 0.12f
        ),
    };
    style.debugInputRect = style.rects[0];
    style.debugProgress = std::clamp(m_detachedRevealProgress, 0.0f, 1.0f);
    m_detachedChromeNode->setPosition(0.0f, 0.0f);
    m_detachedChromeNode->setFrameSize(w, h);
    m_detachedChromeNode->setStyle(style);
  }

  const float kPadding = hasDecoration ? m_activePanel->contentScale() * Style::panelPadding : 0.0f;
  m_contentWidth = panelW - kPadding * 2.0f;
  m_contentHeight = panelH - kPadding * 2.0f;
  {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    m_activePanel->layout(*renderer, m_contentWidth, m_contentHeight);
  }
  if (m_contentNode != nullptr) {
    m_activeContentVisualWidth = panelW;
    m_activeContentVisualHeight = panelH;
    m_activeContentPadding = kPadding;
    if (m_attachedToBar && m_panelMorphAnimation != 0) {
      positionAttachedContentLayers(m_chromeTransition.displayed());
    } else {
      m_contentNode->setPosition(panelX + kPadding, panelY + kPadding);
      m_contentNode->setSize(panelW - kPadding * 2.0f, panelH - kPadding * 2.0f);
    }
  }
  applyPendingPanelFocus();
  m_panelGeometryDirty = false;
  if (m_pointerInside) {
    m_inputDispatcher.syncPointerHover();
  }
}

void PanelManager::applyPendingPanelFocus() {
  if (m_activePanel == nullptr) {
    return;
  }
  if (auto* area = m_activePanel->takePendingFocusArea(); area != nullptr) {
    m_inputDispatcher.setFocus(area);
  }
}

void PanelManager::prepareFrame(bool needsUpdate, bool needsLayout) {
  if (m_renderContext == nullptr || m_surface == nullptr) {
    return;
  }
  if (m_activePanel == nullptr) {
    return;
  }

  m_renderContext->makeCurrent(m_surface->renderTarget());

  const auto width = m_surface->width();
  const auto height = m_surface->height();

  const bool needsSceneBuild = m_sceneRoot == nullptr
      || static_cast<std::uint32_t>(std::round(m_sceneRoot->width())) != width
      || static_cast<std::uint32_t>(std::round(m_sceneRoot->height())) != height
      || m_panelGeometryDirty;
  if (needsSceneBuild) {
    buildScene(width, height);
  }

  if (!needsSceneBuild && needsUpdate) {
    UiPhaseScope updatePhase(UiPhase::Update);
    m_activePanel->update(*m_renderContext);
  }
  if (!needsSceneBuild && (needsUpdate || needsLayout)) {
    syncActivePanelIntrinsicHeight(*m_renderContext, true);
  }
  if (!needsSceneBuild && needsLayout) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    if (m_activePanel != nullptr) {
      m_inputDispatcher.stashTabFocus();
      m_activePanel->layout(*m_renderContext, m_contentWidth, m_contentHeight);
      m_inputDispatcher.restoreStashedTabFocus();
    }
    if (m_pointerInside) {
      m_inputDispatcher.syncPointerHover();
    }
  }
  if (!needsSceneBuild && (needsUpdate || needsLayout)) {
    applyPendingPanelFocus();
  }
}

void PanelManager::registerIpc(IpcService& ipc) {
  const auto canonicalPanelId = [](std::string& panelId) {
    // `sidebar` is the stable public id; retain `notifications` as a compatibility alias.
    if (panelId == "notifications") {
      panelId = "sidebar";
    }
  };
  const auto routeLegacyControlCenter = [](std::string& panelId, std::string& context) -> std::optional<std::string> {
    if (panelId != "control-center") {
      return std::nullopt;
    }
    const std::string_view target = legacyControlCenterTarget(context);
    if (target.empty()) {
      return "error: control-center dashboard was removed; specify a destination panel\n";
    }
    panelId = std::string(target);
    context.clear();
    return std::nullopt;
  };
  auto parseOpenArgs = [](std::string_view rawArgs, std::string_view command, std::string& panelId,
                          std::string& context) -> std::optional<std::string> {
    const std::string_view args = StringUtils::trimLeftView(rawArgs);
    if (args.empty()) {
      return "error: " + std::string(command) + " requires a panel id\n";
    }

    const auto sep = args.find_first_of(" \t\n\r\f\v");
    if (sep == std::string_view::npos) {
      panelId = std::string(args);
      context.clear();
      return std::nullopt;
    }

    panelId = std::string(args.substr(0, sep));
    // Preserve the context verbatim (only strip the separator's leading
    // whitespace) — trailing whitespace can be significant, e.g. a command.
    context = std::string(StringUtils::trimLeftView(args.substr(sep + 1)));
    return std::nullopt;
  };

  auto unknownPanelError = [this](std::string_view panelId) -> std::string {
    std::vector<std::string> ids;
    ids.reserve(m_panels.size());
    for (const auto& entry : m_panels) {
      ids.push_back(entry.first);
    }
    std::ranges::sort(ids);

    std::string error = "error: unknown panel \"" + std::string(panelId) + "\"";
    if (!ids.empty()) {
      error += " (available: " + StringUtils::join(ids, ", ") + ")";
    }
    error += '\n';
    return error;
  };

  ipc.registerHandler(
      "panel",
      [this, unknownPanelError, canonicalPanelId, routeLegacyControlCenter](const std::string& args) -> std::string {
        const auto tokens = StringUtils::splitWhitespace(args);
        if (tokens.size() < 2) {
          return "error: panel requires <open|close|toggle> <id> [--output <name>]\n";
        }
        const std::string& operation = tokens[0];
        std::string panelId = tokens[1];
        canonicalPanelId(panelId);

        wl_output* output = nullptr;
        std::string context;
        for (std::size_t i = 2; i < tokens.size(); ++i) {
          if (tokens[i] == "--output") {
            if (++i >= tokens.size()) {
              return "error: --output requires a connector name\n";
            }
            if (m_platform != nullptr) {
              for (const auto& candidate : m_platform->outputs()) {
                if (candidate.connectorName == tokens[i] || candidate.interfaceName == tokens[i]) {
                  output = candidate.output;
                  break;
                }
              }
            }
            if (output == nullptr) {
              return "error: unknown output \"" + tokens[i] + "\"\n";
            }
          } else {
            context += context.empty() ? tokens[i] : " " + tokens[i];
          }
        }
        if (auto error = routeLegacyControlCenter(panelId, context)) {
          return *error;
        }
        if (!m_panels.contains(panelId)) {
          return unknownPanelError(panelId);
        }

        const PanelOpenRequest request{.output = output, .context = context};
        if (operation == "open") {
          openPanel(panelId, request);
        } else if (operation == "close") {
          if (isOpenPanel(panelId)) {
            closePanel();
          }
        } else if (operation == "toggle") {
          togglePanel(panelId, request);
        } else {
          return "error: panel action must be open, close, or toggle\n";
        }
        return "ok\n";
      },
      "panel <open|close|toggle> <id> [--output <name>] [context]",
      "Open, close, or toggle a panel, optionally routing it to a named output"
  );

  ipc.registerHandler(
      "panel-toggle",
      [this, parseOpenArgs, unknownPanelError, canonicalPanelId, routeLegacyControlCenter](const std::string& args) -> std::string {
        std::string panelId;
        std::string context;
        if (auto error = parseOpenArgs(args, "panel-toggle", panelId, context)) {
          return *error;
        }
        canonicalPanelId(panelId);
        if (auto error = routeLegacyControlCenter(panelId, context)) {
          return *error;
        }
        if (!m_panels.contains(panelId)) {
          return unknownPanelError(panelId);
        }
        // Output left unset: openPanel resolves it (focus source, else compositor probe).
        if (context.empty()) {
          togglePanel(panelId);
        } else {
          togglePanel(panelId, PanelOpenRequest{.context = context});
        }
        return "ok\n";
      },
      "panel-toggle <id> [context]",
      "Toggle a panel by id, optionally with context (e.g. launcher /emo)"
  );

  ipc.registerHandler(
      "panel-open",
      [this, parseOpenArgs, unknownPanelError, canonicalPanelId, routeLegacyControlCenter](const std::string& args) -> std::string {
        std::string panelId;
        std::string context;
        if (auto error = parseOpenArgs(args, "panel-open", panelId, context)) {
          return *error;
        }
        canonicalPanelId(panelId);
        if (auto error = routeLegacyControlCenter(panelId, context)) {
          return *error;
        }
        if (!m_panels.contains(panelId)) {
          return unknownPanelError(panelId);
        }

        if (isOpen() && !m_closing && m_activePanelId == panelId) {
          if (!context.empty() && m_activePanel != nullptr) {
            m_activePanel->onOpen(context);
            refresh();
          }
          return "ok\n";
        }

        // Output left unset: openPanel resolves it (focus source, else compositor probe).
        openPanel(panelId, PanelOpenRequest{.context = context});
        return "ok\n";
      },
      "panel-open <id> [context]",
      "Open a panel by id, optionally with context (e.g. launcher /emo)"
  );

  ipc.registerHandler(
      "panel-close",
      [this, unknownPanelError, canonicalPanelId](const std::string& args) -> std::string {
        std::string panelId = StringUtils::trim(args);
        if (!panelId.empty() && StringUtils::splitWhitespace(panelId).size() != 1) {
          return "error: panel-close accepts at most one panel id\n";
        }
        canonicalPanelId(panelId);
        if (!panelId.empty() && panelId != "control-center" && !m_panels.contains(panelId)) {
          return unknownPanelError(panelId);
        }

        if (panelId.empty() || isOpenPanel(panelId)) {
          closePanel();
        }
        return "ok\n";
      },
      "panel-close [id]", "Close the active panel, or close the named panel if it is active"
  );

  const auto rejectSettingsArgs = [](const std::string& args, std::string_view command) -> std::optional<std::string> {
    if (StringUtils::trim(args).empty()) {
      return std::nullopt;
    }
    return std::format("error: {} accepts no arguments\n", command);
  };

  ipc.registerHandler(
      "settings-open",
      [this](const std::string& args) -> std::string {
        openSettingsWindow(std::string(StringUtils::trimLeftView(args)));
        return "ok\n";
      },
      "settings-open [context]",
      "Open the settings window, or focus it if already open, optionally at a specific section"
  );

  ipc.registerHandler(
      "settings-close",
      [this, rejectSettingsArgs](const std::string& args) -> std::string {
        if (auto error = rejectSettingsArgs(args, "settings-close")) {
          return *error;
        }
        closeSettingsWindow();
        return "ok\n";
      },
      "settings-close", "Close the settings window"
  );

  ipc.registerHandler(
      "settings-toggle",
      [this](const std::string& args) -> std::string {
        toggleSettingsWindow(std::string(StringUtils::trimLeftView(args)));
        return "ok\n";
      },
      "settings-toggle [context]", "Toggle the settings window, optionally at a specific section"
  );
}
