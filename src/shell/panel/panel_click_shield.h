#pragma once

#include "wayland/surface.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

class WaylandConnection;
enum class LayerShellLayer : std::uint32_t;
struct wl_buffer;
struct wl_output;
struct wl_surface;
struct wp_viewport;
struct zwlr_layer_surface_v1;

namespace panel_dismissal {

  // Caelestia's latched drawers own a dismissal group even when they were
  // opened from a shortcut with no trigger surface. Any rail-triggered popout
  // joins the same model; hover-only surfaces are hosted elsewhere and never
  // reach PanelManager.
  [[nodiscard]] constexpr bool usesClickShield(std::string_view panelId, bool railTriggered) noexcept {
    return railTriggered
        || panelId == "launcher"
        || panelId == "session"
        || panelId == "control-center"
        || panelId == "dashboard"
        || panelId == "media"
        || panelId == "audio"
        || panelId == "brightness"
        || panelId == "system"
        || panelId == "battery"
        || panelId == "network"
        || panelId == "bluetooth"
        || panelId == "weather"
        || panelId == "calendar"
        || panelId == "screen-time"
        || panelId == "notifications"
        || panelId == "sidebar"
        || panelId == "utilities"
        || panelId == "tray-drawer"
        || panelId == "settings";
  }

} // namespace panel_dismissal

// Niri-specific outside-click catcher. One transparent layer-shell surface is
// mapped per output below the active drawer. Its input region covers the output
// except for rail rectangles, so the first application click dismisses the
// drawer and is consumed while rail triggers continue to receive clicks.
//
// A shared transparent 1x1 SHM buffer is stretched with wp_viewporter. The
// shield never requests keyboard focus; PanelKeyboardFocusTracker remains a
// fallback for compositors missing one of the required Wayland globals.
class PanelClickShield {
public:
  using ExcludeProvider = std::function<std::vector<InputRect>(wl_output*)>;

  PanelClickShield() = default;
  ~PanelClickShield();

  PanelClickShield(const PanelClickShield&) = delete;
  PanelClickShield& operator=(const PanelClickShield&) = delete;

  void initialize(WaylandConnection& wayland);

  // Must run before the panel's first commit so same-layer ordering leaves the
  // panel above its co-output shield.
  void activate(const std::vector<wl_output*>& outputs, LayerShellLayer layer, ExcludeProvider excludeProvider);

  // Reconcile output hotplug without remapping existing shields. Remapping the
  // target-output shield while its panel is open would put the shield above it.
  void syncOutputs(const std::vector<wl_output*>& outputs);

  // Tear down immediately when a close transition starts. Idempotent.
  void deactivate();

  [[nodiscard]] bool isActive() const noexcept { return !m_shields.empty(); }
  [[nodiscard]] bool isArmed() const noexcept { return m_layer.has_value(); }
  [[nodiscard]] bool ownsSurface(wl_surface* surface) const noexcept;

  static void handleConfigure(
      void* data, zwlr_layer_surface_v1* layerSurface, std::uint32_t serial, std::uint32_t width, std::uint32_t height
  );
  static void handleClosed(void* data, zwlr_layer_surface_v1* layerSurface);

private:
  struct Shield {
    PanelClickShield* owner = nullptr;
    wl_output* output = nullptr;
    wl_surface* surface = nullptr;
    zwlr_layer_surface_v1* layerSurface = nullptr;
    wp_viewport* viewport = nullptr;
    std::int32_t width = 0;
    std::int32_t height = 0;
    bool configured = false;
    bool bufferAttached = false;
    std::vector<InputRect> excludeRects;
  };

  bool ensureSharedBuffer();
  std::unique_ptr<Shield> createShield(wl_output* output, std::vector<InputRect> excludeRects);
  void destroyShield(Shield& shield);
  void applyConfigured(Shield& shield, std::uint32_t width, std::uint32_t height);
  void applyInputRegion(Shield& shield);

  WaylandConnection* m_wayland = nullptr;
  wl_buffer* m_buffer = nullptr;
  std::optional<LayerShellLayer> m_layer;
  ExcludeProvider m_excludeProvider;
  std::unordered_map<wl_output*, std::unique_ptr<Shield>> m_shields;
};
