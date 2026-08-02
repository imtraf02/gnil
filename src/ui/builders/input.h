#pragma once

#include "ui/builders/common.h"
#include "ui/controls/input.h"
#include "ui/controls/keybind_recorder.h"
#include "ui/controls/range_slider.h"
#include "ui/controls/search_picker.h"
#include "ui/controls/select.h"
#include "ui/controls/slider.h"
#include "ui/controls/stepper.h"

namespace ui {

  struct InputProps {
    Input** out = nullptr;
    std::optional<std::string> value = std::nullopt;
    std::optional<std::string> placeholder = std::nullopt;
    std::optional<float> fontSize = std::nullopt;
    std::optional<float> controlHeight = std::nullopt;
    std::optional<float> horizontalPadding = std::nullopt;
    std::optional<bool> clearButtonEnabled = std::nullopt;
    std::optional<bool> passwordMode = std::nullopt;
    std::optional<bool> invalid = std::nullopt;
    std::optional<bool> frameVisible = std::nullopt;
    std::optional<bool> embeddedOnSolidPrimary = std::nullopt;
    std::optional<FontWeight> fontWeight = std::nullopt;
    std::optional<float> minLayoutWidth = std::nullopt;
    std::optional<TextAlign> textAlign = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<float> surfaceOpacity = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(const std::string&)> onChange = nullptr;
    std::function<void(const std::string&)> onSubmit = nullptr;
    std::function<bool(std::uint32_t, std::uint32_t)> onKeyEvent = nullptr;
    std::function<void()> onFocusLoss = nullptr;
    std::optional<bool> submitOnFocusLoss = std::nullopt;
    std::function<void(Input&)> configure = nullptr;
  };


  struct SelectProps {
    Select** out = nullptr;
    std::optional<std::vector<std::string>> options = std::nullopt;
    std::optional<std::size_t> selectedIndex = std::nullopt;
    std::optional<bool> clearSelection = std::nullopt;
    std::optional<std::string> placeholder = std::nullopt;
    std::optional<float> fontSize = std::nullopt;
    std::optional<float> controlHeight = std::nullopt;
    std::optional<float> horizontalPadding = std::nullopt;
    std::optional<float> glyphSize = std::nullopt;
    std::optional<std::vector<ColorSpec>> optionIndicators = std::nullopt;
    std::optional<std::vector<ColorSwatchPreview>> colorSwatchPreviews = std::nullopt;
    std::optional<bool> notifyOnReselect = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<float> surfaceOpacity = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(std::size_t, std::string_view)> onSelectionChanged = nullptr;
    std::function<void(Select&)> configure = nullptr;
  };


  struct SliderProps {
    Slider** out = nullptr;
    std::optional<double> minValue = std::nullopt;
    std::optional<double> maxValue = std::nullopt;
    std::optional<double> step = std::nullopt;
    std::optional<double> value = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<SliderPresentation> presentation = std::nullopt;
    std::optional<std::string> glyph = std::nullopt;
    std::optional<float> glyphSize = std::nullopt;
    std::optional<float> trackHeight = std::nullopt;
    std::optional<float> thumbSize = std::nullopt;
    std::optional<float> controlHeight = std::nullopt;
    std::optional<std::string> tooltip = std::nullopt;
    std::optional<bool> wheelAdjustEnabled = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(double)> onValueChanged = nullptr;
    std::function<void()> onDragEnd = nullptr;
    std::function<void(Slider&)> configure = nullptr;
  };


  struct RangeSliderProps {
    RangeSlider** out = nullptr;
    std::optional<double> minValue = std::nullopt;
    std::optional<double> maxValue = std::nullopt;
    std::optional<double> step = std::nullopt;
    std::optional<double> lowValue = std::nullopt;
    std::optional<double> highValue = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<float> trackHeight = std::nullopt;
    std::optional<float> thumbSize = std::nullopt;
    std::optional<float> controlHeight = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(double)> onLowChanged = nullptr;
    std::function<void(double)> onHighChanged = nullptr;
    std::function<void()> onDragEnd = nullptr;
    std::function<void(RangeSlider&)> configure = nullptr;
  };

  struct SearchPickerProps {
    SearchPicker** out = nullptr;
    std::optional<std::string> placeholder = std::nullopt;
    std::optional<std::string> emptyText = std::nullopt;
    std::optional<std::string> selectedValue = std::nullopt;
    std::optional<std::vector<SearchPickerOption>> options = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(const SearchPickerOption&)> onActivated = nullptr;
    std::function<void()> onCancel = nullptr;
    std::function<void(SearchPicker&)> configure = nullptr;
  };


  struct StepperProps {
    Stepper** out = nullptr;
    std::optional<int> minValue = std::nullopt;
    std::optional<int> maxValue = std::nullopt;
    std::optional<int> step = std::nullopt;
    std::optional<int> value = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<float> scale = std::nullopt;
    std::optional<std::string> valueSuffix = std::nullopt;
    std::optional<float> surfaceOpacity = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(int)> onValueChanged = nullptr;
    std::function<void(int)> onValueCommitted = nullptr;
    std::function<void(Stepper&)> configure = nullptr;
  };


  struct KeybindRecorderProps {
    KeybindRecorder** out = nullptr;
    std::optional<KeyChord> chord = std::nullopt;
    std::optional<float> scale = std::nullopt;
    std::optional<bool> enabled = std::nullopt;
    std::optional<std::string> unsetPlaceholder = std::nullopt;
    std::optional<std::string> recordingPlaceholder = std::nullopt;
    std::optional<ModifierPolicy> modifierPolicy = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(KeyChord)> onCommit = nullptr;
    std::function<void(KeybindRecorder&)> configure = nullptr;
  };


  [[nodiscard]] std::unique_ptr<Input> input(InputProps props);
  [[nodiscard]] std::unique_ptr<Select> select(SelectProps props);
  [[nodiscard]] std::unique_ptr<Slider> slider(SliderProps props);
  [[nodiscard]] std::unique_ptr<RangeSlider> rangeSlider(RangeSliderProps props);
  [[nodiscard]] std::unique_ptr<SearchPicker> searchPicker(SearchPickerProps props);
  [[nodiscard]] std::unique_ptr<Stepper> stepper(StepperProps props);
  [[nodiscard]] std::unique_ptr<KeybindRecorder> keybindRecorder(KeybindRecorderProps props);

} // namespace ui
