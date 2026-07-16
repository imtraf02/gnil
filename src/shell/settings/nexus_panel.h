#pragma once

#include "shell/panel/panel.h"
#include "shell/settings/nexus_route.h"
#include "shell/settings/nexus_services.h"
#include "shell/settings/nexus_view.h"

#include <functional>

class NexusPanel final : public Panel {
public:
  NexusPanel(NexusRoute& route, NexusHostCoordinator& coordinator, NexusServices services);

  void setPipCallback(std::function<void(std::string)> callback) { m_pip = std::move(callback); }
  void setCloseWindowCallback(std::function<void()> callback) { m_closeWindow = std::move(callback); }
  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  void onConfigReloaded() override { m_view.refresh(); }
  [[nodiscard]] bool dismissTransientUi() override;
  [[nodiscard]] float preferredWidth() const override { return scaled(1120.0f); }
  [[nodiscard]] float preferredHeight() const override { return scaled(630.0f); }
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override { return PanelPlacement::Floating; }
  [[nodiscard]] ChromeEdge chromeEdge() const noexcept override { return ChromeEdge::Top; }
  [[nodiscard]] InputArea* initialFocusArea() const override;

private:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;

  NexusRoute& m_route;
  NexusHostCoordinator& m_coordinator;
  NexusView m_view;
  std::function<void(std::string)> m_pip;
  std::function<void()> m_closeWindow;
};
