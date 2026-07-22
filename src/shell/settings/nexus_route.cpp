#include "shell/settings/nexus_route.h"

#include <algorithm>
#include <array>

namespace {
  constexpr std::array<NexusPageDescriptor, 9> kPages = {{
      {NexusPage::WallpaperStyle, "appearance", "settings.nexus.pages.appearance", "palette"},
      {NexusPage::Network, "network", "settings.nexus.pages.network", "wifi"},
      {NexusPage::ConnectedDevices, "devices", "settings.nexus.pages.connected-devices", "devices"},
      {NexusPage::Audio, "audio", "settings.nexus.pages.audio", "volume_up"},
      {NexusPage::Panels, "desktop", "settings.nexus.pages.desktop-chrome", "dock_to_left"},
      {NexusPage::Apps, "apps", "settings.nexus.pages.apps", "apps"},
      {NexusPage::Services, "automation", "settings.nexus.pages.automation-services", "settings_suggest"},
      {NexusPage::LanguageRegion, "regional", "settings.nexus.pages.language-region", "language"},
      {NexusPage::About, "system", "settings.nexus.pages.system-security", "info"},
  }};
}

std::span<const NexusPageDescriptor> nexusPages() noexcept { return kPages; }

const NexusPageDescriptor& nexusPageDescriptor(NexusPage page) noexcept {
  const auto index = std::min(static_cast<std::size_t>(page), kPages.size() - 1);
  return kPages[index];
}

std::optional<NexusPage> nexusPageFromId(std::string_view id) noexcept {
  if (id == "wallpaper-style") id = "appearance";
  else if (id == "connected-devices") id = "devices";
  else if (id == "panels") id = "desktop";
  else if (id == "services") id = "automation";
  else if (id == "language-region") id = "regional";
  else if (id == "about") id = "system";
  const auto it = std::ranges::find(kPages, id, &NexusPageDescriptor::id);
  return it == kPages.end() ? std::nullopt : std::optional<NexusPage>{it->page};
}

void NexusRoute::setPage(NexusPage page) noexcept {
  if (m_page == page) {
    return;
  }
  m_page = page;
  m_subpages.clear();
  m_selectedEntity.clear();
  m_selectedControl.clear();
  m_pendingControl.clear();
}

void NexusRoute::pushSubpage(NexusSubpage subpage) {
  if (!subpage.id.empty()) {
    m_subpages.push_back(std::move(subpage));
  }
}

bool NexusRoute::popSubpage() {
  if (m_subpages.empty()) {
    return false;
  }
  m_subpages.pop_back();
  m_pendingControl.clear();
  return true;
}

const NexusSubpage* NexusRoute::currentSubpage() const noexcept {
  return m_subpages.empty() ? nullptr : &m_subpages.back();
}

std::string NexusRoute::routeKey() const {
  std::string key(nexusPageDescriptor(m_page).id);
  for (const auto& subpage : m_subpages) {
    key += '/';
    key += subpage.id;
  }
  return key;
}

std::string NexusRoute::deepLinkKey() const {
  std::string key = routeKey();
  if (!m_selectedControl.empty()) {
    key += '/';
    key += m_selectedControl;
  }
  return key;
}

void NexusRoute::setScrollOffset(float offset) { m_scrollOffsets[routeKey()] = std::max(0.0f, offset); }

float NexusRoute::scrollOffset() const noexcept {
  const auto it = m_scrollOffsets.find(routeKey());
  return it == m_scrollOffsets.end() ? 0.0f : it->second;
}

bool NexusRoute::deepLink(std::string_view target) {
  const auto separator = target.find('/');
  const std::string_view pageId = target.substr(0, separator);
  const auto page = nexusPageFromId(pageId);
  if (!page.has_value()) {
    return false;
  }
  if (m_page == *page) {
    m_subpages.clear();
    m_selectedEntity.clear();
    m_selectedControl.clear();
    m_pendingControl.clear();
  } else {
    setPage(*page);
  }
  if (separator != std::string_view::npos) {
    const std::string_view control = target.substr(separator + 1);
    if (!control.empty()) {
      m_selectedControl = std::string(control);
      m_pendingControl = std::string(control);
    }
  }
  return true;
}

NexusHost NexusHostCoordinator::activate(NexusHost host, void* output) noexcept {
  const NexusHost previous = m_host;
  m_host = host;
  m_output = output;
  return previous;
}

void NexusHostCoordinator::release(NexusHost host) noexcept {
  if (m_host != host) {
    return;
  }
  m_host = NexusHost::None;
  m_output = nullptr;
}
