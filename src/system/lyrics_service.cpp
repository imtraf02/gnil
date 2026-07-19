#include "system/lyrics_service.h"

#include "net/http_client.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string_view>

namespace {
  constexpr auto kRequestDebounce = std::chrono::milliseconds(50);

  std::optional<std::int64_t> parseTimestampMs(std::string_view timestamp) {
    const auto colon = timestamp.find(':');
    if (colon == std::string_view::npos) {
      return std::nullopt;
    }
    int minutes = 0;
    const auto minutesResult = std::from_chars(timestamp.data(), timestamp.data() + colon, minutes);
    if (minutesResult.ec != std::errc{}) {
      return std::nullopt;
    }
    double seconds = 0.0;
    try {
      seconds = std::stod(std::string(timestamp.substr(colon + 1)));
    } catch (...) {
      return std::nullopt;
    }
    if (minutes < 0 || seconds < 0.0) {
      return std::nullopt;
    }
    return static_cast<std::int64_t>((static_cast<double>(minutes) * 60.0 + seconds) * 1000.0);
  }
}

LyricsService::LyricsService(HttpClient* http, std::function<void()> onChanged)
    : m_http(http), m_onChanged(std::move(onChanged)) {}

LyricsService::~LyricsService() {
  m_debounceTimer.stop();
  m_aliveGuard.reset();
}

void LyricsService::clear(std::string status) {
  m_lines.clear();
  m_status = std::move(status);
  m_loading = false;
  m_synced = false;
}

void LyricsService::syncTrack(const std::optional<MprisPlayerInfo>& player, bool enabled) {
  if (!enabled || !player.has_value() || player->title.empty()) {
    const std::string nextSignature = enabled ? std::string{} : "disabled";
    if (m_trackSignature != nextSignature || m_loading || !m_lines.empty()) {
      ++m_generation;
      m_debounceTimer.stop();
      m_trackSignature = nextSignature;
      clear(enabled ? "No active track" : "Synced lyrics disabled");
      notifyChanged();
    }
    return;
  }

  const std::string signature = trackSignature(*player);
  if (signature == m_trackSignature) {
    return;
  }
  m_trackSignature = signature;
  const std::uint64_t generation = ++m_generation;
  clear("Finding lyrics…");
  m_loading = true;
  notifyChanged();

  const std::string key = cacheKeyFor(signature);
  m_debounceTimer.stop();
  m_debounceTimer.start(kRequestDebounce, [this, player = *player, generation, key]() mutable {
    beginRequest(std::move(player), generation, key);
  });
}

void LyricsService::beginRequest(MprisPlayerInfo player, std::uint64_t generation, std::string cacheKey) {
  if (generation != m_generation) {
    return;
  }

  const auto cached = cachePath(cacheKey);
  {
    std::ifstream file(cached);
    if (file) {
      std::ostringstream body;
      body << file.rdbuf();
      if (applyResponse(body.str())) {
        m_loading = false;
        notifyChanged();
        return;
      }
    }
  }

  if (m_http == nullptr) {
    clear("Lyrics unavailable offline");
    notifyChanged();
    return;
  }

  std::string url = "https://lrclib.net/api/get?track_name=" + percentEncode(player.title)
      + "&artist_name=" + percentEncode(joinedArtists(player.artists));
  if (!player.album.empty()) {
    url += "&album_name=" + percentEncode(player.album);
  }
  if (player.lengthUs > 0) {
    url += "&duration=" + std::to_string(static_cast<long long>((player.lengthUs + 500000) / 1000000));
  }

  const std::weak_ptr<void> alive = m_aliveGuard;
  m_http->request(
      HttpRequest{
          .method = "GET",
          .url = std::move(url),
          .headers = {"Accept: application/json", "User-Agent: GNIL Shell/5.0 (https://github.com/imtraf/gnil)"},
          .followRedirects = true,
      },
      [this, alive, generation, cached](HttpResponse response) {
        if (alive.expired() || generation != m_generation) {
          return;
        }
        if (!response.transportOk || response.status != 200 || !applyResponse(response.body)) {
          clear(response.status == 404 ? "No synced lyrics found" : "Lyrics unavailable");
          notifyChanged();
          return;
        }

        std::error_code ec;
        std::filesystem::create_directories(cached.parent_path(), ec);
        const auto temporary = cached.string() + ".tmp";
        {
          std::ofstream file(temporary, std::ios::trunc);
          if (file) {
            file << response.body;
          }
        }
        std::filesystem::rename(temporary, cached, ec);
        m_loading = false;
        notifyChanged();
      }
  );
}

bool LyricsService::applyResponse(std::string_view body) {
  try {
    const auto json = nlohmann::json::parse(body);
    const std::string syncedLyrics = json.value("syncedLyrics", std::string{});
    auto parsed = parseSyncedLyrics(syncedLyrics);
    if (parsed.empty()) {
      return false;
    }
    m_lines = std::move(parsed);
    m_synced = true;
    m_status.clear();
    return true;
  } catch (const nlohmann::json::exception&) {
    return false;
  }
}

std::optional<std::size_t> LyricsService::currentLine(std::int64_t positionUs) const {
  if (m_lines.empty()) {
    return std::nullopt;
  }
  const std::int64_t positionMs = std::max<std::int64_t>(0, positionUs / 1000);
  const auto it = std::upper_bound(
      m_lines.begin(), m_lines.end(), positionMs,
      [](std::int64_t value, const LyricLine& line) { return value < line.timeMs; }
  );
  if (it == m_lines.begin()) {
    return std::size_t{0};
  }
  return static_cast<std::size_t>(std::distance(m_lines.begin(), it) - 1);
}

std::vector<LyricLine> LyricsService::parseSyncedLyrics(std::string_view source) {
  std::vector<LyricLine> lines;
  std::size_t offset = 0;
  while (offset <= source.size()) {
    const std::size_t end = source.find('\n', offset);
    std::string_view line = source.substr(offset, end == std::string_view::npos ? source.size() - offset : end - offset);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    const auto close = line.find(']');
    if (line.starts_with('[') && close != std::string_view::npos) {
      if (const auto time = parseTimestampMs(line.substr(1, close - 1)); time.has_value()) {
        std::string text(line.substr(close + 1));
        if (!text.empty()) {
          lines.push_back({.timeMs = *time, .text = std::move(text)});
        }
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    offset = end + 1;
  }
  std::ranges::stable_sort(lines, {}, &LyricLine::timeMs);
  return lines;
}

std::string LyricsService::trackSignature(const MprisPlayerInfo& player) {
  return std::format("{}\n{}\n{}\n{}", player.title, joinedArtists(player.artists), player.album, player.lengthUs);
}

std::string LyricsService::percentEncode(std::string_view value) {
  std::ostringstream out;
  out << std::uppercase << std::hex;
  for (const unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'
        || c == '.' || c == '~') {
      out << static_cast<char>(c);
    } else {
      out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    }
  }
  return out.str();
}

std::string LyricsService::cacheKeyFor(std::string_view signature) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char c : signature) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return std::format("{:016x}", hash);
}

std::filesystem::path LyricsService::cachePath(std::string_view key) {
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && xdg[0] != '\0') {
    return std::filesystem::path(xdg) / "gnil" / "lyrics" / (std::string(key) + ".json");
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    return std::filesystem::path(home) / ".cache" / "gnil" / "lyrics" / (std::string(key) + ".json");
  }
  return std::filesystem::path("/tmp") / "gnil-lyrics" / (std::string(key) + ".json");
}

void LyricsService::notifyChanged() {
  if (m_onChanged) {
    m_onChanged();
  }
}
