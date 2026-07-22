#pragma once

#include "shell/control_center/control_center_services.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class Flex;
class ScrollView;

struct ContentPanelSpec {
  std::string id;
  float naturalWidth = 400.0f;
  float naturalHeight = 360.0f;
  bool dynamicHeight = true;
  // Fixed utility panels should remain a single, stable viewport. This also
  // prevents a one-pixel intrinsic overflow from turning into a scrollable UI.
  bool scrollable = true;
  std::function<void()> onOpen;
};

// A single-destination panel host. It deliberately contains no tab strip or
// sidebar: the bar icon is the destination selector and the panel only owns
// the content relevant to that icon.
class ContentPanel final : public Panel {
public:
  ContentPanel(ContentPanelSpec spec, std::unique_ptr<Tab> content);

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  void onFrameTick(float deltaMs) override;
  [[nodiscard]] bool dismissTransientUi() override;
  [[nodiscard]] bool deferExternalRefresh() const override;
  [[nodiscard]] bool deferPointerRelayout() const override { return deferExternalRefresh(); }
  [[nodiscard]] float preferredWidth() const override { return scaled(m_spec.naturalWidth); }
  [[nodiscard]] float preferredHeight() const override { return scaled(m_spec.naturalHeight); }
  [[nodiscard]] bool usesDynamicVisualSize() const noexcept override { return m_spec.dynamicHeight; }
  [[nodiscard]] float initialVisualHeight() const override { return preferredHeight(); }
  [[nodiscard]] std::optional<float> desiredVisualHeight(Renderer& renderer, float visualWidth) override;
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override { return PanelPlacement::Attached; }
  [[nodiscard]] LayerShellLayer layer() const override { return LayerShellLayer::Overlay; }

private:
  void onPanelBordersChanged(bool enabled) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;

  ContentPanelSpec m_spec;
  std::unique_ptr<Tab> m_content;
  ScrollView* m_scrollView = nullptr;
  Flex* m_body = nullptr;
};

using NamedContentPanel = std::pair<std::string, std::unique_ptr<Panel>>;

[[nodiscard]] std::vector<NamedContentPanel> makeContentPanels(const ControlCenterServices& services);
[[nodiscard]] bool isContentPanelId(std::string_view id) noexcept;
