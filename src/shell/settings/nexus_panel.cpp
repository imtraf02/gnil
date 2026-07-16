#include "shell/settings/nexus_panel.h"

#include "render/core/renderer.h"
#include "shell/panel/panel_manager.h"
#include "ui/controls/input.h"

namespace {
  // The standalone SettingsWindow owns modal editors for complex controls such
  // as bar widgets. Map a Nexus page to its corresponding classic section when
  // handing off to that window.
  std::string settingsSectionFor(NexusPage page) {
    switch (page) {
      case NexusPage::WallpaperStyle:
        return "wallpaper";
      case NexusPage::Panels:
        return "bar";
      case NexusPage::Apps:
        return "launcher";
      case NexusPage::Services:
        return "services";
      case NexusPage::LanguageRegion:
        return "shell";
      case NexusPage::Audio:
        return "system";
      case NexusPage::Network:
      case NexusPage::ConnectedDevices:
      case NexusPage::About:
        return "system";
    }
    return "shell";
  }
} // namespace

NexusPanel::NexusPanel(NexusRoute& route, NexusHostCoordinator& coordinator, NexusServices services)
    : m_route(route), m_coordinator(coordinator), m_view(route, services) {}

void NexusPanel::create() {
  setRoot(m_view.build(
      contentScale(),
      [this]() {
        const std::string section = settingsSectionFor(m_route.page());
        (void)m_coordinator.activate(NexusHost::Window, m_coordinator.output());
        PanelManager::instance().closePanel(false);
        if (m_pip) {
          m_pip(section);
        }
      },
      []() { PanelManager::instance().requestLayout(); },
      []() { PanelManager::instance().closePanel(); }
  ));
  if (m_animations != nullptr && root() != nullptr) {
    root()->setAnimationManager(m_animations);
  }
}

void NexusPanel::onOpen(std::string_view context) {
  const NexusHost previous =
      m_coordinator.activate(NexusHost::Panel, PanelManager::instance().attachedPanelOutput());
  if (previous == NexusHost::Window && m_closeWindow) {
    m_closeWindow();
  }
  if (!context.empty()) {
    (void)m_route.deepLink(context);
  }
  m_view.setActive(true);
  m_view.refresh();
}

void NexusPanel::onClose() {
  m_view.setActive(false);
  m_coordinator.release(NexusHost::Panel);
  if (m_coordinator.host() == NexusHost::None) {
    m_view.cancelTransientPrompts();
  }
  clearReleasedRoot();
}

bool NexusPanel::dismissTransientUi() { return m_view.escape(); }

InputArea* NexusPanel::initialFocusArea() const {
  return m_view.initialFocusArea();
}

void NexusPanel::doLayout(Renderer& renderer, float width, float height) { m_view.layout(renderer, width, height); }

void NexusPanel::doUpdate(Renderer& /*renderer*/) { m_view.update(); }
