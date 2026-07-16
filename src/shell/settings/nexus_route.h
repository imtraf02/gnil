#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

enum class NexusPage : std::size_t {
  WallpaperStyle,
  Network,
  ConnectedDevices,
  Audio,
  Panels,
  Apps,
  Services,
  LanguageRegion,
  About,
};

struct NexusPageDescriptor {
  NexusPage page;
  std::string_view id;
  std::string_view title;
  std::string_view glyph;
};

[[nodiscard]] std::span<const NexusPageDescriptor> nexusPages() noexcept;
[[nodiscard]] const NexusPageDescriptor& nexusPageDescriptor(NexusPage page) noexcept;
[[nodiscard]] std::optional<NexusPage> nexusPageFromId(std::string_view id) noexcept;

struct NexusSubpage {
  std::string id;
  std::string title;
  bool operator==(const NexusSubpage&) const = default;
};

class NexusRoute {
public:
  [[nodiscard]] NexusPage page() const noexcept { return m_page; }
  void setPage(NexusPage page) noexcept;

  void pushSubpage(NexusSubpage subpage);
  [[nodiscard]] bool popSubpage();
  [[nodiscard]] const std::vector<NexusSubpage>& subpages() const noexcept { return m_subpages; }
  [[nodiscard]] const NexusSubpage* currentSubpage() const noexcept;

  void setSelectedEntity(std::string entity) { m_selectedEntity = std::move(entity); }
  [[nodiscard]] const std::string& selectedEntity() const noexcept { return m_selectedEntity; }

  void setQuery(std::string query) { m_query = std::move(query); }
  [[nodiscard]] const std::string& query() const noexcept { return m_query; }
  void clearQuery() { m_query.clear(); }

  void setSelectedControl(std::string control) { m_selectedControl = std::move(control); }
  [[nodiscard]] const std::string& selectedControl() const noexcept { return m_selectedControl; }

  void setFocusKey(std::string key) { m_focusKey = std::move(key); }
  [[nodiscard]] const std::string& focusKey() const noexcept { return m_focusKey; }

  void setScrollOffset(float offset);
  [[nodiscard]] float scrollOffset() const noexcept;

  void setPendingControl(std::string control) { m_pendingControl = std::move(control); }
  [[nodiscard]] const std::string& pendingControl() const noexcept { return m_pendingControl; }
  void clearPendingControl() { m_pendingControl.clear(); }

  [[nodiscard]] bool deepLink(std::string_view target);
  [[nodiscard]] std::string routeKey() const;
  [[nodiscard]] std::string deepLinkKey() const;

private:
  NexusPage m_page = NexusPage::WallpaperStyle;
  std::vector<NexusSubpage> m_subpages;
  std::string m_selectedEntity;
  std::string m_query;
  std::string m_selectedControl;
  std::string m_focusKey = "settings.search";
  std::string m_pendingControl;
  std::unordered_map<std::string, float> m_scrollOffsets;
};

enum class NexusHost { None, Panel, Window };

class NexusHostCoordinator {
public:
  [[nodiscard]] NexusHost activate(NexusHost host, void* output) noexcept;
  void release(NexusHost host) noexcept;
  [[nodiscard]] NexusHost host() const noexcept { return m_host; }
  [[nodiscard]] void* output() const noexcept { return m_output; }
  [[nodiscard]] bool isExclusive() const noexcept { return m_host != NexusHost::None; }

private:
  NexusHost m_host = NexusHost::None;
  void* m_output = nullptr;
};
