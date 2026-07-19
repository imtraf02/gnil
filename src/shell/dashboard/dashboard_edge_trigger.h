#pragma once

#include "render/scene/input_dispatcher.h"
#include "wayland/layer_surface.h"

#include <memory>
#include <vector>

class ConfigService;
class PanelManager;
class RenderContext;
class WaylandConnection;
struct PointerEvent;

// A narrow, top-centre drag target inspired by Caelestia's dashboard gesture.
// It creates one non-exclusive layer surface per output.
class DashboardEdgeTrigger {
public:
  explicit DashboardEdgeTrigger(PanelManager& panels);
  ~DashboardEdgeTrigger();

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
    bool dragging = false;
    bool thresholdReached = false;
    float dragStartY = 0.0f;
  };

  void destroySurfaces();
  void open(Instance& instance);

  PanelManager& m_panels;
  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  std::vector<std::unique_ptr<Instance>> m_instances;
};
