#include "shell/dashboard/dashboard_edge_trigger.h"

#include "config/config_service.h"
#include "render/scene/input_area.h"
#include "shell/panel/panel_manager.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <linux/input-event-codes.h>

namespace {
  constexpr std::uint32_t kEdgeStripHeight = 8;
  constexpr std::uint32_t kEdgeStripWidth = 640;
}

DashboardEdgeTrigger::DashboardEdgeTrigger(PanelManager& panels) : m_panels(panels) {}
DashboardEdgeTrigger::~DashboardEdgeTrigger() { destroySurfaces(); }

void DashboardEdgeTrigger::initialize(
    WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext
) {
  m_wayland = &wayland;
  m_config = config;
  m_renderContext = renderContext;
}

void DashboardEdgeTrigger::onConfigReload() { onOutputChange(); }

void DashboardEdgeTrigger::onOutputChange() {
  destroySurfaces();
  if (m_wayland == nullptr || m_config == nullptr || m_renderContext == nullptr) {
    return;
  }
  const auto& config = m_config->config().dashboard;
  if (!config.enabled) {
    return;
  }

  for (const auto& output : m_wayland->outputs()) {
    if (!output.done || output.output == nullptr || !output.hasUsableGeometry()) {
      continue;
    }
    const std::uint32_t width = static_cast<std::uint32_t>(
        std::clamp(output.effectiveLogicalWidth(), 1, static_cast<int>(kEdgeStripWidth))
    );
    auto instance = std::make_unique<Instance>();
    instance->output = output.output;
    instance->surface = std::make_unique<LayerSurface>(
        *m_wayland,
        LayerSurfaceConfig{
            .nameSpace = "gnil-dashboard-edge",
            .layer = LayerShellLayer::Overlay,
            .anchor = LayerShellAnchor::Top,
            .width = width,
            .height = kEdgeStripHeight,
            .exclusiveZone = -1,
            .keyboard = LayerShellKeyboard::None,
            .defaultWidth = width,
            .defaultHeight = kEdgeStripHeight,
        }
    );
    if (!instance->surface->initialize(output.output)) {
      continue;
    }

    Instance* current = instance.get();
    auto area = std::make_unique<InputArea>();
    area->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
    area->setSize(static_cast<float>(width), static_cast<float>(kEdgeStripHeight));
    area->setOnPress([current](const InputArea::PointerData& data) {
      if (data.button != BTN_LEFT) {
        return;
      }
      if (data.pressed) {
        current->dragging = true;
        current->thresholdReached = false;
        current->dragStartY = data.localY;
        return;
      }
      current->dragging = false;
      current->thresholdReached = false;
    });
    area->setOnMotion([this, current](const InputArea::PointerData& data) {
      if (!current->dragging || current->thresholdReached || m_config == nullptr) {
        return;
      }
      const float distance = data.localY - current->dragStartY;
      if (distance >= static_cast<float>(m_config->config().dashboard.dragThreshold)) {
        current->thresholdReached = true;
        open(*current);
      }
    });

    instance->sceneRoot = std::move(area);
    instance->inputDispatcher.setSceneRoot(instance->sceneRoot.get());
    instance->surface->setRenderContext(m_renderContext);
    instance->surface->setSceneRoot(instance->sceneRoot.get());
    instance->surface->requestRedraw();
    m_instances.push_back(std::move(instance));
  }
}

void DashboardEdgeTrigger::destroySurfaces() { m_instances.clear(); }

void DashboardEdgeTrigger::open(Instance& instance) {
  if (m_config == nullptr || !m_config->config().dashboard.enabled) {
    return;
  }
  if (m_panels.isOpenPanel("dashboard")) {
    return;
  }
  m_panels.openPanel("dashboard", PanelOpenRequest{.output = instance.output, .context = "dashboard"});
}

bool DashboardEdgeTrigger::onPointerEvent(const PointerEvent& event) {
  for (const auto& instance : m_instances) {
    if (instance->surface == nullptr || event.surface != instance->surface->wlSurface()) {
      continue;
    }
    switch (event.type) {
    case PointerEvent::Type::Enter:
      instance->inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
      return true;
    case PointerEvent::Type::Leave:
      instance->inputDispatcher.pointerLeave();
      return true;
    case PointerEvent::Type::Motion:
      instance->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
      return true;
    case PointerEvent::Type::Button:
      return instance->inputDispatcher.pointerButton(
          static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, event.state == 1
      );
    case PointerEvent::Type::Axis:
      return true;
    }
  }
  return false;
}
