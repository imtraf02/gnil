#include "calendar/calendar_service.h"

#include "calendar/caldav_client.h"
#include "calendar/caldav_discovery.h"
#include "config/config_service.h"
#include "core/log.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <unordered_set>

namespace {
  constexpr Logger kLog("calendar");
  constexpr const char* kCredentialOwner = "calendar_credentials";
  constexpr const char* kICloudCalDavServerUrl = "https://caldav.icloud.com/";
  // Wide enough for month navigation in the control-center calendar (~1 year each way).
  constexpr auto kWindowBefore = std::chrono::hours{24 * 365};
  constexpr auto kWindowAfter = std::chrono::hours{24 * 365};

  std::int64_t toUnix(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
  }

  std::chrono::system_clock::time_point fromUnix(std::int64_t seconds) {
    return std::chrono::system_clock::time_point{std::chrono::seconds{seconds}};
  }

  std::string caldavServerUrl(const CalendarConfig::Account& account) {
    if (account.provider == "icloud") {
      return kICloudCalDavServerUrl;
    }
    if (account.provider == "custom") {
      return account.serverUrl;
    }
    return {};
  }
} // namespace

CalendarService::CalendarService(
    ConfigService& configService, HttpClient& httpClient, NotificationManager* notifications
)
    : m_configService(configService), m_httpClient(httpClient), m_notifications(notifications) {}

void CalendarService::initialize() {
  m_activeConfig = m_configService.config().calendar;
  m_configService.addReloadCallback([this]() { onConfigReload(); });
  loadCache();
  if (m_activeConfig.enabled) {
    m_nextRefreshAt = std::chrono::steady_clock::now();
  }
}

void CalendarService::addChangeCallback(ChangeCallback callback) {
  if (callback) {
    m_callbacks.push_back(std::move(callback));
  }
}

void CalendarService::notifyChanged() {
  for (auto& callback : m_callbacks) {
    if (callback) {
      callback();
    }
  }
}

void CalendarService::onConfigReload() {
  const CalendarConfig& next = m_configService.config().calendar;
  if (next == m_activeConfig) {
    return;
  }
  m_activeConfig = next;
  if (!m_activeConfig.enabled) {
    m_eventsByAccount.clear();
    m_snapshot = CalendarSnapshot{};
    notifyChanged();
    return;
  }
  // Re-sync soon after any account/config change.
  m_nextRefreshAt = std::chrono::steady_clock::now();
  notifyChanged();
}

int CalendarService::pollTimeoutMs() const {
  const auto now = std::chrono::steady_clock::now();

  int timeout = -1;
  const auto consider = [&](std::chrono::steady_clock::time_point when) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(when - now).count();
    const int clamped = ms < 0 ? 0 : static_cast<int>(std::min<std::int64_t>(ms, 60000));
    timeout = timeout < 0 ? clamped : std::min(timeout, clamped);
  };

  if (m_activeConfig.enabled && !m_refreshing) {
    consider(m_nextRefreshAt);
  }
  return timeout;
}

void CalendarService::tick() {
  const auto now = std::chrono::steady_clock::now();

  if (m_activeConfig.enabled && !m_refreshing && now >= m_nextRefreshAt) {
    startRefresh();
  }
}

void CalendarService::scheduleNextRefresh() {
  const int minutes = std::max<std::int32_t>(1, m_activeConfig.refreshMinutes);
  m_nextRefreshAt = std::chrono::steady_clock::now() + std::chrono::minutes{minutes};
}

void CalendarService::requestRefresh() {
  if (!m_activeConfig.enabled) {
    return;
  }
  m_nextRefreshAt = std::chrono::steady_clock::now();
  notifyChanged();
}

void CalendarService::startRefresh() {
  if (m_activeConfig.accounts.empty()) {
    scheduleNextRefresh();
    return;
  }
  m_refreshing = true;
  m_pendingAccounts = m_activeConfig.accounts.size();
  for (const CalendarConfig::Account& account : m_activeConfig.accounts) {
    if (account.type == "caldav") {
      fetchCalDav(account);
    } else {
      kLog.warn("unknown calendar account type '{}' for id {}", account.type, account.id);
      accountDone(account.id, false, {});
    }
  }
}

void CalendarService::accountDone(const std::string& accountId, bool ok, std::vector<CalendarEvent> events) {
  if (ok) {
    m_eventsByAccount[accountId] = std::move(events);
  }
  if (m_pendingAccounts > 0) {
    --m_pendingAccounts;
  }
  if (m_pendingAccounts == 0) {
    m_refreshing = false;
    rebuildSnapshot();
    saveCache();
    scheduleNextRefresh();
    notifyChanged();
  }
}

void CalendarService::rebuildSnapshot() {
  // Drop cached events for accounts no longer configured.
  for (auto it = m_eventsByAccount.begin(); it != m_eventsByAccount.end();) {
    const bool stillConfigured =
        std::ranges::contains(m_activeConfig.accounts, it->first, &CalendarConfig::Account::id);
    it = stillConfigured ? std::next(it) : m_eventsByAccount.erase(it);
  }

  std::vector<CalendarEvent> merged;
  for (const auto& [accountId, events] : m_eventsByAccount) {
    merged.insert(merged.end(), events.begin(), events.end());
  }
  std::ranges::sort(merged, {}, &CalendarEvent::start);
  m_snapshot.events = std::move(merged);
  m_snapshot.valid = true;
}

void CalendarService::fetchCalDav(const CalendarConfig::Account& account) {
  const std::string serverUrl = caldavServerUrl(account);
  const std::string username = account.username;
  const std::string password = credential(account.id, "password");
  if (serverUrl.empty() || username.empty() || password.empty()) {
    kLog.warn("caldav account {} is missing server_url/username/password", account.id);
    accountDone(account.id, false, {});
    return;
  }

  const auto now = std::chrono::system_clock::now();
  const std::string accountId = account.id;
  const std::string accountColor = account.color;
  const std::vector<std::string> selectedCalendars = account.calendars;
  const bool allowRedirectAuth = account.provider == "icloud";

  calendar::discoverCalDavCollections(
      m_httpClient, serverUrl, username, password, allowRedirectAuth,
      [this, accountId, username, password, accountColor, selectedCalendars, allowRedirectAuth,
       now](bool discovered, std::vector<calendar::CalDavCollection> collections) {
        if (!discovered) {
          accountDone(accountId, false, {});
          return;
        }

        if (!selectedCalendars.empty()) {
          const std::unordered_set<std::string> selected(selectedCalendars.begin(), selectedCalendars.end());
          std::erase_if(collections, [&](const calendar::CalDavCollection& collection) {
            return !selected.contains(collection.id);
          });
        }

        if (collections.empty()) {
          kLog.warn("caldav account {} has no selected calendars after discovery", accountId);
          accountDone(accountId, false, {});
          return;
        }

        struct FetchContext {
          CalendarService* service = nullptr;
          std::string accountId;
          std::size_t pending = 0;
          bool anyOk = false;
          std::vector<CalendarEvent> events;
        };
        auto ctx = std::make_shared<FetchContext>();
        ctx->service = this;
        ctx->accountId = accountId;
        ctx->pending = collections.size();

        for (const calendar::CalDavCollection& collection : collections) {
          calendar::CalDavAccount caldav;
          caldav.url = collection.url;
          caldav.username = username;
          caldav.password = password;
          caldav.calendarName = collection.name;
          caldav.color = accountColor.empty() ? collection.color : accountColor;

          calendar::fetchCalDavEvents(
              m_httpClient, caldav, now - kWindowBefore, now + kWindowAfter, allowRedirectAuth,
              [ctx](bool ok, std::vector<CalendarEvent> events) {
                if (ok) {
                  ctx->anyOk = true;
                  ctx->events.insert(
                      ctx->events.end(), std::make_move_iterator(events.begin()), std::make_move_iterator(events.end())
                  );
                }
                if (ctx->pending > 0) {
                  --ctx->pending;
                }
                if (ctx->pending == 0) {
                  ctx->service->accountDone(ctx->accountId, ctx->anyOk, std::move(ctx->events));
                }
              }
          );
        }
      }
  );
}

std::string CalendarService::credential(const std::string& accountId, const char* field) const {
  return m_configService.stateString(kCredentialOwner, accountId + "_" + field).value_or(std::string{});
}

std::filesystem::path CalendarService::cacheFilePath() {
  const char* xdgCache = std::getenv("XDG_CACHE_HOME");
  std::filesystem::path base;
  if (xdgCache != nullptr && xdgCache[0] != '\0') {
    base = xdgCache;
  } else if (const char* home = std::getenv("HOME"); home != nullptr) {
    base = std::filesystem::path(home) / ".cache";
  } else {
    base = "/tmp";
  }
  return base / "gnil" / "calendar" / "events.json";
}

void CalendarService::loadCache() {
  const std::filesystem::path path = cacheFilePath();
  std::ifstream in(path);
  if (!in.is_open()) {
    return;
  }
  try {
    const auto j = nlohmann::json::parse(in);
    for (const auto& item : j.at("events")) {
      CalendarEvent event;
      event.id = item.value("id", std::string{});
      event.title = item.value("title", std::string{});
      event.calendarName = item.value("calendar", std::string{});
      event.colorHex = item.value("color", std::string{});
      event.location = item.value("location", std::string{});
      event.start = fromUnix(item.value("start", std::int64_t{0}));
      event.end = fromUnix(item.value("end", std::int64_t{0}));
      event.allDay = item.value("all_day", false);
      const std::string account = item.value("account", std::string{});
      m_eventsByAccount[account].push_back(std::move(event));
    }
    if (!m_eventsByAccount.empty()) {
      rebuildSnapshot();
    }
  } catch (const std::exception& e) {
    kLog.warn("failed to load calendar cache: {}", e.what());
    m_eventsByAccount.clear();
  }
}

void CalendarService::saveCache() const {
  const std::filesystem::path path = cacheFilePath();
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    return;
  }

  nlohmann::json events = nlohmann::json::array();
  for (const auto& [accountId, accountEvents] : m_eventsByAccount) {
    for (const CalendarEvent& event : accountEvents) {
      events.push_back({
          {"account", accountId},
          {"id", event.id},
          {"title", event.title},
          {"calendar", event.calendarName},
          {"color", event.colorHex},
          {"location", event.location},
          {"start", toUnix(event.start)},
          {"end", toUnix(event.end)},
          {"all_day", event.allDay},
      });
    }
  }

  const std::filesystem::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out.is_open()) {
      return;
    }
    out << nlohmann::json{{"events", std::move(events)}}.dump();
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
  }
}
