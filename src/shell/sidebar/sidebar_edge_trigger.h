#pragma once

#include "core/timer_manager.h"
#include "render/scene/input_dispatcher.h"
#include "wayland/layer_surface.h"

#include <memory>
#include <vector>

class ConfigService;
class PanelManager;
class RenderContext;
class WaylandConnection;
struct PointerEvent;

class SidebarEdgeTrigger {
public:
  explicit SidebarEdgeTrigger(PanelManager& panels);
  ~SidebarEdgeTrigger();

  void initialize(WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext);
  void onConfigReload();
  void onOutputChange();
  bool onPointerEvent(const PointerEvent& event);

private:
  struct Instance {
    wl_output* output = nullptr;
    std::unique_ptr<LayerSurface> surface;
    std::unique_ptr<Node> sceneRoot;
    InputDispatcher inputDispatcher;
    Timer hoverTimer;
    bool dragging = false;
    bool thresholdReached = false;
    float dragStartX = 0.0f;
  };

  void destroySurfaces();
  void open(Instance& instance);

  PanelManager& m_panels;
  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  std::vector<std::unique_ptr<Instance>> m_instances;
};
