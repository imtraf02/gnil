#include "shell/dashboard/dashboard_panel.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "dbus/mpris/mpris_service.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/control_center/tabs/home_tab.h"
#include "shell/control_center/tabs/media_tab.h"
#include "shell/control_center/tabs/weather_tab.h"
#include "shell/dashboard/performance_tab.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/segmented.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon-keysyms.h>

DashboardPanel::DashboardPanel(const ControlCenterServices& services) : m_services(services) {
  WaylandConnection* wayland = services.platform != nullptr ? &services.platform->wayland() : nullptr;
  m_pages[index(Page::Dashboard)] = std::make_unique<HomeTab>(services);
  m_pages[index(Page::Media)] = std::make_unique<MediaTab>(
      services.mpris, services.httpClient, services.spectrum, services.config, wayland,
      PanelManager::instance().renderContext(), MediaTabPresentation::Dashboard
  );
  m_pages[index(Page::Performance)] = std::make_unique<DashboardPerformanceTab>(services);
  m_pages[index(Page::Weather)] = std::make_unique<WeatherTab>(services.weather, services.config);
}

bool DashboardPanel::pageEnabled(Page page) const {
  if (m_services.config == nullptr) {
    return true;
  }
  const auto& dashboard = m_services.config->config().dashboard;
  switch (page) {
  case Page::Dashboard:
    return dashboard.showDashboard;
  case Page::Media:
    return dashboard.showMedia;
  case Page::Performance:
    return dashboard.showPerformance;
  case Page::Weather:
    return dashboard.showWeather;
  case Page::Count:
    return false;
  }
  return false;
}

DashboardPanel::Page DashboardPanel::firstEnabledPage() const {
  for (const auto& meta : kPages) {
    if (pageEnabled(meta.page)) {
      return meta.page;
    }
  }
  // Settings prevents an unusable panel even if every optional page is hidden.
  return Page::Dashboard;
}

DashboardPanel::Page DashboardPanel::pageFromContext(std::string_view context) const {
  if (context.empty()) {
    return pageEnabled(m_activePage) ? m_activePage : firstEnabledPage();
  }
  if (context == "home" || context == "dashboard") {
    return pageEnabled(Page::Dashboard) ? Page::Dashboard : firstEnabledPage();
  }
  if (context == "system") {
    context = "performance";
  }
  for (const auto& meta : kPages) {
    if (meta.context == context) {
      return pageEnabled(meta.page) ? meta.page : firstEnabledPage();
    }
  }
  return firstEnabledPage();
}

float DashboardPanel::preferredWidth() const {
  const Page page = root() == nullptr ? pageFromContext(pendingOpenContext()) : m_activePage;
  return scaled(kPages[index(page)].width);
}

float DashboardPanel::preferredHeight() const {
  const Page page = root() == nullptr ? pageFromContext(pendingOpenContext()) : m_activePage;
  return scaled(kPages[index(page)].height);
}

void DashboardPanel::create() {
  const float scale = contentScale();
  m_activePage = pageFromContext(pendingOpenContext());
  m_visiblePages.clear();

  for (auto& page : m_pages) {
    page->setContentScale(scale);
    page->setPanelCardOpacity(panelCardOpacity());
    page->setPanelBordersEnabled(panelBordersEnabled());
  }

  auto root = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .padding = 0.0f,
      .clipChildren = true,
  });

  auto tabs = ui::segmented({
      .out = &m_tabStrip,
      .scale = scale,
      .compact = false,
      .surfaceOpacity = 0.0f,
      .equalSegmentWidths = true,
      .onChange = [this](std::size_t slot) {
        if (slot < m_visiblePages.size()) {
          selectPage(m_visiblePages[slot], true);
        }
      },
      .configure = [this, scale](Segmented& segmented) {
        segmented.setAlign(FlexAlign::Center);
        segmented.setFontSize(Style::fontSizeCaption * scale);
        segmented.setPadding(0.0f);
        segmented.setUnderlineSelection(true);
        segmented.setShowSeparators(false);
      },
  });

  for (const auto& meta : kPages) {
    if (!pageEnabled(meta.page)) {
      continue;
    }
    const std::size_t slot = tabs->addOption(meta.label, meta.glyph);
    m_visiblePages.push_back(meta.page);
    if (auto* button = tabs->optionButton(slot); button != nullptr && button->inputArea() != nullptr) {
      button->inputArea()->setOnAxisHandler([this](const InputArea::PointerData& data) {
        if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL || data.scrollSteps() == 0.0f) {
          return false;
        }
        selectAdjacent(data.scrollSteps() > 0.0f ? 1 : -1);
        return true;
      });
    }
  }
  if (m_visiblePages.empty()) {
    const auto& fallback = kPages[index(Page::Dashboard)];
    tabs->addOption(fallback.label, fallback.glyph);
    m_visiblePages.push_back(Page::Dashboard);
    m_activePage = Page::Dashboard;
  }
  root->addChild(std::move(tabs));
  root->addChild(ui::box({
      .out = &m_tabIndicator,
      .fill = colorSpecFromRole(ColorRole::Primary),
      .radius = Style::scaledRadiusSm(scale),
      .participatesInLayout = false,
      .configure = [](Box& indicator) { indicator.setZIndex(3); },
  }));

  auto bodies = ui::column({
      .out = &m_pageBodies,
      .align = FlexAlign::Stretch,
      .gap = 0.0f,
      .clipChildren = true,
      .flexGrow = 1.0f,
  });
  for (const auto& meta : kPages) {
    auto container = m_pages[index(meta.page)]->create();
    container->setFlexGrow(1.0f);
    container->setParticipatesInLayout(false);
    container->setVisible(false);
    m_pageContainers[index(meta.page)] = container.get();
    bodies->addChild(std::move(container));
  }
  root->addChild(std::move(bodies));
  setRoot(std::move(root));
  if (m_animations != nullptr) {
    this->root()->setAnimationManager(m_animations);
  }

  applyPageVisibility();
  m_pages[index(m_activePage)]->setActive(true);
  m_firstOpenAfterCreate = true;
}

void DashboardPanel::onOpen(std::string_view context) {
  if (m_services.mpris != nullptr) {
    m_services.mpris->refreshPlayers();
  }
  const Page requested = pageFromContext(context);
  selectPage(requested, !m_firstOpenAfterCreate && requested != m_activePage);
  m_firstOpenAfterCreate = false;
}

void DashboardPanel::onClose() {
  if (m_transitionAnimation != 0 && m_animations != nullptr) {
    m_animations->cancel(m_transitionAnimation);
  }
  m_transitionAnimation = 0;
  m_transitionActive = false;
  for (auto& page : m_pages) {
    page->setActive(false);
    page->onClose();
  }
  m_pageContainers.fill(nullptr);
  m_rootLayout = nullptr;
  m_tabStrip = nullptr;
  m_tabIndicator = nullptr;
  m_pageBodies = nullptr;
  clearReleasedRoot();
}

void DashboardPanel::onFrameTick(float deltaMs) {
  m_pages[index(m_activePage)]->onFrameTick(deltaMs);
}

bool DashboardPanel::dismissTransientUi() { return m_pages[index(m_activePage)]->dismissTransientUi(); }

bool DashboardPanel::isContextActive(std::string_view context) const {
  return m_activePage == pageFromContext(context);
}

bool DashboardPanel::handleGlobalKey(
    std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit
) {
  if (!pressed || preedit || modifiers != 0) {
    return false;
  }
  if (sym == XKB_KEY_Left) {
    selectAdjacent(-1);
    return true;
  }
  if (sym == XKB_KEY_Right) {
    selectAdjacent(1);
    return true;
  }
  return false;
}

int DashboardPanel::visibleOrdinal(Page page) const {
  for (std::size_t i = 0; i < m_visiblePages.size(); ++i) {
    if (m_visiblePages[i] == page) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

void DashboardPanel::selectAdjacent(int direction) {
  if (direction == 0 || m_visiblePages.empty()) {
    return;
  }
  const int current = visibleOrdinal(m_activePage);
  const int next = current + direction;
  if (next < 0 || next >= static_cast<int>(m_visiblePages.size())) {
    return;
  }
  selectPage(m_visiblePages[static_cast<std::size_t>(next)], true);
}

void DashboardPanel::selectPage(Page page, bool animated) {
  if (!pageEnabled(page)) {
    page = firstEnabledPage();
  }
  if (page == m_activePage) {
    applyPageVisibility();
    return;
  }

  if (m_transitionAnimation != 0 && m_animations != nullptr) {
    m_animations->cancel(m_transitionAnimation);
    m_transitionAnimation = 0;
    finishTransition();
  }

  const Page previous = m_activePage;
  m_outgoingPage = previous;
  m_activePage = page;
  m_pages[index(previous)]->setActive(false);
  m_pages[index(page)]->setActive(true);
  if (m_tabStrip != nullptr) {
    m_tabStrip->setSelectedIndex(static_cast<std::size_t>(visibleOrdinal(page)));
  }
  syncVisualSize(animated);

  if (!animated || m_animations == nullptr || m_pageBodies == nullptr) {
    applyPageVisibility();
    PanelManager::instance().refresh();
    return;
  }

  m_transitionActive = true;
  m_transitionProgress = 0.0f;
  m_transitionDirection = visibleOrdinal(page) >= visibleOrdinal(previous) ? 1 : -1;
  applyPageVisibility();
  layoutPageContainers(m_pageBodies->width(), m_pageBodies->height());
  m_transitionAnimation = m_animations->animate(
      0.0f, 1.0f, static_cast<float>(Style::animNormal), Easing::FluidSpatial,
      [this](float value) {
        m_transitionProgress = value;
        if (m_pageBodies != nullptr) {
          layoutPageContainers(m_pageBodies->width(), m_pageBodies->height());
        }
        PanelManager::instance().requestRedraw();
      },
      [this]() {
        m_transitionAnimation = 0;
        finishTransition();
        PanelManager::instance().requestLayout();
        PanelManager::instance().requestRedraw();
      },
      m_pageBodies
  );
  PanelManager::instance().requestFrameTick();
}

void DashboardPanel::applyPageVisibility() {
  for (const auto& meta : kPages) {
    auto* container = m_pageContainers[index(meta.page)];
    if (container == nullptr) {
      continue;
    }
    const bool visible = meta.page == m_activePage || (m_transitionActive && meta.page == m_outgoingPage);
    container->setVisible(visible);
  }
  if (m_tabStrip != nullptr) {
    m_tabStrip->setSelectedIndex(static_cast<std::size_t>(visibleOrdinal(m_activePage)));
  }
}

void DashboardPanel::layoutPageContainers(float width, float height) {
  const float travel = std::max(0.0f, width);
  for (const auto& meta : kPages) {
    auto* container = m_pageContainers[index(meta.page)];
    if (container == nullptr || !container->visible()) {
      continue;
    }
    float offset = 0.0f;
    float opacity = 1.0f;
    if (m_transitionActive && travel > 0.0f) {
      const float direction = static_cast<float>(m_transitionDirection);
      if (meta.page == m_outgoingPage) {
        offset = -direction * travel * m_transitionProgress;
        opacity = 1.0f - 0.15f * m_transitionProgress;
      } else if (meta.page == m_activePage) {
        offset = direction * travel * (1.0f - m_transitionProgress);
        opacity = 0.85f + 0.15f * m_transitionProgress;
      }
    }
    container->setSize(width, height);
    container->setPosition(offset, 0.0f);
    container->setOpacity(opacity);
    container->setZIndex(meta.page == m_activePage ? 1 : 0);
  }
  layoutIndicator();
}

void DashboardPanel::finishTransition() {
  m_transitionActive = false;
  m_transitionProgress = 1.0f;
  applyPageVisibility();
  for (auto* container : m_pageContainers) {
    if (container != nullptr) {
      container->setPosition(0.0f, 0.0f);
      container->setOpacity(1.0f);
      container->setZIndex(0);
    }
  }
}

void DashboardPanel::syncVisualSize(bool animate) {
  const auto& meta = kPages[index(m_activePage)];
  PanelManager::instance().requestActivePanelVisualSize(scaled(meta.width), scaled(meta.height), animate);
}

void DashboardPanel::layoutIndicator() {
  if (m_tabIndicator == nullptr || m_tabStrip == nullptr || m_visiblePages.empty()) {
    return;
  }
  const float slotWidth = m_tabStrip->width() / static_cast<float>(m_visiblePages.size());
  const float thickness = std::max(3.0f, Style::borderWidth * 3.0f * contentScale());
  float slot = static_cast<float>(visibleOrdinal(m_activePage));
  if (m_transitionActive) {
    const float from = static_cast<float>(visibleOrdinal(m_outgoingPage));
    slot = from + (slot - from) * m_transitionProgress;
  }
  m_tabIndicator->setPosition(
      m_tabStrip->x() + slotWidth * slot,
      m_tabStrip->y() + std::max(0.0f, m_tabStrip->height() - thickness)
  );
  m_tabIndicator->setSize(slotWidth, thickness);
}

void DashboardPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr || m_pageBodies == nullptr) {
    return;
  }
  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);
  layoutIndicator();
  const float bodyWidth = m_pageBodies->width();
  const float bodyHeight = m_pageBodies->height();
  layoutPageContainers(bodyWidth, bodyHeight);

  const auto layoutPage = [this, &renderer, bodyWidth, bodyHeight](Page page) {
    auto* container = m_pageContainers[index(page)];
    if (container != nullptr && container->visible()) {
      m_pages[index(page)]->layout(renderer, bodyWidth, bodyHeight);
    }
  };
  if (m_transitionActive) {
    layoutPage(m_outgoingPage);
  }
  layoutPage(m_activePage);
}

void DashboardPanel::doUpdate(Renderer& renderer) { m_pages[index(m_activePage)]->update(renderer); }

void DashboardPanel::onPanelBordersChanged(bool enabled) {
  for (auto& page : m_pages) {
    page->setPanelBordersEnabled(enabled);
  }
}

void DashboardPanel::onPanelCardOpacityChanged(float opacity) {
  for (auto& page : m_pages) {
    page->setPanelCardOpacity(opacity);
  }
}
