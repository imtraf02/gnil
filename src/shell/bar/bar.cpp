#include "shell/bar/bar.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/process/process.h"
#include "core/scoped_timer.h"
#include "core/timer_manager.h"
#include "core/ui_phase.h"
#include "idle/idle_inhibitor.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "shell/bar/bar_corner_shape.h"
#include "shell/bar/bar_reserved_zone.h"
#include "shell/bar/widget.h"
#include "shell/chrome/chrome_output_host.h"
#include "shell/panel/panel_manager.h"
#include "shell/surface/shadow.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <linux/input-event-codes.h>
#include <optional>
#include <ranges>
#include <unordered_set>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

namespace {

  constexpr Logger kLog("bar");
  constexpr std::chrono::milliseconds kWorkspaceRevealDebounce{80};
  constexpr std::chrono::milliseconds kWorkspacePeekHold{450};
  constexpr std::int32_t kAutoHideTriggerPx = 3;

  enum ChromeExclusionIndex : std::size_t {
    ChromeExclusionLeft = 0,
    ChromeExclusionTop,
    ChromeExclusionRight,
    ChromeExclusionBottom,
  };

  [[nodiscard]] std::string activeWorkspaceId(const std::vector<Workspace>& workspaces) {
    for (const auto& workspace : workspaces) {
      if (workspace.active) {
        if (!workspace.id.empty()) {
          return workspace.id;
        }
        if (!workspace.name.empty()) {
          return workspace.name;
        }
        return std::to_string(workspace.index);
      }
    }
    return {};
  }

  [[nodiscard]] bool barConfigUsesSlideSurface(const BarConfig& cfg) noexcept {
    return cfg.autoHide || cfg.smartAutoHide;
  }

  [[nodiscard]] bool barSupportsSlideBehavior(const BarConfig& cfg) noexcept {
    return cfg.autoHide || cfg.smartAutoHide;
  }

  [[nodiscard]] bool barPointerHideAllowed(const BarInstance& instance) noexcept {
    if (instance.ipcPinnedVisible) {
      return false;
    }
    if (instance.barConfig.smartAutoHide) {
      return !instance.smartAutoHidePinnedVisible;
    }
    return instance.barConfig.autoHide;
  }

  [[nodiscard]] bool workspaceKeyMatchesAssignment(std::string_view assignmentKey, const Workspace& workspace) {
    if (assignmentKey.empty()) {
      return false;
    }
    if (!workspace.id.empty() && assignmentKey == workspace.id) {
      return true;
    }
    if (!workspace.name.empty() && assignmentKey == workspace.name) {
      return true;
    }
    if (workspace.index > 0 && assignmentKey == std::to_string(workspace.index)) {
      return true;
    }
    return false;
  }

  [[nodiscard]] bool activeWorkspaceHasWindows(const CompositorPlatform& platform, wl_output* output) {
    const auto workspaces = platform.workspaces(output);
    const Workspace* active = nullptr;
    for (const auto& workspace : workspaces) {
      if (workspace.active) {
        active = &workspace;
        break;
      }
    }
    if (active == nullptr) {
      return false;
    }

    const auto assignments = platform.workspaceWindowAssignments(output);
    for (const auto& assignment : assignments) {
      if (workspaceKeyMatchesAssignment(assignment.workspaceKey, *active)) {
        return true;
      }
    }
    if (!assignments.empty()) {
      return false;
    }
    return active->occupied;
  }

  [[nodiscard]] bool smartAutoHideWantsPinnedVisible(const CompositorPlatform& platform, wl_output* output) {
    if (platform.hasOverviewState() && platform.isOverviewOpen()) {
      return true;
    }
    return !activeWorkspaceHasWindows(platform, output);
  }

  [[nodiscard]] FontWeight parseWidgetLabelFontWeight(const WidgetConfig& config, FontWeight fallback) {
    const auto it = config.settings.find("font_weight");
    if (it == config.settings.end()) {
      return fallback;
    }

    if (const auto* raw = std::get_if<std::int64_t>(&it->second)) {
      return static_cast<FontWeight>(*raw);
    }
    return fallback;
  }

  // `[widget.*] font_family` override; empty/whitespace or absent inherits the bar-resolved family.
  [[nodiscard]] std::string parseWidgetLabelFontFamily(const WidgetConfig& config, const std::string& fallback) {
    const auto it = config.settings.find("font_family");
    if (it == config.settings.end()) {
      return fallback;
    }
    if (const auto* raw = std::get_if<std::string>(&it->second)) {
      std::string trimmed = StringUtils::trim(*raw);
      if (!trimmed.empty()) {
        return trimmed;
      }
    }
    return fallback;
  }

  [[nodiscard]] std::vector<InputRect>
  barAutoHideTriggerRegion(const BarConfig& cfg, const ShellConfig& shell, int surfW, int surfH) {
    if (surfW <= 0 || surfH <= 0) {
      return {};
    }

    (void)shell;
    const bool vertical = cfg.position == "left" || cfg.position == "right";
    const int strip = std::min(kAutoHideTriggerPx, vertical ? surfW : surfH);
    if (strip <= 0) {
      return {};
    }
    if (cfg.position == "bottom") {
      return {InputRect{0, surfH - strip, surfW, strip}};
    }
    if (cfg.position == "left") {
      return {InputRect{0, 0, strip, surfH}};
    }
    if (cfg.position == "right") {
      return {InputRect{surfW - strip, 0, strip, surfH}};
    }
    return {InputRect{0, 0, surfW, strip}};
  }

  bool pointInsideNode(const Node* node, float sceneX, float sceneY) {
    if (node == nullptr) {
      return false;
    }
    float localX = 0.0f;
    float localY = 0.0f;
    if (!Node::mapFromScene(node, sceneX, sceneY, localX, localY)) {
      return false;
    }
    return localX >= 0.0f && localX < node->width() && localY >= 0.0f && localY < node->height();
  }

  HitTestOutset crossAxisOutsetToSlot(const Node* node, const Node* slot, bool isVertical) {
    if (node == nullptr || slot == nullptr) {
      return {};
    }

    float nodeX = 0.0f;
    float nodeY = 0.0f;
    float slotX = 0.0f;
    float slotY = 0.0f;
    Node::absolutePosition(node, nodeX, nodeY);
    Node::absolutePosition(slot, slotX, slotY);

    if (isVertical) {
      return {
          .left = std::max(0.0f, nodeX - slotX),
          .top = 0.0f,
          .right = std::max(0.0f, (slotX + slot->width()) - (nodeX + node->width())),
          .bottom = 0.0f,
      };
    }

    return {
        .left = 0.0f,
        .top = std::max(0.0f, nodeY - slotY),
        .right = 0.0f,
        .bottom = std::max(0.0f, (slotY + slot->height()) - (nodeY + node->height())),
    };
  }

  void applyBarWidgetHitTargets(Node* node, const Node* slot, bool isVertical) {
    if (node == nullptr || slot == nullptr) {
      return;
    }

    if (dynamic_cast<InputArea*>(node) != nullptr || node->clipChildren()) {
      node->setHitTestOutset(crossAxisOutsetToSlot(node, slot, isVertical));
    }

    for (const auto& child : node->children()) {
      applyBarWidgetHitTargets(child.get(), slot, isVertical);
    }
  }

  Widget* widgetAtPoint(const std::vector<std::unique_ptr<Widget>>& widgets, float sceneX, float sceneY) {
    for (const auto& widgetPtr : std::views::reverse(widgets)) {
      auto* widget = widgetPtr.get();
      if (widget == nullptr || widget->isBarClickThrough() || widget->root() == nullptr || !widget->root()->visible()) {
        continue;
      }
      if (Node::hitTest(widget->root(), sceneX, sceneY) != nullptr || pointInsideNode(widget->root(), sceneX, sceneY)) {
        return widget;
      }
    }
    for (const auto& widgetPtr : std::views::reverse(widgets)) {
      auto* widget = widgetPtr.get();
      if (widget == nullptr || widget->isBarClickThrough()) {
        continue;
      }
      auto* root = widget != nullptr ? widget->root() : nullptr;
      auto* bounds = widget != nullptr ? widget->layoutBoundsNode() : nullptr;
      if (root == nullptr || bounds == nullptr || bounds == root || root->parent() != bounds || !bounds->visible()) {
        continue;
      }
      if (Node::hitTest(bounds, sceneX, sceneY) != nullptr || pointInsideNode(bounds, sceneX, sceneY)) {
        return widget;
      }
    }
    return nullptr;
  }

  Widget* widgetAtPoint(const BarInstance& instance, float sceneX, float sceneY) {
    if (auto* widget = widgetAtPoint(instance.endWidgets, sceneX, sceneY); widget != nullptr) {
      return widget;
    }
    if (auto* widget = widgetAtPoint(instance.centerWidgets, sceneX, sceneY); widget != nullptr) {
      return widget;
    }
    return widgetAtPoint(instance.startWidgets, sceneX, sceneY);
  }

  std::pair<float, float> surfaceOriginForOutputLocal(const BarInstance& instance, const WaylandOutput& outputInfo) {
    if (instance.surface == nullptr) {
      return {0.0f, 0.0f};
    }
    const auto* surface = instance.surface.get();
    const std::uint32_t anchor = surface->anchor();
    const bool aTop = (anchor & LayerShellAnchor::Top) != 0;
    const bool aBottom = (anchor & LayerShellAnchor::Bottom) != 0;
    const bool aLeft = (anchor & LayerShellAnchor::Left) != 0;
    const bool aRight = (anchor & LayerShellAnchor::Right) != 0;
    const auto mTop = static_cast<float>(surface->marginTop());
    const auto mRight = static_cast<float>(surface->marginRight());
    const auto mBottom = static_cast<float>(surface->marginBottom());
    const auto mLeft = static_cast<float>(surface->marginLeft());
    const auto surfW = static_cast<float>(surface->width());
    const auto surfH = static_cast<float>(surface->height());
    const auto outputW = static_cast<float>(outputInfo.effectiveLogicalWidth());
    const auto outputH = static_cast<float>(outputInfo.effectiveLogicalHeight());

    float x = 0.0f;
    float y = 0.0f;
    if (aLeft && aRight) {
      x = mLeft;
    } else if (aRight) {
      x = std::max(0.0f, outputW - mRight - surfW);
    } else {
      x = mLeft;
    }

    if (aTop && aBottom) {
      y = mTop;
    } else if (aBottom) {
      y = std::max(0.0f, outputH - mBottom - surfH);
    } else {
      y = mTop;
    }
    return {x, y};
  }

  bool isBarDeadZone(const BarInstance& instance, float sceneX, float sceneY) {
    if (widgetAtPoint(instance, sceneX, sceneY) != nullptr) {
      return false;
    }
    return pointInsideNode(instance.startSection, sceneX, sceneY)
        || pointInsideNode(instance.centerSection, sceneX, sceneY)
        || pointInsideNode(instance.endSection, sceneX, sceneY)
        || pointInsideNode(instance.sceneRoot.get(), sceneX, sceneY);
  }

  void executeDeadZoneCommand(const std::string& command) {
    if (command.empty()) {
      return;
    }
    if (!process::runAsync(command)) {
      kLog.warn("bar dead zone command failed: {}", command);
    }
  }

  float pointerScrollDelta(const PointerEvent& event) {
    if (event.axis != WL_POINTER_AXIS_VERTICAL_SCROLL && event.axis != WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
      return 0.0f;
    }

    if (event.axisValue120 != 0) {
      return static_cast<float>(event.axisValue120) / 120.0f;
    }
    if (event.axisDiscrete != 0) {
      return static_cast<float>(event.axisDiscrete);
    }
    if (event.axisValue != 0.0) {
      return static_cast<float>(event.axisValue);
    }
    return 0.0f;
  }

  bool handleBarDeadZoneButton(
      BarInstance& instance, float sx, float sy, std::uint32_t button, CompositorPlatform* platform
  ) {
    (void)platform;
    if (!isBarDeadZone(instance, sx, sy)) {
      return false;
    }

    const auto& deadZone = instance.barConfig.deadZone;
    if (button == BTN_LEFT && !deadZone.command.empty()) {
      executeDeadZoneCommand(deadZone.command);
      return true;
    }
    if (button == BTN_RIGHT) {
      if (!deadZone.rightCommand.empty()) {
        executeDeadZoneCommand(deadZone.rightCommand);
        return true;
      }
      PanelManager::instance().closePanel();
      return true;
    }
    if (button == BTN_MIDDLE && !deadZone.middleCommand.empty()) {
      executeDeadZoneCommand(deadZone.middleCommand);
      return true;
    }
    return false;
  }

  bool handleBarDeadZoneAxis(BarInstance& instance, float sx, float sy, const PointerEvent& event) {
    if (!isBarDeadZone(instance, sx, sy)) {
      return false;
    }

    const float delta = pointerScrollDelta(event);
    if (delta == 0.0f) {
      return false;
    }

    const auto& deadZone = instance.barConfig.deadZone;
    const std::string& command = delta < 0.0f ? deadZone.scrollUpCommand : deadZone.scrollDownCommand;
    if (command.empty()) {
      return false;
    }

    executeDeadZoneCommand(command);
    return true;
  }

  ColorSpec withOpacity(const ColorSpec& color, float opacity) {
    ColorSpec out = color;
    out.alpha = std::clamp(out.alpha * std::clamp(opacity, 0.0f, 1.0f), 0.0f, 1.0f);
    return out;
  }

  // Hover highlight: peak fill alpha of the widget-foreground tint, and the cross-axis inset
  // (logical px, content-scaled) of per-member pills inside capsule groups.
  constexpr float kWidgetHoverFillAlpha = 0.1f;
  constexpr float kGroupHoverCrossInset = 2.0f;

  // Sizes a run's hover overlays after the capsule geometry is final. Single runs (and plain
  // widgets) get one overlay covering the whole shell; group runs get one pill per member so
  // only the hovered member lights up.
  void placeCapsuleHoverBoxes(
      BarCapsuleRun& run, bool isVertical, float shellW, float shellH, float contentX, float contentY,
      float capsuleRadius
  ) {
    if (run.hoverBoxes.empty()) {
      return;
    }
    if (run.container == nullptr) {
      Box* box = run.hoverBoxes.front();
      if (box == nullptr) {
        return;
      }
      box->setPosition(0.0f, 0.0f);
      box->setSize(shellW, shellH);
      box->setRadius(capsuleRadius);
      return;
    }
    const float scale = run.contentScale;
    const float crossInset = kGroupHoverCrossInset * scale;
    // Same breathing room a single capsule would give the member; pills may reach into the
    // gap toward a neighbor, which is fine — only the hovered member's pill is visible.
    const float mainPad = run.spec.padding * scale;
    const float shellMain = isVertical ? shellH : shellW;
    const float shellCross = isVertical ? shellW : shellH;
    const float contentMain = isVertical ? contentY : contentX;
    const float crossExtent = std::max(0.0f, shellCross - 2.0f * crossInset);
    for (std::size_t i = 0; i < run.widgets.size() && i < run.hoverBoxes.size(); ++i) {
      Node* root = run.widgets[i] != nullptr ? run.widgets[i]->root() : nullptr;
      Box* box = run.hoverBoxes[i];
      if (root == nullptr || box == nullptr) {
        continue;
      }
      const float rootStart = contentMain + (isVertical ? root->y() : root->x());
      const float rootExtent = isVertical ? root->height() : root->width();
      const float mainStart = std::max(0.0f, rootStart - mainPad);
      const float mainExtent = std::max(0.0f, std::min(shellMain, rootStart + rootExtent + mainPad) - mainStart);
      if (isVertical) {
        box->setPosition(crossInset, mainStart);
        box->setSize(crossExtent, mainExtent);
      } else {
        box->setPosition(mainStart, crossInset);
        box->setSize(mainExtent, crossExtent);
      }
      const float pillRadius = std::max(0.0f, std::min(box->width(), box->height()) * 0.5f);
      box->setRadius(std::min(pillRadius, std::max(0.0f, capsuleRadius - crossInset)));
    }
  }

  // Extends each member's hit target across the capsule padding and half the gap to its
  // neighbors, so hover/click coverage matches the capsule ink instead of stopping at the
  // content edge. Runs after applyBarWidgetHitTargets (which replaces outsets each layout).
  void extendCapsuleHitTargets(std::vector<BarCapsuleRun>& runs, bool isVertical) {
    for (auto& run : runs) {
      if (run.shell == nullptr || run.hoverBoxes.empty()) {
        continue;
      }

      // Single runs (capsule or ghost pill): the hover overlay rect is the hit region.
      if (run.container == nullptr) {
        Widget* widget = !run.widgets.empty() ? run.widgets.front() : nullptr;
        Box* box = run.hoverBoxes.front();
        auto* area = widget != nullptr ? dynamic_cast<InputArea*>(widget->root()) : nullptr;
        if (area == nullptr || box == nullptr) {
          continue;
        }
        const float areaStart = isVertical ? area->y() : area->x();
        const float areaEnd = areaStart + (isVertical ? area->height() : area->width());
        const float boxStart = isVertical ? box->y() : box->x();
        const float boxEnd = boxStart + (isVertical ? box->height() : box->width());
        auto outset = area->hitTestOutset();
        if (isVertical) {
          outset.top += std::max(0.0f, areaStart - boxStart);
          outset.bottom += std::max(0.0f, boxEnd - areaEnd);
        } else {
          outset.left += std::max(0.0f, areaStart - boxStart);
          outset.right += std::max(0.0f, boxEnd - areaEnd);
        }
        area->setHitTestOutset(outset);
        continue;
      }

      // Group runs: tile the capsule between members (midpoint of gaps).
      const float shellMain = isVertical ? run.shell->height() : run.shell->width();
      const float containerMain = isVertical ? run.container->y() : run.container->x();
      auto memberStart = [&](std::size_t i) {
        const Node* root = run.widgets[i]->root();
        return containerMain + (isVertical ? root->y() : root->x());
      };
      auto memberEnd = [&](std::size_t i) {
        const Node* root = run.widgets[i]->root();
        return memberStart(i) + (isVertical ? root->height() : root->width());
      };
      for (std::size_t i = 0; i < run.widgets.size(); ++i) {
        Widget* widget = run.widgets[i];
        auto* area = widget != nullptr ? dynamic_cast<InputArea*>(widget->root()) : nullptr;
        if (area == nullptr) {
          continue;
        }
        const float sliceStart = i > 0 ? (memberEnd(i - 1) + memberStart(i)) * 0.5f : 0.0f;
        const float sliceEnd = i + 1 < run.widgets.size() ? (memberEnd(i) + memberStart(i + 1)) * 0.5f : shellMain;
        auto outset = area->hitTestOutset();
        const float before = std::max(0.0f, memberStart(i) - sliceStart);
        const float after = std::max(0.0f, sliceEnd - memberEnd(i));
        if (isVertical) {
          outset.top += before;
          outset.bottom += after;
        } else {
          outset.left += before;
          outset.right += after;
        }
        area->setHitTestOutset(outset);
      }
    }
  }

  struct BarVisualGeometry {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
  };

  struct BarSurfaceSpec {
    std::int32_t marginTop = 0;
    std::int32_t marginRight = 0;
    std::int32_t marginBottom = 0;
    std::int32_t marginLeft = 0;
    std::uint32_t surfaceWidth = 0;
    std::uint32_t surfaceHeight = 0;
    std::int32_t exclusiveZone = 0;
  };

  [[nodiscard]] BarSurfaceSpec
  computeBarSurfaceSpec(const BarConfig& barConfig, const ShellConfig::ShadowConfig& shadowConfig) {
    (void)shadowConfig;
    BarSurfaceSpec spec;
    // Width/height 0 with all four anchors keeps the visual chrome fullscreen.
    spec.exclusiveZone = reservedBarExclusiveZone(barConfig, shadowConfig);
    return spec;
  }

  // Returns true when two bar configs can keep the same layer-shell objects.
  // The visual surface is fullscreen and its four exclusion surfaces update
  // their zones in-place, so bar thickness is geometry rather than surface
  // identity. Recreating on a thickness edit briefly orphaned attached panel
  // chrome and could leave its freshly configured buffer white.
  bool barConfigSurfaceFieldsEqual(
      const BarConfig& a, const BarConfig& b, const ShellConfig::ShadowConfig& previousShadow,
      const ShellConfig::ShadowConfig& nextShadow
  ) {
    (void)previousShadow;
    (void)nextShadow;
    return a.name == b.name
        && a.position == b.position
        && a.enabled == b.enabled
        && a.autoHide == b.autoHide
        && a.smartAutoHide == b.smartAutoHide
        && a.layer == b.layer
        && a.monitorOverrides == b.monitorOverrides;
  }

  bool barSurfaceOrderRequiresRecreate(const std::vector<BarConfig>& previous, const std::vector<BarConfig>& next) {
    std::vector<std::string> preserved;
    preserved.reserve(previous.size());
    for (const auto& oldBar : previous) {
      const auto it = std::ranges::find(next, oldBar.name, &BarConfig::name);
      if (it != next.end()) {
        preserved.push_back(oldBar.name);
      }
    }

    if (preserved.size() > next.size()) {
      return true;
    }
    for (std::size_t i = 0; i < preserved.size(); ++i) {
      if (next[i].name != preserved[i]) {
        return true;
      }
    }
    return false;
  }

  BarVisualGeometry computeBarVisualGeometry(
      const BarConfig& cfg, const ShellConfig::ShadowConfig& shadow, float surfaceWidth, float surfaceHeight
  ) {
    (void)shadow;
    // A visible rail replaces the frame on its own edge. Insetting it by the
    // frame leaves a wallpaper gutter and makes it read as a floating pill.
    const auto barThickness = static_cast<float>(cfg.thickness);
    const bool isBottom = cfg.position == "bottom";
    const bool isRight = cfg.position == "right";
    const bool isVertical = (cfg.position == "left" || cfg.position == "right");

    if (isVertical) {
      return {
          .x = isRight ? std::max(0.0f, surfaceWidth - barThickness) : 0.0f,
          .y = 0.0f,
          .width = std::min(barThickness, std::max(0.0f, surfaceWidth)),
          .height = std::max(0.0f, surfaceHeight),
      };
    }

    return {
        .x = 0.0f,
        .y = isBottom ? std::max(0.0f, surfaceHeight - barThickness) : 0.0f,
        .width = std::max(0.0f, surfaceWidth),
        .height = std::min(barThickness, std::max(0.0f, surfaceHeight)),
    };
  }

  [[nodiscard]] float currentBarThickness(const BarInstance& instance, const ShellConfig& shell) noexcept {
    const float collapsed = std::max(1.0f, shell.chrome.frameThickness);
    const float expanded = std::max(collapsed, static_cast<float>(instance.barConfig.thickness));
    return std::lerp(collapsed, expanded, std::clamp(instance.hideOpacity, 0.0f, 1.0f));
  }

  [[nodiscard]] BarVisualGeometry currentBarVisualGeometry(
      const BarInstance& instance, const ShellConfig& shell, float surfaceWidth, float surfaceHeight
  ) noexcept {
    BarConfig sampled = instance.barConfig;
    sampled.thickness = static_cast<std::int32_t>(std::lround(currentBarThickness(instance, shell)));
    return computeBarVisualGeometry(sampled, shell.shadow, surfaceWidth, surfaceHeight);
  }

  void applyBarShadowStyle(
      BarInstance& instance, const ShellConfig& shell, float surfaceWidth, float surfaceHeight
  ) {
    (void)shell;
    (void)surfaceWidth;
    (void)surfaceHeight;
    // Bar, attached panels and toasts are one opaque chrome silhouette. A
    // second legacy Box shadow produces a dark seam exactly at their joins.
    if (instance.shadow != nullptr) {
      instance.shadow->setHitTestVisible(false);
      instance.shadow->setVisible(false);
    }

    if (instance.shadowLeftClip != nullptr) {
      instance.shadowLeftClip->setVisible(false);
    }
    if (instance.shadowRightClip != nullptr) {
      instance.shadowRightClip->setVisible(false);
    }
  }

  void applyAttachedPanelBackdrop(BarInstance& instance) {
    if (instance.chromeHost == nullptr) {
      return;
    }
    instance.chromeHost->setPanelState(instance.chromePanelState);
    instance.chromeHost->setToastStates(instance.chromeToastStates);
  }

  void layoutBarSections(
      BarInstance& instance, Renderer& renderer, float barAreaW, float barAreaH, float padding, bool isVertical
  ) {
    const float slotCross = isVertical ? barAreaW : barAreaH;

    auto layoutWidgets = [&](std::vector<std::unique_ptr<Widget>>& widgets) {
      for (auto& widget : widgets) {
        if (widget->root() != nullptr) {
          widget->layout(renderer, barAreaW, barAreaH);
        }
      }
    };
    layoutWidgets(instance.startWidgets);
    layoutWidgets(instance.centerWidgets);
    layoutWidgets(instance.endWidgets);

    // Capsule cross-size is a fraction of the bar thickness (capsule_thickness), the same for every capsule
    // regardless of per-widget content scale. The max() guard keeps a thin bar from yielding a 0px capsule.
    const float capsuleCross = std::max(1.0f, std::round(slotCross * instance.barConfig.capsuleThickness));
    auto finalizeCapsules = [isVertical, capsuleCross, &renderer](std::vector<BarCapsuleRun>& runs) {
      for (auto& run : runs) {
        Node* shell = run.shell;
        Box* bg = run.bg;
        Node* content = run.content;
        if (shell == nullptr || bg == nullptr || content == nullptr) {
          continue;
        }
        if (run.container != nullptr) {
          run.container->layout(renderer);
        }

        bool hasVisibleContent = false;
        bool hasVisibleInk = false;
        for (Widget* widget : run.widgets) {
          if (widget == nullptr || widget->root() == nullptr) {
            continue;
          }
          hasVisibleContent = hasVisibleContent || widget->root()->visible();
          hasVisibleInk = hasVisibleInk || widget->shouldShowBarCapsule();
        }

        shell->setVisible(hasVisibleContent);
        const float scale = run.contentScale;
        const float iw = content->width();
        const float ih = content->height();
        if (!hasVisibleInk) {
          shell->setSize(iw, ih);
          content->setPosition(0.0f, 0.0f);
          bg->setVisible(false);
          bg->setPosition(0.0f, 0.0f);
          bg->setSize(iw, ih);
          placeCapsuleHoverBoxes(run, isVertical, iw, ih, 0.0f, 0.0f, std::min(iw, ih) * 0.5f);
          continue;
        }
        const float pad = run.spec.padding * scale;
        const float padMain = pad;
        // Cross-size is the fixed capsuleCross, independent of per-widget content scale: scaling a widget
        // enlarges its glyph/text inside the fixed-height pill rather than resizing the capsule (so a
        // differently scaled member can't grow or split its capsule group). The main axis is content plus
        // per-widget padding, so an icon-only widget reads as a near-circular pill at the default padding
        // and widens as padding increases.
        const float shellMain = (isVertical ? ih : iw) + 2.0f * padMain;
        const float shellCross = capsuleCross;
        const float shellW = isVertical ? shellCross : shellMain;
        const float shellH = isVertical ? shellMain : shellCross;
        const float contentX = (shellW - iw) * 0.5f;
        const float contentY = (shellH - ih) * 0.5f;
        shell->setSize(shellW, shellH);
        bg->setVisible(true);
        bg->setPosition(0.0f, 0.0f);
        bg->setSize(shellW, shellH);
        content->setPosition(contentX, contentY);
        const Widget* radiusSource = !run.widgets.empty() ? run.widgets.front() : nullptr;
        const float capsuleRadius = radiusSource != nullptr ? radiusSource->resolvedBarCapsuleRadius(shellW, shellH)
                                                            : std::max(0.0f, std::min(shellW, shellH) * 0.5f);
        bg->setRadius(capsuleRadius);
        placeCapsuleHoverBoxes(run, isVertical, shellW, shellH, contentX, contentY, capsuleRadius);
      }
    };
    finalizeCapsules(instance.startCapsuleRuns);
    finalizeCapsules(instance.centerCapsuleRuns);
    finalizeCapsules(instance.endCapsuleRuns);

    // When bar touches screen edge, put the padding inside the sections, and extend the hit targets of
    // the first/last widgets to cover the area. So clicking on the screen edge still triggers the widget.
    const bool screenEdgeClick = padding > 0;
    const float paddingInsideSection = screenEdgeClick ? padding : 0.0f;
    const float contentMainStart = screenEdgeClick ? 0.0f : padding;
    const float contentMainEnd =
        std::max(contentMainStart, (isVertical ? barAreaH : barAreaW) - (screenEdgeClick ? 0.0f : padding));
    const float contentMainSpan = std::max(0.0f, contentMainEnd - contentMainStart);

    auto configureSlot = [&](Node* slot, float mainOffset, float mainSize) {
      slot->setClipChildren(true);
      if (isVertical) {
        slot->setPosition(0.0f, mainOffset);
        slot->setSize(slotCross, mainSize);
      } else {
        slot->setPosition(mainOffset, 0.0f);
        slot->setSize(mainSize, slotCross);
      }
    };

    auto configureSection = [&](Flex* section, FlexJustify justify) {
      section->setJustify(justify);
      section->layout(renderer);
    };

    if (screenEdgeClick) {
      if (isVertical) {
        instance.startSection->setPadding(paddingInsideSection, 0.0f, 0.0f, 0.0f);
        instance.endSection->setPadding(0.0f, 0.0f, paddingInsideSection, 0.0f);
      } else {
        instance.startSection->setPadding(0.0f, 0.0f, 0.0f, paddingInsideSection);
        instance.endSection->setPadding(0.0f, paddingInsideSection, 0.0f, 0.0f);
      }
    } else {
      instance.startSection->setPadding(0.0f);
      instance.endSection->setPadding(0.0f);
    }

    configureSection(instance.startSection, FlexJustify::Start);
    configureSection(instance.centerSection, FlexJustify::Center);
    configureSection(instance.endSection, FlexJustify::End);

    // Anchor mode: if a center widget is flagged as the anchor, pin its center to the
    // bar midline so surrounding siblings growing/shrinking cannot drift it sideways.
    const Node* anchorNode = nullptr;
    for (const auto& widget : instance.centerWidgets) {
      if (widget != nullptr && widget->isAnchor() && widget->layoutBoundsNode() != nullptr) {
        anchorNode = widget->layoutBoundsNode();
        break;
      }
    }

    const float barMidline = contentMainStart + contentMainSpan * 0.5f;
    const float centerNaturalMain = isVertical ? instance.centerSection->height() : instance.centerSection->width();

    float centerSlotStart;
    float centerSlotMain;
    float centerSectionOffset; // offset of section origin within its slot along main axis
    if (anchorNode != nullptr) {
      const float anchorOffsetInSection = isVertical ? anchorNode->y() : anchorNode->x();
      const float anchorSpan = isVertical ? anchorNode->height() : anchorNode->width();
      const float anchorCenterInSection = anchorOffsetInSection + anchorSpan * 0.5f;
      // Place the section so that the anchor's center sits at barMidline.
      float desiredSectionStart = barMidline - anchorCenterInSection;
      // Clamp so the section stays within the content area.
      const float maxStart = contentMainEnd - centerNaturalMain;
      desiredSectionStart = std::clamp(desiredSectionStart, contentMainStart, std::max(contentMainStart, maxStart));
      centerSlotStart = desiredSectionStart;
      centerSlotMain = std::min(centerNaturalMain, contentMainEnd - centerSlotStart);
      centerSectionOffset = 0.0f;
    } else {
      centerSlotMain = std::min(contentMainSpan, centerNaturalMain);
      centerSlotStart = contentMainStart + std::max(0.0f, (contentMainSpan - centerSlotMain) * 0.5f);
      centerSectionOffset = (centerSlotMain - centerNaturalMain) * 0.5f;
    }
    const float centerSlotEnd = centerSlotStart + centerSlotMain;
    float startSlotMain;
    float endSlotMain;
    if (!instance.centerWidgets.empty()) {
      startSlotMain = std::max(0.0f, centerSlotStart - contentMainStart);
      endSlotMain = std::max(0.0f, contentMainEnd - centerSlotEnd);
      configureSlot(instance.startSlot, contentMainStart, startSlotMain);
      configureSlot(instance.centerSlot, centerSlotStart, centerSlotMain);
      configureSlot(instance.endSlot, centerSlotEnd, endSlotMain);
    } else {
      // Allow start/end sections to take the full width if center is empty
      const float startNaturalMain = isVertical ? instance.startSection->height() : instance.startSection->width();
      const float endNaturalMain = isVertical ? instance.endSection->height() : instance.endSection->width();

      // Prioritize end section, because control center is likely to be there, and we don't
      // want it to be clipped by a super long start section, so the user loses access to settings.
      endSlotMain = std::min(endNaturalMain, contentMainSpan);
      startSlotMain = std::min(startNaturalMain, contentMainSpan - endSlotMain);
      configureSlot(instance.startSlot, contentMainStart, startSlotMain);
      configureSlot(instance.centerSlot, contentMainStart + startSlotMain, 0.0f);
      configureSlot(instance.endSlot, contentMainEnd - endSlotMain, endSlotMain);
    }

    if (isVertical) {
      instance.startSection->setPosition((slotCross - instance.startSection->width()) * 0.5f, 0.0f);
      instance.centerSection->setPosition((slotCross - instance.centerSection->width()) * 0.5f, centerSectionOffset);
      instance.endSection->setPosition(
          (slotCross - instance.endSection->width()) * 0.5f, endSlotMain - instance.endSection->height()
      );
    } else {
      instance.startSection->setPosition(0.0f, (slotCross - instance.startSection->height()) * 0.5f);
      instance.centerSection->setPosition(centerSectionOffset, (slotCross - instance.centerSection->height()) * 0.5f);
      instance.endSection->setPosition(
          endSlotMain - instance.endSection->width(), (slotCross - instance.endSection->height()) * 0.5f
      );
    }

    applyBarWidgetHitTargets(instance.startSection, instance.startSlot, isVertical);
    applyBarWidgetHitTargets(instance.centerSection, instance.centerSlot, isVertical);
    applyBarWidgetHitTargets(instance.endSection, instance.endSlot, isVertical);
    extendCapsuleHitTargets(instance.startCapsuleRuns, isVertical);
    extendCapsuleHitTargets(instance.centerCapsuleRuns, isVertical);
    extendCapsuleHitTargets(instance.endCapsuleRuns, isVertical);

    // Ghost pills for capsule-less widgets: positioned on the bar-level underlay with the
    // metrics an enabled capsule would have (capsuleCross across the bar, capsule padding
    // along it). Runs after sections are positioned so absolute coordinates are final; the
    // widget's hit target is widened to match the pill.
    if (instance.hoverUnderlay != nullptr) {
      float underlayX = 0.0f;
      float underlayY = 0.0f;
      Node::absolutePosition(instance.hoverUnderlay, underlayX, underlayY);
      auto placeGhostPills = [&](std::vector<std::unique_ptr<Widget>>& widgets) {
        for (auto& widget : widgets) {
          Box* box = widget->barHoverBox();
          Node* root = widget->root();
          if (box == nullptr || root == nullptr || widget->barCapsuleShell() != nullptr) {
            continue;
          }
          float rootX = 0.0f;
          float rootY = 0.0f;
          Node::absolutePosition(root, rootX, rootY);
          rootX -= underlayX;
          rootY -= underlayY;
          const float mainExtent = isVertical ? root->height() : root->width();
          const float pad = mainExtent > 0.5f ? widget->barCapsuleSpec().padding * widget->contentScale() : 0.0f;
          const float hoverW = isVertical ? capsuleCross : root->width() + 2.0f * pad;
          const float hoverH = isVertical ? root->height() + 2.0f * pad : capsuleCross;
          box->setPosition(
              isVertical ? rootX + (root->width() - capsuleCross) * 0.5f : rootX - pad,
              isVertical ? rootY - pad : rootY + (root->height() - capsuleCross) * 0.5f
          );
          box->setSize(hoverW, hoverH);
          box->setRadius(widget->resolvedBarCapsuleRadius(hoverW, hoverH));
          if (auto* area = dynamic_cast<InputArea*>(root)) {
            auto outset = area->hitTestOutset();
            if (isVertical) {
              outset.top += pad;
              outset.bottom += pad;
            } else {
              outset.left += pad;
              outset.right += pad;
            }
            area->setHitTestOutset(outset);
          }
        }
      };
      placeGhostPills(instance.startWidgets);
      placeGhostPills(instance.centerWidgets);
      placeGhostPills(instance.endWidgets);
    }
    if (screenEdgeClick) {
      if (!instance.startSection->children().empty()) {
        auto node = instance.startSection->children().front().get();
        auto hitTestOutset = node->hitTestOutset();
        if (isVertical) {
          hitTestOutset.top += paddingInsideSection;
        } else {
          hitTestOutset.left += paddingInsideSection;
        }
        node->setHitTestOutset(hitTestOutset);
      }
      if (!instance.endSection->children().empty()) {
        auto node = instance.endSection->children().back().get();
        auto hitTestOutset = node->hitTestOutset();
        if (isVertical) {
          hitTestOutset.bottom += paddingInsideSection;
        } else {
          hitTestOutset.right += paddingInsideSection;
        }
        node->setHitTestOutset(hitTestOutset);
      }
    }
  }

  void tickWidgets(std::vector<std::unique_ptr<Widget>>& widgets, float deltaMs) {
    for (auto& widget : widgets) {
      if (widget != nullptr && widget->needsFrameTick()) {
        widget->onFrameTick(deltaMs);
      }
    }
  }

  bool widgetsNeedFrameTick(const std::vector<std::unique_ptr<Widget>>& widgets) {
    return std::ranges::any_of(widgets, [](const auto& widget) {
      return widget != nullptr && widget->needsFrameTick();
    });
  }

} // namespace

Bar::Bar() = default;

bool Bar::initialize(const BarServices& services) {
  m_platform = &services.platform;
  m_config = &services.config;
  m_notifications = services.notifications;
  m_tray = services.tray;
  m_audio = services.audio;
  m_easyEffects = services.easyEffects;
  m_upower = services.upower;
  m_sysmon = services.sysmon;
  m_powerProfiles = services.powerProfiles;
  m_network = services.network;
  m_idleInhibitor = services.idleInhibitor;
  m_mpris = services.mpris;
  m_audioSpectrum = services.audioSpectrum;
  m_httpClient = services.httpClient;
  m_weatherService = services.weather;
  m_renderContext = services.renderContext;
  m_nightLight = services.nightLight;
  m_themeService = services.theme;
  m_bluetooth = services.bluetooth;
  m_brightness = services.brightness;
  m_lockKeys = services.lockKeys;
  m_clipboard = services.clipboard;
  m_fileWatcher = services.fileWatcher;
  m_screenshots = services.screenshots;
  m_widgetFactory = std::make_unique<WidgetFactory>(services);

  m_lastBars = m_config->config().bars;
  m_lastWidgets = m_config->config().widgets;
  m_lastShadow = m_config->config().shell.shadow;
  m_lastChrome = m_config->config().shell.chrome;
  m_config->addReloadCallback(
      [this]() {
        const auto& cfg = m_config->config();
        if (cfg.bars == m_lastBars
            && cfg.widgets == m_lastWidgets
            && cfg.shell.shadow == m_lastShadow) {
          if (cfg.shell.chrome != m_lastChrome) {
            m_lastChrome = cfg.shell.chrome;
            refreshChromeLayout();
          }
          return;
        }
        reload();
      },
      "bar"
  );

  return true;
}

BarServices Bar::services() const {
  return {
      .platform = *m_platform,
      .config = *m_config,
      .notifications = m_notifications,
      .tray = m_tray,
      .audio = m_audio,
      .easyEffects = m_easyEffects,
      .upower = m_upower,
      .sysmon = m_sysmon,
      .powerProfiles = m_powerProfiles,
      .network = m_network,
      .idleInhibitor = m_idleInhibitor,
      .mpris = m_mpris,
      .audioSpectrum = m_audioSpectrum,
      .httpClient = m_httpClient,
      .weather = m_weatherService,
      .renderContext = m_renderContext,
      .nightLight = m_nightLight,
      .theme = m_themeService,
      .bluetooth = m_bluetooth,
      .brightness = m_brightness,
      .lockKeys = m_lockKeys,
      .clipboard = m_clipboard,
      .fileWatcher = m_fileWatcher,
      .screenshots = m_screenshots,
  };
}

void Bar::onSecondTick() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestUpdate();
    }
  }
}

void Bar::reload() {
  gnil::profiling::ScopedTimer t(kLog, "bar: reload (all instances)");
  kLog.info("reloading config");
  const auto previousBars = m_lastBars;
  const auto previousShadow = m_lastShadow;
  const bool recreateForOrder = barSurfaceOrderRequiresRecreate(previousBars, m_config->config().bars);
  m_lastBars = m_config->config().bars;
  m_lastWidgets = m_config->config().widgets;
  m_lastShadow = m_config->config().shell.shadow;
  m_lastChrome = m_config->config().shell.chrome;
  m_widgetFactory = std::make_unique<WidgetFactory>(services());

  if (recreateForOrder) {
    kLog.info("bar order changed; recreating layer-shell surfaces");
    closeAllInstances();
    if (wl_display_roundtrip(m_platform->display()) < 0) {
      const int roundtripErrno = errno;
      kLog.error(
          "Wayland roundtrip failed after destroying bar surfaces for order change: {}",
          m_platform->wayland().describeDisplayError(roundtripErrno)
      );
    }
    syncInstances();
    return;
  }

  // Look up new bar configs by name.
  std::unordered_map<std::string, std::pair<const BarConfig*, std::size_t>> newBarsByName;
  newBarsByName.reserve(m_lastBars.size());
  for (std::size_t i = 0; i < m_lastBars.size(); ++i) {
    newBarsByName[m_lastBars[i].name] = {&m_lastBars[i], i};
  }

  // Exclusive-zone geometry on an output depends on the order its bar surfaces are
  // created: bars on the same edge stack in creation order, and bars on adjacent
  // edges (e.g. top + left) compete for the shared corner the same way. Rebuilding
  // one bar's surface in place while recreating another would commit them out of
  // config order and reshuffle that geometry. So if any bar on an output needs a
  // surface recreate, recreate every bar on that output — syncInstances rebuilds
  // them in config order. Scoped per output so other monitors are untouched.
  const auto needsSurfaceRecreate = [&](const BarInstance& inst) -> bool {
    auto it = newBarsByName.find(inst.barConfig.name);
    if (it == newBarsByName.end()) {
      return true;
    }
    const auto& outputs = m_platform->outputs();
    auto outIt = std::ranges::find(outputs, inst.outputName, &WaylandOutput::name);
    if (outIt == outputs.end()) {
      return true;
    }
    auto resolved = ConfigService::resolveForOutput(*it->second.first, *outIt);
    if (!resolved.enabled) {
      return true;
    }
    return !barConfigSurfaceFieldsEqual(inst.barConfig, resolved, previousShadow, m_lastShadow);
  };
  std::unordered_set<std::uint32_t> outputsNeedingRecreate;
  for (const auto& instUp : m_instances) {
    if (needsSurfaceRecreate(*instUp)) {
      outputsNeedingRecreate.insert(instUp->outputName);
    }
  }

  // For each existing instance, decide whether to rebuild contents in place
  // (surface preserved → no exclusive-zone churn) or destroy (will be recreated
  // by syncInstances below). Any bar on an output flagged above is destroyed so the
  // whole output is rebuilt in config order.
  bool destroyedAny = false;
  std::erase_if(m_instances, [&](const std::unique_ptr<BarInstance>& instUp) {
    auto& inst = *instUp;
    auto it = newBarsByName.find(inst.barConfig.name);
    auto destroy = [&]() {
      if (inst.surface != nullptr) {
        m_surfaceMap.erase(inst.surface->wlSurface());
      }
      if (m_hoveredInstance == &inst) {
        m_hoveredInstance = nullptr;
      }
      destroyedAny = true;
      return true;
    };
    if (outputsNeedingRecreate.contains(inst.outputName)) {
      return destroy();
    }
    if (it == newBarsByName.end()) {
      return destroy();
    }

    const auto& outputs = m_platform->outputs();
    auto outIt = std::ranges::find(outputs, inst.outputName, &WaylandOutput::name);
    if (outIt == outputs.end()) {
      return destroy();
    }

    auto resolved = ConfigService::resolveForOutput(*it->second.first, *outIt);
    if (!resolved.enabled) {
      return destroy();
    }

    inst.barIndex = it->second.second;
    rebuildInstanceContents(inst, resolved);
    return false;
  });

  if (destroyedAny) {
    // Drain pending Wayland events for the just-destroyed surfaces before
    // creating new ones. Without this, the roundtrip inside LayerSurface::initialize
    // reads stale closures for dead proxies, which libwayland drops without freeing.
    if (wl_display_roundtrip(m_platform->display()) < 0) {
      const int roundtripErrno = errno;
      kLog.error(
          "Wayland roundtrip failed after destroying stale bar surfaces: {}",
          m_platform->wayland().describeDisplayError(roundtripErrno)
      );
    }
  }

  syncInstances();
}

void Bar::refreshChromeLayout() {
  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->surface == nullptr) {
      continue;
    }
    buildScene(*instance, instance->surface->width(), instance->surface->height());
    instance->surface->requestRedraw();
  }
}

void Bar::closeAllInstances() {
  m_surfaceMap.clear();
  m_hoveredInstance = nullptr;
  m_instances.clear();
}

void Bar::onOutputChange() { syncInstances(); }

void Bar::onWorkspaceChanged() {
  if (m_platform == nullptr || m_overlayDisplaySuppressed) {
    return;
  }

  bool anyChanged = false;
  for (const auto& output : m_platform->outputs()) {
    const std::string activeId = activeWorkspaceId(m_platform->workspaces(output.output));
    if (activeId.empty()) {
      continue;
    }

    auto& last = m_lastActiveWorkspaceByOutput[output.name];
    if (!last.empty() && last != activeId) {
      m_pendingWorkspaceRevealOutputs.insert(output.name);
      anyChanged = true;
    }
    last = activeId;
  }

  if (anyChanged) {
    m_workspaceRevealDebounce.start(kWorkspaceRevealDebounce, [this]() { applyPendingWorkspaceReveal(); });
  }
  scheduleSmartAutoHideReevaluation();
}

void Bar::scheduleSmartAutoHideReevaluation() {
  if (m_smartAutoHideReevalQueued) {
    return;
  }
  m_smartAutoHideReevalQueued = true;
  DeferredCall::callLater([this]() {
    m_smartAutoHideReevalQueued = false;
    reevaluateSmartAutoHide();
  });
}

void Bar::reevaluateSmartAutoHide() {
  if (m_platform == nullptr || m_overlayDisplaySuppressed) {
    return;
  }

  for (const auto& instanceUp : m_instances) {
    BarInstance* instance = instanceUp.get();
    if (instance == nullptr
        || !instance->barConfig.enabled
        || !instance->barConfig.smartAutoHide
        || instance->surface == nullptr) {
      continue;
    }

    const bool wantsPinned = smartAutoHideWantsPinnedVisible(*m_platform, instance->output);
    const bool pinnedChanged = wantsPinned != instance->smartAutoHidePinnedVisible;
    instance->smartAutoHidePinnedVisible = wantsPinned;

    const bool suppressAutoHide =
        (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;

    bool needsRedraw = pinnedChanged;
    if (wantsPinned) {
      if (instance->hideOpacity < 1.0f || pinnedChanged) {
        revealAutoHideBar(*instance);
        syncBarAutoHideInputRegion(*instance);
        syncBarSurfaceChrome(*instance);
        needsRedraw = true;
      }
    } else if (!instance->pointerInside && instance->attachedPopupCount == 0 && !suppressAutoHide) {
      if (instance->hideOpacity > 0.0f || pinnedChanged) {
        startHideFadeOut(*instance);
        needsRedraw = true;
      }
    }

    if (needsRedraw && instance->surface != nullptr) {
      instance->surface->requestRedraw();
    }
  }
}

void Bar::applyPendingWorkspaceReveal() {
  if (m_platform == nullptr) {
    return;
  }

  const auto pendingOutputs = std::move(m_pendingWorkspaceRevealOutputs);
  m_pendingWorkspaceRevealOutputs.clear();

  std::vector<BarInstance*> peeked;
  peeked.reserve(m_instances.size());
  for (const std::uint32_t outputName : pendingOutputs) {
    for (const auto& instanceUp : m_instances) {
      auto* instance = instanceUp.get();
      if (instance == nullptr
          || instance->outputName != outputName
          || !instance->barConfig.enabled
          || !instance->barConfig.autoHide
          || instance->barConfig.smartAutoHide
          || !instance->barConfig.showOnWorkspaceSwitch
          || instance->surface == nullptr) {
        continue;
      }

      revealAutoHideBar(*instance);
      if (instance->pointerInside) {
        continue;
      }
      const bool suppressAutoHide =
          (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;
      if (!suppressAutoHide) {
        peeked.push_back(instance);
      }
    }
  }

  if (peeked.empty()) {
    return;
  }

  m_workspacePeekHideTimer.start(kWorkspacePeekHold, [this, peeked = std::move(peeked)]() {
    for (BarInstance* instance : peeked) {
      if (instance == nullptr || !instance->barConfig.autoHide || instance->pointerInside) {
        continue;
      }
      const bool suppressAutoHide =
          (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;
      if (!suppressAutoHide) {
        startHideFadeOut(*instance);
      }
    }
  });
}

void Bar::refresh() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestUpdate();
      if (inst->animations.hasActive() || instanceNeedsFrameTick(*inst)) {
        inst->surface->requestRedraw();
      }
    }
  }
}

void Bar::requestRedraw() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestRedraw();
    }
  }
}

void Bar::requestLayout() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestLayout();
    }
  }
}

void Bar::setAutoHideSuppressionCallback(std::function<bool(const BarInstance&)> callback) {
  m_autoHideSuppressionCallback = std::move(callback);
}

void Bar::reevaluateAutoHide() {
  for (const auto& instance : m_instances) {
    if (instance == nullptr
        || !barPointerHideAllowed(*instance)
        || instance->pointerInside
        || instance->attachedPopupCount > 0) {
      continue;
    }
    const bool suppressAutoHide =
        (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;
    if (suppressAutoHide || instance->hideOpacity <= 0.001f) {
      continue;
    }
    startHideFadeOut(*instance);
  }
}

void Bar::setOpenWidgetSettingsCallback(std::function<void(std::string, std::string)> callback) {
  m_openWidgetSettingsCallback = std::move(callback);
}

bool Bar::isRunning() const noexcept {
  return std::ranges::any_of(m_instances, [](const auto& inst) { return inst->surface && inst->surface->isRunning(); });
}

bool Bar::instanceEffectivelyVisible(const BarInstance& instance) const noexcept {
  if (barSupportsSlideBehavior(instance.barConfig)) {
    return instance.hideOpacity > 0.5f;
  }
  return instance.slideRoot == nullptr || instance.hideOpacity > 0.5f;
}

bool Bar::instanceAcceptsPointerInput(const BarInstance& instance) const noexcept {
  (void)instance;
  // The collapsed frame edge remains a real interaction surface so a hidden
  // bar can still be revealed by the edge gesture.
  return true;
}

bool Bar::isVisible() const noexcept {
  return std::ranges::any_of(m_instances, [this](const auto& inst) { return instanceEffectivelyVisible(*inst); });
}

void Bar::clearInstancePointerState(BarInstance& instance) {
  instance.pointerInside = false;
  instance.inputDispatcher.pointerLeave();
  if (m_hoveredInstance == &instance) {
    m_hoveredInstance = nullptr;
  }
}

void Bar::setInstanceIpcVisible(BarInstance& instance, bool visible) {
  if (instance.surface == nullptr) {
    return;
  }
  if (barSupportsSlideBehavior(instance.barConfig)) {
    instance.ipcPinnedVisible = visible;
    if (visible) {
      revealAutoHideBar(instance);
    } else {
      startHideFadeOut(instance);
    }
    return;
  }
  if (instance.slideRoot == nullptr) {
    return;
  }
  instance.animations.cancelForOwner(instance.slideRoot);
  instance.slideRoot->setOpacity(1.0f);
  if (!visible) {
    clearInstancePointerState(instance);
  }
  const float current = instance.hideOpacity;
  const float target = visible ? 1.0f : 0.0f;
  instance.animations.animate(
      current, target, visible ? Style::animChromeSpatial : Style::animChromeExit,
      visible ? Easing::CaelestiaExpressiveSpatial : Easing::EaseInQuad,
      [inst = &instance, this](float v) {
        inst->hideOpacity = v;
        syncBarSlideLayerTransform(*inst);
        syncBarSurfaceChrome(*inst);
      },
      [inst = &instance, this]() {
        syncBarSlideLayerTransform(*inst);
        syncBarAutoHideInputRegion(*inst);
        syncBarSurfaceChrome(*inst);
        if (inst->surface != nullptr) {
          inst->surface->requestRedraw();
        }
      },
      instance.slideRoot
  );
  syncBarAutoHideInputRegion(instance);
  syncBarSurfaceChrome(instance);
  instance.surface->requestRedraw();
}

void Bar::applyIpcVisibility(bool visible) {
  for (const auto& instance : m_instances) {
    if (instance == nullptr) {
      continue;
    }
    setInstanceIpcVisible(*instance, visible);
    syncBarSurfaceChrome(*instance);
  }
  syncIdleInhibitorAnchors();
}

void Bar::syncIdleInhibitorAnchors() {
  if (m_idleInhibitor != nullptr) {
    m_idleInhibitor->resyncAnchorSurfaces();
  }
}

bool Bar::barContentVisuallyShown(const BarInstance& instance) const noexcept {
  constexpr float kShownThreshold = 0.02f;
  if (barSupportsSlideBehavior(instance.barConfig)) {
    return instance.hideOpacity > kShownThreshold;
  }
  return instance.slideRoot == nullptr || instance.hideOpacity > kShownThreshold;
}

bool Bar::shouldReserveExclusiveZone(const BarInstance& instance) const noexcept {
  // The frame always reserves its own thickness.  This return value means
  // "publish structural exclusions", not "reserve the expanded bar".
  return true;
}

void Bar::syncBarExclusiveZone(BarInstance& instance) {
  if (m_config == nullptr) {
    return;
  }
  const bool reserve = shouldReserveExclusiveZone(instance);
  const auto& chrome = m_config->config().shell.chrome;
  ChromeGeometryModel geometry(
      ChromeGeometrySettings{
          .frameThickness = reserve ? chrome.frameThickness : 0.0f,
          .rounding = chrome.rounding,
          .smoothing = chrome.smoothing,
          .deformScale = chrome.deformScale,
      }
  );
  const ChromeEdge edge = instance.barConfig.position == "top" ? ChromeEdge::Top
      : instance.barConfig.position == "right"                 ? ChromeEdge::Right
      : instance.barConfig.position == "bottom"                ? ChromeEdge::Bottom
                                                               : ChromeEdge::Left;
  const float barZone = reserve
      ? static_cast<float>(reservedBarExclusiveZone(instance.barConfig, m_config->config().shell.shadow))
      : 0.0f;
  const bool reserveExpandedBar = reserve
      && (!instance.barConfig.autoHide || instance.ipcPinnedVisible || instance.smartAutoHidePinnedVisible)
      && !instance.ipcLayoutReleased;
  const auto zones = geometry.exclusiveZones(edge, barZone, reserveExpandedBar);
  for (std::size_t i = 0; i < instance.exclusionSurfaces.size(); ++i) {
    if (instance.exclusionSurfaces[i] != nullptr) {
      instance.exclusionSurfaces[i]->setExclusiveZone(zones[i]);
    }
  }
}

void Bar::syncBarSurfaceChrome(BarInstance& instance) {
  syncBarExclusiveZone(instance);
  if (instance.chromeHost != nullptr && m_config != nullptr) {
    const auto surfaceWidth = instance.surface != nullptr ? static_cast<std::int32_t>(instance.surface->width()) : 0;
    const auto surfaceHeight = instance.surface != nullptr ? static_cast<std::int32_t>(instance.surface->height()) : 0;
    const auto context = resolveChromeLayoutContext(
        instance.barConfig, m_config->config().shell, surfaceWidth, surfaceHeight,
        currentBarThickness(instance, m_config->config().shell)
    );
    instance.chromeHost->setGeometrySettings(context.geometry);
    instance.chromeHost->setColors(
        colorForRole(ColorRole::Surface), Color{}
    );
    instance.chromeHost->setShadow(0.0f, 0.0f, 0.0f);
    instance.chromeHost->setPanelState(instance.chromePanelState);
    instance.chromeHost->setToastStates(instance.chromeToastStates);
  }
  applyBarCompositorBlur(instance);
}

std::optional<LayerPopupParentContext> Bar::popupParentContextForSurface(wl_surface* surface) const noexcept {
  auto* instance = instanceForSurface(surface);
  if (instance == nullptr || instance->surface == nullptr) {
    return std::nullopt;
  }

  auto* layerSurface = instance->surface->layerSurface();
  const auto width = instance->surface->width();
  const auto height = instance->surface->height();
  if (layerSurface == nullptr || width == 0 || height == 0) {
    return std::nullopt;
  }

  return LayerPopupParentContext{
      .surface = instance->surface->wlSurface(),
      .layerSurface = layerSurface,
      .output = instance->output,
      .width = width,
      .height = height,
  };
}

std::optional<LayerPopupParentContext> Bar::preferredPopupParentContext(wl_output* output) const noexcept {
  BarInstance* instance = instanceForOutput(output);
  if (instance == nullptr && !m_instances.empty()) {
    instance = m_instances.front().get();
  }
  return instance != nullptr && instance->surface != nullptr
      ? popupParentContextForSurface(instance->surface->wlSurface())
      : std::nullopt;
}

std::vector<InputRect> Bar::surfaceRectsForOutput(wl_output* output) const {
  std::vector<InputRect> rects;
  if (m_platform == nullptr || output == nullptr) {
    return rects;
  }

  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->output != output || instance->surface == nullptr) {
      continue;
    }
    if (!instanceAcceptsPointerInput(*instance)) {
      continue;
    }
    const auto* surface = instance->surface.get();
    const float width = static_cast<float>(surface->width());
    const float height = static_cast<float>(surface->height());
    if (width <= 0.0f || height <= 0.0f || m_config == nullptr) {
      continue;
    }
    const auto& shadow = m_config->config().shell.shadow;
    const auto visual = computeBarVisualGeometry(instance->barConfig, shadow, width, height);
    if (visual.width > 0.0f && visual.height > 0.0f) {
      rects.push_back(
          InputRect{
              static_cast<int>(std::lround(visual.x)), static_cast<int>(std::lround(visual.y)),
              static_cast<int>(std::lround(visual.width)), static_cast<int>(std::lround(visual.height))
          }
      );
    }
  }

  return rects;
}

std::vector<wl_surface*> Bar::allBarSurfaces() const {
  std::vector<wl_surface*> surfaces;
  surfaces.reserve(m_instances.size());
  for (const auto& instance : m_instances) {
    if (instance != nullptr && instance->surface != nullptr && instanceAcceptsPointerInput(*instance)) {
      if (wl_surface* s = instance->surface->wlSurface(); s != nullptr) {
        surfaces.push_back(s);
      }
    }
  }
  return surfaces;
}

std::vector<wl_surface*> Bar::caffeineAnchorSurfaces() const {
  std::vector<wl_surface*> surfaces;
  surfaces.reserve(m_instances.size());
  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->surface == nullptr || !instanceAcceptsPointerInput(*instance)) {
      continue;
    }
    if (!barContentVisuallyShown(*instance)) {
      continue;
    }
    if (wl_surface* s = instance->surface->wlSurface(); s != nullptr) {
      surfaces.push_back(s);
    }
  }
  return surfaces;
}

bool Bar::canAttachPanelToBar(wl_output* output, std::string_view barName) const noexcept {
  const BarInstance* instance = instanceForBar(output, barName);
  if (instance == nullptr || instance->surface == nullptr || !instance->barConfig.enabled) {
    return false;
  }
  return barSupportsSlideBehavior(instance->barConfig) || instanceEffectivelyVisible(*instance);
}

std::optional<std::string> Bar::layerForBar(wl_output* output, std::string_view barName) const noexcept {
  const BarInstance* instance = instanceForBar(output, barName);
  if (instance == nullptr || instance->surface == nullptr || !instance->barConfig.enabled) {
    return std::nullopt;
  }
  return instance->barConfig.layer;
}

LayerShellLayer Bar::highestLayerForOutput(wl_output* output) const noexcept {
  LayerShellLayer highest = LayerShellLayer::Top;
  for (const auto& instance : m_instances) {
    if (instance->output != output || !instance->barConfig.enabled) {
      continue;
    }
    const LayerShellLayer layer = layerShellLayerFromConfig(instance->barConfig.layer);
    if (static_cast<std::uint32_t>(layer) > static_cast<std::uint32_t>(highest)) {
      highest = layer;
    }
  }
  return highest;
}

bool Bar::isAttachedPanelBarSettled(wl_output* output, std::string_view barName) const noexcept {
  const BarInstance* instance = instanceForBar(output, barName);
  if (instance == nullptr || !barSupportsSlideBehavior(instance->barConfig)) {
    return true;
  }
  constexpr float kSettledThreshold = 0.999f;
  return instance->hideOpacity >= kSettledThreshold;
}

void Bar::revealAutoHideForAttachedPanel(wl_output* output, std::string_view barName) {
  BarInstance* instance = instanceForBar(output, barName);
  if (instance != nullptr) {
    revealAutoHideBar(*instance);
  }
}

void Bar::setChromePanelState(wl_output* output, std::string_view barName, std::optional<ChromePanelState> state) {
  BarInstance* instance = instanceForBar(output, barName);
  if (instance == nullptr) {
    return;
  }

  if (!state.has_value() || !state->inputEnabled) {
    if (instance->activePanelWidget != nullptr) {
      instance->activePanelWidget->setPanelActive(false);
      instance->activePanelWidget = nullptr;
    }
  }
  instance->chromePanelState = state;
  applyAttachedPanelBackdrop(*instance);
  if (instance->surface != nullptr && instance->surface->width() > 0 && instance->surface->height() > 0) {
    applyBarShadowStyle(
        *instance, m_config->config().shell, static_cast<float>(instance->surface->width()),
        static_cast<float>(instance->surface->height())
    );
    instance->surface->requestRedraw();
  }
}

bool Bar::setChromeToastStates(wl_output* output, std::vector<ChromePanelState> states) {
  bool hosted = false;
  for (auto& instance : m_instances) {
    if (instance == nullptr || instance->output != output || instance->chromeHost == nullptr) {
      continue;
    }
    instance->chromeToastStates = states;
    instance->chromeHost->setToastStates(instance->chromeToastStates);
    if (instance->surface != nullptr) {
      instance->surface->requestRedraw();
    }
    hosted = true;
  }
  return hosted;
}

void Bar::setActivePanelWidget(BarInstance* instance, Widget* widget) {
  clearPanelActiveWidget();
  if (instance == nullptr || widget == nullptr) {
    return;
  }
  instance->activePanelWidget = widget;
  widget->setPanelActive(true);
}

void Bar::clearPanelActiveWidget() {
  for (auto& instance : m_instances) {
    if (instance == nullptr || instance->activePanelWidget == nullptr) {
      continue;
    }
    instance->activePanelWidget->setPanelActive(false);
    instance->activePanelWidget = nullptr;
  }
}

void Bar::beginAttachedPopup(wl_surface* surface) {
  auto* instance = instanceForSurface(surface);
  if (instance == nullptr) {
    return;
  }
  ++instance->attachedPopupCount;
}

void Bar::endAttachedPopup(wl_surface* surface) {
  auto* instance = instanceForSurface(surface);
  if (instance == nullptr) {
    return;
  }
  if (instance->attachedPopupCount > 0) {
    --instance->attachedPopupCount;
  }
  if (instance->attachedPopupCount > 0) {
    return;
  }
  instance->pointerInside =
      m_platform != nullptr && m_platform->hasPointerPosition() && m_platform->lastPointerSurface() == surface;
  if (instance->pointerInside) {
    instance->lastPointerSx = static_cast<float>(m_platform->lastPointerX());
    instance->lastPointerSy = static_cast<float>(m_platform->lastPointerY());
    instance->inputDispatcher.pointerEnter(
        instance->lastPointerSx, instance->lastPointerSy, m_platform->lastInputSerial()
    );
  } else {
    instance->inputDispatcher.pointerLeave();
  }
  if (instance->surface != nullptr) {
    instance->surface->requestRedraw();
  }
  if (!instance->pointerInside && m_hoveredInstance == instance) {
    m_hoveredInstance = nullptr;
  } else if (instance->pointerInside) {
    m_hoveredInstance = instance;
  }
  if (!barPointerHideAllowed(*instance) || instance->pointerInside) {
    return;
  }
  const bool suppressAutoHide =
      (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;
  if (!suppressAutoHide) {
    startHideFadeOut(*instance);
  }
}

void Bar::show() {
  for (const auto& instance : m_instances) {
    if (instance != nullptr) {
      instance->ipcLayoutReleased = false;
    }
  }
  applyIpcVisibility(true);
}

void Bar::hide() {
  for (const auto& instance : m_instances) {
    if (instance != nullptr && !barSupportsSlideBehavior(instance->barConfig)) {
      // bar-hide IPC always frees layout on non-autohide bars (v4 isVisible=false), regardless of reserve_space.
      instance->ipcLayoutReleased = true;
    }
  }
  applyIpcVisibility(false);
}

void Bar::suppressDisplay() {
  if (m_overlayDisplaySuppressed) {
    return;
  }
  m_overlayDisplaySuppressed = true;
  m_wasVisibleBeforeOverlaySuppress = isVisible();
  hide();
}

void Bar::unsuppressDisplay() {
  if (!m_overlayDisplaySuppressed) {
    return;
  }
  m_overlayDisplaySuppressed = false;
  if (m_wasVisibleBeforeOverlaySuppress) {
    show();
  }
}

void Bar::toggle() {
  const bool anyEffectivelyVisible = std::ranges::any_of(m_instances, [this](const auto& inst) {
    return inst != nullptr && instanceEffectivelyVisible(*inst);
  });

  if (anyEffectivelyVisible) {
    for (const auto& instance : m_instances) {
      if (instance != nullptr && !barSupportsSlideBehavior(instance->barConfig)) {
        instance->ipcLayoutReleased = true;
      }
    }
    applyIpcVisibility(false);
    return;
  }

  for (const auto& instance : m_instances) {
    if (instance != nullptr) {
      instance->ipcLayoutReleased = false;
    }
  }
  applyIpcVisibility(true);
}

void Bar::syncInstances() {
  const auto& outputs = m_platform->outputs();
  const auto& bars = m_config->config().bars;

  std::erase_if(m_lastActiveWorkspaceByOutput, [&outputs](const auto& pair) {
    return std::ranges::find(outputs, pair.first, &WaylandOutput::name) == outputs.end();
  });

  for (const auto& output : outputs) {
    if (!output.done || !output.hasUsableGeometry()) {
      continue;
    }
    auto& last = m_lastActiveWorkspaceByOutput[output.name];
    if (last.empty()) {
      last = activeWorkspaceId(m_platform->workspaces(output.output));
    }
  }

  // Remove instances for outputs that no longer exist
  std::erase_if(m_instances, [&outputs, this](const auto& inst) {
    const auto it = std::ranges::find(outputs, inst->outputName, &WaylandOutput::name);
    const bool found = it != outputs.end() && it->done && it->hasUsableGeometry();
    if (!found) {
      kLog.info("removing instance for output {}", inst->outputName);
    }
    if (!found) {
      if (inst->surface != nullptr) {
        m_surfaceMap.erase(inst->surface->wlSurface());
      }
      if (m_hoveredInstance == inst.get()) {
        m_hoveredInstance = nullptr;
      }
    }
    return !found;
  });

  // Create instances for each bar definition × each output
  for (std::size_t barIdx = 0; barIdx < bars.size(); ++barIdx) {
    for (const auto& output : outputs) {
      if (!output.done || !output.hasUsableGeometry()) {
        continue;
      }

      bool exists = std::ranges::any_of(m_instances, [&output, barIdx](const auto& inst) {
        return inst->outputName == output.name && inst->barIndex == barIdx;
      });
      if (!exists) {
        auto resolved = ConfigService::resolveForOutput(bars[barIdx], output);
        if (!resolved.enabled) {
          continue;
        }
        createInstance(output, barIdx, resolved);
      }
    }
  }

  syncIdleInhibitorAnchors();
  reevaluateSmartAutoHide();
}

void Bar::createInstance(const WaylandOutput& output, std::size_t barIndex, const BarConfig& barConfig) {
  auto instance = std::make_unique<BarInstance>();
  instance->outputName = output.name;
  instance->output = output.output;
  instance->scale = output.scale;
  instance->barConfig = barConfig;
  instance->barIndex = barIndex;

  const auto surfaceSpec = computeBarSurfaceSpec(barConfig, m_config->config().shell.shadow);

  kLog.info(
      "creating unified chrome #{} \"{}\" on {} ({}), thickness={} exclusive_zone={}", barIndex, barConfig.name,
      output.connectorName, output.description, barConfig.thickness, surfaceSpec.exclusiveZone
  );

  auto surfaceConfig = LayerSurfaceConfig{
      .nameSpace = "gnil-chrome-" + barConfig.name,
      .layer = layerShellLayerFromConfig(barConfig.layer),
      .anchor = LayerShellAnchor::Top | LayerShellAnchor::Right | LayerShellAnchor::Bottom | LayerShellAnchor::Left,
      .width = 0,
      .height = 0,
      // The chrome is a full-output canvas. It must not be reconfigured into
      // the work area reserved by the four invisible exclusion surfaces below:
      // otherwise Niri shifts this very surface right/down by its own frame,
      // leaving an exposed wallpaper strip at the output edges.  -1 is the
      // layer-shell "ignore exclusive zones" mode (the same split Caelestia
      // uses for its visual frame and its work-area exclusions).
      .exclusiveZone = -1,
      .defaultWidth = 1920,
      .defaultHeight = 1080,
  };

  instance->surface = std::make_unique<LayerSurface>(m_platform->wayland(), std::move(surfaceConfig));
  instance->surface->setRenderContext(m_renderContext);

  auto* inst = instance.get();
  instance->surface->setConfigureCallback([this, inst](std::uint32_t width, std::uint32_t height) {
    buildScene(*inst, width, height);
  });
  instance->surface->setPrepareFrameCallback([this, inst](bool needsUpdate, bool needsLayout) {
    prepareFrame(*inst, needsUpdate, needsLayout);
  });
  instance->surface->setFrameTickCallback([inst](float deltaMs) {
    tickWidgets(inst->startWidgets, deltaMs);
    tickWidgets(inst->centerWidgets, deltaMs);
    tickWidgets(inst->endWidgets, deltaMs);
  });

  instance->surface->setAnimationManager(&instance->animations);
  populateWidgets(*instance);

  if (!instance->surface->initialize(output.output)) {
    kLog.warn("failed to initialize surface for output {}", output.name);
    return;
  }

  const std::int32_t frameZone = chromeFrameThickness(m_config->config().shell);
  // The main surface has completed its first configure at this point, so
  // hideOpacity already reflects persistent vs auto-hide startup. Creating the
  // exclusion with the final state avoids a stale full-width work-area inset
  // until the first pointer event.
  const ChromeEdge barEdge = barConfig.position == "top" ? ChromeEdge::Top
      : barConfig.position == "right"                    ? ChromeEdge::Right
      : barConfig.position == "bottom"                   ? ChromeEdge::Bottom
                                                         : ChromeEdge::Left;
  const ChromeGeometryModel chromeGeometry(
      ChromeGeometrySettings{
          .frameThickness = static_cast<float>(frameZone),
          .rounding = m_config->config().shell.chrome.rounding,
          .smoothing = m_config->config().shell.chrome.smoothing,
          .deformScale = m_config->config().shell.chrome.deformScale,
      }
  );
  const auto initialZones = chromeGeometry.exclusiveZones(
      barEdge, static_cast<float>(reservedBarExclusiveZone(barConfig, m_config->config().shell.shadow)),
      barContentVisuallyShown(*instance)
  );
  struct ExclusionSpec {
    std::string_view suffix;
    std::uint32_t anchor;
    std::uint32_t edge;
    std::uint32_t width;
    std::uint32_t height;
    std::int32_t zone;
  };
  const std::array<ExclusionSpec, 4> exclusionSpecs = {
      ExclusionSpec{"left", LayerShellAnchor::Left, LayerShellExclusiveEdge::Left, 1, 1, initialZones[0]},
      ExclusionSpec{"top", LayerShellAnchor::Top, LayerShellExclusiveEdge::Top, 1, 1, initialZones[1]},
      ExclusionSpec{"right", LayerShellAnchor::Right, LayerShellExclusiveEdge::Right, 1, 1, initialZones[2]},
      ExclusionSpec{"bottom", LayerShellAnchor::Bottom, LayerShellExclusiveEdge::Bottom, 1, 1, initialZones[3]},
  };
  for (std::size_t i = 0; i < exclusionSpecs.size(); ++i) {
    const auto& spec = exclusionSpecs[i];
    LayerSurfaceConfig exclusionConfig{
        .nameSpace = "gnil-chrome-exclusion-" + barConfig.name + "-" + std::string(spec.suffix),
        .layer = layerShellLayerFromConfig(barConfig.layer),
        .anchor = spec.anchor,
        .width = spec.width,
        .height = spec.height,
        .exclusiveZone = spec.zone,
        .exclusiveEdge = spec.edge,
        .keyboard = LayerShellKeyboard::None,
        .defaultWidth = spec.width == 0 ? 1920u : spec.width,
        .defaultHeight = spec.height == 0 ? 1080u : spec.height,
    };
    auto exclusion = std::make_unique<LayerSurface>(m_platform->wayland(), std::move(exclusionConfig));
    if (!exclusion->initialize(output.output)) {
      kLog.warn("failed to initialize {} chrome exclusion surface for output {}", spec.suffix, output.name);
      return;
    }
    exclusion->setClickThrough(true);
    instance->exclusionSurfaces[i] = std::move(exclusion);
  }

  m_surfaceMap[instance->surface->wlSurface()] = instance.get();
  m_instances.push_back(std::move(instance));
}

void Bar::destroyInstance(std::uint32_t outputName) {
  std::erase_if(m_instances, [outputName, this](const auto& inst) {
    if (inst->surface != nullptr) {
      m_surfaceMap.erase(inst->surface->wlSurface());
    }
    if (m_hoveredInstance == inst.get()) {
      m_hoveredInstance = nullptr;
    }
    return inst->outputName == outputName;
  });
}

void Bar::populateWidgets(BarInstance& instance) {
  const auto& widgetConfigs = m_config->config().widgets;
  const auto labelFontWeight = static_cast<FontWeight>(instance.barConfig.fontWeight);
  const std::string barFontFamily = (instance.barConfig.fontFamily && !instance.barConfig.fontFamily->empty())
      ? *instance.barConfig.fontFamily
      : m_config->config().shell.fontFamily;
  // Creates one widget for `name`. When `groupSpec` is set the widget is a member of a capsule group and
  // takes the group's capsule style + foreground; otherwise it resolves its own per-widget/bar capsule.
  auto createWidget = [&](const std::string& name, const WidgetBarCapsuleSpec* groupSpec,
                          const std::optional<ColorSpec>* groupForeground, std::vector<std::unique_ptr<Widget>>& dest) {
    const WidgetConfig* wcPtr = nullptr;
    if (auto it = widgetConfigs.find(name); it != widgetConfigs.end()) {
      wcPtr = &it->second;
    }
    const float contentScale = resolveWidgetContentScale(instance.barConfig.scale, wcPtr, "widget." + name + ".scale");
    auto widget = m_widgetFactory->create(
        name, instance.output, contentScale, instance.barConfig.position, instance.barConfig.name,
        static_cast<float>(instance.barConfig.widgetSpacing)
    );
    if (widget == nullptr) {
      return;
    }
    widget->setConfigName(name);
    if (wcPtr != nullptr) {
      widget->setAnchor(wcPtr->getBool("anchor", false));
      widget->setNonInteractive(!wcPtr->getBool("interactive", true));
      if (!wcPtr->getBool("enabled", true)) {
        return;
      }
    }
    widget->setBarCapsuleSpec(
        groupSpec != nullptr ? *groupSpec : resolveWidgetBarCapsuleSpec(instance.barConfig, wcPtr)
    );
    widget->setLabelFontWeight(
        wcPtr != nullptr ? parseWidgetLabelFontWeight(*wcPtr, labelFontWeight) : labelFontWeight
    );
    widget->setLabelFontFamily(wcPtr != nullptr ? parseWidgetLabelFontFamily(*wcPtr, barFontFamily) : barFontFamily);
    if (wcPtr != nullptr && wcPtr->hasSetting("color")) {
      widget->setWidgetForeground(wcPtr->getOptionalColorSpec("color", "widget." + name + ".color"));
    } else if (groupForeground != nullptr && groupForeground->has_value()) {
      widget->setWidgetForeground(*groupForeground);
    } else if (instance.barConfig.widgetColor.has_value()) {
      widget->setWidgetForeground(instance.barConfig.widgetColor);
    }
    if (wcPtr != nullptr && wcPtr->hasSetting("icon_color")) {
      widget->setWidgetIconColor(wcPtr->getOptionalColorSpec("icon_color", "widget." + name + ".icon_color"));
    } else if (groupForeground != nullptr && groupForeground->has_value()) {
      widget->setWidgetIconColor(*groupForeground);
    } else if (instance.barConfig.widgetIconColor.has_value()) {
      widget->setWidgetIconColor(instance.barConfig.widgetIconColor);
    }
    dest.push_back(std::move(widget));
  };

  // Expands a lane's entries: group tokens become contiguous member widgets sharing the group's capsule.
  auto createWidgets = [&](const std::vector<std::string>& names, std::vector<std::unique_ptr<Widget>>& dest) {
    for (const auto& name : names) {
      if (isCapsuleGroupToken(name)) {
        const BarCapsuleGroupStyle* group = findBarCapsuleGroupStyle(instance.barConfig, capsuleGroupTokenId(name));
        if (group == nullptr || !group->enabled) {
          continue;
        }
        const WidgetBarCapsuleSpec groupSpec = capsuleSpecFromGroup(instance.barConfig, *group);
        for (const auto& member : group->members) {
          createWidget(member, &groupSpec, &group->foreground, dest);
        }
        continue;
      }
      createWidget(name, nullptr, nullptr, dest);
    }
  };

  createWidgets(instance.barConfig.startWidgets, instance.startWidgets);
  createWidgets(instance.barConfig.centerWidgets, instance.centerWidgets);
  createWidgets(instance.barConfig.endWidgets, instance.endWidgets);

#ifndef NDEBUG
  // Prepend a red "debug" pill to the end section if running a debug build
  auto debugWidget = m_widgetFactory->create(
      "debug_indicator", instance.output, instance.barConfig.scale, instance.barConfig.position,
      instance.barConfig.name, static_cast<float>(instance.barConfig.widgetSpacing)
  );
  if (debugWidget != nullptr) {
    debugWidget->setConfigName("debug_indicator");
    debugWidget->setLabelFontWeight(labelFontWeight);
    debugWidget->setLabelFontFamily(barFontFamily);
    debugWidget->create();
    instance.endWidgets.insert(instance.endWidgets.begin(), std::move(debugWidget));
  }
#endif
}

void Bar::attachWidgetsToSections(BarInstance& instance) {
  const bool isVertical = instance.barConfig.position == "left" || instance.barConfig.position == "right";
  const auto widgetSpacing = static_cast<float>(instance.barConfig.widgetSpacing);
  const bool hoverHighlight = instance.barConfig.hoverHighlight;

  instance.widgetByRoot.clear();
  instance.hoverHighlightWidget = nullptr;
  if (instance.hoverUnderlay != nullptr) {
    while (!instance.hoverUnderlay->children().empty()) {
      instance.hoverUnderlay->removeChild(instance.hoverUnderlay->children().back().get());
    }
  }

  // Hover overlay: sits above the capsule fill (same zIndex, later sibling) and below the
  // content; fill/visibility are driven by the hover animation.
  auto addHoverBox = [hoverHighlight](Widget& widget, Node& shell) -> Box* {
    if (!hoverHighlight || !widget.wantsBarHoverHighlight()) {
      return nullptr;
    }
    Box* boxPtr = nullptr;
    shell.addChild(
        ui::box({
            .out = &boxPtr,
            .fill = withOpacity(widget.widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)), 0.0f),
            .visible = false,
            .configure = [](Box& box) { box.setZIndex(-1); },
        })
    );
    widget.setBarHoverBox(boxPtr);
    return boxPtr;
  };

  auto attach = [&](std::vector<std::unique_ptr<Widget>>& widgets, std::vector<BarCapsuleRun>& capsuleRuns,
                    Flex* section) {
    if (section == nullptr) {
      return;
    }

    for (auto& widget : widgets) {
      widget->setAnimationManager(&instance.animations);
      widget->setUpdateCallback([surface = instance.surface.get()]() {
        if (surface != nullptr) {
          surface->requestUpdate();
        }
      });
      widget->setRedrawCallback([surface = instance.surface.get()]() {
        if (surface != nullptr) {
          surface->requestRedraw();
        }
      });
      widget->setFrameTickRequestCallback([surface = instance.surface.get()]() {
        if (surface != nullptr) {
          surface->requestFrameTick();
        }
      });
      widget->setPanelToggleCallback([this, inst = &instance, trigger = widget.get()](
                                         std::string_view panelId, std::string_view context,
                                         std::optional<float> anchorSurfaceX, std::optional<float> anchorSurfaceY
                                     ) {
        float anchorX = inst->lastPointerSx;
        float anchorY = inst->lastPointerSy;
        if (anchorSurfaceX.has_value()) {
          anchorX = *anchorSurfaceX;
        }
        if (anchorSurfaceY.has_value()) {
          anchorY = *anchorSurfaceY;
        }
        if (m_platform != nullptr && inst->output != nullptr) {
          if (const auto* out = m_platform->findOutputByWl(inst->output); out != nullptr && out->hasUsableGeometry()) {
            const auto [surfaceX, surfaceY] = surfaceOriginForOutputLocal(*inst, *out);
            anchorX += surfaceX;
            anchorY += surfaceY;
          }
        }
        auto& panels = PanelManager::instance();
        const bool closesCurrent =
            panels.isOpenPanel(panelId) && (context.empty() || panels.isActivePanelContext(context));
        clearPanelActiveWidget();
        panels.togglePanel(
            std::string(panelId),
            PanelOpenRequest{
                .output = inst->output,
                .triggerSurface = inst->surface != nullptr ? inst->surface->wlSurface() : nullptr,
                .anchorX = anchorX,
                .anchorY = anchorY,
                .hasExplicitAnchor = anchorSurfaceX.has_value() || anchorSurfaceY.has_value(),
                .hasAnchorPosition = true,
                .context = context,
                .sourceBarName = inst->barConfig.name
            }
        );
        if (!closesCurrent && panels.isOpenPanel(panelId)) {
          setActivePanelWidget(inst, trigger);
        }
      });
      widget->create();
      if (widget->root() != nullptr) {
        instance.widgetByRoot[widget->root()] = widget.get();
      }
    }

    capsuleRuns.clear();

    auto addPlainWidget = [&](Widget& widget) {
      widget.setBarCapsuleScene(nullptr, nullptr);
      // No capsule: the hover pill lives on the bar-level underlay (unclipped, no layout
      // footprint) and is positioned after sections are laid out.
      if (hoverHighlight
          && instance.hoverUnderlay != nullptr
          && !widget.isBarClickThrough()
          && !widget.noGapAroundMe()) {
        addHoverBox(widget, *instance.hoverUnderlay);
      }
      auto* added = section->addChild(widget.releaseRoot());
      if (widget.noGapAroundMe()) {
        section->setChildGapExcluded(added, true);
      }
    };

    auto addSingleCapsule = [&](Widget& widget) {
      const auto& cap = widget.barCapsuleSpec();
      auto shell = std::make_unique<Node>();
      Node* shellPtr = shell.get();
      shellPtr->setClipChildren(true);
      const float scale = widget.contentScale();
      Box* bgPtr = nullptr;
      auto capsuleBg = ui::box({
          .out = &bgPtr,
          .fill = withOpacity(cap.fill, cap.opacity),
          .configure = [&cap, scale](Box& bg) {
            if (cap.border.has_value()) {
              bg.setBorder(*cap.border, Style::borderWidth * scale);
            } else {
              bg.clearBorder();
            }
            bg.setZIndex(-1);
          },
      });
      shellPtr->addChild(std::move(capsuleBg));
      Box* hoverPtr = addHoverBox(widget, *shellPtr);
      shellPtr->addChild(widget.releaseRoot());
      widget.setBarCapsuleScene(shellPtr, bgPtr);
      if (auto* area = dynamic_cast<InputArea*>(widget.root())) {
        area->setTooltipAnchorNode(shellPtr);
      }
      capsuleRuns.push_back(
          BarCapsuleRun{
              .shell = shellPtr,
              .bg = bgPtr,
              .container = nullptr,
              .content = widget.root(),
              .spec = cap,
              .contentScale = widget.contentScale(),
              .widgets = {&widget},
              .hoverBoxes = hoverPtr != nullptr ? std::vector<Box*>{hoverPtr} : std::vector<Box*>{},
          }
      );
      auto* added = section->addChild(std::move(shell));
      if (widget.noGapAroundMe()) {
        section->setChildGapExcluded(added, true);
      }
    };

    // Members of the same group share one resolved style by construction (see resolveWidgetBarCapsuleSpec),
    // so adjacency + matching group ID is sufficient to merge. Per-widget scale does not split the group:
    // the run is sized from its largest member's scale below so a differently scaled member still fits.
    auto canJoinCapsuleGroup = [](const Widget& first, const Widget& next) {
      const auto& firstSpec = first.barCapsuleSpec();
      const auto& nextSpec = next.barCapsuleSpec();
      return firstSpec.enabled
          && nextSpec.enabled
          && !first.isAnchor()
          && !next.isAnchor()
          && !firstSpec.group.empty()
          && firstSpec.group == nextSpec.group;
    };

    std::size_t index = 0;
    while (index < widgets.size()) {
      auto& widget = widgets[index];
      if (widget->root() == nullptr) {
        ++index;
        continue;
      }

      const auto& cap = widget->barCapsuleSpec();
      if (!cap.enabled) {
        addPlainWidget(*widget);
        ++index;
        continue;
      }

      if (widget->isAnchor() || cap.group.empty()) {
        addSingleCapsule(*widget);
        ++index;
        continue;
      }

      std::size_t runEnd = index + 1;
      while (runEnd < widgets.size()
             && widgets[runEnd] != nullptr
             && widgets[runEnd]->root() != nullptr
             && canJoinCapsuleGroup(*widget, *widgets[runEnd])) {
        ++runEnd;
      }

      if (runEnd - index < 2) {
        addSingleCapsule(*widget);
        ++index;
        continue;
      }

      auto shell = std::make_unique<Node>();
      Node* shellPtr = shell.get();
      shellPtr->setClipChildren(true);
      const float scale = widget->contentScale();
      Box* bgPtr = nullptr;
      auto capsuleBg = ui::box({
          .out = &bgPtr,
          .fill = withOpacity(cap.fill, cap.opacity),
          .configure = [&cap, scale](Box& bg) {
            if (cap.border.has_value()) {
              bg.setBorder(*cap.border, Style::borderWidth * scale);
            } else {
              bg.clearBorder();
            }
            bg.setZIndex(-1);
          },
      });
      shellPtr->addChild(std::move(capsuleBg));

      std::vector<Box*> hoverBoxes;
      if (hoverHighlight) {
        hoverBoxes.reserve(runEnd - index);
        for (std::size_t memberIndex = index; memberIndex < runEnd; ++memberIndex) {
          hoverBoxes.push_back(addHoverBox(*widgets[memberIndex], *shellPtr));
        }
      }

      auto inner = ui::flex(
          isVertical ? FlexDirection::Vertical : FlexDirection::Horizontal,
          {
              .align = FlexAlign::Center,
              .gap = widgetSpacing,
          }
      );
      Flex* innerPtr = inner.get();
      shellPtr->addChild(std::move(inner));

      BarCapsuleRun run;
      run.shell = shellPtr;
      run.bg = bgPtr;
      run.container = innerPtr;
      run.content = innerPtr;
      run.spec = cap;
      run.contentScale = widget->contentScale();
      run.hoverBoxes = std::move(hoverBoxes);

      for (std::size_t memberIndex = index; memberIndex < runEnd; ++memberIndex) {
        auto& member = widgets[memberIndex];
        member->setBarCapsuleScene(shellPtr, bgPtr);
        run.widgets.push_back(member.get());
        auto* added = innerPtr->addChild(member->releaseRoot());
        if (auto* area = dynamic_cast<InputArea*>(member->root())) {
          area->setTooltipAnchorNode(shellPtr);
        }
        if (member->noGapAroundMe()) {
          innerPtr->setChildGapExcluded(added, true);
        }
      }

      capsuleRuns.push_back(std::move(run));
      section->addChild(std::move(shell));
      index = runEnd;
    }
  };

  attach(instance.startWidgets, instance.startCapsuleRuns, instance.startSection);
  attach(instance.centerWidgets, instance.centerCapsuleRuns, instance.centerSection);
  attach(instance.endWidgets, instance.endCapsuleRuns, instance.endSection);
}

void Bar::updateWidgetHoverHighlight(BarInstance& instance, InputArea* hoveredArea) {
  Widget* target = nullptr;
  for (const Node* node = hoveredArea; node != nullptr; node = node->parent()) {
    if (auto it = instance.widgetByRoot.find(node); it != instance.widgetByRoot.end()) {
      target = it->second;
      break;
    }
  }
  if (target != nullptr && target->barHoverBox() == nullptr) {
    target = nullptr;
  }
  if (target == instance.hoverHighlightWidget) {
    return;
  }
  if (instance.hoverHighlightWidget != nullptr) {
    animateWidgetHoverHighlight(instance, *instance.hoverHighlightWidget, false);
  }
  instance.hoverHighlightWidget = target;
  if (target != nullptr) {
    animateWidgetHoverHighlight(instance, *target, true);
  }
}

void Bar::animateWidgetHoverHighlight(BarInstance& instance, Widget& widget, bool hovered) {
  Box* box = widget.barHoverBox();
  if (box == nullptr) {
    return;
  }
  const ColorSpec fill = widget.widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface));
  instance.animations.cancelForOwner(box);
  instance.animations.animate(
      widget.barHoverProgress(), hovered ? 1.0f : 0.0f, Style::animFast, Easing::EaseOutCubic,
      [&widget, box, fill](float progress) {
        widget.setBarHoverProgress(progress);
        box->setVisible(progress > 0.001f);
        box->setFill(withOpacity(fill, kWidgetHoverFillAlpha * progress));
      },
      {}, box
  );
  if (instance.surface != nullptr) {
    instance.surface->requestRedraw();
  }
}

void Bar::rebuildInstanceContents(BarInstance& instance, const BarConfig& newConfig) {
  gnil::profiling::ScopedTimer t(kLog, std::format("bar: rebuild contents [{}]", newConfig.name));

  // Drop any pointer hover/capture state pointing into the widgets we're about
  // to destroy. Hover will be re-acquired on the next pointer motion.
  instance.inputDispatcher.pointerLeave();

  instance.barConfig = newConfig;

  // Detach old widget root nodes from their sections and destroy the widgets.
  // Widgets release their root into the section on creation, so the section
  // owns those nodes — clearing the section frees the scene tree.
  auto clearChildren = [](Node* node) {
    if (node == nullptr) {
      return;
    }
    while (!node->children().empty()) {
      node->removeChild(node->children().back().get());
    }
  };
  clearChildren(instance.startSection);
  clearChildren(instance.centerSection);
  clearChildren(instance.endSection);
  instance.startWidgets.clear();
  instance.centerWidgets.clear();
  instance.endWidgets.clear();
  instance.startCapsuleRuns.clear();
  instance.centerCapsuleRuns.clear();
  instance.endCapsuleRuns.clear();

  // Refresh section-level layout knobs that may have changed (gap; direction
  // doesn't change because position is part of the surface-fields gate).
  const auto widgetSpacing = static_cast<float>(instance.barConfig.widgetSpacing);
  if (instance.startSection != nullptr) {
    instance.startSection->setGap(widgetSpacing);
  }
  if (instance.centerSection != nullptr) {
    instance.centerSection->setGap(widgetSpacing);
  }
  if (instance.endSection != nullptr) {
    instance.endSection->setGap(widgetSpacing);
  }

  populateWidgets(instance);
  attachWidgetsToSections(instance);

  applyBackgroundPalette(instance);
  syncBarSurfaceChrome(instance);

  if (instance.surface != nullptr) {
    // Re-run buildScene at the current surface size so radii / styling pick
    // up changes. The first-frame branch is skipped because sceneRoot is
    // already in place.
    const auto w = instance.surface->width();
    const auto h = instance.surface->height();
    if (w > 0 && h > 0) {
      buildScene(instance, w, h);
    }
    instance.surface->requestLayout();
  }
}

void Bar::tickWidgets(std::vector<std::unique_ptr<Widget>>& widgets, float deltaMs) { ::tickWidgets(widgets, deltaMs); }

bool Bar::widgetsNeedFrameTick(const std::vector<std::unique_ptr<Widget>>& widgets) {
  return ::widgetsNeedFrameTick(widgets);
}

bool Bar::instanceNeedsFrameTick(const BarInstance& instance) {
  return widgetsNeedFrameTick(instance.startWidgets)
      || widgetsNeedFrameTick(instance.centerWidgets)
      || widgetsNeedFrameTick(instance.endWidgets);
}

void Bar::applyBackgroundPalette(BarInstance& instance) {
  if (instance.bg == nullptr) {
    return;
  }
  auto style = instance.bg->style();
  style.fill = colorForRole(ColorRole::Surface);
  style.border = {};
  style.borderWidth = 0.0f;
  instance.bg->setStyle(style);
}

void Bar::syncBarAutoHideInputRegion(BarInstance& instance) const {
  if (instance.surface == nullptr) {
    return;
  }
  const int surfW = static_cast<int>(instance.surface->width());
  const int surfH = static_cast<int>(instance.surface->height());
  const auto visual = currentBarVisualGeometry(
      instance, m_config->config().shell, static_cast<float>(surfW), static_cast<float>(surfH)
  );
  const InputRect visibleRect{
      static_cast<int>(std::lround(visual.x)), static_cast<int>(std::lround(visual.y)),
      static_cast<int>(std::lround(visual.width)), static_cast<int>(std::lround(visual.height))
  };
  if (barConfigUsesSlideSurface(instance.barConfig)) {
    auto rects = barAutoHideTriggerRegion(instance.barConfig, m_config->config().shell, surfW, surfH);
    rects.push_back(visibleRect);
    instance.surface->setInputRegion(rects);
    return;
  }
  instance.surface->setInputRegion({visibleRect});
}

void Bar::revealAutoHideBar(BarInstance& instance) {
  if (instance.autoHideDisablePending) {
    return;
  }
  if (!barSupportsSlideBehavior(instance.barConfig) || instance.surface == nullptr || instance.slideRoot == nullptr) {
    return;
  }

  instance.ipcLayoutReleased = false;
  instance.animations.cancelForOwner(instance.slideRoot);
  const float current = instance.hideOpacity;
  wl_output* output = instance.output;
  const std::string barName = instance.barConfig.name;
  const auto notifyAttachedPanel = [output, barName]() {
    PanelManager::instance().onAttachedBarRevealSettled(output, barName);
  };

  constexpr float kSettledThreshold = 0.999f;
  if (current >= kSettledThreshold) {
    syncBarAutoHideInputRegion(instance);
    syncBarSurfaceChrome(instance);
    instance.surface->requestRedraw();
    notifyAttachedPanel();
    return;
  }

  instance.animations.animate(
      current, 1.0f, Style::animChromeSpatial, Easing::CaelestiaExpressiveSpatial,
      [inst = &instance, this](float v) {
        inst->hideOpacity = v;
        syncBarSlideLayerTransform(*inst);
        syncBarAutoHideInputRegion(*inst);
        syncBarSurfaceChrome(*inst);
      },
      notifyAttachedPanel, instance.slideRoot
  );
  syncBarAutoHideInputRegion(instance);
  syncBarSurfaceChrome(instance);
  instance.surface->requestRedraw();
}

void Bar::syncBarSlideLayerTransform(BarInstance& instance) const {
  if (instance.slideRoot == nullptr) {
    return;
  }
  instance.slideRoot->setPosition(0.0f, 0.0f);
  if (instance.surface == nullptr || instance.contentClip == nullptr || m_config == nullptr) {
    return;
  }
  const float width = static_cast<float>(instance.surface->width());
  const float height = static_cast<float>(instance.surface->height());
  const auto visual = currentBarVisualGeometry(instance, m_config->config().shell, width, height);
  instance.contentClip->setPosition(visual.x, visual.y);
  instance.contentClip->setSize(visual.width, visual.height);
  if (instance.surface != nullptr) {
    instance.surface->requestRedraw();
  }
}

void Bar::applyBarCompositorBlur(BarInstance& instance) const {
  if (instance.surface == nullptr) {
    return;
  }
  // GNIL deliberately has no compositor/backdrop blur.  The Material palette
  // supplies depth through opaque Surface and SurfaceContainer colors instead.
  instance.surface->clearBlurRegion();
}

void Bar::startHideFadeOut(BarInstance& instance) {
  if (instance.autoHideDisablePending || instance.smartAutoHidePinnedVisible || instance.ipcPinnedVisible) {
    return;
  }
  const float current = instance.hideOpacity;
  instance.animations.animate(
      current, 0.0f, Style::animChromeExit, Easing::EaseInQuad,
      [this, inst = &instance](float v) {
        inst->hideOpacity = v;
        syncBarSlideLayerTransform(*inst);
        syncBarAutoHideInputRegion(*inst);
        syncBarSurfaceChrome(*inst);
      },
      [inst = &instance, this]() {
        if (inst->surface == nullptr) {
          return;
        }
        syncBarAutoHideInputRegion(*inst);
        syncBarSurfaceChrome(*inst);
        inst->surface->requestRedraw();
      },
      instance.slideRoot
  );
  syncBarSurfaceChrome(instance);
  if (instance.surface != nullptr) {
    instance.surface->requestRedraw();
  }
}

void Bar::buildScene(BarInstance& instance, std::uint32_t width, std::uint32_t height) {
  uiAssertNotRendering("Bar::buildScene");
  if (m_renderContext == nullptr) {
    return;
  }
  auto* renderer = m_renderContext;

  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);
  const auto padding = resolvedBarMainAxisPadding(instance.barConfig, m_config->config().shell);
  const auto widgetSpacing = static_cast<float>(instance.barConfig.widgetSpacing);
  const auto& shadowConfig = m_config->config().shell.shadow;
  const bool isVertical = (instance.barConfig.position == "left" || instance.barConfig.position == "right");
  const auto concave = barConcaveShape(instance.barConfig);

  const auto barVisual = computeBarVisualGeometry(instance.barConfig, shadowConfig, w, h);
  const float barAreaX = barVisual.x;
  const float barAreaY = barVisual.y;
  const float barAreaW = barVisual.width;
  const float barAreaH = barVisual.height;

  if (instance.sceneRoot == nullptr) {
    instance.sceneRoot = std::make_unique<Node>();
    instance.sceneRoot->setAnimationManager(&instance.animations);
    instance.sceneRoot->setSize(w, h);

    instance.chromeHost = std::make_unique<ChromeOutputHost>(*instance.sceneRoot);

    auto slide = std::make_unique<Node>();
    slide->setParticipatesInLayout(false);
    instance.slideRoot = instance.sceneRoot->addChild(std::move(slide));

    // Bar background
    instance.bg = static_cast<Box*>(instance.slideRoot->addChild(ui::box()));
    // Layout geometry remains on this node, while the actual material is
    // painted by ChromeOutputHost's single SDF pass.
    instance.bg->setVisible(false);

    // A joined drawer is painted by this same fullscreen chrome surface. Its
    // separate layer surface contains only content/input, so no wallpaper or
    // rounded edge can be composited a second time at the join.
    // Shadow is part of the same SDF pass, so there are no per-piece shadow
    // nodes to overlap at a smooth-union seam.

    auto hoverUnderlay = std::make_unique<Node>();
    hoverUnderlay->setHitTestVisible(false);
    hoverUnderlay->setSize(static_cast<float>(w), static_cast<float>(h));
    instance.hoverUnderlay = instance.slideRoot->addChild(std::move(hoverUnderlay));

    auto contentClip = std::make_unique<Node>();
    contentClip->setClipChildren(true);
    instance.contentClip = instance.slideRoot->addChild(std::move(contentClip));

    auto makeSlot = [&instance]() {
      auto slot = std::make_unique<Node>();
      slot->setClipChildren(true);
      return instance.contentClip->addChild(std::move(slot));
    };
    instance.startSlot = makeSlot();
    instance.centerSlot = makeSlot();
    instance.endSlot = makeSlot();

    // Create section boxes
    auto makeSection = [widgetSpacing, isVertical]() {
      return ui::flex(
          isVertical ? FlexDirection::Vertical : FlexDirection::Horizontal,
          {
              .align = FlexAlign::Center,
              .gap = widgetSpacing,
          }
      );
    };

    instance.startSection = static_cast<Flex*>(instance.startSlot->addChild(makeSection()));
    instance.centerSection = static_cast<Flex*>(instance.centerSlot->addChild(makeSection()));
    instance.endSection = static_cast<Flex*>(instance.endSlot->addChild(makeSection()));

    attachWidgetsToSections(instance);

    // Wire up InputDispatcher for this instance
    instance.inputDispatcher.setSceneRoot(instance.sceneRoot.get());
    instance.inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_platform->setCursorShape(serial, shape);
    });
    instance.inputDispatcher.setHoverChangeCallback([this, inst = &instance](InputArea* /*old*/, InputArea* next) {
      TooltipManager::instance().onHoverChange(next, inst->surface->layerSurface(), inst->output);
      updateWidgetHoverHighlight(*inst, next);
    });

    if (instance.barConfig.smartAutoHide && m_platform != nullptr) {
      instance.smartAutoHidePinnedVisible = smartAutoHideWantsPinnedVisible(*m_platform, instance.output);
    }
    if (barConfigUsesSlideSurface(instance.barConfig)) {
      instance.slideRoot->setOpacity(1.0f);
      const bool startHidden =
          instance.barConfig.smartAutoHide ? !instance.smartAutoHidePinnedVisible : instance.barConfig.autoHide;
      instance.hideOpacity = startHidden ? 0.0f : 1.0f;
    } else {
      instance.slideRoot->setOpacity(1.0f);
      instance.hideOpacity = 1.0f;
    }

    instance.surface->setSceneRoot(instance.sceneRoot.get());
  }

  // Update root size on reconfigure
  instance.sceneRoot->setSize(w, h);
  if (instance.chromeHost != nullptr) {
    instance.chromeHost->setSurfaceSize(w, h);
  }
  if (instance.slideRoot != nullptr) {
    instance.slideRoot->setSize(w, h);
  }
  if (instance.hoverUnderlay != nullptr) {
    instance.hoverUnderlay->setSize(w, h);
  }

  applyBackgroundPalette(instance);

  // Background covers only the bar visual area (not the shadow extension).
  // Keep it exactly aligned with the shadow shape; the shadow shader now
  // draws only outside the rect, so any size mismatch is visible at corners.
  if (instance.bg != nullptr) {
    const RoundedRectStyle bgStyle{
        .fill = colorForRole(ColorRole::Surface),
        .border = {},
        .fillMode = FillMode::Solid,
        .corners = concave.corners,
        .logicalInset = concave.logicalInset,
        .radius = concave.radii,
        .softness = 0.0f,
        .borderWidth = 0.0f,
    };
    instance.bg->setStyle(bgStyle);
    // (barAreaX/Y/W/H) is the body; the shader expands outward by logicalInset into
    // the visual rect, so the node must be sized to the visual rect.
    instance.bg->setPosition(barAreaX - concave.logicalInset.left, barAreaY - concave.logicalInset.top);
    instance.bg->setSize(
        barAreaW + concave.logicalInset.left + concave.logicalInset.right,
        barAreaH + concave.logicalInset.top + concave.logicalInset.bottom
    );
  }
  applyAttachedPanelBackdrop(instance);

  instance.paletteConn = paletteChanged().connect([this, inst = &instance] {
    applyBackgroundPalette(*inst);
    applyAttachedPanelBackdrop(*inst);
    syncBarSurfaceChrome(*inst);
    if (inst->surface != nullptr) {
      inst->surface->requestRedraw();
    }
  });
  if (instance.contentClip != nullptr) {
    instance.contentClip->setPosition(barAreaX, barAreaY);
    instance.contentClip->setSize(barAreaW, barAreaH);
  }

  applyBarShadowStyle(instance, m_config->config().shell, w, h);

  layoutBarSections(instance, *renderer, barAreaW, barAreaH, padding, isVertical);

  syncBarSlideLayerTransform(instance);

  syncBarAutoHideInputRegion(instance);
  syncBarSurfaceChrome(instance);
}

void Bar::updateWidgets(BarInstance& instance) {
  if (m_renderContext == nullptr) {
    return;
  }
  auto* renderer = m_renderContext;

  const auto w = static_cast<float>(instance.surface->width());
  const auto h = static_cast<float>(instance.surface->height());
  const auto padding = resolvedBarMainAxisPadding(instance.barConfig, m_config->config().shell);
  const bool isVertical = (instance.barConfig.position == "left" || instance.barConfig.position == "right");
  const auto& shadowConfig = m_config->config().shell.shadow;
  const auto barVisual = computeBarVisualGeometry(instance.barConfig, shadowConfig, w, h);
  const float barAreaW = barVisual.width;
  const float barAreaH = barVisual.height;

  auto updateSection = [&](std::vector<std::unique_ptr<Widget>>& widgets) {
    for (auto& widget : widgets) {
      if (widget->root() == nullptr) {
        continue;
      }
      widget->update(*renderer);
      widget->layout(*renderer, barAreaW, barAreaH);
    }
  };

  updateSection(instance.startWidgets);
  updateSection(instance.centerWidgets);
  updateSection(instance.endWidgets);
  layoutBarSections(instance, *renderer, barAreaW, barAreaH, padding, isVertical);
}

void Bar::prepareFrame(BarInstance& instance, bool needsUpdate, bool needsLayout) {
  if (m_renderContext == nullptr || instance.surface == nullptr) {
    return;
  }

  m_renderContext->makeCurrent(instance.surface->renderTarget());

  if (needsUpdate) {
    UiPhaseScope updatePhase(UiPhase::Update);
    updateWidgets(instance);
    return;
  }

  if (!needsLayout) {
    return;
  }

  const auto w = static_cast<float>(instance.surface->width());
  const auto h = static_cast<float>(instance.surface->height());
  const auto padding = resolvedBarMainAxisPadding(instance.barConfig, m_config->config().shell);
  const bool isVertical = (instance.barConfig.position == "left" || instance.barConfig.position == "right");
  const auto& shadowConfig = m_config->config().shell.shadow;
  const auto barVisual = computeBarVisualGeometry(instance.barConfig, shadowConfig, w, h);
  const float barAreaW = barVisual.width;
  const float barAreaH = barVisual.height;

  {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    for (auto& widget : instance.startWidgets) {
      widget->layout(*m_renderContext, barAreaW, barAreaH);
    }
    for (auto& widget : instance.centerWidgets) {
      widget->layout(*m_renderContext, barAreaW, barAreaH);
    }
    for (auto& widget : instance.endWidgets) {
      widget->layout(*m_renderContext, barAreaW, barAreaH);
    }
    layoutBarSections(instance, *m_renderContext, barAreaW, barAreaH, padding, isVertical);
  }
}

bool Bar::onPointerEvent(const PointerEvent& event) {
  bool consumed = false;
  BarInstance* targetInstance = nullptr;
  if (event.surface != nullptr) {
    targetInstance = instanceForSurface(event.surface);
  } else {
    targetInstance = m_hoveredInstance;
  }

  auto routeWidgetPopups = [&](BarInstance& instance) {
    auto routeGroup = [&](std::vector<std::unique_ptr<Widget>>& widgets) {
      for (auto& widget : widgets) {
        if (widget != nullptr && widget->onPointerEvent(event)) {
          return true;
        }
      }
      return false;
    };
    return routeGroup(instance.startWidgets) || routeGroup(instance.centerWidgets) || routeGroup(instance.endWidgets);
  };
  if (targetInstance != nullptr) {
    if (!instanceAcceptsPointerInput(*targetInstance)) {
      clearInstancePointerState(*targetInstance);
      return false;
    }
    if (routeWidgetPopups(*targetInstance)) {
      return true;
    }
  } else {
    for (const auto& instance : m_instances) {
      if (instance != nullptr && instanceAcceptsPointerInput(*instance) && routeWidgetPopups(*instance)) {
        return true;
      }
    }
  }

  if (targetInstance != nullptr
      && event.type == PointerEvent::Type::Button
      && event.button == BTN_MIDDLE
      && event.state == 1
      && m_config != nullptr
      && m_config->config().shell.middleClickOpensWidgetSettings) {
    auto* widget = widgetAtPoint(*targetInstance, static_cast<float>(event.sx), static_cast<float>(event.sy));
    if (widget != nullptr
        && !widget->reservesMiddleClick()
        && !widget->configName().empty()
        && m_openWidgetSettingsCallback) {
      m_openWidgetSettingsCallback(targetInstance->barConfig.name, std::string(widget->configName()));
      return true;
    }
  }

  if (targetInstance != nullptr && targetInstance->attachedPopupCount > 0) {
    switch (event.type) {
    case PointerEvent::Type::Enter:
      m_hoveredInstance = targetInstance;
      targetInstance->pointerInside = true;
      break;
    case PointerEvent::Type::Leave:
      targetInstance->pointerInside = false;
      if (m_hoveredInstance == targetInstance) {
        m_hoveredInstance = nullptr;
      }
      break;
    case PointerEvent::Type::Motion:
    case PointerEvent::Type::Button:
    case PointerEvent::Type::Axis:
      if (event.type == PointerEvent::Type::Button && event.button == BTN_RIGHT && event.state == 1) {
        const auto sx = static_cast<float>(event.sx);
        const auto sy = static_cast<float>(event.sy);
        const auto& deadZone = targetInstance->barConfig.deadZone;
        if (!deadZone.rightCommand.empty() && isBarDeadZone(*targetInstance, sx, sy)) {
          executeDeadZoneCommand(deadZone.rightCommand);
        } else {
          PanelManager::instance().closePanel();
        }
        return true;
      }
      break;
    }
    return false;
  }

  switch (event.type) {
  case PointerEvent::Type::Enter: {
    auto it = m_surfaceMap.find(event.surface);
    if (it == m_surfaceMap.end()) {
      break;
    }
    m_hoveredInstance = it->second;
    BarInstance* const entered = m_hoveredInstance;
    entered->lastPointerSx = static_cast<float>(event.sx);
    entered->lastPointerSy = static_cast<float>(event.sy);
    entered->pointerInside = true;
    entered->inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    // pointerEnter can re-enter the Wayland event loop (tooltip popup work),
    // which may clear or change m_hoveredInstance before we dereference it.
    if (m_hoveredInstance != entered) {
      break;
    }
    if (barSupportsSlideBehavior(m_hoveredInstance->barConfig)
        && m_hoveredInstance->barConfig.showOnHover
        && m_hoveredInstance->sceneRoot != nullptr) {
      revealAutoHideBar(*m_hoveredInstance);
    }
    break;
  }
  case PointerEvent::Type::Leave: {
    if (m_hoveredInstance != nullptr) {
      m_hoveredInstance->pointerInside = false;
      m_hoveredInstance->inputDispatcher.pointerLeave();
      const bool suppressAutoHide =
          (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*m_hoveredInstance) : false;
      if (barPointerHideAllowed(*m_hoveredInstance) && !suppressAutoHide) {
        startHideFadeOut(*m_hoveredInstance);
      }
      m_hoveredInstance = nullptr;
    }
    break;
  }
  case PointerEvent::Type::Motion: {
    if (m_hoveredInstance == nullptr)
      break;
    BarInstance* const hovered = m_hoveredInstance;
    hovered->lastPointerSx = static_cast<float>(event.sx);
    hovered->lastPointerSy = static_cast<float>(event.sy);
    if (hovered->edgeDragActive) {
      const float delta = static_cast<float>(event.sx) - hovered->edgeDragStart;
      if (delta >= 20.0f) {
        hovered->ipcPinnedVisible = true;
        hovered->ipcLayoutReleased = false;
        hovered->edgeDragActive = false;
        revealAutoHideBar(*hovered);
      } else if (delta <= -20.0f) {
        hovered->ipcPinnedVisible = false;
        hovered->edgeDragActive = false;
        startHideFadeOut(*hovered);
      }
    }
    hovered->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
    // pointerMotion can re-enter the Wayland event loop (tooltip popup work),
    // which may clear or change m_hoveredInstance before we dereference it.
    if (m_hoveredInstance != hovered) {
      break;
    }
    break;
  }
  case PointerEvent::Type::Button: {
    if (m_hoveredInstance == nullptr)
      break;
    m_hoveredInstance->lastPointerSx = static_cast<float>(event.sx);
    m_hoveredInstance->lastPointerSy = static_cast<float>(event.sy);
    const auto sx = static_cast<float>(event.sx);
    const auto sy = static_cast<float>(event.sy);
    bool pressed = (event.state == 1); // WL_POINTER_BUTTON_STATE_PRESSED
    if (event.button == BTN_LEFT) {
      if (pressed && sx <= std::max(3.0f, m_config->config().shell.chrome.frameThickness + 1.0f)) {
        m_hoveredInstance->edgeDragActive = true;
        m_hoveredInstance->edgeDragStart = sx;
      } else if (!pressed) {
        m_hoveredInstance->edgeDragActive = false;
      }
    }
    consumed = m_hoveredInstance->inputDispatcher.pointerButton(sx, sy, event.button, pressed);
    if (pressed && !consumed) {
      if (handleBarDeadZoneButton(*m_hoveredInstance, sx, sy, event.button, m_platform)) {
        consumed = true;
      }
    }
    break;
  }
  case PointerEvent::Type::Axis: {
    if (m_hoveredInstance == nullptr)
      break;
    m_hoveredInstance->lastPointerSx = static_cast<float>(event.sx);
    m_hoveredInstance->lastPointerSy = static_cast<float>(event.sy);
    const auto sx = static_cast<float>(event.sx);
    const auto sy = static_cast<float>(event.sy);
    const bool axisConsumed = m_hoveredInstance->inputDispatcher.pointerAxis(
        sx, sy, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines
    );
    if (!axisConsumed) {
      handleBarDeadZoneAxis(*m_hoveredInstance, sx, sy, event);
    }
    break;
  }
  }

  // Trigger redraw if any widget changed visual state
  if (m_hoveredInstance != nullptr
      && m_hoveredInstance->sceneRoot != nullptr
      && (m_hoveredInstance->sceneRoot->paintDirty() || m_hoveredInstance->sceneRoot->layoutDirty())) {
    if (m_hoveredInstance->sceneRoot->layoutDirty()) {
      m_hoveredInstance->surface->requestLayout();
    } else {
      m_hoveredInstance->surface->requestRedraw();
    }
  }

  return consumed;
}

BarInstance* Bar::instanceForSurface(wl_surface* surface) const noexcept {
  if (surface == nullptr) {
    return nullptr;
  }
  const auto it = m_surfaceMap.find(surface);
  return it != m_surfaceMap.end() ? it->second : nullptr;
}

BarInstance* Bar::instanceForOutput(wl_output* output) const noexcept { return instanceForBar(output, {}); }

BarInstance* Bar::instanceForBar(wl_output* output, std::string_view barName) const noexcept {
  if (output == nullptr) {
    return nullptr;
  }

  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->output != output || instance->surface == nullptr) {
      continue;
    }
    if (barName.empty() || instance->barConfig.name == barName) {
      return instance.get();
    }
  }
  return nullptr;
}

std::optional<std::string> Bar::collectBarIpcInstances(
    std::optional<std::string> barName, std::optional<std::string> monitorSelector,
    std::vector<BarInstance*>& instancesOut
) {
  instancesOut.clear();

  if (m_config == nullptr) {
    return "error: config service not initialized\n";
  }

  if (barName.has_value()) {
    const bool knownBar = std::ranges::contains(m_config->config().bars, *barName, &BarConfig::name);
    if (!knownBar) {
      if (!monitorSelector.has_value()) {
        monitorSelector = std::move(barName);
        barName = std::nullopt;
      } else {
        std::vector<std::string> knownBars;
        knownBars.reserve(m_config->config().bars.size());
        for (const auto& bar : m_config->config().bars) {
          knownBars.push_back(bar.name);
        }
        const std::string suffix =
            knownBars.empty() ? std::string() : std::string("; known: ") + StringUtils::join(knownBars, ", ");
        return "error: unknown bar \"" + std::string(*barName) + "\"" + suffix + "\n";
      }
    }
  }

  const auto matchesBar = [&](const BarInstance& instance) {
    return !barName.has_value() || instance.barConfig.name == *barName;
  };

  if (!monitorSelector.has_value()) {
    for (const auto& instance : m_instances) {
      if (instance != nullptr && matchesBar(*instance)) {
        instancesOut.push_back(instance.get());
      }
    }
    if (instancesOut.empty()) {
      if (barName.has_value()) {
        return "error: no instances matched bar \"" + std::string(*barName) + "\"\n";
      }
      return "error: no bar instances are active\n";
    }
    return std::nullopt;
  }

  if (m_platform == nullptr) {
    return "error: bar service not initialized\n";
  }

  const std::string selector(*monitorSelector);
  std::vector<std::string> outputMatches;
  std::vector<std::string> knownOutputs;
  for (const auto& output : m_platform->outputs()) {
    if (output.connectorName.empty()) {
      continue;
    }
    knownOutputs.push_back(output.connectorName);
    if (outputMatchesSelector(selector, output)) {
      outputMatches.push_back(output.connectorName);
    }
  }

  std::ranges::sort(knownOutputs);
  knownOutputs.erase(std::ranges::unique(knownOutputs).begin(), knownOutputs.end());
  std::ranges::sort(outputMatches);
  outputMatches.erase(std::ranges::unique(outputMatches).begin(), outputMatches.end());

  if (outputMatches.empty()) {
    std::string error = "error: unknown monitor selector \"" + selector + "\"";
    if (!knownOutputs.empty()) {
      error += " (available: " + StringUtils::join(knownOutputs, ", ") + ")";
    }
    error += "\n";
    return error;
  }
  if (outputMatches.size() > 1) {
    return "error: monitor selector \""
        + selector
        + "\" matched multiple outputs: "
        + StringUtils::join(outputMatches, ", ")
        + "\n";
  }

  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->output == nullptr || !matchesBar(*instance)) {
      continue;
    }
    const auto it = std::find_if(
        m_platform->outputs().begin(), m_platform->outputs().end(),
        [&instance](const WaylandOutput& output) { return output.output == instance->output; }
    );
    if (it != m_platform->outputs().end() && it->connectorName == outputMatches.front()) {
      instancesOut.push_back(instance.get());
    }
  }

  if (instancesOut.empty()) {
    std::string error = "error: no instances matched";
    if (barName.has_value()) {
      error += " bar \"" + std::string(*barName) + "\"";
    }
    error += " on \"" + outputMatches.front() + "\"\n";
    return error;
  }

  return std::nullopt;
}

namespace {

  [[nodiscard]] std::optional<std::string> parseBarVisibilityIpcArgs(
      std::string_view command, std::string_view args, std::optional<std::string>& barName,
      std::optional<std::string>& monitorSelector
  ) {
    const auto parts = gnil::ipc::splitWords(args);
    if (parts.size() > 2) {
      return "error: usage: " + std::string(command) + " [bar-name] [monitor-selector]\n";
    }
    barName = std::nullopt;
    monitorSelector = std::nullopt;
    if (!parts.empty() && !parts[0].empty()) {
      barName = parts[0];
    }
    if (parts.size() >= 2 && !parts[1].empty()) {
      monitorSelector = parts[1];
    }
    return std::nullopt;
  }

} // namespace

std::string Bar::showBarIpc(std::string_view args) {
  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (const auto parseError = parseBarVisibilityIpcArgs("bar-show", args, barName, monitorSelector)) {
    return *parseError;
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  for (BarInstance* instance : targets) {
    instance->ipcLayoutReleased = false;
    instance->ipcPinnedVisible = true;
    setInstanceIpcVisible(*instance, true);
    syncBarSurfaceChrome(*instance);
  }
  return "ok\n";
}

std::string Bar::hideBarIpc(std::string_view args) {
  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (const auto parseError = parseBarVisibilityIpcArgs("bar-hide", args, barName, monitorSelector)) {
    return *parseError;
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  for (BarInstance* instance : targets) {
    if (!barSupportsSlideBehavior(instance->barConfig)) {
      instance->ipcLayoutReleased = true;
    }
    instance->ipcPinnedVisible = false;
    setInstanceIpcVisible(*instance, false);
    syncBarSurfaceChrome(*instance);
  }
  return "ok\n";
}

std::string Bar::toggleBarIpc(std::string_view args) {
  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (const auto parseError = parseBarVisibilityIpcArgs("bar-toggle", args, barName, monitorSelector)) {
    return *parseError;
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  const bool anyEffectivelyVisible = std::ranges::any_of(targets, [this](const BarInstance* instance) {
    return instance != nullptr && instanceEffectivelyVisible(*instance);
  });

  if (anyEffectivelyVisible) {
    for (BarInstance* instance : targets) {
      if (!barSupportsSlideBehavior(instance->barConfig)) {
        instance->ipcLayoutReleased = true;
      }
      instance->ipcPinnedVisible = false;
      setInstanceIpcVisible(*instance, false);
      syncBarSurfaceChrome(*instance);
    }
    return "ok\n";
  }

  for (BarInstance* instance : targets) {
    instance->ipcLayoutReleased = false;
    instance->ipcPinnedVisible = true;
    setInstanceIpcVisible(*instance, true);
    syncBarSurfaceChrome(*instance);
  }
  return "ok\n";
}

std::string Bar::toggleBarReserveSpaceIpc(std::string_view args) {
  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (const auto parseError = parseBarVisibilityIpcArgs("bar-reserve-toggle", args, barName, monitorSelector)) {
    return *parseError;
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  return "error: GNIL's Caelestia frame always reserves its visible area\n";
}

std::string Bar::setBarAutoHideIpc(std::string_view args) {
  if (m_config == nullptr) {
    return "error: config service not initialized\n";
  }

  const auto parts = gnil::ipc::splitWords(args);
  if (parts.empty() || parts.size() > 3) {
    return "error: usage: bar-auto-hide-set <on|off|true|false|1|0> [bar-name] [monitor-selector]\n";
  }

  const std::string& value = parts[0];
  bool enabled = false;
  if (value == "on" || value == "true" || value == "1") {
    enabled = true;
  } else if (value == "off" || value == "false" || value == "0") {
    enabled = false;
  } else {
    return "error: invalid value (use on/off, true/false, 1/0)\n";
  }

  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (parts.size() >= 2 && !parts[1].empty()) {
    barName = parts[1];
  }
  if (parts.size() >= 3 && !parts[2].empty()) {
    monitorSelector = parts[2];
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  auto applyTransientAutoHide = [this, enabled](BarInstance& instance) {
    auto applySurfaceSpec = [this](BarInstance& inst) {
      if (inst.surface == nullptr) {
        return;
      }
      const auto spec = computeBarSurfaceSpec(inst.barConfig, m_config->config().shell.shadow);
      inst.surface->setMargins(spec.marginTop, spec.marginRight, spec.marginBottom, spec.marginLeft);
      inst.surface->requestSize(spec.surfaceWidth, spec.surfaceHeight);
    };

    instance.ipcLayoutReleased = false;
    instance.autoHideDisablePending = false;
    instance.animations.cancelForOwner(instance.slideRoot);

    if (enabled) {
      instance.barConfig.autoHide = true;
      applySurfaceSpec(instance);
      if (instance.slideRoot != nullptr) {
        instance.slideRoot->setOpacity(1.0f);
      }
      const bool suppressAutoHide =
          (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(instance) : false;
      if (instance.pointerInside || instance.attachedPopupCount > 0 || suppressAutoHide) {
        revealAutoHideBar(instance);
      } else {
        startHideFadeOut(instance);
      }
      return;
    }

    if (instance.barConfig.autoHide && instance.hideOpacity < 0.999f) {
      const float current = instance.hideOpacity;
      instance.autoHideDisablePending = true;
      instance.animations.animate(
          current, 1.0f, Style::animChromeSpatial, Easing::CaelestiaExpressiveSpatial,
          [this, inst = &instance](float v) {
            inst->hideOpacity = v;
            syncBarSlideLayerTransform(*inst);
            syncBarSurfaceChrome(*inst);
          },
          [this, inst = &instance, applySurfaceSpec]() {
            inst->autoHideDisablePending = false;
            inst->barConfig.autoHide = false;
            applySurfaceSpec(*inst);
            syncBarSlideLayerTransform(*inst);
            syncBarAutoHideInputRegion(*inst);
            syncBarSurfaceChrome(*inst);
            if (inst->surface != nullptr) {
              inst->surface->requestRedraw();
            }
          },
          instance.slideRoot
      );
      if (instance.surface != nullptr) {
        instance.surface->requestRedraw();
      }
      return;
    }

    instance.barConfig.autoHide = false;
    instance.autoHideDisablePending = false;
    instance.hideOpacity = 1.0f;
    if (instance.slideRoot != nullptr) {
      instance.slideRoot->setOpacity(1.0f);
    }
    applySurfaceSpec(instance);
    syncBarSlideLayerTransform(instance);
    syncBarAutoHideInputRegion(instance);
    syncBarSurfaceChrome(instance);
    if (instance.surface != nullptr) {
      instance.surface->requestRedraw();
    }
  };

  for (BarInstance* instance : targets) {
    applyTransientAutoHide(*instance);
  }
  return "ok\n";
}

std::string Bar::setBarLayerIpc(std::string_view args) {
  const auto parts = gnil::ipc::splitWords(args);
  if (parts.empty() || parts.size() > 3) {
    return "error: usage: bar-layer-set <top|overlay> [bar-name] [monitor-selector]\n";
  }

  const std::string& layer = parts[0];
  if (layer != "top" && layer != "overlay") {
    return "error: invalid layer (use top or overlay)\n";
  }

  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (parts.size() >= 2 && !parts[1].empty()) {
    barName = parts[1];
  }
  if (parts.size() >= 3 && !parts[2].empty()) {
    monitorSelector = parts[2];
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  const LayerShellLayer shellLayer = layerShellLayerFromConfig(layer);
  for (BarInstance* instance : targets) {
    if (instance == nullptr || instance->surface == nullptr) {
      continue;
    }
    instance->surface->setLayer(shellLayer);
    instance->barConfig.layer = layer;
  }

  return "ok\n";
}

void Bar::registerIpc(IpcService& ipc) {
  ipc.registerHandler(
      "bar-show", [this](const std::string& args) -> std::string { return showBarIpc(args); },
      "bar-show [bar-name] [monitor-selector]", "Show one or all bars"
  );

  ipc.registerHandler(
      "bar-hide", [this](const std::string& args) -> std::string { return hideBarIpc(args); },
      "bar-hide [bar-name] [monitor-selector]", "Hide one or all bars and release their layout gaps"
  );

  ipc.registerHandler(
      "bar-toggle", [this](const std::string& args) -> std::string { return toggleBarIpc(args); },
      "bar-toggle [bar-name] [monitor-selector]", "Toggle visibility for one or all bars"
  );

  ipc.registerHandler(
      "bar-reserve-toggle", [this](const std::string& args) -> std::string { return toggleBarReserveSpaceIpc(args); },
      "bar-reserve-toggle [bar-name] [monitor-selector]", "Toggle reserve space for one or all bars"
  );

  ipc.registerHandler(
      "bar-auto-hide-set", [this](const std::string& args) -> std::string { return setBarAutoHideIpc(args); },
      "bar-auto-hide-set <on|off|true|false|1|0> [bar-name] [monitor-selector]", "Set auto-hide state for a bar"
  );

  ipc.registerHandler(
      "bar-layer-set", [this](const std::string& args) -> std::string { return setBarLayerIpc(args); },
      "bar-layer-set <top|overlay> [bar-name] [monitor-selector]", "Set one or all bar layers"
  );
}
