#pragma once

#include "calendar/calendar_types.h"
#include "config/config_types.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

class ConfigService;
class HttpClient;
class NotificationManager;

// Background service that syncs configured CalDAV calendars and exposes a merged,
// read-only event snapshot. Modeled on
// WeatherService: timer-driven via pollTimeoutMs()/tick(), credentials live in state.toml, and a
// disk cache provides last-known-good data across restarts and network failures.
class CalendarService {
public:
  using ChangeCallback = std::function<void()>;
  CalendarService(ConfigService& configService, HttpClient& httpClient, NotificationManager* notifications = nullptr);

  void initialize();
  void addChangeCallback(ChangeCallback callback);

  [[nodiscard]] int pollTimeoutMs() const;
  void tick();

  [[nodiscard]] bool enabled() const noexcept { return m_activeConfig.enabled; }
  [[nodiscard]] bool hasData() const noexcept { return m_snapshot.valid; }
  [[nodiscard]] const CalendarSnapshot& snapshot() const noexcept { return m_snapshot; }

  // Schedule an immediate sync (used after saving CalDAV credentials).
  void requestRefresh();

private:
  void onConfigReload();
  void notifyChanged();
  void startRefresh();
  void accountDone(const std::string& accountId, bool ok, std::vector<CalendarEvent> events);
  void rebuildSnapshot();
  void scheduleNextRefresh();

  void fetchCalDav(const CalendarConfig::Account& account);

  // Credential helpers (state.toml, owner "calendar_credentials").
  [[nodiscard]] std::string credential(const std::string& accountId, const char* field) const;

  void loadCache();
  void saveCache() const;
  [[nodiscard]] static std::filesystem::path cacheFilePath();

  ConfigService& m_configService;
  HttpClient& m_httpClient;
  NotificationManager* m_notifications = nullptr;
  CalendarConfig m_activeConfig;
  std::vector<ChangeCallback> m_callbacks;

  CalendarSnapshot m_snapshot;
  std::map<std::string, std::vector<CalendarEvent>> m_eventsByAccount;
  std::chrono::steady_clock::time_point m_nextRefreshAt;
  bool m_refreshing = false;
  std::size_t m_pendingAccounts = 0;
};
