#include "shell/settings/settings_sidebar.h"

#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/settings/settings_registry.h"
#include "ui/builders.h"
#include "ui/controls/roving_list_nav.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

namespace settings {
  namespace {

    constexpr float kSidebarWidth = 200.0f;
    constexpr float kSidebarPadding = 6.0f;
    constexpr float kSidebarGap = 2.0f;
    constexpr float kPrimaryNavGlyphSize = 18.0f;
    constexpr float kPrimaryNavGap = 6.0f;
    constexpr float kPrimaryNavPaddingH = 10.0f;

    void addNavButton(RovingListNavHost& nav, std::unique_ptr<Button> button, std::function<void()> onClick) {
      Button* raw = button.get();
      nav.registerItem(raw, onClick);
      nav.addChild(std::move(button));
    }

    std::string normalizedConfigId(std::string_view text) { return StringUtils::trim(text); }

    bool isValidConfigId(std::string_view text) {
      const auto trimmed = StringUtils::trim(text);
      if (trimmed.empty()) {
        return false;
      }
      return std::ranges::all_of(trimmed, [](unsigned char c) { return std::isalnum(c) != 0 || c == '_' || c == '-'; });
    }

    void makeButtonLabelBold(Button& button) {
      if (button.label() != nullptr) {
        button.label()->setFontWeight(FontWeight::Bold);
      }
    }

    // Primary sidebar nav style: top-level section rows with a bolder label.
    std::unique_ptr<Button> makePrimaryNavButton(
        std::string_view glyph, std::string text, float scale, bool selected, std::function<void()> onClick
    ) {
      return ui::button({
          .text = std::move(text),
          .glyph = std::string(glyph),
          .fontSize = Style::fontSizeCaption * scale,
          .glyphSize = kPrimaryNavGlyphSize * scale,
          .contentAlign = ButtonContentAlign::Start,
          .variant = selected ? ButtonVariant::TabActive : ButtonVariant::Tab,
          .minHeight = Style::controlHeightSm * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = kPrimaryNavPaddingH * scale,
          .gap = kPrimaryNavGap * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = std::move(onClick),
          .configure = [](Button& button) {
            makeButtonLabelBold(button);
            button.setTabStop(false);
          },
      });
    }

    // Secondary sidebar nav style: indented compact rows for bars and monitors.
    std::unique_ptr<Button> makeSecondaryNavButton(
        std::string_view glyph, std::string text, float scale, bool selected, std::function<void()> onClick
    ) {
      return ui::button({
          .text = std::move(text),
          .glyph = std::string(glyph),
          .fontSize = Style::fontSizeCaption * scale,
          .glyphSize = Style::fontSizeCaption * scale,
          .contentAlign = ButtonContentAlign::Start,
          .variant = selected ? ButtonVariant::TabActive : ButtonVariant::Tab,
          .minHeight = Style::controlHeightSm * scale,
          .paddingTop = Style::spaceXs * scale,
          .paddingRight = Style::spaceMd * scale,
          .paddingBottom = Style::spaceXs * scale,
          .paddingLeft = Style::spaceLg * scale,
          .gap = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = std::move(onClick),
          .configure = [](Button& button) { button.setTabStop(false); },
      });
    }

    std::unique_ptr<Button> makeCreateButton(std::string text, float scale, std::function<void()> onClick) {
      return ui::button({
          .text = std::move(text),
          .fontSize = Style::fontSizeCaption * scale,
          .variant = ButtonVariant::Default,
          .minHeight = Style::controlHeightSm * scale,
          .paddingV = Style::spaceXs * scale,
          .paddingH = Style::spaceSm * scale,
          .radius = Style::scaledRadiusSm(scale),
          .onClick = std::move(onClick),
      });
    }

    std::unique_ptr<Button> makeCreateCancelButton(float scale, std::function<void()> onClick) {
      return ui::button({
          .glyph = "close",
          .glyphSize = Style::fontSizeCaption * scale,
          .variant = ButtonVariant::Ghost,
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusSm(scale),
          .onClick = std::move(onClick),
      });
    }

  } // namespace

  std::unique_ptr<Flex> buildSettingsSidebar(SettingsSidebarContext ctx) {
    const Config& cfg = ctx.config;

    auto* scroll = &ctx.contentScrollState;
    auto* selectedSection = &ctx.selectedSection;
    auto* selectedBarName = &ctx.selectedBarName;
    auto* selectedMonitorOverride = &ctx.selectedMonitorOverride;

    const auto clearTransientState = std::move(ctx.clearTransientState);
    const auto clearSearchQuery = std::move(ctx.clearSearchQuery);
    const auto requestRebuild = std::move(ctx.requestRebuild);
    const float scale = ctx.scale;
    const bool searchActive = ctx.globalSearchActive;
    const bool showActiveTab = !searchActive;

    auto sidebarScroll = ui::scrollView({
        .state = &ctx.sidebarScrollState,
        .scrollbarVisible = true,
        .viewportPaddingH = 0.0f,
        .viewportPaddingV = 0.0f,
        .fill = colorSpecFromRole(ColorRole::Surface),
        .radius = Style::scaledRadiusXl(scale),
        .minWidth = kSidebarWidth * scale,
        .fillHeight = true,
        .width = kSidebarWidth * scale,
        .height = 0.0f,
        .configure = [](ScrollView& scrollView) { scrollView.clearBorder(); },
    });

    auto sidebarNav = std::make_unique<RovingListNavHost>(RovingListNavController::Options{
        .axis = RovingListNavAxis::Vertical,
        .mode = RovingListNavMode::FollowFocus,
        .keepItemsInTabOrder = false,
        .scrollIntoView = std::move(ctx.scrollSidebarNodeIntoView),
        .syncIndexFromSelection = {},
    });
    sidebarNav->setTabFocusKey("settings.sidebar");
    sidebarNav->setGap(kSidebarGap * scale);
    sidebarNav->setPadding(kSidebarPadding * scale);
    RovingListNavHost* nav = sidebarNav.get();

    for (const auto& parent : parentCategories()) {
      bool hasVisibleSubSection = false;
      for (const auto& sec : parent.subSections) {
        if (std::ranges::contains(ctx.sections, sec)) {
          hasVisibleSubSection = true;
          break;
        }
      }
      if (!hasVisibleSubSection) {
        continue;
      }

      bool selected = false;
      if (showActiveTab) {
        if (*selectedSection == parent.id) {
          selected = true;
        } else {
          const auto currentSectionOpt = settingsSectionFromId(*selectedSection);
          if (currentSectionOpt.has_value() && std::ranges::contains(parent.subSections, *currentSectionOpt)) {
            selected = true;
          }
        }
      }

      const std::string parentId(parent.id);
      const auto onClick = [selectedSection, scroll, parentId, searchActive, clearTransientState, clearSearchQuery,
                            requestRebuild]() {
        if (searchActive || *selectedSection != parentId) {
          scroll->offset = 0.0f;
        }
        *selectedSection = parentId;
        clearSearchQuery();
        clearTransientState();
        requestRebuild();
      };

      addNavButton(
          *nav,
          makePrimaryNavButton(
              parent.glyph, i18n::tr(std::string(parent.labelKey)), scale, selected, onClick
          ),
          onClick
      );
    }

    auto* sidebar = sidebarScroll->content();
    sidebar->setDirection(FlexDirection::Vertical);
    sidebar->setAlign(FlexAlign::Stretch);
    sidebar->addChild(std::move(sidebarNav));

    if (ctx.outNav != nullptr) {
      *ctx.outNav = nav;
    }

    return sidebarScroll;
  }

} // namespace settings
