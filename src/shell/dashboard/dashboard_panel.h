#pragma once

#include "render/animation/animation_manager.h"
#include "shell/control_center/control_center_services.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

class Box;
class Flex;
class InputArea;
class Segmented;

// The top-centre Caelestia-style dashboard. Technical destinations such as
// network, audio and bluetooth deliberately remain standalone panels.
class DashboardPanel final : public Panel {
public:
  explicit DashboardPanel(const ControlCenterServices& services);

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  void onFrameTick(float deltaMs) override;
  [[nodiscard]] bool dismissTransientUi() override;
  [[nodiscard]] bool isContextActive(std::string_view context) const override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

  [[nodiscard]] float preferredWidth() const override;
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] bool usesDynamicVisualSize() const noexcept override { return true; }
  [[nodiscard]] float initialVisualWidth() const override { return preferredWidth(); }
  [[nodiscard]] float initialVisualHeight() const override { return preferredHeight(); }
  [[nodiscard]] ChromeEdge chromeEdge() const noexcept override { return ChromeEdge::Top; }
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override { return PanelPlacement::Floating; }
  [[nodiscard]] std::string panelScreenPosition() const override { return "top_center"; }
  [[nodiscard]] LayerShellLayer layer() const override { return LayerShellLayer::Overlay; }

private:
  enum class Page : std::uint8_t { Dashboard, Media, Performance, Weather, Count };
  static constexpr std::size_t kPageCount = static_cast<std::size_t>(Page::Count);

  struct PageMeta {
    Page page;
    std::string_view context;
    std::string_view label;
    std::string_view glyph;
    float width;
    float height;
  };

  static constexpr std::array<PageMeta, kPageCount> kPages{{
      {Page::Dashboard, "dashboard", "Dashboard", "dashboard", 1000.0f, 600.0f},
      {Page::Media, "media", "Media", "disc-filled", 1000.0f, 520.0f},
      {Page::Performance, "performance", "Performance", "activity-heartbeat", 1000.0f, 650.0f},
      {Page::Weather, "weather", "Weather", "weather-cloud-sun", 840.0f, 500.0f},
  }};

  void onPanelBordersChanged(bool enabled) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;

  [[nodiscard]] static constexpr std::size_t index(Page page) { return static_cast<std::size_t>(page); }
  [[nodiscard]] Page pageFromContext(std::string_view context) const;
  [[nodiscard]] bool pageEnabled(Page page) const;
  [[nodiscard]] Page firstEnabledPage() const;
  [[nodiscard]] int visibleOrdinal(Page page) const;
  void selectPage(Page page, bool animated);
  void selectAdjacent(int direction);
  void applyPageVisibility();
  void layoutPageContainers(float width, float height);
  void finishTransition();
  void syncVisualSize(bool animate);
  void layoutIndicator();

  ControlCenterServices m_services;
  std::array<std::unique_ptr<Tab>, kPageCount> m_pages;
  std::array<Flex*, kPageCount> m_pageContainers{};
  std::vector<Page> m_visiblePages;
  Page m_activePage = Page::Dashboard;
  Page m_outgoingPage = Page::Dashboard;

  Flex* m_rootLayout = nullptr;
  Segmented* m_tabStrip = nullptr;
  Box* m_tabIndicator = nullptr;
  Flex* m_pageBodies = nullptr;
  AnimationManager::Id m_transitionAnimation = 0;
  float m_transitionProgress = 1.0f;
  int m_transitionDirection = 1;
  bool m_transitionActive = false;
  bool m_firstOpenAfterCreate = false;
};
