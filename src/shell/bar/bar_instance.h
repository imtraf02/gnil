#pragma once

#include "config/config_service.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "shell/bar/widget.h"
#include "shell/chrome/chrome_geometry.h"
#include "shell/chrome/chrome_output_host.h"
#include "ui/signal.h"
#include "wayland/layer_surface.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

class Box;
class Flex;
class Node;

struct BarCapsuleRun {
  Node* shell = nullptr;
  Box* bg = nullptr;
  Flex* container = nullptr;
  Node* content = nullptr;
  WidgetBarCapsuleSpec spec{};
  float contentScale = 1.0f;
  std::vector<Widget*> widgets;
  // Hover highlight overlays, parallel to `widgets` for group runs; one shared box for single runs.
  std::vector<Box*> hoverBoxes;
};

struct BarInstance {
  std::uint32_t outputName = 0;
  wl_output* output = nullptr;
  std::int32_t scale = 1;
  std::size_t barIndex = 0;
  BarConfig barConfig;
  std::unique_ptr<LayerSurface> surface;
  // Layout-only companion to the fullscreen chrome canvas. Layer-shell only
  // applies an exclusive zone reliably to an edge-anchored surface; this one
  // has no scene or input region and never contributes visual chrome.
  // Four input-transparent edge surfaces publish the work-area reservation.
  // Keeping them separate lets Niri apply the frame thickness to every edge
  // while the left edge independently follows the persistent bar state.
  std::array<std::unique_ptr<LayerSurface>, 4> exclusionSurfaces;
  // sceneRoot must be destroyed before `animations` — ~Node() calls cancelForOwner().
  AnimationManager animations;
  std::unique_ptr<Node> sceneRoot;
  // Frame and rail share this one scene tree/surface.  The closed rail is the
  // frame's left material edge rather than an overlapping second surface.
  std::unique_ptr<ChromeOutputHost> chromeHost;
  Node* slideRoot = nullptr;
  InputDispatcher inputDispatcher;
  float hideOpacity = 1.0f;
  // bar-hide/toggle IPC on non-autohide bars: release compositor exclusive zone until bar-show (v4 isVisible=false).
  bool ipcLayoutReleased = false;
  // Ling distinguishes an explicit toggle from a temporary hover reveal:
  // only the former expands the compositor work area.
  bool ipcPinnedVisible = false;
  // bar-auto-hide-set off keeps autoHide true until the reveal completes; block hover helpers from replacing it.
  bool autoHideDisablePending = false;
  // smart_auto_hide: active workspace empty (or overview open) — keep the bar visible.
  bool smartAutoHidePinnedVisible = false;
  bool pointerInside = false;
  bool edgeDragActive = false;
  float edgeDragStart = 0.0f;
  float lastPointerSx = 0.0f;
  float lastPointerSy = 0.0f;
  std::size_t attachedPopupCount = 0;

  // Bar background, shadow, and layout sections (start/center/end along main axis)
  Box* bg = nullptr;
  // The attached panel background is painted on the same full-output chrome
  // surface as the rail and frame. Its own layer surface carries only content.
  Box* attachedPanelBackdrop = nullptr;
  Box* shadow = nullptr;
  Node* shadowLeftClip = nullptr;
  Node* shadowRightClip = nullptr;
  Box* shadowLeft = nullptr;
  Box* shadowRight = nullptr;
  Node* contentClip = nullptr;
  // Unclipped layer between the bar background and contentClip; hosts the hover pills of
  // capsule-less widgets so they neither affect layout nor get clipped at section boundaries.
  Node* hoverUnderlay = nullptr;
  Node* startSlot = nullptr;
  Node* centerSlot = nullptr;
  Node* endSlot = nullptr;
  Flex* startSection = nullptr;
  Flex* centerSection = nullptr;
  Flex* endSection = nullptr;

  std::vector<std::unique_ptr<Widget>> startWidgets;
  std::vector<std::unique_ptr<Widget>> centerWidgets;
  std::vector<std::unique_ptr<Widget>> endWidgets;
  std::vector<BarCapsuleRun> startCapsuleRuns;
  std::vector<BarCapsuleRun> centerCapsuleRuns;
  std::vector<BarCapsuleRun> endCapsuleRuns;

  // Maps each widget's root node to its Widget so hover-change events resolve to the owning widget.
  std::unordered_map<const Node*, Widget*> widgetByRoot;
  Widget* hoverHighlightWidget = nullptr;
  Widget* activePanelWidget = nullptr;

  Signal<>::ScopedConnection paletteConn;
  std::optional<ChromePanelState> chromePanelState;
  std::vector<ChromePanelState> chromeToastStates;
};
