#include "shell/launcher/launcher_panel.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/animation/animation_manager.h"
#include "render/core/async_texture_cache.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/launcher/launcher_visual_size.h"
#include "shell/panel/panel_manager.h"
#include "shell/wallpaper/wallpaper.h"
#include "shell/wallpaper/cyclic_picker.h"
#include "system/desktop_entry.h"
#include "theme/builtin_palettes.h"
#include "theme/custom_palettes.h"
#include "ui/app_icon_colorization.h"
#include "ui/builders.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/signal.h"
#include "ui/style.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string_view>
#include <tuple>

namespace {

  constexpr std::size_t kRowOverscan = 3;
  // Minimum trimmed query length before prefixed opt-in providers join the global search.
  constexpr std::size_t kGlobalOptInMinChars = 2;
  constexpr float kIconSizeDefault = 40.0f;
  constexpr float kIconSizeCompact = 28.0f;
  constexpr std::size_t kAppGridColumns = 5;
  constexpr std::string_view kApplicationsProviderId = "Applications";
  constexpr std::string_view kWallpaperProviderId = "Wallpaper";
  constexpr std::string_view kLiveWallpaperProviderId = "LiveWallpaper";
  constexpr double kUsageScorePerCount = 0.1;
  constexpr double kTypedUsageScoreCap = 0.5;
  constexpr std::string_view kProviderOverviewProviderId = "__launcher_provider_overview__";
  constexpr std::string_view kProviderOverviewResultPrefix = "provider:";
  constexpr std::string_view kCommandProviderId = "__command__";
  constexpr std::string_view kCommandResultPrefix = "command:";
  constexpr std::string_view kCommandVariantPrefix = "variant:";
  constexpr std::string_view kCommandSchemePrefix = "scheme:";

  double usageBoostForScore(double score, int usageCount, bool typedQuery) {
    if (usageCount <= 0) {
      return 0.0;
    }

    const double rawBoost = static_cast<double>(usageCount) * kUsageScorePerCount;
    if (!typedQuery) {
      return rawBoost;
    }
    if (!FuzzyMatch::isMatch(score)) {
      return 0.0;
    }

    // For typed searches, usage should nudge close matches without letting a
    // weak fuzzy hit outrank a much stronger lexical match.
    return std::min(rawBoost, kTypedUsageScoreCap);
  }

  [[nodiscard]] bool startsWithSlash(std::string_view text) { return !text.empty() && text.front() == '/'; }

  [[nodiscard]] bool favouriteMatches(const std::string& configured, const DesktopEntry& entry) {
    const std::string needle = StringUtils::toLower(configured);
    if (needle.empty()) {
      return false;
    }
    return needle == StringUtils::toLower(entry.id)
        || needle == StringUtils::toLower(entry.path)
        || (!entry.startupWmClass.empty() && needle == StringUtils::toLower(entry.startupWmClass));
  }

  [[nodiscard]] bool isFavourite(const std::vector<std::string>& favourites, const DesktopEntry& entry) {
    return std::ranges::any_of(favourites, [&entry](const std::string& value) {
      return favouriteMatches(value, entry);
    });
  }

  [[nodiscard]] bool isDescendantOf(const Node* node, const Node* ancestor) {
    if (node == nullptr || ancestor == nullptr) {
      return false;
    }
    for (const Node* current = node; current != nullptr; current = current->parent()) {
      if (current == ancestor) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] std::string singleLinePreview(std::string_view text) {
    std::string preview;
    preview.reserve(text.size());
    bool lastWasSpace = false;
    for (const char c : text) {
      const bool whitespace = c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\v';
      if (whitespace) {
        if (!lastWasSpace) {
          preview.push_back(' ');
          lastWasSpace = true;
        }
        continue;
      }
      preview.push_back(c);
      lastWasSpace = c == ' ';
    }
    return preview;
  }

  [[nodiscard]] bool isDetailPresentation(const LauncherResult& result) { return result.presentation == "detail"; }

  [[nodiscard]] std::string providerOverviewId(std::string_view prefix) {
    std::string id(kProviderOverviewResultPrefix);
    id += prefix;
    return id;
  }

  void sortResultsByScore(std::vector<LauncherResult>& results) {
    std::ranges::stable_sort(results, std::ranges::greater{}, &LauncherResult::score);
  }

  struct LauncherListStyle {
    float scale = 1.0f;
    bool showIcons = true;
    bool compact = false;
    std::optional<ColorSpec> appIconColorizeTint;
  };

  [[nodiscard]] float launcherIconSize(const LauncherListStyle& style) {
    return (style.compact ? kIconSizeCompact : kIconSizeDefault) * style.scale;
  }

  [[nodiscard]] float stableLabelHeight(const TextMetrics& metrics) { return std::round(metrics.bottom - metrics.top); }

  [[nodiscard]] float launcherTextStackHeight(Renderer& renderer, const LauncherListStyle& style) {
    const float bodySize = Style::fontSizeBody * style.scale;
    float textHeight = stableLabelHeight(renderer.measureFont(bodySize, FontWeight::SemiBold));
    if (!style.compact) {
      const float captionSize = Style::fontSizeCaption * style.scale;
      textHeight += stableLabelHeight(renderer.measureFont(captionSize, FontWeight::Normal));
    }
    return textHeight;
  }

  [[nodiscard]] float launcherRowHeight(Renderer& renderer, const LauncherListStyle& style) {
    const float paddingY = (style.compact ? Style::spaceXs * 0.5f : Style::spaceXs) * style.scale;
    const float textHeight = launcherTextStackHeight(renderer, style);
    if (!style.showIcons) {
      return std::ceil(textHeight + paddingY * 2.0f);
    }
    return std::ceil(std::max(launcherIconSize(style), textHeight) + paddingY * 2.0f);
  }

  [[nodiscard]] float launcherRowHeightEstimate(const LauncherListStyle& style) {
    const float paddingY = (style.compact ? Style::spaceXs * 0.5f : Style::spaceXs) * style.scale;
    const float bodySize = Style::fontSizeBody * style.scale;
    const float captionSize = Style::fontSizeCaption * style.scale;
    const float textHeight = bodySize + (style.compact ? 0.0f : captionSize);
    if (!style.showIcons) {
      return std::ceil(textHeight + paddingY * 2.0f);
    }
    return std::ceil(std::max(launcherIconSize(style), textHeight) + paddingY * 2.0f);
  }

  [[nodiscard]] float launcherAppGridLabelHeight(Renderer& renderer, const LauncherListStyle& style, float wrapWidth) {
    const float fontSize = Style::fontSizeCaption * style.scale;
    const TextMetrics metrics =
        renderer.measureText("Ag\nyg", fontSize, FontWeight::Normal, wrapWidth, 2, TextAlign::Center);
    const float actualHeight = metrics.bottom - metrics.top;
    const float inkSpan = std::max(0.0f, metrics.inkBottom - metrics.inkTop);
    const float rowExtent = renderer.fontRowExtent(fontSize, FontWeight::Normal);
    return std::ceil(std::max({actualHeight, inkSpan, rowExtent * 2.0f}));
  }

  [[nodiscard]] float launcherAppGridCellHeight(Renderer& renderer, const LauncherListStyle& style, float wrapWidth) {
    const float paddingY = Style::spaceSm * style.scale;
    const float gap = Style::spaceXs * style.scale;
    const float iconSize = launcherIconSize(style);
    const float labelHeight = launcherAppGridLabelHeight(renderer, style, wrapWidth);
    return std::ceil(paddingY * 2.0f + iconSize + gap + labelHeight);
  }

  [[nodiscard]] float launcherAppGridCellHeightEstimate(const LauncherListStyle& style) {
    const float paddingY = Style::spaceSm * style.scale;
    const float gap = Style::spaceXs * style.scale;
    const float iconSize = launcherIconSize(style);
    const float labelHeight = Style::fontSizeCaption * style.scale * 2.4f;
    return std::ceil(paddingY * 2.0f + iconSize + gap + labelHeight);
  }

  [[nodiscard]] float wallpaperCardImageHeight(float width) { return std::round(width * 9.0f / 16.0f); }

  [[nodiscard]] LauncherListStyle launcherListStyleFrom(const ConfigService* config, float scale) {
    LauncherListStyle style{.scale = scale, .appIconColorizeTint = std::nullopt};
    if (config != nullptr) {
      const auto& launcher = config->config().shell.launcher;
      style.showIcons = launcher.showIcons;
      style.compact = launcher.compact;
      style.appIconColorizeTint = effectiveShellAppIconColorizationTint(config->config().shell);
    }
    return style;
  }

  class LauncherResultRow final : public Node {
  public:
    LauncherResultRow(LauncherListStyle style, AsyncTextureCache* asyncTextures)
        : m_style(style), m_asyncTextures(asyncTextures) {
      const float iconSize = launcherIconSize(m_style);
      const float gap = (m_style.compact ? Style::spaceSm : Style::spaceMd) * m_style.scale;
      const float paddingV = (m_style.compact ? Style::spaceXs * 0.5f : Style::spaceXs) * m_style.scale;
      auto row = ui::row(
          {.out = &m_row,
           .align = FlexAlign::Center,
           .gap = gap,
           .paddingV = paddingV,
           .paddingH = Style::spaceSm * m_style.scale,
           .radius = Style::scaledRadiusMd(m_style.scale)}
      );
      addChild(std::move(row));

      m_row->addChild(
          ui::label({
              .out = &m_badgeLabel,
              .fontSize = iconSize,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .visible = false,
          })
      );

      m_row->addChild(
          ui::image({
              .out = &m_image,
              .width = iconSize,
              .height = iconSize,
              .visible = false,
          })
      );

      m_row->addChild(
          ui::glyph({
              .out = &m_glyph,
              .glyphSize = iconSize,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .visible = false,
          })
      );

      m_image->setAsyncReadyCallback([this]() {
        if (!m_style.showIcons
            || m_badgeVisible
            || m_iconPath.empty()
            || m_image == nullptr
            || m_glyph == nullptr
            || !m_image->hasImage()) {
          return;
        }
        m_image->setVisible(true);
        m_glyph->setVisible(false);
      });

      m_row->addChild(
          ui::column(
              {
                  .out = &m_textCol,
                  .align = FlexAlign::Start,
                  .gap = 0.0f,
                  .flexGrow = 1.0f,
              },
              ui::label({
                  .out = &m_title,
                  .fontSize = Style::fontSizeBody * m_style.scale,
                  .fontWeight = FontWeight::SemiBold,
                  .color = colorSpecFromRole(ColorRole::OnSurface),
                  .maxLines = 1,
                  .baselineMode = LabelBaselineMode::TextFixedHeight,
              }),
              ui::label({
                  .out = &m_subtitle,
                  .fontSize = Style::fontSizeCaption * m_style.scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .maxLines = 1,
                  .baselineMode = LabelBaselineMode::TextFixedHeight,
              })
          )
      );
    }

    void setListStyle(LauncherListStyle style) { m_style = style; }

    void
    bind(Renderer& renderer, const LauncherResult& result, float width, float height, bool selected, bool hovered) {
      m_selected = selected;
      m_hovered = hovered;
      m_available = result.available;
      m_iconPath = result.iconPath;
      m_fallbackGlyph = result.glyphName.empty() ? "app-window" : result.glyphName;
      const float iconSize = launcherIconSize(m_style);
      m_iconTargetSize = static_cast<int>(std::round(iconSize));
      m_badgeVisible = !result.badge.empty();
      m_rowHeight = height;

      setSize(width, height);
      m_row->setFrameSize(width, height);

      m_badgeLabel->setVisible(false);
      m_badgeLabel->setParticipatesInLayout(false);
      m_image->setVisible(false);
      m_image->setParticipatesInLayout(false);
      m_glyph->setVisible(false);
      m_glyph->setParticipatesInLayout(false);

      const bool showAppIcon = m_style.showIcons && !m_badgeVisible;
      const bool showLeadingVisual = m_badgeVisible || showAppIcon;
      if (m_badgeVisible) {
        m_badgeLabel->setText(singleLinePreview(result.badge));
        m_badgeLabel->setSize(iconSize, iconSize);
        m_badgeLabel->setVisible(true);
        m_badgeLabel->setParticipatesInLayout(true);
        m_image->clear(renderer);
      } else if (showAppIcon) {
        m_image->setParticipatesInLayout(true);
        m_glyph->setParticipatesInLayout(true);
        if (!m_iconPath.empty()) {
          const bool ready = refreshAsyncIcon(renderer);
          m_image->setVisible(ready);
          m_glyph->setGlyph(m_fallbackGlyph);
          m_glyph->setVisible(!ready);
        } else {
          m_image->clear(renderer);
          m_glyph->setGlyph(m_fallbackGlyph);
          m_glyph->setVisible(true);
        }
      } else {
        m_image->clear(renderer);
      }

      const float gap = (m_style.compact ? Style::spaceSm : Style::spaceMd) * m_style.scale;
      const float horizontalPad = Style::spaceSm * m_style.scale * 2.0f;
      const float leadingWidth = showLeadingVisual ? iconSize + gap : 0.0f;
      const float textWidth = std::max(0.0f, width - leadingWidth - horizontalPad);
      m_title->setText(singleLinePreview(result.title));
      m_title->setMaxWidth(textWidth);

      const bool showSubtitle = !m_style.compact && !result.subtitle.empty();
      if (!showSubtitle) {
        m_subtitle->setVisible(false);
        m_subtitle->setText("");
      } else {
        m_subtitle->setVisible(true);
        m_subtitle->setText(singleLinePreview(result.subtitle));
        m_subtitle->setMaxWidth(textWidth);
      }

      applyVisualState();
    }

    bool refreshAsyncIcon(Renderer& renderer) {
      if (!m_style.showIcons || m_badgeVisible || m_iconPath.empty()) {
        m_image->setVisible(false);
        m_glyph->setVisible(false);
        return false;
      }

      m_image->setAppIconColorization(m_style.appIconColorizeTint);

      bool ready = false;
      if (m_asyncTextures != nullptr) {
        ready = m_image->setSourceFileAsync(renderer, *m_asyncTextures, m_iconPath, m_iconTargetSize, true);
      } else {
        ready = m_image->setSourceFile(renderer, m_iconPath, m_iconTargetSize, true);
      }

      m_image->setSize(launcherIconSize(m_style), launcherIconSize(m_style));
      m_image->setVisible(ready);
      m_glyph->setGlyph(m_fallbackGlyph);
      m_glyph->setVisible(!ready);
      return ready;
    }

  protected:
    void doLayout(Renderer& renderer) override {
      if (m_style.showIcons && !m_badgeVisible && !m_iconPath.empty()) {
        (void)refreshAsyncIcon(renderer);
      }
      Node::doLayout(renderer);
    }

  private:
    void applyVisualState() {
      if (m_selected) {
        m_row->setFill(colorSpecFromRole(ColorRole::Primary));
      } else if (m_hovered) {
        m_row->setFill(colorSpecFromRole(ColorRole::Hover));
      } else {
        m_row->setFill(rgba(0, 0, 0, 0));
      }

      const auto activeRole = m_selected ? ColorRole::OnPrimary : ColorRole::OnHover;
      const bool active = m_selected || m_hovered;
      const ColorSpec foreground = m_available
          ? colorSpecFromRole(active ? activeRole : ColorRole::OnSurface)
          : colorSpecFromRole(active ? activeRole : ColorRole::OnSurfaceVariant, 0.45f);
      const ColorSpec mutedForeground =
          active ? colorSpecFromRole(activeRole, 0.7f) : colorSpecFromRole(ColorRole::OnSurfaceVariant);
      m_badgeLabel->setColor(foreground);
      m_glyph->setColor(foreground);
      m_title->setColor(foreground);
      m_subtitle->setColor(mutedForeground);
    }

    LauncherListStyle m_style{};
    float m_rowHeight = 0.0f;
    bool m_selected = false;
    bool m_hovered = false;
    bool m_available = true;
    Flex* m_row = nullptr;
    Label* m_badgeLabel = nullptr;
    Image* m_image = nullptr;
    Glyph* m_glyph = nullptr;
    Flex* m_textCol = nullptr;
    Label* m_title = nullptr;
    Label* m_subtitle = nullptr;
    AsyncTextureCache* m_asyncTextures = nullptr;
    std::string m_iconPath;
    std::string m_fallbackGlyph;
    int m_iconTargetSize = 0;
    bool m_badgeVisible = false;
  };

  class LauncherAppGridTile final : public Node {
  public:
    LauncherAppGridTile(LauncherListStyle style, AsyncTextureCache* asyncTextures)
        : m_style(style), m_asyncTextures(asyncTextures) {
      const float gap = Style::spaceXs * m_style.scale;
      const float padding = Style::spaceSm * m_style.scale;
      auto col = ui::column({
          .out = &m_col,
          .align = FlexAlign::Center,
          .gap = gap,
          .paddingV = padding,
          .paddingH = padding,
          .radius = Style::scaledRadiusMd(m_style.scale),
          .fillWidth = true,
          .fillHeight = true,
      });
      addChild(std::move(col));

      m_col->addChild(
          ui::image({
              .out = &m_image,
              .visible = false,
          })
      );

      m_col->addChild(
          ui::glyph({
              .out = &m_glyph,
              .glyphSize = launcherIconSize(m_style),
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .visible = false,
          })
      );

      m_image->setAsyncReadyCallback([this]() {
        if ((!m_style.showIcons && !m_wallpaperCard)
            || m_iconPath.empty()
            || m_image == nullptr
            || m_glyph == nullptr
            || !m_image->hasImage()) {
          return;
        }
        m_image->setVisible(true);
        m_glyph->setVisible(false);
      });

      m_col->addChild(
          ui::label({
              .out = &m_title,
              .fontSize = Style::fontSizeCaption * m_style.scale,
              .fontWeight = FontWeight::Normal,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 2,
              .configure = [](Label& label) { label.setTextAlign(TextAlign::Center); },
          })
      );
    }

    void setListStyle(LauncherListStyle style) { m_style = style; }

    void
    bind(Renderer& renderer, const LauncherResult& result, float width, float height, bool selected, bool hovered) {
      m_selected = selected;
      m_hovered = hovered;
      m_iconPath = result.iconPath;
      m_fallbackGlyph = result.glyphName.empty() ? "app-window" : result.glyphName;
      m_wallpaperCard = result.providerId == kWallpaperProviderId && !result.iconPath.empty();
      const float iconSize = launcherIconSize(m_style);
      const float padding = Style::spaceSm * m_style.scale;
      const float wallpaperWidth = std::max(0.0f, width - padding * 2.0f);
      const float visualWidth = m_wallpaperCard ? wallpaperWidth : iconSize;
      const float visualHeight = m_wallpaperCard ? wallpaperCardImageHeight(wallpaperWidth) : iconSize;
      m_iconTargetSize = static_cast<int>(std::round(std::max(visualWidth, visualHeight)));

      setSize(width, height);
      m_col->setSize(width, height);

      m_image->setVisible(false);
      m_image->setParticipatesInLayout(false);
      m_glyph->setVisible(false);
      m_glyph->setParticipatesInLayout(false);

      const bool showVisual = m_style.showIcons || m_wallpaperCard;
      if (showVisual) {
        m_image->setParticipatesInLayout(true);
        m_glyph->setParticipatesInLayout(true);
        m_image->setSize(visualWidth, visualHeight);
        m_image->setRadius(m_wallpaperCard ? Style::scaledRadiusMd(m_style.scale) : 0.0f);
        m_image->setFit(m_wallpaperCard ? ImageFit::Cover : ImageFit::Contain);
        m_glyph->setGlyphSize(iconSize);
        if (!m_iconPath.empty()) {
          const bool ready = refreshAsyncIcon(renderer);
          m_image->setVisible(ready);
          m_glyph->setGlyph(m_fallbackGlyph);
          m_glyph->setVisible(!ready);
        } else {
          m_image->clear(renderer);
          m_glyph->setGlyph(m_fallbackGlyph);
          m_glyph->setVisible(true);
        }
      } else {
        m_image->clear(renderer);
      }

      const float horizontalPad = Style::spaceSm * m_style.scale * 2.0f;
      const float textWidth = std::max(0.0f, width - horizontalPad);
      m_title->setText(singleLinePreview(result.title));
      m_title->setMaxWidth(textWidth);

      applyVisualState();
    }

    bool refreshAsyncIcon(Renderer& renderer) {
      if ((!m_style.showIcons && !m_wallpaperCard) || m_iconPath.empty()) {
        m_image->setVisible(false);
        m_glyph->setVisible(false);
        return false;
      }

      m_image->setAppIconColorization(m_wallpaperCard ? std::nullopt : m_style.appIconColorizeTint);

      bool ready = false;
      if (m_asyncTextures != nullptr) {
        ready = m_image->setSourceFileAsync(renderer, *m_asyncTextures, m_iconPath, m_iconTargetSize, true);
      } else {
        ready = m_image->setSourceFile(renderer, m_iconPath, m_iconTargetSize, true);
      }

      const float padding = Style::spaceSm * m_style.scale;
      const float wallpaperWidth = std::max(0.0f, width() - padding * 2.0f);
      const float iconSize = launcherIconSize(m_style);
      const float visualWidth = m_wallpaperCard ? wallpaperWidth : iconSize;
      const float visualHeight = m_wallpaperCard ? wallpaperCardImageHeight(wallpaperWidth) : iconSize;
      m_image->setSize(visualWidth, visualHeight);
      m_image->setVisible(ready);
      m_glyph->setGlyph(m_fallbackGlyph);
      m_glyph->setVisible(!ready);
      return ready;
    }

  protected:
    void doLayout(Renderer& renderer) override {
      m_col->setSize(width(), height());
      if ((m_style.showIcons || m_wallpaperCard) && !m_iconPath.empty()) {
        (void)refreshAsyncIcon(renderer);
      }
      Node::doLayout(renderer);
    }

  private:
    void applyVisualState() {
      if (m_selected) {
        m_col->setFill(colorSpecFromRole(ColorRole::Primary));
      } else if (m_hovered) {
        m_col->setFill(colorSpecFromRole(ColorRole::Hover));
      } else {
        m_col->setFill(rgba(0, 0, 0, 0));
      }

      const auto activeRole = m_selected ? ColorRole::OnPrimary : ColorRole::OnHover;
      const bool active = m_selected || m_hovered;
      const ColorSpec foreground = colorSpecFromRole(active ? activeRole : ColorRole::OnSurface);
      m_glyph->setColor(foreground);
      m_title->setColor(foreground);
    }

    LauncherListStyle m_style{};
    bool m_selected = false;
    bool m_hovered = false;
    Flex* m_col = nullptr;
    Image* m_image = nullptr;
    Glyph* m_glyph = nullptr;
    Label* m_title = nullptr;
    AsyncTextureCache* m_asyncTextures = nullptr;
    std::string m_iconPath;
    std::string m_fallbackGlyph;
    int m_iconTargetSize = 0;
    bool m_wallpaperCard = false;
  };

} // namespace

class WallpaperCarousel final : public Node {
public:
  explicit WallpaperCarousel(AsyncTextureCache* asyncTextures) : m_asyncTextures(asyncTextures) {
    setClipChildren(true);
    setFlexGrow(1.0f);
  }

  void setResults(const std::vector<LauncherResult>* results) { m_results = results; }

  void setSelectedIndex(std::size_t index) {
    if (m_results == nullptr || m_results->empty()) {
      return;
    }
    const std::size_t next = std::min(index, m_results->size() - 1);
    if (next == m_selectedIndex && m_cycle.count() == m_results->size()) {
      return;
    }
    const std::size_t previous = m_selectedIndex;
    m_selectedIndex = next;
    if (m_cycle.count() != m_results->size()) {
      m_cycle.setCount(m_results->size());
      (void)m_cycle.select(next);
    } else if (next == (previous + 1) % m_results->size()) {
      (void)m_cycle.next();
    } else if (next == (previous + m_results->size() - 1) % m_results->size()) {
      (void)m_cycle.previous();
    } else {
      (void)m_cycle.select(next);
    }
    animateToSelection();
  }

protected:
  void doLayout(Renderer& renderer) override {
    rebuildIfNeeded(renderer);
    layoutCards(renderer);
    Node::doLayout(renderer);
  }

private:
  struct Card {
    Flex* root = nullptr;
    Image* image = nullptr;
    Label* title = nullptr;
    std::string imagePath;
  };

  void rebuildIfNeeded(Renderer& renderer) {
    std::vector<std::string> ids;
    if (m_results != nullptr) {
      ids.reserve(m_results->size());
      for (const auto& result : *m_results) {
        ids.push_back(result.id);
      }
    }
    if (ids == m_resultIds) {
      return;
    }
    while (!children().empty()) {
      (void)removeChild(children().front().get());
    }
    m_cards.clear();
    m_resultIds = std::move(ids);
    m_selectedIndex = std::min(m_selectedIndex, m_resultIds.empty() ? std::size_t{0} : m_resultIds.size() - 1);
    m_positionInitialized = false;
    if (m_results == nullptr) {
      return;
    }

    m_cycle.setCount(m_results->size());
    (void)m_cycle.select(m_selectedIndex);
    for (std::size_t copy = 0; copy < 3; ++copy) {
      for (const auto& result : *m_results) {
        Card card;
        auto root = ui::column({
          .out = &card.root,
          .align = FlexAlign::Center,
          .gap = Style::spaceSm,
          .padding = Style::spaceSm,
          .radius = Style::radiusLg,
      });
        root->addChild(ui::image({.out = &card.image}));
        root->addChild(
          ui::label({
              .out = &card.title,
              .fontSize = Style::fontSizeCaption,
              .fontWeight = FontWeight::Medium,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
              .configure = [](Label& label) { label.setTextAlign(TextAlign::Center); },
          })
        );
        card.imagePath = result.iconPath;
        card.title->setText(singleLinePreview(result.title));
        card.image->setFit(ImageFit::Cover);
        card.image->setRadius(Style::radiusMd);
        card.image->setAsyncReadyCallback([]() { PanelManager::instance().requestRedraw(); });
        if (!card.imagePath.empty()) {
          if (m_asyncTextures != nullptr) {
            (void)card.image->setSourceFileAsync(renderer, *m_asyncTextures, card.imagePath, 640, true);
          } else {
            (void)card.image->setSourceFile(renderer, card.imagePath, 640, true);
          }
        }
        addChild(std::move(root));
        m_cards.push_back(std::move(card));
      }
    }
  }

  [[nodiscard]] float selectedOffset() const {
    return width() * 0.5f - m_cardWidth * 0.5f
        - static_cast<float>(m_cycle.physicalIndex()) * (m_cardWidth + m_gap);
  }

  void animateToSelection() {
    if (m_cards.empty() || width() <= 0.0f) {
      return;
    }
    const float target = selectedOffset();
    if (AnimationManager* animations = animationManager(); animations != nullptr) {
      if (m_slideAnim != 0) {
        animations->cancel(m_slideAnim);
      }
      m_slideAnim = animations->animate(
          m_offsetX, target, Style::animNormal, Easing::EaseOutCubic,
          [this](float offset) {
            m_offsetX = offset;
            applyCardPositions();
          },
          [this]() {
            m_slideAnim = 0;
            if (m_cycle.rebase()) {
              m_offsetX = selectedOffset();
              applyCardPositions();
            }
          }, this
      );
    } else {
      m_offsetX = target;
      applyCardPositions();
    }
  }

  void applyCardPositions() {
    for (std::size_t i = 0; i < m_cards.size(); ++i) {
      auto& card = m_cards[i];
      card.root->setPosition(m_offsetX + static_cast<float>(i) * (m_cardWidth + m_gap), 0.0f);
      const bool selected = i == m_cycle.physicalIndex();
      const bool adjacent = i + 1 == m_cycle.physicalIndex() || i == m_cycle.physicalIndex() + 1;
      card.root->setScale(selected ? 1.0f : 0.8f);
      card.root->setOpacity(selected ? 1.0f : (adjacent ? 0.68f : 0.42f));
    }
  }

  void layoutCards(Renderer& renderer) {
    if (m_cards.empty()) {
      return;
    }
    m_gap = Style::spaceLg;
    m_cardWidth = std::min(460.0f, std::max(220.0f, width() * 0.64f));
    const float imageWidth = std::max(1.0f, m_cardWidth - Style::spaceSm * 2.0f);
    const float imageHeight = wallpaperCardImageHeight(imageWidth);
    const float cardHeight = imageHeight + Style::fontSizeCaption + Style::spaceSm * 3.0f;
    for (auto& card : m_cards) {
      card.root->setSize(m_cardWidth, cardHeight);
      card.image->setSize(imageWidth, imageHeight);
      card.title->setMaxWidth(imageWidth);
      card.root->layout(renderer);
    }
    if (!m_positionInitialized) {
      m_positionInitialized = true;
      m_offsetX = selectedOffset();
    }
    applyCardPositions();
  }

  AsyncTextureCache* m_asyncTextures = nullptr;
  const std::vector<LauncherResult>* m_results = nullptr;
  std::vector<std::string> m_resultIds;
  std::vector<Card> m_cards;
  std::size_t m_selectedIndex = 0;
  CyclicPicker m_cycle;
  float m_cardWidth = 0.0f;
  float m_gap = 0.0f;
  float m_offsetX = 0.0f;
  bool m_positionInitialized = false;
  AnimationManager::Id m_slideAnim = 0;
};

class LauncherResultAdapter final : public VirtualGridAdapter {
public:
  using ActivateCallback = std::function<void(std::size_t)>;
  using SecondaryActivateCallback = std::function<void(std::size_t, float, float)>;

  LauncherResultAdapter(LauncherListStyle style, AsyncTextureCache* cache) : m_style(style), m_cache(cache) {}

  void setListStyle(LauncherListStyle style) { m_style = style; }
  void setResults(const std::vector<LauncherResult>* results) { m_results = results; }
  void setRenderer(Renderer* renderer) { m_renderer = renderer; }
  void setOnActivate(ActivateCallback callback) { m_onActivate = std::move(callback); }
  void setOnSecondaryActivate(SecondaryActivateCallback callback) { m_onSecondaryActivate = std::move(callback); }

  [[nodiscard]] std::size_t itemCount() const override { return m_results == nullptr ? 0u : m_results->size(); }

  [[nodiscard]] std::unique_ptr<Node> createTile() override {
    return std::make_unique<LauncherResultRow>(m_style, m_cache);
  }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (m_renderer == nullptr || m_results == nullptr || index >= m_results->size()) {
      return;
    }
    auto* row = static_cast<LauncherResultRow*>(&tile);
    row->setListStyle(m_style);
    row->bind(*m_renderer, (*m_results)[index], tile.width(), tile.height(), selected, hovered);
  }

  void onActivate(std::size_t index) override {
    if (m_onActivate) {
      m_onActivate(index);
    }
  }

  void onSecondaryActivate(std::size_t index, float anchorX, float anchorY) override {
    if (m_onSecondaryActivate) {
      m_onSecondaryActivate(index, anchorX, anchorY);
    }
  }

private:
  LauncherListStyle m_style{};
  AsyncTextureCache* m_cache = nullptr;
  Renderer* m_renderer = nullptr;
  const std::vector<LauncherResult>* m_results = nullptr;
  ActivateCallback m_onActivate;
  SecondaryActivateCallback m_onSecondaryActivate;
};

class LauncherAppGridAdapter final : public VirtualGridAdapter {
public:
  using ActivateCallback = std::function<void(std::size_t)>;
  using SecondaryActivateCallback = std::function<void(std::size_t, float, float)>;

  LauncherAppGridAdapter(LauncherListStyle style, AsyncTextureCache* cache) : m_style(style), m_cache(cache) {}

  void setListStyle(LauncherListStyle style) { m_style = style; }
  void setResults(const std::vector<LauncherResult>* results) { m_results = results; }
  void setRenderer(Renderer* renderer) { m_renderer = renderer; }
  void setOnActivate(ActivateCallback callback) { m_onActivate = std::move(callback); }
  void setOnSecondaryActivate(SecondaryActivateCallback callback) { m_onSecondaryActivate = std::move(callback); }

  [[nodiscard]] std::size_t itemCount() const override { return m_results == nullptr ? 0u : m_results->size(); }

  [[nodiscard]] std::unique_ptr<Node> createTile() override {
    return std::make_unique<LauncherAppGridTile>(m_style, m_cache);
  }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (m_renderer == nullptr || m_results == nullptr || index >= m_results->size()) {
      return;
    }
    auto* gridTile = static_cast<LauncherAppGridTile*>(&tile);
    gridTile->setListStyle(m_style);
    gridTile->bind(*m_renderer, (*m_results)[index], tile.width(), tile.height(), selected, hovered);
  }

  void onActivate(std::size_t index) override {
    if (m_onActivate) {
      m_onActivate(index);
    }
  }

  void onSecondaryActivate(std::size_t index, float anchorX, float anchorY) override {
    if (m_onSecondaryActivate) {
      m_onSecondaryActivate(index, anchorX, anchorY);
    }
  }

private:
  LauncherListStyle m_style{};
  AsyncTextureCache* m_cache = nullptr;
  Renderer* m_renderer = nullptr;
  const std::vector<LauncherResult>* m_results = nullptr;
  ActivateCallback m_onActivate;
  SecondaryActivateCallback m_onSecondaryActivate;
};

LauncherPanel::LauncherPanel(ConfigService* config, AsyncTextureCache* asyncTextures, Wallpaper* wallpaper)
    : m_config(config), m_asyncTextures(asyncTextures), m_wallpaper(wallpaper) {
  syncUsageTrackingState();
}

LauncherPanel::~LauncherPanel() = default;

PanelPlacement LauncherPanel::panelPlacement() const noexcept { return PanelPlacement::Floating; }

void LauncherPanel::addProvider(std::unique_ptr<LauncherProvider> provider) {
  provider->initialize();
  provider->setResultsChangedCallback([this]() { onProviderResultsChanged(); });
  provider->setQueryRequestedCallback([this](std::string query) { setQuery(std::move(query)); });
  LauncherProvider* providerPtr = provider.get();
  provider->setActivationDoneCallback([this, providerPtr](const std::string& resultId) {
    if (shouldTrackUsage() && providerPtr->trackUsage()) {
      m_usageTracker.record(providerPtr->id(), resultId);
    }
    PanelManager::instance().closePanel(false);
  });
  m_providers.push_back(std::move(provider));
}

void LauncherPanel::clearProvidersWithIdPrefix(std::string_view prefix) {
  std::erase_if(m_providers, [&](const std::unique_ptr<LauncherProvider>& provider) {
    return provider->id().starts_with(prefix);
  });
}

void LauncherPanel::setScopedProvider(std::string_view providerId, std::string_view placeholder) {
  m_scopedProviderId = providerId;
  m_scopedPlaceholder = placeholder;
  if (m_input != nullptr) {
    m_input->setPlaceholder(
        m_scopedPlaceholder.empty() ? i18n::tr("launcher.search-placeholder") : m_scopedPlaceholder
    );
  }
}

void LauncherPanel::create() {
  m_launcherRowHeight = 0.0f;
  const float scale = contentScale();
  auto container = ui::column({
      .out = &m_container,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });

  auto input = ui::input({
      .out = &m_input,
      .placeholder = m_scopedPlaceholder.empty() ? "Type \"/\" for commands" : m_scopedPlaceholder,
      .fontSize = Style::fontSizeBody * scale,
      .controlHeight = Style::controlHeight * scale,
      .horizontalPadding = Style::spaceMd * scale,
      .clearButtonEnabled = true,
      .surfaceOpacity = panelCardOpacity(),
      .onChange =
          [this](const std::string& text) {
            onInputChanged(text);
            if (m_input == nullptr) {
              return;
            }
            const std::string preview = singleLinePreview(text);
            if (preview != text) {
              m_input->setValue(preview);
            }
          },
      .onSubmit = [this](const std::string& /*text*/) { activateSelected(); },
      .onKeyEvent = [this](std::uint32_t sym, std::uint32_t modifiers) { return handleKeyEvent(sym, modifiers); },
      .configure =
          [](Input& search) {
            // Caelestia SearchBar: stable m3surfaceContainer fill, no focus
            // outline, and a fully rounded pill at the bottom of the drawer.
            search.setFlatSurfaceFrame(true);
            search.setFrameRadius(Style::controlHeight * 0.5f);
          },
  });

  auto categoryFilter = ui::segmented({
      .out = &m_categoryFilter,
      .scale = scale,
      .compact = true,
      .surfaceOpacity = panelCardOpacity(),
      .equalSegmentWidths = true,
      .visible = false,
      .participatesInLayout = false,
      .configure = [](Segmented& segmented) { segmented.setAlign(FlexAlign::Center); },
  });

  auto wallpaperModeTab = ui::segmented({
      .out = &m_wallpaperModeTab,
      .scale = scale,
      .compact = true,
      .surfaceOpacity = panelCardOpacity(),
      .equalSegmentWidths = true,
      .visible = false,
      .participatesInLayout = false,
      .configure = [this](Segmented& segmented) {
        segmented.setAlign(FlexAlign::Center);
        segmented.addOption("Images", "image");
        segmented.addOption("Videos", "smart_display");
        segmented.setSelectedIndex(0);
        segmented.setOnChange([this](std::size_t /*idx*/) {
          m_selectedIndex = 0;
          reapplyCurrentQuery();
        });
      },
  });

  auto body = ui::column({
      .out = &m_body,
      .align = FlexAlign::Stretch,
      .fillWidth = true,
      .flexGrow = 1.0f,
  });

  const LauncherListStyle initialStyle = launcherListStyleFrom(m_config, scale);
  m_listAdapter = std::make_unique<LauncherResultAdapter>(initialStyle, m_asyncTextures);
  m_gridAdapter = std::make_unique<LauncherAppGridAdapter>(initialStyle, m_asyncTextures);
  m_listAdapter->setResults(&m_results);
  m_gridAdapter->setResults(&m_results);
  const auto onActivate = [this](std::size_t index) { activateAt(index); };
  const auto onSecondaryActivate = [this](std::size_t index, float ax, float ay) { openAppActionsMenu(index, ax, ay); };
  m_listAdapter->setOnActivate(onActivate);
  m_listAdapter->setOnSecondaryActivate(onSecondaryActivate);
  m_gridAdapter->setOnActivate(onActivate);
  m_gridAdapter->setOnSecondaryActivate(onSecondaryActivate);

  body->addChild(
      ui::virtualGridView({
          .out = &m_grid,
          .columns = 1,
          .cellHeight = launcherRowHeightEstimate(initialStyle),
          .squareCells = false,
          .columnGap = 0.0f,
          .rowGap = Style::spaceXs * scale,
          .overscanRows = kRowOverscan,
          .adapter = m_listAdapter.get(),
          .flexGrow = 1.0f,
          .onSelectionChanged =
              [this](std::optional<std::size_t> idx) {
                if (idx.has_value() && *idx < m_results.size()) {
                  m_selectedIndex = *idx;
                }
              },
          .configure = [](VirtualGridView& grid) { grid.setFillWidth(true); },
      })
  );

  auto wallpaperCarousel = std::make_unique<WallpaperCarousel>(m_asyncTextures);
  m_wallpaperCarousel = wallpaperCarousel.get();
  wallpaperCarousel->setVisible(false);
  wallpaperCarousel->setParticipatesInLayout(false);
  body->addChild(std::move(wallpaperCarousel));

  auto detailScroll = ui::scrollView({
      .out = &m_detailScroll,
      .scrollbarVisible = true,
      .viewportPaddingH = Style::spaceSm * scale,
      .viewportPaddingV = Style::spaceSm * scale,
      .flexGrow = 1.0f,
      .visible = false,
      .participatesInLayout = false,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](ScrollView& scrollView) {
        scrollView.setCardStyle(scale, opacity, borders);
      },
  });
  auto* detailContent = detailScroll->content();
  detailContent->setDirection(FlexDirection::Vertical);
  detailContent->setAlign(FlexAlign::Stretch);
  detailContent->setGap(Style::spaceSm * scale);
  detailContent->addChild(
      ui::label({
          .out = &m_detailSubtitle,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  detailContent->addChild(
      ui::label({
          .out = &m_detailBody,
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 0,
          .flexGrow = 1.0f,
      })
  );
  body->addChild(std::move(detailScroll));

  body->addChild(
      ui::label({
          .out = &m_emptyLabel,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .visible = false,
          .participatesInLayout = false,
      })
  );

  container->addChild(std::move(body));
  // Caelestia's launcher grows upward from the search field. Results and
  // provider controls therefore live above the persistent bottom input.
  container->addChild(std::move(categoryFilter));
  container->addChild(std::move(wallpaperModeTab));
  container->addChild(std::move(input));

  setRoot(std::move(container));

  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }

  m_appIconColorizeConn = shellAppIconColorizationChanged().connect([this]() { refreshLauncherAppIconColorization(); });

  syncLauncherListStyle();
}

void LauncherPanel::refreshLauncherAppIconColorization() {
  if (m_listAdapter == nullptr || m_gridAdapter == nullptr || m_grid == nullptr) {
    return;
  }
  const LauncherListStyle style = launcherListStyleFrom(m_config, contentScale());
  m_listAdapter->setListStyle(style);
  m_gridAdapter->setListStyle(style);
  m_grid->notifyDataChanged();
}

bool LauncherPanel::shouldUseAppGrid() const {
  if (m_results.empty()) {
    return false;
  }
  if (isWallpaperBrowse() || isLiveWallpaperBrowse()) {
    return true;
  }
  if (m_config == nullptr || !m_config->config().shell.launcher.appGrid || !m_launcherShowIcons) {
    return false;
  }
  return std::ranges::all_of(m_results, [](const LauncherResult& result) {
    return result.providerId == kApplicationsProviderId;
  });
}

bool LauncherPanel::isWallpaperBrowse() const {
  return !m_results.empty() && std::ranges::all_of(m_results, [](const LauncherResult& result) {
    return result.providerId == kWallpaperProviderId;
  });
}

bool LauncherPanel::isLiveWallpaperBrowse() const {
  return !m_results.empty() && std::ranges::all_of(m_results, [](const LauncherResult& result) {
    return result.providerId == kLiveWallpaperProviderId;
  });
}

LauncherPanel::Presentation LauncherPanel::currentPresentation() const {
  if (isLiveWallpaperBrowse()) {
    return Presentation::LiveWallpaper;
  }
  if (isWallpaperBrowse() || std::string_view(m_query).starts_with("/wall")) {
    if (m_wallpaperModeTab != nullptr && m_wallpaperModeTab->visible() && m_wallpaperModeTab->selectedIndex() == 1) {
      return Presentation::LiveWallpaper;
    }
    return Presentation::Wallpaper;
  }
  if (std::string_view(m_query).starts_with("/emo")
      || (!m_results.empty() && std::ranges::all_of(m_results, [](const LauncherResult& result) {
           return result.providerId == "Emoji";
         }))) {
    return Presentation::Emoji;
  }
  if (shouldUseDetailPresentation()) {
    return Presentation::Detail;
  }
  if (startsWithSlash(m_query)) {
    return Presentation::ProviderOverview;
  }
  return Presentation::Applications;
}

void LauncherPanel::syncDynamicVisualSize(bool animate) {
  const Presentation next = currentPresentation();
  const bool presentationChanged = next != m_presentation;
  m_presentation = next;

  float width = 630.0f;
  float height = 420.0f;
  const auto dynamicListHeight = [this](std::size_t rows, std::size_t maxRows, bool categoryVisible) {
    const float chromeHeight =
        Style::panelPadding * 2.0f + 48.0f + Style::spaceSm + (categoryVisible ? 36.0f + Style::spaceSm : 0.0f);
    const float rowHeight = std::max(48.0f, m_launcherRowHeight > 0.0f ? m_launcherRowHeight : 56.0f);
    return launcher_visual::dynamicListHeight(chromeHeight, rowHeight, Style::spaceXs, rows, maxRows);
  };
  switch (next) {
  case Presentation::Wallpaper:
  case Presentation::LiveWallpaper:
    // A wide, shallow single-row carousel, matching Caelestia's wallpaper mode.
    width = 920.0f;
    height = 430.0f;
    break;
  case Presentation::Emoji:
    width = 700.0f;
    height = 480.0f;
    break;
  case Presentation::ProviderOverview: {
    width = 570.0f;
    // Prefix discovery is a normal one-column result list. Its body should
    // grow by the number of visible providers just like application results,
    // rather than reserving a fixed empty block for every `/` query.
    const auto maxRows =
        m_config != nullptr ? static_cast<std::size_t>(m_config->config().shell.launcher.maxShown) : std::size_t{7};
    height = dynamicListHeight(m_results.size(), maxRows, false);
    break;
  }
  case Presentation::Detail:
    width = 720.0f;
    height = 480.0f;
    break;
  case Presentation::Applications: {
    // Caelestia lets the result wrapper contribute its actual implicit height:
    // an empty search therefore collapses to the search row, while each real
    // row grows the drawer until its normal cap. Keep the same relationship
    // here instead of reserving at least one (and previously 210 px) row.
    const bool categoryVisible = m_categoryFilter != nullptr && m_categoryFilter->visible();
    const std::size_t resultRows = m_results.empty()
        ? 0
        : (m_usingAppGrid ? (m_results.size() + kAppGridColumns - 1) / kAppGridColumns : m_results.size());
    height = dynamicListHeight(resultRows, m_usingAppGrid ? 4 : 7, categoryVisible);
    break;
  }
  }

  PanelManager::instance().requestActivePanelVisualSize(scaled(width), scaled(height), animate);
  if (presentationChanged && animate && root() != nullptr && m_animations != nullptr) {
    if (m_modeTransitionAnimation != 0) {
      m_animations->cancel(m_modeTransitionAnimation);
    }
    Node* const scene = root();
    m_modeTransitionAnimation = m_animations->animate(
        0.0f, 1.0f, Style::animFast * 0.5f, Easing::EaseInQuad,
        [scene](float progress) {
          scene->setScale(1.0f - 0.1f * progress);
          scene->setOpacity(1.0f - 0.2f * progress);
        },
        [this, scene]() {
          m_modeTransitionAnimation = m_animations->animate(
              0.0f, 1.0f, Style::animFast * 0.5f, Easing::EaseOutCubic,
              [scene](float progress) {
                scene->setScale(0.9f + 0.1f * progress);
                scene->setOpacity(0.8f + 0.2f * progress);
              },
              [this, scene]() {
                scene->setScale(1.0f);
                scene->setOpacity(1.0f);
                m_modeTransitionAnimation = 0;
              },
              scene
          );
        },
        scene
    );
  }
}

void LauncherPanel::syncLauncherViewLayout(Renderer* renderer) {
  if (m_grid == nullptr || m_listAdapter == nullptr || m_gridAdapter == nullptr) {
    return;
  }

  const bool useGrid = shouldUseAppGrid();
  const bool useWallpaperGrid = useGrid && isWallpaperBrowse();
  const float scale = contentScale();
  const LauncherListStyle style = launcherListStyleFrom(m_config, scale);
  m_listAdapter->setListStyle(style);
  m_gridAdapter->setListStyle(style);
  if (renderer != nullptr) {
    m_listAdapter->setRenderer(renderer);
    m_gridAdapter->setRenderer(renderer);
  }

  const bool modeChanged = useGrid != m_usingAppGrid || useWallpaperGrid != m_usingWallpaperGrid;
  m_usingAppGrid = useGrid;
  m_usingWallpaperGrid = useWallpaperGrid;
  if (modeChanged) {
    m_launcherRowHeight = 0.0f;
  }

  if (useGrid) {
    m_grid->setColumns(kAppGridColumns);
    m_grid->setSquareCells(false);
    m_grid->setColumnGap(Style::spaceSm * scale);
    m_grid->setRowGap(Style::spaceSm * scale);
    m_grid->setCellHeight(launcherAppGridCellHeightEstimate(style));
    if (modeChanged) {
      m_grid->setAdapter(m_gridAdapter.get());
    }
  } else {
    m_grid->setColumns(1);
    m_grid->setColumnGap(0.0f);
    m_grid->setRowGap(Style::spaceXs * scale);
    const float listCellHeight =
        renderer != nullptr ? launcherRowHeight(*renderer, style) : launcherRowHeightEstimate(style);
    m_grid->setCellHeight(listCellHeight);
    if (renderer != nullptr) {
      m_launcherRowHeight = listCellHeight;
    }
    if (modeChanged) {
      m_grid->setAdapter(m_listAdapter.get());
    }
  }

  if (modeChanged) {
    if (renderer != nullptr) {
      updateLauncherGridMetrics(*renderer);
    }
    m_grid->notifyDataChanged();
  }
}

void LauncherPanel::syncLauncherListStyle() {
  const bool showIcons = m_config == nullptr || m_config->config().shell.launcher.showIcons;
  const bool compact = m_config != nullptr && m_config->config().shell.launcher.compact;
  const bool appGrid = m_config != nullptr && m_config->config().shell.launcher.appGrid;
  if (showIcons == m_launcherShowIcons
      && compact == m_launcherCompact
      && appGrid == m_launcherAppGrid
      && m_listAdapter != nullptr) {
    return;
  }
  m_launcherShowIcons = showIcons;
  m_launcherCompact = compact;
  m_launcherAppGrid = appGrid;
  m_launcherRowHeight = 0.0f;
  syncLauncherViewLayout(nullptr);
  if (m_grid != nullptr) {
    m_grid->notifyDataChanged();
  }
}

void LauncherPanel::updateLauncherGridMetrics(Renderer& renderer) {
  if (m_grid == nullptr) {
    return;
  }

  const LauncherListStyle style = launcherListStyleFrom(m_config, contentScale());
  float cellHeight = launcherRowHeight(renderer, style);
  if (m_usingAppGrid) {
    float wrapWidth = 0.0f;
    const std::size_t columns = std::max<std::size_t>(1, m_grid->layoutColumnCount());
    const float viewportW = m_grid->scrollView().contentViewportWidth();
    const float gap = Style::spaceSm * contentScale();
    const float cellW =
        columns > 0 ? (viewportW - static_cast<float>(columns - 1) * gap) / static_cast<float>(columns) : viewportW;
    const float paddingH = Style::spaceSm * contentScale() * 2.0f;
    wrapWidth = std::max(0.0f, cellW - paddingH);
    cellHeight = launcherAppGridCellHeight(renderer, style, wrapWidth);
  }
  if (std::abs(cellHeight - m_launcherRowHeight) < 0.5f) {
    return;
  }

  m_launcherRowHeight = cellHeight;
  m_grid->setCellHeight(cellHeight);
}

void LauncherPanel::onPanelCardOpacityChanged(float opacity) {
  if (m_input != nullptr) {
    m_input->setSurfaceOpacity(opacity);
  }
  if (m_categoryFilter != nullptr) {
    m_categoryFilter->setSurfaceOpacity(opacity);
  }
  if (m_wallpaperModeTab != nullptr) {
    m_wallpaperModeTab->setSurfaceOpacity(opacity);
  }
  if (m_detailScroll != nullptr) {
    m_detailScroll->setCardStyle(contentScale(), opacity, panelBordersEnabled());
  }
}

void LauncherPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_container == nullptr || m_input == nullptr) {
    return;
  }

  syncLauncherListStyle();
  syncLauncherViewLayout(&renderer);
  updateLauncherGridMetrics(renderer);

  m_container->setSize(width, height);
  m_container->layout(renderer);
  if (m_wallpaperCarousel != nullptr && m_wallpaperCarousel->visible() && m_body != nullptr) {
    m_wallpaperCarousel->setSize(m_body->width(), m_body->height());
    m_wallpaperCarousel->layout(renderer);
  }
}

void LauncherPanel::onOpen(std::string_view context) {
  // Pick up apps installed since the last scan (notably Nix profile swaps that
  // inotify cannot observe). Cheap stat-only check; only rescans on real change.
  refreshDesktopEntriesIfSourcesChanged();

  m_categoryFilterVisible = m_config != nullptr && m_config->config().shell.launcher.categories;
  m_activeCategoryType = All;
  m_activeCategory.clear();
  m_currentCategories.clear();
  m_categoryFilterSlots.clear();
  m_hasRecentlyUsed = false;
  if (m_categoryFilter != nullptr) {
    m_categoryFilter->clearOptions();
    m_categoryFilter->setVisible(false);
    m_categoryFilter->setParticipatesInLayout(false);
  }

  const std::string initialValue(context);
  if (m_input != nullptr) {
    m_input->setPlaceholder(m_scopedPlaceholder.empty() ? "Type \"/\" for commands" : m_scopedPlaceholder);
    m_input->setValue(singleLinePreview(initialValue));
  }
  if (m_grid != nullptr) {
    m_grid->scrollView().setScrollOffset(0.0f);
  }
  onInputChanged(initialValue);
}

void LauncherPanel::onClose() {
  if (m_wallpaper != nullptr) {
    m_wallpaper->clearPreview();
  }
  if (m_actionsMenu != nullptr && m_actionsMenu->isOpen()) {
    m_actionsMenu->close();
  }

  if (m_asyncTextures != nullptr) {
    DeferredCall::callLater([asyncTextures = m_asyncTextures]() { asyncTextures->trimUnused(0); });
  }

  for (auto& provider : m_providers) {
    provider->reset();
  }

  m_query.clear();
  m_results.clear();
  m_allResults.clear();
  m_scopedProviderId.clear();
  m_scopedPlaceholder.clear();
  m_activeCategoryType = All;
  m_activeCategory.clear();
  m_currentCategories.clear();
  m_categoryFilterSlots.clear();
  m_hasRecentlyUsed = false;
  m_selectedIndex = 0;
  m_usingAppGrid = false;
  m_usingWallpaperGrid = false;
  m_launcherRowHeight = 0.0f;

  if (m_grid != nullptr) {
    m_grid->setAdapter(nullptr);
  }
  m_listAdapter.reset();
  m_gridAdapter.reset();

  // The scene tree (and all nodes) is destroyed by PanelManager after onClose().
  m_container = nullptr;
  m_input = nullptr;
  m_categoryFilter = nullptr;
  m_wallpaperModeTab = nullptr;
  m_body = nullptr;
  m_grid = nullptr;
  m_wallpaperCarousel = nullptr;
  m_detailScroll = nullptr;
  m_detailSubtitle = nullptr;
  m_detailBody = nullptr;
  m_emptyLabel = nullptr;
  clearReleasedRoot();
}

void LauncherPanel::onIconThemeChanged() { reapplyCurrentQuery(); }

void LauncherPanel::clearUsage() {
  m_usageTracker.clear();
  if (m_input != nullptr) {
    reapplyCurrentQuery();
  }
}

bool LauncherPanel::shouldTrackUsage() const {
  return m_config != nullptr && m_config->config().shell.launcher.sortByUsage;
}

void LauncherPanel::syncUsageTrackingState() {
  if (!shouldTrackUsage()) {
    clearUsage();
  }
}

void LauncherPanel::reapplyCurrentQuery() {
  std::string selectedProvider;
  std::string selectedId;
  if (m_selectedIndex < m_results.size()) {
    selectedProvider = m_results[m_selectedIndex].providerId;
    selectedId = m_results[m_selectedIndex].id;
  }

  onInputChanged(m_query);

  if (!selectedId.empty()) {
    for (std::size_t i = 0; i < m_results.size(); ++i) {
      if (m_results[i].providerId == selectedProvider && m_results[i].id == selectedId) {
        m_selectedIndex = i;
        break;
      }
    }
  }
  refreshResults();
}

void LauncherPanel::setQuery(std::string query) {
  if (m_input == nullptr) {
    return;
  }
  m_input->setValue(singleLinePreview(query));
  if (m_grid != nullptr) {
    m_grid->scrollView().setScrollOffset(0.0f);
  }
  onInputChanged(query);
}

void LauncherPanel::onProviderResultsChanged() {
  // Only re-gather while the panel is open and built; after onClose the scene
  // nodes are gone and a refresh would touch null grid/label pointers.
  if (m_input == nullptr) {
    return;
  }
  reapplyCurrentQuery();
}

InputArea* LauncherPanel::initialFocusArea() const { return m_input != nullptr ? m_input->inputArea() : nullptr; }

bool LauncherPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (!pressed || preedit) {
    return false;
  }

  auto& dispatcher = PanelManager::instance().inputDispatcher();
  InputArea* const focused = dispatcher.focusedArea();
  if (focused != nullptr) {
    const bool onInput = (m_input != nullptr && focused == m_input->inputArea());
    const bool inResults = (m_grid != nullptr && isDescendantOf(focused, m_grid));
    if (!onInput && !inResults) {
      return false;
    }
    // The launcher search field always owns literal spaces. Wallpaper preview
    // starts once keyboard navigation has moved focus into the carousel.
    if (onInput && KeySymbol::isSpace(sym)) {
      return false;
    }
    if (onInput && m_usingWallpaperGrid
        && (KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)
            || KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)
            || KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)
            || KeybindMatcher::matches(KeybindAction::Down, sym, modifiers))
        && m_grid != nullptr && m_grid->focusArea() != nullptr) {
      dispatcher.setFocus(m_grid->focusArea());
    }
  }

  if (m_categoryFilter != nullptr && m_categoryFilter->visible()) {
    if (focused == m_categoryFilter->focusArea()) {
      return false;
    }
  }

  return handleKeyEvent(sym, modifiers);
}

void LauncherPanel::onInputChanged(const std::string& text) {
  m_query = text;
  m_allResults.clear();

  std::vector<LauncherCategory> newCategories;
  bool hasRecentlyUsed = false;

  if (m_scopedProviderId.empty() && routeCommandQuery(text)) {
    // The command router owns every slash query before legacy provider-prefix
    // matching. This makes `/cal…` discovery unambiguous while retaining the
    // old providers as the implementation of specialized modes.
  } else if (!m_scopedProviderId.empty()) {
    for (auto& provider : m_providers) {
      if (provider->id() != m_scopedProviderId) {
        continue;
      }
      m_allResults = provider->query(text);
      for (auto& result : m_allResults) {
        result.providerId = provider->id();
      }
      sortResultsByScore(m_allResults);
      break;
    }
  } else {
    // Route query to providers
    LauncherProvider* activeProvider = nullptr;
    std::string_view queryText = text;

    // Check for prefix match (longest first)
    for (auto& provider : m_providers) {
      auto prefix = provider->prefix();
      if (prefix.empty()) {
        continue;
      }
      if (text.size() >= prefix.size()
          && std::string_view(text).starts_with(prefix)
          && (activeProvider == nullptr || prefix.size() > activeProvider->prefix().size())) {
        activeProvider = provider.get();
        queryText = std::string_view(text).substr(prefix.size());
      }
    }
    // Trim leading space after prefix
    if (activeProvider != nullptr && !queryText.empty() && queryText.front() == ' ') {
      queryText = queryText.substr(1);
    }

    if (activeProvider != nullptr && activeProvider->id() == kWallpaperProviderId) {
      if (m_wallpaperModeTab != nullptr && m_wallpaperModeTab->selectedIndex() == 1) {
        for (auto& provider : m_providers) {
          if (provider->id() == kLiveWallpaperProviderId) {
            activeProvider = provider.get();
            break;
          }
        }
      }
    }

    const bool isWallpaper = activeProvider != nullptr && (activeProvider->id() == kWallpaperProviderId || activeProvider->id() == kLiveWallpaperProviderId);
    if (m_wallpaperModeTab != nullptr) {
      m_wallpaperModeTab->setVisible(isWallpaper);
      m_wallpaperModeTab->setParticipatesInLayout(isWallpaper);
    }

    const bool typedQuery = !queryText.empty();
    const bool sortByUsage = m_config != nullptr && m_config->config().shell.launcher.sortByUsage;

    auto applyUsageBoost = [&](std::vector<LauncherResult>& results, const LauncherProvider& provider) {
      if (!sortByUsage) {
        return;
      }
      for (auto& result : results) {
        const int usageCount = m_usageTracker.getCount(provider.id(), result.id);
        result.score += usageBoostForScore(result.score, usageCount, typedQuery);
        result.recentlyUsedIndex = m_usageTracker.getRecentlyUsedIndex(provider.id(), result.id);
      }
    };

    if (activeProvider != nullptr) {
      m_allResults = activeProvider->query(queryText);
      if (activeProvider->trackUsage()) {
        applyUsageBoost(m_allResults, *activeProvider);
        if (sortByUsage && m_usageTracker.getRecentlyUsedCount(activeProvider->id()) > 0) {
          hasRecentlyUsed = true;
        }
      }
      for (auto& result : m_allResults) {
        result.providerId = activeProvider->id();
      }
      sortResultsByScore(m_allResults);
      newCategories = activeProvider->categories();
    } else if (startsWithSlash(text)) {
      m_allResults = providerOverviewResults(text);
    } else {
      // Query default providers (empty prefix), plus prefixed providers that opt into global search.
      // Prefixed opt-in providers (e.g. Session) only contribute once the query is long enough,
      // so opening the launcher with no/short input does not flood it with their entries.
      const bool allowGlobalOptIn =
          StringUtils::trimRightView(StringUtils::trimLeftView(queryText)).size() >= kGlobalOptInMinChars;
      for (auto& provider : m_providers) {
        const bool isDefault = provider->prefix().empty();
        if (!isDefault && (!provider->includeInGlobalSearch() || !allowGlobalOptIn)) {
          continue;
        }
        auto results = provider->query(queryText);
        if (provider->trackUsage()) {
          applyUsageBoost(results, *provider);
          if (sortByUsage && m_usageTracker.getRecentlyUsedCount(provider->id()) > 0) {
            hasRecentlyUsed = true;
          }
        }
        for (auto& result : results) {
          result.providerId = provider->id();
        }
        m_allResults.insert(
            m_allResults.end(), std::make_move_iterator(results.begin()), std::make_move_iterator(results.end())
        );
        auto providerCats = provider->categories();
        for (auto& cat : providerCats) {
          newCategories.push_back(std::move(cat));
        }
      }
      sortResultsByScore(m_allResults);
    }
  }

  const int iconTargetSize =
      static_cast<int>(std::round(launcherIconSize(launcherListStyleFrom(m_config, contentScale()))));
  for (auto& result : m_allResults) {
    if (result.iconPath.empty() && !result.iconName.empty()) {
      const std::string& resolved = m_iconResolver.resolve(result.iconName, iconTargetSize);
      if (!resolved.empty()) {
        result.iconPath = resolved;
      } else if (result.iconName != "application-x-executable") {
        const std::string& fallback = m_iconResolver.resolve("application-x-executable", iconTargetSize);
        if (!fallback.empty()) {
          result.iconPath = fallback;
        }
      }
      result.iconName.clear();
    }
  }

  bool categoriesChanged = newCategories.size() != m_currentCategories.size();
  if (!categoriesChanged) {
    for (std::size_t i = 0; i < newCategories.size(); ++i) {
      if (newCategories[i].label != m_currentCategories[i].label) {
        categoriesChanged = true;
        break;
      }
    }
  }

  if (hasRecentlyUsed != m_hasRecentlyUsed) {
    m_hasRecentlyUsed = hasRecentlyUsed;
    categoriesChanged = true;
  }

  if (categoriesChanged) {
    m_activeCategoryType = All;
    m_activeCategory.clear();
    rebuildCategoryFilter(newCategories);
  }

  applyActiveCategory();
}

std::vector<LauncherResult> LauncherPanel::commandCatalogResults(std::string_view filter) {
  std::vector<LauncherResult> results;
  for (const auto& action : m_commandRouter.catalog(filter)) {
    LauncherResult result;
    result.id = std::string(kCommandResultPrefix) + std::to_string(static_cast<int>(action.id));
    result.providerId = std::string(kCommandProviderId);
    result.title = std::string(action.title);
    result.subtitle = action.available ? "/" + std::string(action.command) : "Unavailable";
    result.glyphName = std::string(action.glyph);
    result.available = action.available;
    results.push_back(std::move(result));
  }
  return results;
}

std::vector<LauncherResult> LauncherPanel::commandSchemeResults(std::string_view filter) const {
  const std::string needle = StringUtils::toLower(StringUtils::trim(filter));
  std::vector<LauncherResult> results;
  const auto append = [&](std::string_view source, std::string_view name) {
    const std::string searchable = StringUtils::toLower(std::string(name) + " " + std::string(source));
    if (!needle.empty() && !searchable.contains(needle)) {
      return;
    }
    results.push_back({
        .id = std::string(kCommandSchemePrefix) + std::string(source) + ":" + std::string(name),
        .providerId = std::string(kCommandProviderId),
        .title = std::string(name),
        .subtitle = std::string(source),
        .glyphName = "palette",
    });
  };
  for (const auto& entry : gnil::theme::builtinPalettes()) {
    append("builtin", entry.name);
  }
  for (const auto& entry : gnil::theme::availableCustomPalettes()) {
    append("custom", entry.name);
  }
  return results;
}

std::vector<LauncherResult> LauncherPanel::commandVariantResults(std::string_view filter) const {
  struct VariantEntry {
    std::string_view title;
    std::string_view scheme;
  };
  static constexpr VariantEntry variants[] = {
      {"Vibrant", "vibrant"},      {"Tonal Spot", "m3-tonal-spot"}, {"Expressive", "m3-expressive"},
      {"Fidelity", "m3-fidelity"}, {"Content", "m3-content"},       {"Fruit Salad", "m3-fruit-salad"},
      {"Rainbow", "m3-rainbow"},   {"Neutral", "m3-neutral"},       {"Monochrome", "m3-monochrome"},
  };
  const std::string needle = StringUtils::toLower(StringUtils::trim(filter));
  std::vector<LauncherResult> results;
  for (const auto& variant : variants) {
    const std::string searchable = StringUtils::toLower(std::string(variant.title));
    if (!needle.empty() && !searchable.contains(needle)) {
      continue;
    }
    results.push_back({
        .id = std::string(kCommandVariantPrefix) + std::string(variant.scheme),
        .providerId = std::string(kCommandProviderId),
        .title = std::string(variant.title),
        .subtitle = std::string(variant.scheme),
        .glyphName = "colors",
    });
  }
  return results;
}

bool LauncherPanel::routeCommandQuery(std::string_view text) {
  const bool dangerous = m_config != nullptr && m_config->config().shell.launcher.enableDangerousActions;
  const bool lockscreen = m_config == nullptr || m_config->isLockScreenEnabled();
  m_commandRouter.configure(dangerous, lockscreen);
  const auto route = m_commandRouter.route(text);
  if (!route.active()) {
    return false;
  }

  if (route.mode == launcher_command::Mode::Catalog) {
    m_allResults = commandCatalogResults(route.query);
    return true;
  }
  if (route.mode == launcher_command::Mode::Scheme) {
    m_allResults = commandSchemeResults(route.query);
    return true;
  }
  if (route.mode == launcher_command::Mode::Variant) {
    m_allResults = commandVariantResults(route.query);
    return true;
  }

  const std::string_view providerId = route.mode == launcher_command::Mode::Calculator ? "Calculator" : "Wallpaper";
  for (const auto& provider : m_providers) {
    if (provider->id() != providerId) {
      continue;
    }
    m_allResults = provider->query(route.query);
    const std::size_t limit = route.mode == launcher_command::Mode::Wallpaper && m_config != nullptr
        ? static_cast<std::size_t>(m_config->config().shell.launcher.maxWallpapers)
        : m_allResults.size();
    if (m_allResults.size() > limit) {
      m_allResults.resize(limit);
    }
    for (auto& result : m_allResults) {
      result.providerId = std::string(provider->id());
    }
    sortResultsByScore(m_allResults);
    break;
  }
  return true;
}

void LauncherPanel::rebuildCategoryFilter(const std::vector<LauncherCategory>& categories) {
  m_currentCategories = categories;
  m_categoryFilterSlots.clear();
  if (categories.empty() && !m_hasRecentlyUsed) {
    if (m_categoryFilter != nullptr) {
      m_categoryFilter->clearOptions();
      setCategoryFilterVisible(false);
    }
    return;
  }

  m_categoryFilterSlots.push_back({All, 0});
  if (m_hasRecentlyUsed) {
    m_categoryFilterSlots.push_back({RecentlyUsed, 0});
  }
  for (std::size_t i = 0; i < categories.size(); ++i) {
    m_categoryFilterSlots.push_back({Category, i});
  }

  if (m_categoryFilter == nullptr) {
    return;
  }

  m_categoryFilter->clearOptions();
  for (std::size_t i = 0; i < m_categoryFilterSlots.size(); ++i) {
    const auto& slot = m_categoryFilterSlots[i];
    switch (slot.type) {
    case All:
      m_categoryFilter->addOption("", "layout-grid");
      m_categoryFilter->setOptionTooltip(i, i18n::tr("launcher.categories.all"));
      break;
    case RecentlyUsed:
      m_categoryFilter->addOption("", "history");
      m_categoryFilter->setOptionTooltip(i, i18n::tr("launcher.categories.recently-used"));
      break;
    case Category:
      m_categoryFilter->addOption("", categories[slot.categoryIndex].glyphName);
      m_categoryFilter->setOptionTooltip(i, categories[slot.categoryIndex].label);
      break;
    }
  }
  m_categoryFilter->setSelectedIndex(0);
  m_categoryFilter->setOnChange([this](std::size_t idx) { setActiveCategorySlot(idx); });
  setCategoryFilterVisible(m_categoryFilterVisible);
}

void LauncherPanel::setActiveCategorySlot(std::size_t slotIndex) {
  if (slotIndex >= m_categoryFilterSlots.size()) {
    return;
  }

  const auto& slot = m_categoryFilterSlots[slotIndex];
  m_activeCategoryType = slot.type;
  if (slot.type == Category && slot.categoryIndex < m_currentCategories.size()) {
    m_activeCategory = m_currentCategories[slot.categoryIndex].label;
  } else {
    m_activeCategory.clear();
  }
  applyActiveCategory();
}

void LauncherPanel::setCategoryFilterVisible(bool visible) {
  if (m_categoryFilter == nullptr) {
    return;
  }
  const bool show = visible && !m_categoryFilterSlots.empty();
  m_categoryFilter->setVisible(show);
  m_categoryFilter->setParticipatesInLayout(show);
  if (m_container != nullptr) {
    m_container->markLayoutDirty();
  }
}

std::vector<LauncherResult> LauncherPanel::providerOverviewResults(std::string_view text) const {
  std::string filter;
  if (startsWithSlash(text)) {
    filter = StringUtils::toLower(StringUtils::trim(text.substr(1)));
  }

  std::vector<LauncherResult> results;
  results.reserve(m_providers.size());
  for (const auto& provider : m_providers) {
    const std::string_view prefix = provider->prefix();
    if (prefix.empty()) {
      continue;
    }

    const std::string title(provider->displayName());
    const std::string prefixText(prefix);
    const std::string searchable = StringUtils::toLower(title + " " + prefixText);
    const double score = filter.empty() ? 0.0 : FuzzyMatch::score(filter, searchable);
    if (!filter.empty() && !FuzzyMatch::isMatch(score)) {
      continue;
    }

    LauncherResult result;
    result.id = providerOverviewId(prefix);
    result.providerId = std::string(kProviderOverviewProviderId);
    result.title = title;
    result.subtitle = prefixText;
    result.glyphName = std::string(provider->defaultGlyphName());
    result.score = score;
    results.push_back(std::move(result));
  }

  if (!filter.empty()) {
    sortResultsByScore(results);
  }
  return results;
}

void LauncherPanel::applyActiveCategory() {
  m_results.clear();
  switch (m_activeCategoryType) {
  case All:
    m_results = m_allResults;
    break;
  case RecentlyUsed:
    std::ranges::copy_if(m_allResults, std::back_inserter(m_results), [](const LauncherResult& r) {
      return r.recentlyUsedIndex > 0;
    });
    std::ranges::sort(m_results, [](const LauncherResult& a, const LauncherResult& b) {
      return a.recentlyUsedIndex > b.recentlyUsedIndex
          || (a.recentlyUsedIndex == b.recentlyUsedIndex
              && std::tie(a.providerId, a.id) < std::tie(b.providerId, b.id));
    });
    break;
  case Category:
    for (const auto& r : m_allResults) {
      if (r.category == m_activeCategory) {
        m_results.push_back(r);
      }
    }
    break;
  }
  m_selectedIndex = 0;
  refreshResults();
}

void LauncherPanel::refreshResults() {
  uiAssertNotRendering("LauncherPanel::refreshResults");
  if (m_grid == nullptr || m_emptyLabel == nullptr) {
    return;
  }

  syncLauncherViewLayout(nullptr);
  if (m_wallpaperCarousel != nullptr) {
    m_wallpaperCarousel->setResults(&m_results);
    m_wallpaperCarousel->setSelectedIndex(m_selectedIndex);
  }
  m_grid->notifyDataChanged();
  if (m_results.empty()) {
    m_grid->setSelectedIndex(std::nullopt);
    m_grid->scrollView().setScrollOffset(0.0f);
  } else {
    m_grid->setSelectedIndex(m_selectedIndex);
  }
  bindDetailResult();
  applyEmptyState();
  syncDynamicVisualSize(true);
}

void LauncherPanel::applyEmptyState() {
  if (m_grid == nullptr || m_emptyLabel == nullptr) {
    return;
  }
  const bool empty = m_results.empty();
  const bool detail = !empty && shouldUseDetailPresentation();
  const bool wallpaper = !empty && isWallpaperBrowse();
  m_grid->setVisible(!empty && !detail && !wallpaper);
  m_grid->setParticipatesInLayout(!empty && !detail && !wallpaper);
  if (m_wallpaperCarousel != nullptr) {
    m_wallpaperCarousel->setVisible(wallpaper);
    m_wallpaperCarousel->setParticipatesInLayout(wallpaper);
  }
  if (m_detailScroll != nullptr) {
    m_detailScroll->setVisible(detail);
    m_detailScroll->setParticipatesInLayout(detail);
  }
  m_emptyLabel->setVisible(empty);
  m_emptyLabel->setParticipatesInLayout(empty);
  if (empty) {
    m_emptyLabel->setText(
        m_query.empty() ? i18n::tr("launcher.empty.type-to-search") : i18n::tr("launcher.empty.no-results")
    );
  }
}

bool LauncherPanel::shouldUseDetailPresentation() const {
  return m_results.size() == 1 && isDetailPresentation(m_results.front());
}

void LauncherPanel::bindDetailResult() {
  if (!shouldUseDetailPresentation()
      || m_detailScroll == nullptr
      || m_detailSubtitle == nullptr
      || m_detailBody == nullptr) {
    return;
  }

  const LauncherResult& result = m_results.front();
  const bool hasSubtitle = !result.subtitle.empty();
  m_detailSubtitle->setVisible(hasSubtitle);
  m_detailSubtitle->setParticipatesInLayout(hasSubtitle);
  m_detailSubtitle->setText(singleLinePreview(result.subtitle));
  m_detailBody->setText(result.title.empty() ? result.id : result.title);
  m_detailScroll->setScrollOffset(0.0f);
}

void LauncherPanel::openAppActionsMenu(std::size_t index, float anchorX, float anchorY) {
  if (index >= m_results.size()) {
    return;
  }
  const LauncherResult& base = m_results[index];

  const DesktopEntry* match = nullptr;
  for (const auto& e : desktopEntries()) {
    if (e.path == base.id) {
      match = &e;
      break;
    }
  }
  if (match == nullptr) {
    return;
  }

  WaylandConnection* wl = PanelManager::instance().wayland();
  RenderContext* rc = PanelManager::instance().renderContext();
  if (wl == nullptr || rc == nullptr) {
    return;
  }

  const auto parentCtx = PanelManager::instance().fallbackPopupParentContext();
  if (!parentCtx.has_value()) {
    return;
  }

  if (m_actionsMenu == nullptr) {
    m_actionsMenu = std::make_unique<ContextMenuPopup>(*wl, *rc);
  }

  std::vector<DesktopAction> actionsCopy = match->actions;
  const bool favourite = m_config != nullptr && isFavourite(m_config->config().shell.launcher.favouriteApps, *match);
  const bool canAddFavourite = m_config != nullptr && !favourite;
  const bool canRemoveFavourite = m_config != nullptr && favourite;

  constexpr std::int32_t kActionOpen = -1;
  constexpr std::int32_t kActionAddFavourite = -2;
  constexpr std::int32_t kActionRemoveFavourite = -3;

  std::vector<ContextMenuControlEntry> entries;
  entries.reserve(actionsCopy.size() + 2);
  entries.push_back(
      ContextMenuControlEntry{
          .id = kActionOpen,
          .label = i18n::tr("launcher.context-menu.open"),
          .enabled = true,
          .separator = false,
          .hasSubmenu = false,
      }
  );
  if (canAddFavourite) {
    entries.push_back(
        ContextMenuControlEntry{
            .id = kActionAddFavourite,
            .label = i18n::tr("launcher.context-menu.add-favourite"),
            .enabled = true,
            .separator = false,
            .hasSubmenu = false,
        }
    );
  } else if (canRemoveFavourite) {
    entries.push_back(
        ContextMenuControlEntry{
            .id = kActionRemoveFavourite,
            .label = i18n::tr("launcher.context-menu.remove-favourite"),
            .enabled = true,
            .separator = false,
            .hasSubmenu = false,
        }
    );
  }
  for (std::int32_t i = 0; i < static_cast<std::int32_t>(actionsCopy.size()); ++i) {
    entries.push_back(
        ContextMenuControlEntry{
            .id = i,
            .label = actionsCopy[static_cast<std::size_t>(i)].name,
            .enabled = true,
            .separator = false,
            .hasSubmenu = false,
        }
    );
  }

  const float scale = contentScale();
  constexpr float kMenuWidth = 240.0f;
  const float menuWidth = kMenuWidth * scale;

  if (m_config != nullptr) {
    m_actionsMenu->setShadowConfig(m_config->config().shell.shadow);
  }
  PanelManager::instance().beginAttachedPopup(parentCtx->surface);
  PanelManager::instance().setActivePopup(m_actionsMenu.get());

  m_actionsMenu->setOnDismissed([parentSurface = parentCtx->surface]() {
    PanelManager::instance().clearActivePopup();
    PanelManager::instance().endAttachedPopup(parentSurface);
  });

  m_actionsMenu->setOnActivate([this, base, actionsCopy = std::move(actionsCopy),
                                favouriteEntry = *match](const ContextMenuControlEntry& entry) {
    LauncherResult result = base;
    result.desktopActionId.clear();
    if (entry.id == kActionAddFavourite) {
      if (m_config == nullptr
          || favouriteEntry.id.empty()
          || isFavourite(m_config->config().shell.launcher.favouriteApps, favouriteEntry)) {
        return;
      }
      std::vector<std::string> favourites = m_config->config().shell.launcher.favouriteApps;
      favourites.push_back(favouriteEntry.id);
      (void)m_config->setOverride({"shell", "launcher", "favourite_apps"}, std::move(favourites));
      return;
    }
    if (entry.id == kActionRemoveFavourite) {
      if (m_config == nullptr) {
        return;
      }
      std::vector<std::string> favourites = m_config->config().shell.launcher.favouriteApps;
      std::erase_if(favourites, [&favouriteEntry](const std::string& value) {
        return favouriteMatches(value, favouriteEntry);
      });
      (void)m_config->setOverride({"shell", "launcher", "favourite_apps"}, std::move(favourites));
      return;
    }
    if (entry.id >= 0 && entry.id < static_cast<std::int32_t>(actionsCopy.size())) {
      result.desktopActionId = actionsCopy[static_cast<std::size_t>(entry.id)].id;
    } else if (entry.id != kActionOpen) {
      return;
    }

    for (auto& provider : m_providers) {
      if (provider->id() != std::string_view(result.providerId)) {
        continue;
      }
      if (!provider->activate(result)) {
        return;
      }
      if (shouldTrackUsage() && provider->trackUsage()) {
        m_usageTracker.record(provider->id(), result.id);
      }
      PanelManager::instance().closePanel(false);
      return;
    }
    return;
  });

  const float inset = std::round(std::max(4.0f, Style::spaceXs * scale));
  const auto ax = static_cast<std::int32_t>(std::round(anchorX - inset));
  const auto ay = static_cast<std::int32_t>(std::round(anchorY - inset));
  const auto aw = static_cast<std::int32_t>(std::round(inset * 2.0f));
  const auto ah = static_cast<std::int32_t>(std::round(inset * 2.0f));

  m_actionsMenu->open(
      ContextMenuPopupRequest{
          .entries = std::move(entries),
          .menuWidth = menuWidth,
          .maxVisible = 12,
          .anchor =
              PopupAnchorRect{
                  .x = ax,
                  .y = ay,
                  .width = std::max(1, aw),
                  .height = std::max(1, ah),
              },
          .parent = PopupSurfaceParent{
              .layerSurface = parentCtx->layerSurface,
              .output = parentCtx->output,
          },
      }
  );
}

void LauncherPanel::activateAt(std::size_t index) {
  if (index >= m_results.size()) {
    return;
  }
  m_selectedIndex = index;
  activateSelected();
}

void LauncherPanel::activateSelected() {
  if (m_selectedIndex >= m_results.size()) {
    return;
  }

  const auto& result = m_results[m_selectedIndex];
  if (!result.available) {
    return;
  }
  if (result.providerId == kCommandProviderId) {
    (void)activateCommandResult(result);
    return;
  }
  if (result.providerId == kProviderOverviewProviderId && result.id.starts_with(kProviderOverviewResultPrefix)) {
    std::string prefix = result.id.substr(kProviderOverviewResultPrefix.size());
    if (!prefix.empty()) {
      prefix += ' ';
    }
    if (m_input != nullptr) {
      m_input->setValue(prefix);
    }
    if (m_grid != nullptr) {
      m_grid->scrollView().setScrollOffset(0.0f);
    }
    onInputChanged(prefix);
    return;
  }

  const bool wallpaperResult = result.providerId == kWallpaperProviderId;
  if (wallpaperResult && m_wallpaper != nullptr) {
    m_wallpaper->commitPreview();
  }

  // Dispatch only to the provider that produced this result. Providers can use
  // overlapping id shapes, so probing every provider risks side effects.
  for (auto& provider : m_providers) {
    if (provider->id() != std::string_view(result.providerId)) {
      continue;
    }

    if (!provider->activate(result)) {
      return;
    }

    if (shouldTrackUsage() && provider->trackUsage()) {
      m_usageTracker.record(provider->id(), result.id);
    }
    PanelManager::instance().closePanel(wallpaperResult);
    return;
  }
}

bool LauncherPanel::activateCommandResult(const LauncherResult& result) {
  if (result.id.starts_with(kCommandVariantPrefix)) {
    const bool changed = m_config != nullptr
        && m_config->setThemeColorScheme(PaletteSource::Wallpaper, result.id.substr(kCommandVariantPrefix.size()));
    if (changed) {
      PanelManager::instance().closePanel(false);
    }
    return changed;
  }
  if (result.id.starts_with(kCommandSchemePrefix)) {
    std::string_view value(result.id);
    value.remove_prefix(kCommandSchemePrefix.size());
    const auto separator = value.find(':');
    if (separator == std::string_view::npos || m_config == nullptr) {
      return false;
    }
    const std::string_view source = value.substr(0, separator);
    const std::string_view name = value.substr(separator + 1);
    PaletteSource paletteSource = PaletteSource::Builtin;
    if (source == "custom") {
      paletteSource = PaletteSource::Custom;
    }
    if (m_config->setThemeColorScheme(paletteSource, name)) {
      PanelManager::instance().closePanel(false);
      return true;
    }
    return false;
  }
  if (!result.id.starts_with(kCommandResultPrefix)) {
    return false;
  }
  std::string_view encoded(result.id);
  encoded.remove_prefix(kCommandResultPrefix.size());
  int raw = -1;
  const auto [ptr, ec] = std::from_chars(encoded.data(), encoded.data() + encoded.size(), raw);
  if (ec != std::errc{}
      || ptr != encoded.data() + encoded.size()
      || raw < 0
      || raw > static_cast<int>(launcher_command::ActionId::Session)) {
    return false;
  }

  const auto action = static_cast<launcher_command::ActionId>(raw);
  const auto activation = m_commandRouter.activate(action);
  if (activation.kind == launcher_command::ActivationKind::Unavailable) {
    return false;
  }
  if (activation.kind == launcher_command::ActivationKind::RewriteQuery) {
    setQuery(activation.query);
    return true;
  }
  if (activation.kind == launcher_command::ActivationKind::Confirm) {
    for (auto& candidate : m_allResults) {
      if (candidate.id == result.id) {
        candidate.subtitle = "Press Enter again to confirm";
      }
    }
    applyActiveCategory();
    return true;
  }

  using launcher_command::ActionId;
  if (m_config != nullptr && action == ActionId::Light) {
    m_config->setThemeMode(ThemeMode::Light);
  } else if (m_config != nullptr && action == ActionId::Dark) {
    m_config->setThemeMode(ThemeMode::Dark);
  } else if (action == ActionId::Settings) {
    PanelManager::instance().closePanel(false);
    DeferredCall::callLater([]() { PanelManager::instance().openPanel("settings"); });
    return true;
  } else if (action == ActionId::Random) {
    for (const auto& provider : m_providers) {
      if (provider->id() != kWallpaperProviderId) {
        continue;
      }
      auto wallpapers = provider->query({});
      if (wallpapers.empty()) {
        return false;
      }
      static std::mt19937 generator(std::random_device{}());
      std::uniform_int_distribution<std::size_t> pick(0, wallpapers.size() - 1);
      LauncherResult selected = wallpapers[pick(generator)];
      selected.providerId = std::string(provider->id());
      if (provider->activate(selected)) {
        PanelManager::instance().closePanel();
        return true;
      }
      return false;
    }
    return false;
  } else {
    std::string_view command;
    switch (action) {
    case ActionId::Shutdown:
      command = "shutdown";
      break;
    case ActionId::Reboot:
      command = "reboot";
      break;
    case ActionId::Logout:
      command = "logout";
      break;
    case ActionId::Lock:
      command = "lock";
      break;
    case ActionId::Sleep:
      command = "suspend";
      break;
    default:
      break;
    }
    if (!command.empty()) {
      for (const auto& provider : m_providers) {
        if (provider->id() != "Session") {
          continue;
        }
        auto actions = provider->query(command);
        const auto match = std::ranges::find_if(actions, [command](const LauncherResult& candidate) {
          return candidate.id.ends_with(":" + std::string(command));
        });
        if (match != actions.end()) {
          LauncherResult selected = *match;
          selected.providerId = "Session";
          if (provider->activate(selected)) {
            PanelManager::instance().closePanel(false);
            return true;
          }
        }
      }
      return false;
    }
  }
  PanelManager::instance().closePanel(false);
  return true;
}

bool LauncherPanel::dismissTransientUi() {
  if (!m_commandRouter.escape()) {
    return false;
  }
  reapplyCurrentQuery();
  return true;
}

bool LauncherPanel::handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers) {
  const bool wallpaperNav = m_usingWallpaperGrid && m_wallpaperCarousel != nullptr;
  const bool gridNav = m_usingAppGrid && !wallpaperNav && m_grid != nullptr;
  const int columns = gridNav ? static_cast<int>(std::max<std::size_t>(1, m_grid->layoutColumnCount())) : 1;

  const auto moveSelection = [this, wallpaperNav](int delta) {
    if (m_results.empty()) {
      return;
    }
    const int last = static_cast<int>(m_results.size() - 1);
    const int next = wallpaperNav
        ? (static_cast<int>(m_selectedIndex) + delta + last + 1) % (last + 1)
        : std::clamp(static_cast<int>(m_selectedIndex) + delta, 0, last);
    if (next == static_cast<int>(m_selectedIndex)) {
      return;
    }
    m_selectedIndex = static_cast<std::size_t>(next);
    if (m_grid != nullptr) {
      m_grid->setSelectedIndex(m_selectedIndex);
    }
    if (m_wallpaperCarousel != nullptr) {
      m_wallpaperCarousel->setSelectedIndex(m_selectedIndex);
    }
  };

  const auto cycleCategory = [this](bool reverse) {
    if (m_categoryFilter == nullptr) {
      return false;
    }
    const std::size_t total = m_categoryFilterSlots.size();
    if (total == 0) {
      return false;
    }

    const bool wasVisible = m_categoryFilter->visible();
    m_categoryFilterVisible = true;
    setCategoryFilterVisible(true);
    if (!wasVisible) {
      return true;
    }

    const std::size_t selected = std::min(m_categoryFilter->selectedIndex(), total - 1);
    const std::size_t next =
        reverse ? (selected == 0 ? total - 1 : selected - 1) : (selected + 1 < total ? selected + 1 : 0);
    m_categoryFilter->setSelectedIndex(next);
    return true;
  };

  if (sym == XKB_KEY_F6 && (modifiers & ~(KeyMod::Shift)) == 0) {
    return cycleCategory((modifiers & KeyMod::Shift) != 0);
  }

  if (KeySymbol::isPageUp(sym)) {
    const int stride = m_grid != nullptr ? static_cast<int>(m_grid->pageItemStride()) : 1;
    moveSelection(-stride);
    return true;
  }

  if (KeySymbol::isPageDown(sym)) {
    const int stride = m_grid != nullptr ? static_cast<int>(m_grid->pageItemStride()) : 1;
    moveSelection(stride);
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
    moveSelection(gridNav ? -columns : -1);
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
    moveSelection(gridNav ? columns : 1);
    return true;
  }

  if ((gridNav || wallpaperNav) && KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)) {
    moveSelection(-1);
    return true;
  }

  if ((gridNav || wallpaperNav) && KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)) {
    moveSelection(1);
    return true;
  }

  if (wallpaperNav && KeySymbol::isSpace(sym)) {
    if (m_selectedIndex < m_results.size() && m_wallpaper != nullptr) {
      (void)m_wallpaper->previewWallpaperImage(std::nullopt, m_results[m_selectedIndex].id);
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    activateSelected();
    return true;
  }

  return false;
}
