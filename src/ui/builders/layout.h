#pragma once

#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders/common.h"
#include "ui/controls/box.h"
#include "ui/controls/flex.h"
#include "ui/controls/separator.h"
#include "ui/controls/spacer.h"

namespace ui {

  struct NodeProps {
    Node** out = nullptr;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::optional<bool> clipChildren = std::nullopt;
    std::function<void(Node&)> configure = nullptr;
  };


  struct InputAreaProps {
    InputArea** out = nullptr;
    std::optional<std::uint32_t> acceptedButtons = std::nullopt;
    std::optional<std::uint32_t> cursorShape = std::nullopt;
    std::optional<bool> propagateEvents = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<InputArea::HitShape> hitShape = std::nullopt;
    std::optional<bool> focusable = std::nullopt;
    std::optional<std::string> tooltip = std::nullopt;
    std::optional<std::vector<TooltipRow>> tooltipRows = std::nullopt;
    std::function<TooltipContent()> tooltipProvider = nullptr;
    std::optional<std::chrono::milliseconds> tooltipRefreshInterval = std::nullopt;
    std::optional<TooltipPlacement> tooltipPlacement = std::nullopt;
    std::optional<TooltipAnchorInsets> tooltipAnchorInsets = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::optional<bool> clipChildren = std::nullopt;
    std::function<void(const InputArea::PointerData&)> onEnter = nullptr;
    std::function<void()> onLeave = nullptr;
    std::function<void(const InputArea::PointerData&)> onMotion = nullptr;
    std::function<void(const InputArea::PointerData&)> onPress = nullptr;
    std::function<void(const InputArea::PointerData&)> onClick = nullptr;
    std::function<void(const InputArea::PointerData&)> onAxis = nullptr;
    std::function<bool(const InputArea::PointerData&)> onAxisHandler = nullptr;
    std::function<void(const InputArea::KeyData&)> onKeyDown = nullptr;
    std::function<void(const InputArea::KeyData&)> onKeyUp = nullptr;
    std::function<void()> onFocusGain = nullptr;
    std::function<void()> onFocusLoss = nullptr;
    std::function<void(InputArea&)> configure = nullptr;
  };


  struct FlexProps {
    Flex** out = nullptr;
    std::optional<FlexAlign> align = std::nullopt;
    std::optional<FlexJustify> justify = std::nullopt;
    std::optional<bool> wrap = std::nullopt;
    std::optional<float> gap = std::nullopt;
    std::optional<float> padding = std::nullopt;  // uniform; overridden per-axis by paddingV/paddingH
    std::optional<float> paddingV = std::nullopt; // vertical (top+bottom)
    std::optional<float> paddingH = std::nullopt; // horizontal (left+right)
    std::optional<ColorSpec> fill = std::nullopt;
    std::optional<float> radius = std::nullopt;
    std::optional<ColorSpec> border = std::nullopt;
    std::optional<float> borderWidth = std::nullopt; // defaults to 1.0 when `border` is set
    std::optional<float> minWidth = std::nullopt;
    std::optional<float> minHeight = std::nullopt;
    std::optional<float> maxWidth = std::nullopt;
    std::optional<float> maxHeight = std::nullopt;
    std::optional<FlexSizePolicy> widthPolicy = std::nullopt;
    std::optional<FlexSizePolicy> heightPolicy = std::nullopt;
    std::optional<bool> fillWidth = std::nullopt;
    std::optional<bool> fillHeight = std::nullopt;
    std::optional<bool> clipChildren = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(Flex&)> configure = nullptr;
  };


  struct BoxProps {
    Box** out = nullptr;
    std::optional<ColorSpec> fill = std::nullopt;
    std::optional<float> radius = std::nullopt;
    std::optional<float> softness = std::nullopt;
    std::optional<float> cardStyleScale = std::nullopt;
    std::optional<float> cardStyleFillOpacity = std::nullopt;
    std::optional<bool> cardStyleShowBorder = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(Box&)> configure = nullptr;
  };


  struct SeparatorProps {
    Separator** out = nullptr;
    std::optional<ColorSpec> color = std::nullopt;
    std::optional<float> thickness = std::nullopt;
    std::optional<float> spacing = std::nullopt;
    std::optional<SeparatorOrientation> orientation = std::nullopt;
    std::optional<bool> gradientEdges = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(Separator&)> configure = nullptr;
  };


  [[nodiscard]] std::unique_ptr<Flex> flex(FlexDirection direction, FlexProps props);
  [[nodiscard]] std::unique_ptr<InputArea> inputArea(InputAreaProps props = {});
  [[nodiscard]] std::unique_ptr<Node> node(NodeProps props = {});
  [[nodiscard]] std::unique_ptr<Box> box(BoxProps props = {});
  [[nodiscard]] std::unique_ptr<Separator> separator(SeparatorProps props = {});
  [[nodiscard]] std::unique_ptr<Spacer> spacer();

  template <typename... Children> [[nodiscard]] std::unique_ptr<Flex> row(FlexProps props, Children&&... children) {
    auto container = flex(FlexDirection::Horizontal, std::move(props));
    (container->addChild(std::forward<Children>(children)), ...);
    return container;
  }

  template <typename... Children> [[nodiscard]] std::unique_ptr<Flex> column(FlexProps props, Children&&... children) {
    auto container = flex(FlexDirection::Vertical, std::move(props));
    (container->addChild(std::forward<Children>(children)), ...);
    return container;
  }

  template <typename... Children> [[nodiscard]] std::unique_ptr<Node> node(NodeProps props, Children&&... children) {
    auto container = node(std::move(props));
    (container->addChild(std::forward<Children>(children)), ...);
    return container;
  }

  template <typename... Children>
  [[nodiscard]] std::unique_ptr<InputArea> inputArea(InputAreaProps props, Children&&... children) {
    auto container = inputArea(std::move(props));
    (container->addChild(std::forward<Children>(children)), ...);
    return container;
  }

} // namespace ui
