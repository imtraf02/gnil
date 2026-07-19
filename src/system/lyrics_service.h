#pragma once

#include "core/timer_manager.h"
#include "dbus/mpris/mpris_service.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class HttpClient;

struct LyricLine {
  std::int64_t timeMs = 0;
  std::string text;
};

class LyricsService {
public:
  explicit LyricsService(HttpClient* http, std::function<void()> onChanged = {});
  ~LyricsService();

  void syncTrack(const std::optional<MprisPlayerInfo>& player, bool enabled);
  [[nodiscard]] const std::vector<LyricLine>& lines() const noexcept { return m_lines; }
  [[nodiscard]] bool loading() const noexcept { return m_loading; }
  [[nodiscard]] bool synced() const noexcept { return m_synced; }
  [[nodiscard]] const std::string& status() const noexcept { return m_status; }
  [[nodiscard]] std::optional<std::size_t> currentLine(std::int64_t positionUs) const;

  [[nodiscard]] static std::vector<LyricLine> parseSyncedLyrics(std::string_view source);

private:
  void clear(std::string status = {});
  void beginRequest(MprisPlayerInfo player, std::uint64_t generation, std::string cacheKey);
  bool applyResponse(std::string_view body);
  void notifyChanged();
  [[nodiscard]] static std::string trackSignature(const MprisPlayerInfo& player);
  [[nodiscard]] static std::string percentEncode(std::string_view value);
  [[nodiscard]] static std::string cacheKeyFor(std::string_view signature);
  [[nodiscard]] static std::filesystem::path cachePath(std::string_view key);

  HttpClient* m_http = nullptr;
  std::function<void()> m_onChanged;
  Timer m_debounceTimer;
  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);
  std::string m_trackSignature;
  std::vector<LyricLine> m_lines;
  std::string m_status;
  std::uint64_t m_generation = 0;
  bool m_loading = false;
  bool m_synced = false;
};
