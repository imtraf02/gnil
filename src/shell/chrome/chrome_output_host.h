#pragma once

#include "render/core/color.h"
#include "shell/chrome/chrome_geometry.h"

#include <optional>
#include <vector>

class ChromeBlobNode;
class Node;

[[nodiscard]] bool gnilChromeDebugEnabled() noexcept;

// Visual host for one output. It owns exactly one ChromeBlobNode draw pass;
// callers only publish geometry/state and never create independent chrome
// backgrounds or shadows.
class ChromeOutputHost {
public:
  explicit ChromeOutputHost(Node& sceneRoot);

  void setSurfaceSize(float width, float height);
  void setGeometrySettings(ChromeGeometrySettings settings);
  void setColors(Color fill, Color shadow);
  void setShadow(float radius, float offsetX, float offsetY);
  void setPanelState(std::optional<ChromePanelState> state);
  void setToastStates(std::vector<ChromePanelState> states);

  [[nodiscard]] ChromeGeometryModel& geometry() noexcept { return m_geometry; }
  [[nodiscard]] const ChromeGeometryModel& geometry() const noexcept { return m_geometry; }
  [[nodiscard]] const std::optional<ChromePanelState>& panelState() const noexcept { return m_panel; }
  [[nodiscard]] bool debugEnabled() const noexcept { return m_debug; }

private:
  void syncNode();

  ChromeBlobNode* m_node = nullptr;
  ChromeGeometryModel m_geometry;
  std::optional<ChromePanelState> m_panel;
  std::vector<ChromePanelState> m_toasts;
  Color m_fill{};
  Color m_shadow{};
  float m_shadowRadius = 18.0f;
  float m_shadowOffsetX = 0.0f;
  float m_shadowOffsetY = 4.0f;
  float m_width = 0.0f;
  float m_height = 0.0f;
  bool m_debug = false;
};
