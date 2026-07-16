#include "i18n/i18n_service.h"

#include "core/files/resource_paths.h"
#include "core/log.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace i18n {

  namespace {

    constexpr Logger kLog("i18n");

    void flatten(const nlohmann::json& node, const std::string& prefix, Catalog& out) {
      for (auto it = node.begin(); it != node.end(); ++it) {
        std::string path = prefix.empty() ? it.key() : prefix + "." + it.key();
        if (it->is_object()) {
          flatten(*it, path, out);
        } else if (it->is_string()) {
          out.insert_or_assign(std::move(path), it->get<std::string>());
        }
      }
    }

  } // namespace

  Service& Service::instance() {
    static Service s_instance;
    return s_instance;
  }

  bool Service::loadCatalog(std::string_view lang, Catalog& out) const {
    const std::filesystem::path path = paths::assetPath("translations/" + std::string(lang) + ".json");
    std::ifstream file(path);
    if (!file.is_open()) {
      return false;
    }
    try {
      auto json = nlohmann::json::parse(file);
      if (!json.is_object()) {
        kLog.warn("catalog {} is not a JSON object", path.string());
        return false;
      }
      Catalog fresh;
      flatten(json, {}, fresh);
      out = std::move(fresh);
      return true;
    } catch (const std::exception& e) {
      kLog.error("failed to parse {}: {}", path.string(), e.what());
      return false;
    }
  }

  void Service::init(std::string_view) {
    if (!loadCatalog("en", m_active)) {
      m_active.clear();
      kLog.warn("could not load English catalog");
    }
    m_language = "en";
    kLog.info("language: en (English-only)");
  }

  std::string_view Service::lookup(std::string_view dottedKey) const {
    if (auto it = m_active.find(dottedKey); it != m_active.end()) {
      return it->second;
    }
    return {};
  }

} // namespace i18n
