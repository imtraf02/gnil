#include "shell/chrome/chrome_output_host.h"

#include "render/scene/chrome_blob_node.h"
#include "render/scene/node.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string_view>

bool gnilChromeDebugEnabled() noexcept {
  const char* value = std::getenv("GNIL_CHROME_DEBUG");
  return value != nullptr && !std::string_view(value).empty() && std::string_view(value) != "0";
}

ChromeOutputHost::ChromeOutputHost(Node& sceneRoot) : m_debug(gnilChromeDebugEnabled()) {
  auto node = std::make_unique<ChromeBlobNode>();
  node->setHitTestVisible(false);
  node->setParticipatesInLayout(false);
  node->setZIndex(-20); // shadow/chrome below panel and bar content
  m_node = static_cast<ChromeBlobNode*>(sceneRoot.addChild(std::move(node)));
}

void ChromeOutputHost::setSurfaceSize(float width, float height) {
  const float nextWidth = std::max(0.0f, width);
  const float nextHeight = std::max(0.0f, height);
  if (m_width == nextWidth && m_height == nextHeight) {
    return;
  }
  m_width = nextWidth;
  m_height = nextHeight;
  if (m_node != nullptr) {
    m_node->setPosition(0.0f, 0.0f);
    m_node->setFrameSize(m_width, m_height);
  }
  syncNode();
}

void ChromeOutputHost::setGeometrySettings(ChromeGeometrySettings settings) {
  if (m_geometry.settings() == settings) {
    return;
  }
  m_geometry.setSettings(settings);
  syncNode();
}

void ChromeOutputHost::setColors(Color fill, Color shadow) {
  shadow = Color{};
  if (m_fill == fill && m_shadow == shadow) {
    return;
  }
  m_fill = fill;
  m_shadow = shadow;
  syncNode();
}

void ChromeOutputHost::setShadow(float radius, float offsetX, float offsetY) {
  (void)radius;
  (void)offsetX;
  (void)offsetY;
  const float nextRadius = 0.0f;
  offsetX = 0.0f;
  offsetY = 0.0f;
  if (m_shadowRadius == nextRadius && m_shadowOffsetX == offsetX && m_shadowOffsetY == offsetY) {
    return;
  }
  m_shadowRadius = nextRadius;
  m_shadowOffsetX = offsetX;
  m_shadowOffsetY = offsetY;
  syncNode();
}

void ChromeOutputHost::setPanelState(std::optional<ChromePanelState> state) {
  if (m_panel == state) {
    return;
  }
  m_panel = state;
  syncNode();
}

void ChromeOutputHost::setToastStates(std::vector<ChromePanelState> states) {
  if (m_toasts == states) {
    return;
  }
  m_toasts = std::move(states);
  syncNode();
}

void ChromeOutputHost::syncNode() {
  if (m_node == nullptr) {
    return;
  }
  const auto& settings = m_geometry.settings();
  const auto insets = m_geometry.resolvedApertureInsets();
  // A structural join cannot be hard even when decorative blob smoothing is
  // disabled. Tie the fillet to the configured rounding, with conservative
  // bounds for small GNIL frames.
  const float structuralSmoothing =
      std::max(settings.smoothing, std::clamp(settings.rounding, 4.0f, 20.0f));
  ChromeBlobStyle style{
      .fill = m_fill,
      .shadow = m_shadow,
      .apertureInsets = {
          .left = insets.left,
          .top = insets.top,
          .right = insets.right,
          .bottom = insets.bottom,
      },
      .rounding = settings.rounding,
      .smoothing = structuralSmoothing,
      .shadowRadius = m_shadowRadius,
      .shadowOffsetX = m_shadowOffsetX,
      .shadowOffsetY = m_shadowOffsetY,
      .drawFrame = true,
      .debug = m_debug,
  };
  auto appendPanel = [&](const std::optional<ChromePanelState>& panel, bool publishDebugState) {
    if (!panel
        || !panel->visible
        || panel->rect.width <= 0.0f
        || panel->rect.height <= 0.0f
        || style.rectCount >= style.rects.size()) {
      return;
    }
    const ChromePanelState resolved = m_geometry.resolveJoinedShape(*panel, m_width, m_height);
    const auto renderJoinedEdges = [](std::uint8_t edges) {
      std::uint8_t rendered = ChromeBlobJoinEdge::None;
      if ((edges & ChromeJoinEdge::Top) != 0) rendered |= ChromeBlobJoinEdge::Top;
      if ((edges & ChromeJoinEdge::Right) != 0) rendered |= ChromeBlobJoinEdge::Right;
      if ((edges & ChromeJoinEdge::Bottom) != 0) rendered |= ChromeBlobJoinEdge::Bottom;
      if ((edges & ChromeJoinEdge::Left) != 0) rendered |= ChromeBlobJoinEdge::Left;
      return rendered;
    };
    style.rects[style.rectCount++] = ChromeBlobRect{
        .x = resolved.rect.x,
        .y = resolved.rect.y,
        .width = resolved.rect.width,
        .height = resolved.rect.height,
        .radius = std::max(1.0f, resolved.radius),
        .deformation = resolved.deformation,
        .joinedEdges = renderJoinedEdges(resolved.joinedEdges),
        .unionSmoothing = resolved.joinedEdges == ChromeJoinEdge::None ? 0.0f : structuralSmoothing,
    };
    if (!publishDebugState) {
      return;
    }
    style.debugInputRect = ChromeBlobRect{
        .x = panel->rect.x,
        .y = panel->rect.y,
        .width = panel->rect.width,
        .height = panel->rect.height,
        .radius = std::max(1.0f, panel->radius),
        .deformation = panel->deformation,
    };
    style.debugProgress = panel->progress;
    style.debugInputEnabled = panel->inputEnabled;
  };
  appendPanel(m_panel, true);
  for (const ChromePanelState& toast : m_toasts) {
    appendPanel(toast, false);
  }
  m_node->setStyle(style);
}
