#include "shell/tray/tray_menu.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "render/animation/animation.h"
#include "render/core/renderer.h"
#include "shell/panel/panel_manager.h"
#include "shell/tray/tray_identifier.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/scroll_view.h"
#include "ui/style.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <iterator>
#include <limits>
#include <ranges>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  constexpr Logger kLog("tray");
  constexpr std::size_t kVisibleItems = 20;
  constexpr std::int32_t kPinToggleEntryId = -2147000000;
  constexpr std::int32_t kBackEntryId = -2147000001;

  bool trayDrawerEnabled(const ConfigService* config) {
    if (config == nullptr) return false;
    const auto it = config->config().widgets.find("tray");
    return it != config->config().widgets.end() && it->second.getBool("drawer", false);
  }

  std::string readableLabel(const TrayMenuEntry& entry) {
    if (!entry.label.empty()) return entry.label;
    std::string_view icon = entry.iconName;
    if (icon.ends_with("-symbolic")) icon.remove_suffix(9);
    std::string result;
    bool upper = true;
    for (const char c : icon) {
      if (c == '-' || c == '_') {
        result.push_back(' ');
        upper = true;
      } else {
        result.push_back(upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
        upper = false;
      }
    }
    return result;
  }
} // namespace

TrayMenu::TrayMenu(ConfigService* config, TrayService* tray) : m_config(config), m_tray(tray) {}

void TrayMenu::create() {
  auto scroll = std::make_unique<ScrollView>();
  m_scrollView = scroll.get();
  scroll->setViewportPaddingH(0.0f);
  scroll->setViewportPaddingV(0.0f);
  // Tray menus keep wheel/touchpad scrolling for exceptionally long menus,
  // but their compact contextual surface does not need persistent scrollbar
  // chrome. Normal menus are sized to their full intrinsic height below.
  scroll->setScrollbarVisible(false);
  scroll->clearFill();
  scroll->clearBorder();
  scroll->setRadius(0.0f);

  const float menuWidth = std::max(1.0f, preferredWidth() - Style::panelPadding * contentScale() * 2.0f);
  auto transition = std::make_unique<Node>();
  m_menuTransition = transition.get();
  transition->setSize(menuWidth, initialVisualHeight());
  auto menu = std::make_unique<ContextMenuControl>();
  m_menu = menu.get();
  menu->setContentScale(contentScale());
  menu->setMenuWidth(menuWidth);
  menu->setMaxVisible(kVisibleItems);
  menu->setExternalChrome(true);
  menu->setRedrawCallback([]() { PanelManager::instance().requestRedraw(); });
  menu->setOnActivate([this](const ContextMenuControlEntry& entry) { activate(entry.id); });
  menu->setOnSubmenuOpen([this](const ContextMenuControlEntry& entry, float) { openSubmenu(entry.id); });
  transition->addChild(std::move(menu));
  scroll->content()->addChild(std::move(transition));
  setRoot(std::move(scroll));
  rebuildMenu(true, false);
}

void TrayMenu::onOpen(std::string_view context) {
  const std::string nextItem(context);
  if (nextItem.empty()) return;
  const bool switchingItem = !m_activeItemId.empty() && m_activeItemId != nextItem;
  if (switchingItem && m_tray != nullptr) {
    for (auto it = m_levels.rbegin(); it != m_levels.rend(); ++it) {
      if (it->parentId != 0) m_tray->notifyMenuClosed(m_activeItemId, it->parentId);
    }
    m_tray->notifyMenuClosed(m_activeItemId);
  }
  m_retryTimer.stop();
  m_activeItemId = nextItem;
  m_levels.clear();
  if (m_tray != nullptr) m_tray->notifyMenuOpened(m_activeItemId);
  m_levels.push_back(Level{.parentId = 0, .entries = rootEntries()});
  // First open is revealed by PanelManager. Switching between tray apps keeps
  // the same shared panel surface, so animate both its new intrinsic height
  // and the replacement menu content instead of snapping the context.
  rebuildMenu(true, switchingItem);
  requestIntrinsicHeight(switchingItem);
}

void TrayMenu::onClose() {
  m_retryTimer.stop();
  if (m_tray != nullptr && !m_activeItemId.empty()) {
    for (auto it = m_levels.rbegin(); it != m_levels.rend(); ++it) {
      if (it->parentId != 0) m_tray->notifyMenuClosed(m_activeItemId, it->parentId);
    }
    m_tray->notifyMenuClosed(m_activeItemId);
  }
  m_activeItemId.clear();
  m_levels.clear();
  m_scrollView = nullptr;
  m_menuTransition = nullptr;
  m_menu = nullptr;
  m_lastRequestedHeight = 0.0f;
  clearReleasedRoot();
}

void TrayMenu::onConfigReloaded() {
  if (!m_activeItemId.empty()) {
    refreshCurrentLevel();
    rebuildMenu(true, false);
  }
}

bool TrayMenu::isContextActive(std::string_view context) const { return context == m_activeItemId; }

bool TrayMenu::handleGlobalKey(std::uint32_t sym, std::uint32_t, bool pressed, bool preedit) {
  if (!pressed || preedit || sym != XKB_KEY_Escape) return false;
  return goBack();
}

bool TrayMenu::dismissTransientUi() { return goBack(); }

float TrayMenu::preferredHeight() const {
  std::vector<ContextMenuControlEntry> converted;
  const auto entries = currentEntries();
  converted.reserve(entries.size() + (m_levels.size() > 1 ? 1U : 0U));
  if (m_levels.size() > 1) converted.push_back({.id = kBackEntryId, .label = "Back"});
  for (const auto& entry : entries) {
    converted.push_back({
        .id = entry.id, .label = readableLabel(entry), .enabled = entry.enabled,
        .separator = entry.separator, .hasSubmenu = entry.hasSubmenu,
        .checkmark = entry.checkmark, .radio = entry.radio, .toggleState = entry.toggleState,
    });
  }
  const float content = ContextMenuControl::preferredHeight(converted, kVisibleItems, contentScale());
  return std::clamp(content + Style::panelPadding * contentScale() * 2.0f, scaled(80.0f), scaled(720.0f));
}

std::optional<float> TrayMenu::desiredVisualHeight(Renderer& /*renderer*/, float /*visualWidth*/) {
  // PanelManager asks dynamic destinations for this value after create() and
  // onOpen(). At that point the DBus menu entries are available, so returning
  // the real menu height prevents a panel-to-tray switch from falling back to
  // initialVisualHeight() and briefly constraining the menu to 160 px.
  return preferredHeight();
}

void TrayMenu::onTrayChanged() {
  if (m_activeItemId.empty()) return;
  m_needsRefresh = true;
  PanelManager::instance().requestUpdateOnly();
}

void TrayMenu::doLayout(Renderer& renderer, float width, float height) {
  if (m_scrollView == nullptr) return;
  m_scrollView->setSize(width, height);
  m_scrollView->layout(renderer);
}

void TrayMenu::doUpdate(Renderer&) {
  if (!m_needsRefresh) return;
  m_needsRefresh = false;
  refreshCurrentLevel();
  rebuildMenu(true, false);
}

std::vector<TrayMenuEntry> TrayMenu::rootEntries() {
  m_retryTimer.stop();
  if (m_tray == nullptr || m_activeItemId.empty()) return {};
  auto entries = m_tray->menuEntries(m_activeItemId);
  if (!entries.empty() && trayDrawerEnabled(m_config)) {
    entries.insert(entries.begin(), TrayMenuEntry{
        .id = kPinToggleEntryId,
        .label = i18n::tr(activeItemPinned() ? "tray.menu.unpin" : "tray.menu.pin"),
    });
  }
  if (entries.empty()) {
    entries.push_back(TrayMenuEntry{.id = -1, .label = i18n::tr("tray.menu.empty"), .enabled = false});
    scheduleEntryRetry(0);
  }
  return entries;
}

std::vector<TrayMenuEntry> TrayMenu::currentEntries() const {
  return m_levels.empty() ? std::vector<TrayMenuEntry>{} : m_levels.back().entries;
}

void TrayMenu::refreshCurrentLevel() {
  if (m_levels.empty() || m_tray == nullptr || m_activeItemId.empty()) return;
  auto& level = m_levels.back();
  level.entries = level.parentId == 0 ? rootEntries() : m_tray->menuEntriesForParent(m_activeItemId, level.parentId);
}

void TrayMenu::rebuildMenu(bool forward, bool animate) {
  if (m_menu == nullptr) return;
  std::vector<ContextMenuControlEntry> entries;
  if (m_levels.size() > 1) {
    entries.push_back({.id = kBackEntryId, .label = "Back", .enabled = true});
  }
  for (const auto& entry : currentEntries()) {
    entries.push_back({
        .id = entry.id, .label = readableLabel(entry), .enabled = entry.enabled,
        .separator = entry.separator, .hasSubmenu = entry.hasSubmenu,
        .checkmark = entry.checkmark, .radio = entry.radio, .toggleState = entry.toggleState,
    });
  }
  m_menu->setEntries(std::move(entries));
  if (m_menuTransition != nullptr) {
    m_menuTransition->setSize(
        std::max(1.0f, preferredWidth() - Style::panelPadding * contentScale() * 2.0f),
        m_menu->preferredHeight()
    );
  }
  requestIntrinsicHeight(animate);
  if (!animate || m_animations == nullptr || m_menuTransition == nullptr) {
    if (m_menuTransition != nullptr) {
      m_menuTransition->setPosition(0.0f, 0.0f);
      m_menuTransition->setOpacity(1.0f);
    }
    PanelManager::instance().requestLayout();
    return;
  }
  const float travel = scaled(24.0f) * (forward ? 1.0f : -1.0f);
  m_menuTransition->setPosition(travel, 0.0f);
  m_menuTransition->setOpacity(0.0f);
  m_animations->cancelForOwner(m_menuTransition);
  m_animations->animate(
      0.0f, 1.0f, Style::animChromeSpatial, Easing::CaelestiaExpressiveSpatial,
      [this, travel](float progress) {
        if (m_menuTransition == nullptr) return;
        m_menuTransition->setPosition(travel * (1.0f - progress), 0.0f);
        m_menuTransition->setOpacity(progress);
        PanelManager::instance().requestRedraw();
      }, {}, m_menuTransition
  );
  PanelManager::instance().requestFrameTick();
  PanelManager::instance().requestLayout();
}

void TrayMenu::openSubmenu(std::int32_t parentId) {
  if (m_tray == nullptr || m_activeItemId.empty()) return;
  m_tray->notifyMenuOpened(m_activeItemId, parentId);
  auto entries = m_tray->menuEntriesForParent(m_activeItemId, parentId);
  m_levels.push_back(Level{.parentId = parentId, .entries = std::move(entries)});
  rebuildMenu(true, true);
}

bool TrayMenu::goBack() {
  if (m_levels.size() <= 1 || m_tray == nullptr) return false;
  m_tray->notifyMenuClosed(m_activeItemId, m_levels.back().parentId);
  m_levels.pop_back();
  rebuildMenu(false, true);
  return true;
}

void TrayMenu::activate(std::int32_t entryId) {
  if (entryId == kBackEntryId) {
    (void)goBack();
    return;
  }
  if (entryId == kPinToggleEntryId) {
    DeferredCall::callLater([this]() {
      (void)toggleActiveItemPinned();
      PanelManager::instance().closePanel();
    });
    return;
  }
  if (m_tray == nullptr || m_activeItemId.empty()) return;
  const std::string item = m_activeItemId;
  DeferredCall::callLater([this, item, entryId]() {
    if (m_tray != nullptr) (void)m_tray->activateMenuEntry(item, entryId);
    PanelManager::instance().closePanel();
  });
}

void TrayMenu::scheduleEntryRetry(int attempt) {
  constexpr int delays[] = {300, 900, 2000};
  if (attempt >= static_cast<int>(std::size(delays)) || m_tray == nullptr) return;
  const std::string item = m_activeItemId;
  m_retryTimer.start(std::chrono::milliseconds(delays[attempt]), [this, attempt, item]() {
    if (item != m_activeItemId || m_tray == nullptr || !PanelManager::instance().isOpenPanel("tray-menu")) return;
    auto entries = m_tray->menuEntries(item);
    if (entries.empty()) {
      scheduleEntryRetry(attempt + 1);
      return;
    }
    kLog.debug("tray menu recovered (attempt {}) for id={}", attempt + 1, item);
    m_levels.front().entries = std::move(entries);
    rebuildMenu(true, true);
  });
}

void TrayMenu::requestIntrinsicHeight(bool animate) {
  const float target = preferredHeight();
  if (std::abs(target - m_lastRequestedHeight) < 0.5f) return;
  m_lastRequestedHeight = target;
  PanelManager::instance().requestActivePanelVisualSize(preferredWidth(), target, animate);
}

std::optional<TrayItemInfo> TrayMenu::activeTrayItem() const {
  if (m_tray == nullptr || m_activeItemId.empty()) return std::nullopt;
  const auto items = m_tray->items();
  const auto it = std::ranges::find(items, m_activeItemId, &TrayItemInfo::id);
  return it == items.end() ? std::nullopt : std::optional<TrayItemInfo>(*it);
}

bool TrayMenu::activeItemPinned() const {
  if (m_config == nullptr) return false;
  const auto item = activeTrayItem();
  if (!item) return false;
  const auto it = m_config->config().widgets.find("tray");
  const auto pinned = it == m_config->config().widgets.end() ? std::vector<std::string>{}
                                                             : it->second.getStringList("pinned");
  return std::ranges::any_of(pinned, [&](const std::string& token) { return tray::tokenMatchesItem(token, *item); });
}

bool TrayMenu::toggleActiveItemPinned() {
  if (m_config == nullptr) return false;
  const auto item = activeTrayItem();
  if (!item) return false;
  const auto it = m_config->config().widgets.find("tray");
  std::vector<std::string> pinned = it == m_config->config().widgets.end() ? std::vector<std::string>{}
                                                                           : it->second.getStringList("pinned");
  std::erase_if(pinned, [](const std::string& token) {
    return tray::looksGenericStatusItemName(token) || tray::isTransientUniqueIdentifier(token);
  });
  const bool has = std::ranges::any_of(pinned, [&](const std::string& token) {
    return tray::tokenMatchesItem(token, *item);
  });
  if (has) {
    std::erase_if(pinned, [&](const std::string& token) { return tray::tokenMatchesItem(token, *item); });
  } else {
    const std::string token = tray::preferredPinToken(*item);
    if (token.empty()) return false;
    pinned.push_back(token);
  }
  return m_config->setOverride({"widget", "tray", "pinned"}, pinned);
}
