#pragma once

#include "ui/builders/common.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/progress_bar.h"
#include "ui/controls/spinner.h"

namespace ui {

  struct LabelProps {
    Label** out = nullptr;
    std::optional<std::string> text = std::nullopt;
    std::optional<float> fontSize = std::nullopt;
    std::optional<FontWeight> fontWeight = std::nullopt;
    std::optional<std::string> fontFamily = std::nullopt;
    std::optional<ColorSpec> color = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> minWidth = std::nullopt;
    std::optional<float> maxWidth = std::nullopt;
    std::optional<int> maxLines = std::nullopt;
    std::optional<TextAlign> textAlign = std::nullopt;
    std::optional<TextEllipsize> ellipsize = std::nullopt;
    std::optional<LabelBaselineMode> baselineMode = std::nullopt;
    std::optional<bool> autoScroll = std::nullopt;
    std::optional<float> autoScrollSpeed = std::nullopt;
    std::optional<bool> autoScrollOnlyWhenHovered = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(Label&)> configure = nullptr;
  };


  struct GlyphProps {
    Glyph** out = nullptr;
    std::optional<std::string> glyph = std::nullopt;
    std::optional<char32_t> codepoint = std::nullopt;
    std::optional<float> glyphSize = std::nullopt;
    std::optional<ColorSpec> color = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(Glyph&)> configure = nullptr;
  };


  struct ImageProps {
    Image** out = nullptr;
    std::optional<ImageFit> fit = std::nullopt;
    std::optional<float> radius = std::nullopt;
    std::optional<float> padding = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(Image&)> configure = nullptr;
  };


  struct SpinnerProps {
    Spinner** out = nullptr;
    std::optional<ColorSpec> color = std::nullopt;
    std::optional<float> spinnerSize = std::nullopt;
    std::optional<float> thickness = std::nullopt;
    std::optional<bool> spinning = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(Spinner&)> configure = nullptr;
  };


  struct ProgressBarProps {
    ProgressBar** out = nullptr;
    std::optional<ColorSpec> fill = std::nullopt;
    std::optional<ColorSpec> track = std::nullopt;
    std::optional<float> radius = std::nullopt;
    std::optional<float> softness = std::nullopt;
    std::optional<ProgressBarOrientation> orientation = std::nullopt;
    std::optional<float> progress = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(ProgressBar&)> configure = nullptr;
  };


  [[nodiscard]] std::unique_ptr<Label> label(LabelProps props);
  [[nodiscard]] std::unique_ptr<Glyph> glyph(GlyphProps props = {});
  [[nodiscard]] std::unique_ptr<Image> image(ImageProps props = {});
  [[nodiscard]] std::unique_ptr<Spinner> spinner(SpinnerProps props = {});
  [[nodiscard]] std::unique_ptr<ProgressBar> progressBar(ProgressBarProps props = {});

} // namespace ui
