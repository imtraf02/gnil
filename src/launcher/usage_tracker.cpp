#include "launcher/usage_tracker.h"

#include "util/file_utils.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

UsageTracker::UsageTracker() {
  const std::string dir = FileUtils::stateDir();
  m_usageCountsPath = (dir.empty() ? "." : dir) + "/usage_counts.json";
  load();
}

void UsageTracker::record(std::string_view providerId, std::string_view resultId) {
  ++m_counts[std::string(providerId)][std::string(resultId)];
  save();
}

void UsageTracker::clear() {
  m_counts.clear();
  save();
}

int UsageTracker::getCount(std::string_view providerId, std::string_view resultId) const {
  const auto provIt = m_counts.find(std::string(providerId));
  if (provIt == m_counts.end()) {
    return 0;
  }
  const auto idIt = provIt->second.find(std::string(resultId));
  return idIt != provIt->second.end() ? idIt->second : 0;
}

void UsageTracker::load() {
  std::ifstream file(m_usageCountsPath);
  if (file.is_open()) {
    try {
      const auto json = nlohmann::json::parse(file);
      for (const auto& [provider, ids] : json.items()) {
        for (const auto& [id, count] : ids.items()) {
          m_counts[provider][id] = count.get<int>();
        }
      }
    } catch (const nlohmann::json::exception&) {
      // Ignore malformed file — starts fresh
    }
  }
}

void UsageTracker::save() const {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(m_usageCountsPath).parent_path(), ec);
  if (ec) {
    return;
  }
  nlohmann::json json = m_counts;
  std::ofstream file(m_usageCountsPath, std::ios::trunc);
  file << json.dump(2) << '\n';
}
