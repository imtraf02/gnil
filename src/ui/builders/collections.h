#pragma once

#include "ui/builders/common.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/controls/virtual_list_view.h"

namespace ui {

  struct ScrollViewProps {
    ScrollView** out = nullptr;
    ScrollViewState* state = nullptr;
    std::optional<bool> scrollbarVisible = std::nullopt;
    std::optional<float> viewportPaddingH = std::nullopt;
    std::optional<float> viewportPaddingV = std::nullopt;
    std::optional<ColorSpec> fill = std::nullopt;
    std::optional<float> radius = std::nullopt;
    std::optional<float> softness = std::nullopt;
    std::optional<float> minWidth = std::nullopt;
    std::optional<float> minHeight = std::nullopt;
    std::optional<bool> fillWidth = std::nullopt;
    std::optional<bool> fillHeight = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(float)> onScrollChanged = nullptr;
    std::function<void(ScrollView&)> configure = nullptr;
  };


  struct VirtualGridViewProps {
    VirtualGridView** out = nullptr;
    std::optional<std::size_t> columns = std::nullopt;
    std::optional<float> minCellWidth = std::nullopt;
    std::optional<float> cellHeight = std::nullopt;
    std::optional<bool> squareCells = std::nullopt;
    std::optional<float> columnGap = std::nullopt;
    std::optional<float> rowGap = std::nullopt;
    std::optional<std::size_t> overscanRows = std::nullopt;
    std::optional<bool> scrollbarVisible = std::nullopt;
    std::optional<float> scrollCardStyleScale = std::nullopt;
    VirtualGridAdapter* adapter = nullptr;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(std::optional<std::size_t>)> onSelectionChanged = nullptr;
    std::function<void(VirtualGridView&)> configure = nullptr;
  };


  struct VirtualListViewProps {
    VirtualListView** out = nullptr;
    std::optional<float> itemGap = std::nullopt;
    std::optional<std::size_t> overscanItems = std::nullopt;
    VirtualListAdapter* adapter = nullptr;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(VirtualListView&)> configure = nullptr;
  };


  [[nodiscard]] std::unique_ptr<ScrollView> scrollView(ScrollViewProps props = {});
  [[nodiscard]] std::unique_ptr<VirtualGridView> virtualGridView(VirtualGridViewProps props);
  [[nodiscard]] std::unique_ptr<VirtualListView> virtualListView(VirtualListViewProps props);

} // namespace ui
