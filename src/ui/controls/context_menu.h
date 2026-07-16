#pragma once

#include "render/core/renderer.h"
#include "render/scene/node.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Renderer;

enum class ContextSubmenuDirection : std::uint8_t {
  Right,
  Left,
};

struct ContextMenuControlEntry {
  std::int32_t id = 0;
  std::string label;
  bool enabled = true;
  bool separator = false;
  bool hasSubmenu = false;
  bool checkmark = false;
  bool radio = false;
  std::int32_t toggleState = -1;
  TextEllipsize ellipsize = TextEllipsize::End;
};

class ContextMenuControl : public Node {
public:
  ContextMenuControl();

  void setEntries(std::vector<ContextMenuControlEntry> entries);
  void setMaxVisible(std::size_t maxVisible);
  void setMenuWidth(float width);
  void setContentScale(float scale);
  void setSubmenuDirection(ContextSubmenuDirection direction);
  void setFrameJoin(std::string edge, bool joinsStartFrame = false, bool joinsEndFrame = false);
  // The output chrome host owns the background for bar-attached popups. The
  // popup scene then contains content only, avoiding two independently rounded
  // layers at the join.
  void setExternalChrome(bool external);
  void setOnActivate(std::function<void(const ContextMenuControlEntry&)> onActivate);
  void setOnSubmenuOpen(std::function<void(const ContextMenuControlEntry&, float rowCenterY)> onSubmenuOpen);
  void setRedrawCallback(std::function<void()> redrawCallback);

  [[nodiscard]] float preferredHeight() const;
  [[nodiscard]] static float
  preferredHeight(const std::vector<ContextMenuControlEntry>& entries, std::size_t maxVisible, float scale = 1.0f);

private:
  void doLayout(Renderer& renderer) override;
  void rebuild(Renderer& renderer);
  void rebuildRows(Renderer& renderer);

  std::vector<ContextMenuControlEntry> m_entries;
  std::size_t m_maxVisible = 14;
  float m_menuWidth = 246.0f;
  float m_contentScale = 1.0f;
  ContextSubmenuDirection m_submenuDirection = ContextSubmenuDirection::Right;
  std::string m_frameJoinEdge;
  bool m_joinsStartFrame = false;
  bool m_joinsEndFrame = false;
  bool m_externalChrome = false;
  bool m_needsRebuild = true;
  std::function<void(const ContextMenuControlEntry&)> m_onActivate;
  std::function<void(const ContextMenuControlEntry&, float rowCenterY)> m_onSubmenuOpen;
  std::function<void()> m_redrawCallback;
};
