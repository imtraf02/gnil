#pragma once

#include "render/core/render_styles.h"
#include "render/scene/node.h"

class ChromeBlobNode final : public Node {
public:
  ChromeBlobNode() : Node(NodeType::ChromeBlob) {}

  [[nodiscard]] const ChromeBlobStyle& style() const noexcept { return m_style; }

  void setStyle(const ChromeBlobStyle& style) {
    if (m_style == style) {
      return;
    }
    m_style = style;
    markPaintDirty();
  }

private:
  ChromeBlobStyle m_style;
};
