#include "shell/control_center/content_panel.h"

#include "compositors/compositor_platform.h"
#include "dbus/mpris/mpris_service.h"
#include "render/core/renderer.h"
#include "shell/control_center/tabs/audio_tab.h"
#include "shell/control_center/tabs/bluetooth_tab.h"
#include "shell/control_center/tabs/calendar_tab.h"
#include "shell/control_center/tabs/media_tab.h"
#include "shell/control_center/tabs/monitor_tab.h"
#include "shell/control_center/tabs/network_tab.h"
#include "shell/control_center/tabs/night_light_tab.h"
#include "shell/control_center/tabs/power_tab.h"
#include "shell/control_center/tabs/screen_time_tab.h"
#include "shell/control_center/tabs/system_tab.h"
#include "shell/control_center/tabs/weather_tab.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/scroll_view.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>

ContentPanel::ContentPanel(ContentPanelSpec spec, std::unique_ptr<Tab> content)
    : m_spec(std::move(spec)), m_content(std::move(content)) {}

void ContentPanel::create() {
  const float scale = contentScale();
  m_content->setContentScale(scale);
  m_content->setPanelCardOpacity(panelCardOpacity());
  m_content->setPanelBordersEnabled(panelBordersEnabled());

  auto root = std::make_unique<ScrollView>();
  m_scrollView = root.get();
  root->setViewportPaddingH(0.0f);
  root->setViewportPaddingV(0.0f);
  root->setScrollbarVisible(m_spec.dynamicHeight && m_spec.scrollable);
  if (!m_spec.scrollable) {
    root->setDragScrollingEnabled(false);
    root->setWheelHandler([](float) { return true; });
  }
  auto body = m_content->create();
  m_body = body.get();
  root->content()->addChild(std::move(body));
  setRoot(std::move(root));

  if (m_animations != nullptr && this->root() != nullptr) {
    this->root()->setAnimationManager(m_animations);
  }
}

void ContentPanel::onOpen(std::string_view /*context*/) {
  m_content->setActive(true);
  if (m_spec.onOpen) {
    m_spec.onOpen();
  }
}

void ContentPanel::onClose() {
  m_content->setActive(false);
  m_content->onClose();
  m_scrollView = nullptr;
  m_body = nullptr;
  clearReleasedRoot();
}

void ContentPanel::onFrameTick(float deltaMs) { m_content->onFrameTick(deltaMs); }

bool ContentPanel::dismissTransientUi() { return m_content->dismissTransientUi(); }

bool ContentPanel::deferExternalRefresh() const {
  if (const auto* audio = dynamic_cast<const AudioTab*>(m_content.get()); audio != nullptr) {
    return audio->dragging();
  }
  if (const auto* brightness = dynamic_cast<const MonitorTab*>(m_content.get()); brightness != nullptr) {
    return brightness->dragging();
  }
  if (const auto* nightLight = dynamic_cast<const NightLightTab*>(m_content.get()); nightLight != nullptr) {
    return nightLight->dragging();
  }
  return false;
}

void ContentPanel::onPanelBordersChanged(bool enabled) { m_content->setPanelBordersEnabled(enabled); }

void ContentPanel::onPanelCardOpacityChanged(float opacity) { m_content->setPanelCardOpacity(opacity); }

void ContentPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_scrollView == nullptr || m_body == nullptr) {
    return;
  }
  m_scrollView->setSize(width, height);
  m_scrollView->layout(renderer);

  // Fixed-viewport panels (notably Network and Media) own their scrolling
  // internally.  Passing the outer ScrollView's intrinsic child height here
  // leaves an inner flex-grow ScrollView with no remaining height, so its list
  // is laid out below the visible panel or at zero height.  Give these tabs the
  // actual viewport contract instead; the outer host remains only a clip/input
  // surface and never competes for scrolling.
  const float contentWidth = m_spec.scrollable ? m_body->width() : m_scrollView->contentViewportWidth();
  const float contentHeight = m_spec.scrollable ? m_body->height() : m_scrollView->contentViewportHeight();
  m_content->layout(renderer, std::max(1.0f, contentWidth), std::max(1.0f, contentHeight));
}

void ContentPanel::doUpdate(Renderer& renderer) {
  m_content->update(renderer);
}

std::optional<float> ContentPanel::desiredVisualHeight(Renderer& renderer, float visualWidth) {
  if (!m_spec.dynamicHeight) {
    return preferredHeight();
  }
  if (m_body == nullptr) {
    return std::nullopt;
  }

  const float scale = contentScale();
  const float outerPadding = Style::panelPadding * scale * 2.0f;
  const float contentWidth = std::max(1.0f, visualWidth - outerPadding);
  const float maxContentHeight = std::max(1.0f, 720.0f * scale - outerPadding);
  m_content->prepareIntrinsicLayout(renderer, contentWidth, maxContentHeight);
  auto constraints = LayoutConstraints::unconstrained();
  constraints.setExactWidth(contentWidth);
  const float contentHeight = m_body->measureIntrinsic(renderer, constraints).height;
  if (!std::isfinite(contentHeight)) {
    return std::nullopt;
  }
  return std::max(1.0f, contentHeight + outerPadding);
}

std::vector<NamedContentPanel> makeContentPanels(const ControlCenterServices& services) {
  std::vector<NamedContentPanel> panels;
  panels.reserve(11);

  WaylandConnection* wayland = services.platform != nullptr ? &services.platform->wayland() : nullptr;
  RenderContext* renderContext = PanelManager::instance().renderContext();
  const auto add = [&panels](ContentPanelSpec spec, std::unique_ptr<Tab> content) {
    const std::string id = spec.id;
    panels.emplace_back(id, std::make_unique<ContentPanel>(std::move(spec), std::move(content)));
  };

  add(
      {.id = "media", .naturalWidth = 680.0f, .naturalHeight = 580.0f, .dynamicHeight = true,
       .scrollable = false,
       .onOpen = [mpris = services.mpris]() { if (mpris != nullptr) mpris->refreshPlayers(); }},
      std::make_unique<MediaTab>(
          services.mpris, services.httpClient, services.spectrum, services.config, wayland, renderContext,
          MediaTabPresentation::ReferencePanel
      )
  );
  add(
      {.id = "audio", .naturalWidth = 400.0f},
      std::make_unique<AudioTab>(
          services.audio, services.easyEffects, services.mpris, services.config, wayland, renderContext
      )
  );
  add(
      {.id = "brightness", .naturalWidth = 400.0f},
      std::make_unique<MonitorTab>(services.brightness, services.config)
  );
  add(
      {.id = "night-light", .naturalWidth = 440.0f, .naturalHeight = 610.0f, .dynamicHeight = true},
      std::make_unique<NightLightTab>(services.nightLight, services.config, services.platform)
  );
  add(
      {.id = "system", .naturalWidth = 480.0f},
      std::make_unique<SystemTab>(services.sysmon)
  );
  add(
      {.id = "battery", .naturalWidth = 400.0f},
      std::make_unique<PowerTab>(services.upower, services.powerProfiles)
  );
  add(
      {.id = "network", .naturalWidth = 620.0f, .naturalHeight = 640.0f, .dynamicHeight = false,
       .scrollable = false},
      std::make_unique<NetworkTab>(
          services.network, services.networkSecrets, services.config, wayland, renderContext,
          NetworkTabPresentation::ReferencePanel
      )
  );
  add(
      {.id = "bluetooth", .naturalWidth = 480.0f},
      std::make_unique<BluetoothTab>(services.bluetooth, services.bluetoothAgent)
  );
  add(
      {.id = "weather", .naturalWidth = 480.0f},
      std::make_unique<WeatherTab>(services.weather, services.config)
  );
  add(
      {.id = "calendar", .naturalWidth = 360.0f},
      std::make_unique<CalendarTab>(services.config, services.calendar)
  );
  add(
      {.id = "screen-time", .naturalWidth = 520.0f},
      std::make_unique<ScreenTimeTab>(services.screenTime)
  );
  return panels;
}

bool isContentPanelId(std::string_view id) noexcept {
  return id == "media" || id == "audio" || id == "brightness" || id == "system" || id == "battery"
      || id == "night-light" || id == "network" || id == "bluetooth" || id == "weather" || id == "calendar"
      || id == "screen-time";
}
