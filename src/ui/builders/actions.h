#pragma once

#include "ui/builders/common.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/radio_button.h"
#include "ui/controls/segmented.h"
#include "ui/controls/toggle.h"

namespace ui {

  struct ButtonProps {
    Button** out = nullptr;
    std::optional<std::string> text = std::nullopt;
    std::optional<std::string> glyph = std::nullopt;
    std::optional<float> fontSize = std::nullopt;
    std::optional<float> glyphSize = std::nullopt;
    std::optional<float> controlHeight = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<bool> selected = std::nullopt;
    std::optional<ButtonContentAlign> contentAlign = std::nullopt;
    std::optional<ButtonVariant> variant = std::nullopt;
    std::optional<Button::ButtonPalette> customPalette = std::nullopt;
    std::optional<float> surfaceOpacity = std::nullopt;
    std::optional<std::string> badge = std::nullopt;
    std::optional<float> badgeFontSize = std::nullopt;
    std::optional<std::string> tooltip = std::nullopt;
    std::optional<float> minWidth = std::nullopt;
    std::optional<float> minHeight = std::nullopt;
    std::optional<float> maxWidth = std::nullopt;
    std::optional<float> maxHeight = std::nullopt;
    std::optional<float> padding = std::nullopt;
    std::optional<float> paddingV = std::nullopt;
    std::optional<float> paddingH = std::nullopt;
    std::optional<float> paddingTop = std::nullopt;
    std::optional<float> paddingRight = std::nullopt;
    std::optional<float> paddingBottom = std::nullopt;
    std::optional<float> paddingLeft = std::nullopt;
    std::optional<float> gap = std::nullopt;
    std::optional<float> radius = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void()> onClick = nullptr;
    std::function<void()> onRightClick = nullptr;
    std::function<void(float, float, bool)> onPress = nullptr;
    std::function<void()> onMotion = nullptr;
    std::function<void(float, float)> onPointerMotion = nullptr;
    std::function<void()> onEnter = nullptr;
    std::function<void()> onLeave = nullptr;
    std::function<void(Button&)> configure = nullptr;
  };


  struct SegmentedOption {
    std::string label;
    std::string glyph;
    std::string tooltip;
  };


  struct SegmentedProps {
    Segmented** out = nullptr;
    std::optional<std::vector<SegmentedOption>> options = std::nullopt;
    std::optional<std::size_t> selectedIndex = std::nullopt;
    std::optional<float> fontSize = std::nullopt;
    std::optional<float> scale = std::nullopt;
    std::optional<bool> compact = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<SegmentedPresentation> presentation = std::nullopt;
    std::optional<float> surfaceOpacity = std::nullopt;
    std::optional<ColorRole> surfaceRole = std::nullopt;
    std::optional<bool> equalSegmentWidths = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(std::size_t)> onChange = nullptr;
    std::function<void(Segmented&)> configure = nullptr;
  };


  struct ToggleProps {
    Toggle** out = nullptr;
    std::optional<bool> checked = std::nullopt;
    std::optional<bool> checkedImmediate = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<ToggleSize> toggleSize = std::nullopt;
    std::optional<float> scale = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(bool)> onChange = nullptr;
    std::function<void(Toggle&)> configure = nullptr;
  };


  struct CheckboxProps {
    Checkbox** out = nullptr;
    std::optional<bool> checked = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<float> scale = std::nullopt;
    std::optional<ColorSpec> checkedFill = std::nullopt;
    std::optional<ColorSpec> checkedBorder = std::nullopt;
    std::optional<ColorSpec> checkedGlyph = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(bool)> onChange = nullptr;
    std::function<void(Checkbox&)> configure = nullptr;
  };


  struct RadioButtonProps {
    RadioButton** out = nullptr;
    std::optional<bool> checked = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<float> scale = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(bool)> onChange = nullptr;
    std::function<void(RadioButton&)> configure = nullptr;
  };


  [[nodiscard]] std::unique_ptr<Button> button(ButtonProps props);
  [[nodiscard]] std::unique_ptr<Segmented> segmented(SegmentedProps props);
  [[nodiscard]] std::unique_ptr<Toggle> toggle(ToggleProps props);
  [[nodiscard]] std::unique_ptr<Checkbox> checkbox(CheckboxProps props);
  [[nodiscard]] std::unique_ptr<RadioButton> radioButton(RadioButtonProps props);

} // namespace ui
