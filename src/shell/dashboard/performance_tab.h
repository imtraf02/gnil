#pragma once

#include "core/frame_rate_limiter.h"
#include "render/animation/animation_manager.h"
#include "shell/control_center/control_center_services.h"
#include "shell/control_center/tab.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <vector>

class Button;
class ConfigService;
class Flex;
class Graph;
class Label;
class ProgressBar;
class Segmented;
class SystemMonitorService;

class DashboardPerformanceTab final : public Tab {
public:
  explicit DashboardPerformanceTab(const ControlCenterServices& services);
  ~DashboardPerformanceTab() override;

  std::unique_ptr<Flex> create() override;
  void setActive(bool active) override;
  void onFrameTick(float deltaMs) override;
  void onClose() override;
  [[nodiscard]] bool dismissTransientUi() override;

  struct DetailRow {
    Flex* row = nullptr;
    Label* name = nullptr;
    Label* secondary = nullptr;
    Label* value = nullptr;
  };

private:
  enum class Tray { None, Processes, Sensors };
  static constexpr std::size_t kSummaryRows = 5;
  static constexpr std::size_t kProcessDetailRows = 48;
  static constexpr std::size_t kSensorDetailRows = 32;

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncVisibility();
  void syncGraphs(Renderer& renderer);
  void syncLabels();
  void openTray(Tray tray);
  void closeTray(bool animated);
  void setTrayProgress(float progress);
  void layoutOverlay();

  ControlCenterServices m_services;
  SystemMonitorService* m_monitor = nullptr;
  ConfigService* m_config = nullptr;
  bool m_active = false;
  bool m_detailsRetained = false;
  bool m_graphInitialized = false;
  std::chrono::steady_clock::time_point m_lastSampleAt;
  std::chrono::steady_clock::time_point m_lastDetailsAt;
  FrameRateLimiter m_redrawLimiter{std::chrono::milliseconds{120}};

  Flex* m_root = nullptr;
  Flex* m_topRow = nullptr;
  Flex* m_middleRow = nullptr;
  Flex* m_bottomRow = nullptr;
  Flex* m_cpuCard = nullptr;
  Flex* m_memoryCard = nullptr;
  Flex* m_batteryCard = nullptr;
  Flex* m_networkCard = nullptr;
  Flex* m_diskCard = nullptr;
  Flex* m_systemCard = nullptr;
  Flex* m_processCard = nullptr;
  Flex* m_sensorCard = nullptr;

  Graph* m_cpuGraph = nullptr;
  Graph* m_memoryGraph = nullptr;
  Graph* m_networkGraph = nullptr;
  Label* m_cpuValue = nullptr;
  Label* m_cpuTemp = nullptr;
  Label* m_cpuDetails = nullptr;
  Label* m_memoryValue = nullptr;
  Label* m_memoryDetails = nullptr;
  Label* m_batteryValue = nullptr;
  Label* m_batteryState = nullptr;
  Label* m_batteryElectrical = nullptr;
  ProgressBar* m_batteryLevel = nullptr;
  Segmented* m_powerProfiles = nullptr;
  std::vector<std::string> m_profileOrder;
  Label* m_networkState = nullptr;
  Label* m_networkAddress = nullptr;
  Label* m_networkTotals = nullptr;
  Label* m_networkRates = nullptr;
  Label* m_diskValue = nullptr;
  Label* m_diskName = nullptr;
  Label* m_diskUsage = nullptr;
  Label* m_diskIo = nullptr;
  ProgressBar* m_diskLevel = nullptr;
  std::array<Label*, 6> m_systemLines{};
  std::array<DetailRow, kSummaryRows> m_processSummary{};
  std::array<DetailRow, kSummaryRows> m_sensorSummary{};

  Flex* m_overlay = nullptr;
  Flex* m_processTray = nullptr;
  Flex* m_sensorTray = nullptr;
  std::array<DetailRow, kProcessDetailRows> m_processDetails{};
  std::array<DetailRow, kSensorDetailRows> m_sensorDetails{};
  Tray m_openTray = Tray::None;
  float m_trayProgress = 0.0f;
  AnimationManager::Id m_trayAnimation = 0;
  float m_layoutWidth = 0.0f;
  float m_layoutHeight = 0.0f;
  double m_networkPeak = 10'000.0;
};
