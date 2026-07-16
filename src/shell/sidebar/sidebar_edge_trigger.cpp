#include "shell/sidebar/sidebar_edge_trigger.h"

#include "config/config_service.h"
#include "render/scene/input_area.h"
#include "shell/panel/panel_manager.h"
#include "shell/sidebar/sidebar_edge_gesture.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <chrono>
#include <linux/input-event-codes.h>

namespace {
  constexpr std::uint32_t kEdgeStripWidth = 8;
}

SidebarEdgeTrigger::SidebarEdgeTrigger(PanelManager& panels) : m_panels(panels) {}
SidebarEdgeTrigger::~SidebarEdgeTrigger() { destroySurfaces(); }

void SidebarEdgeTrigger::initialize(
    WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext
) {
  m_wayland = &wayland;
  m_config = config;
  m_renderContext = renderContext;
}

void SidebarEdgeTrigger::onConfigReload() { onOutputChange(); }

void SidebarEdgeTrigger::onOutputChange() {
  destroySurfaces();
  if (m_wayland == nullptr || m_config == nullptr || m_renderContext == nullptr
      || !m_config->config().sidebar.enabled) {
    return;
  }

  for (const auto& output : m_wayland->outputs()) {
    if (!output.done || output.output == nullptr || !output.hasUsableGeometry()) {
      continue;
    }

    auto instance = std::make_unique<Instance>();
    instance->output = output.output;
    instance->surface = std::make_unique<LayerSurface>(
        *m_wayland,
        LayerSurfaceConfig{
            .nameSpace = "gnil-sidebar-edge",
            .layer = LayerShellLayer::Top,
            .anchor = LayerShellAnchor::Top | LayerShellAnchor::Right | LayerShellAnchor::Bottom,
            .width = kEdgeStripWidth,
            .height = 0,
            .exclusiveZone = -1,
            .keyboard = LayerShellKeyboard::None,
            .defaultWidth = kEdgeStripWidth,
            .defaultHeight = static_cast<std::uint32_t>(std::max(1, output.effectiveLogicalHeight())),
        }
    );
    if (!instance->surface->initialize(output.output)) {
      continue;
    }

    Instance* current = instance.get();
    auto area = std::make_unique<InputArea>();
    area->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
    area->setSize(static_cast<float>(kEdgeStripWidth), static_cast<float>(std::max(1, output.effectiveLogicalHeight())));
    area->setOnEnter([this, current](const InputArea::PointerData&) {
      if (m_config == nullptr || !m_config->config().sidebar.showOnHover || current->dragging) {
        return;
      }
      const int delay = std::max(0, m_config->config().sidebar.minHoverThresholdMs);
      current->hoverTimer.start(std::chrono::milliseconds(delay), [this, current]() { open(*current); });
    });
    area->setOnLeave([current]() {
      if (!current->dragging) {
        current->hoverTimer.stop();
      }
    });
    area->setOnPress([this, current](const InputArea::PointerData& data) {
      if (data.button != BTN_LEFT) {
        return;
      }
      if (data.pressed) {
        current->hoverTimer.stop();
        current->dragging = true;
        current->thresholdReached = false;
        current->dragStartX = data.localX;
        return;
      }
      const bool shouldOpen = current->dragging && current->thresholdReached;
      current->dragging = false;
      current->thresholdReached = false;
      if (shouldOpen) {
        open(*current);
      }
    });
    area->setOnMotion([this, current](const InputArea::PointerData& data) {
      if (!current->dragging || m_config == nullptr) {
        return;
      }
      current->thresholdReached = sidebar_edge_gesture::revealReached(
          current->dragStartX, data.localX, static_cast<float>(m_config->config().sidebar.dragThreshold)
      );
    });

    instance->sceneRoot = std::move(area);
    instance->inputDispatcher.setSceneRoot(instance->sceneRoot.get());
    instance->surface->setRenderContext(m_renderContext);
    instance->surface->setSceneRoot(instance->sceneRoot.get());
    instance->surface->requestRedraw();
    m_instances.push_back(std::move(instance));
  }
}

void SidebarEdgeTrigger::destroySurfaces() { m_instances.clear(); }

void SidebarEdgeTrigger::open(Instance& instance) {
  instance.hoverTimer.stop();
  if (m_config == nullptr || !m_config->config().sidebar.enabled) {
    return;
  }
  m_panels.openPanel("sidebar", PanelOpenRequest{.output = instance.output, .context = "sidebar"});
}

bool SidebarEdgeTrigger::onPointerEvent(const PointerEvent& event) {
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
