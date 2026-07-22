#pragma once

#include "config/config_types.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

struct SystemStats {
  struct NetThroughput {
    double rxBytesPerSec{0.0};
    double txBytesPerSec{0.0};
  };

  std::chrono::steady_clock::time_point sampledAt;
  double cpuUsagePercent{0.0};
  double ramUsagePercent{0.0};
  std::uint64_t ramUsedMb{0};
  std::uint64_t ramTotalMb{0};
  std::uint64_t swapUsedMb{0};
  std::uint64_t swapTotalMb{0};
  std::optional<double> cpuTempC;
  bool cpuTempAvailable{false};
  std::optional<double> gpuTempC;
  std::optional<double> gpuUsagePercent;
  std::optional<std::uint64_t> gpuVramUsedBytes;
  std::optional<std::uint64_t> gpuVramTotalBytes;
  double netRxBytesPerSec{0.0};
  double netTxBytesPerSec{0.0};
  std::unordered_map<std::string, NetThroughput> netThroughputByInterface;
  double loadAvg1{0.0};
  double loadAvg5{0.0};
  double loadAvg15{0.0};
};

// Lower-frequency details used by the full Performance dashboard.  The regular
// SystemStats ring intentionally stays small because it is copied into graph
// history on every sample.
struct ProcessStats {
  std::int32_t pid{0};
  std::string name;
  double cpuUsagePercent{0.0};
  std::uint64_t residentBytes{0};
};

struct TemperatureReading {
  std::string group;
  std::string name;
  double temperatureC{0.0};
  std::optional<double> maximumC;
  std::optional<double> criticalC;
};

struct DiskStats {
  std::string device;
  std::string model;
  std::string mountPoint;
  std::uint64_t totalBytes{0};
  std::uint64_t usedBytes{0};
  double readBytesPerSec{0.0};
  double writeBytesPerSec{0.0};
  double activePercent{0.0};
};

struct NetworkDetails {
  std::string interfaceName;
  std::string gateway;
  std::vector<std::string> dnsServers;
  std::uint64_t receivedBytes{0};
  std::uint64_t sentBytes{0};
};

struct SystemDetailsSnapshot {
  std::chrono::steady_clock::time_point sampledAt;
  std::string cpuModel;
  double cpuFrequencyGhz{0.0};
  double cpuBaseFrequencyGhz{0.0};
  std::uint32_t cpuSockets{0};
  std::uint32_t cpuCores{0};
  std::uint32_t cpuThreads{0};
  std::uint32_t processCount{0};
  std::uint64_t memoryAvailableMb{0};
  std::uint64_t memoryCommittedMb{0};
  std::uint64_t memoryCacheMb{0};
  NetworkDetails network;
  std::vector<DiskStats> disks;
  std::vector<ProcessStats> processes;
  std::vector<TemperatureReading> sensors;
};

class SystemMonitorService {
public:
  explicit SystemMonitorService(const SystemConfig::MonitorConfig& config = {});
  ~SystemMonitorService();

  SystemMonitorService(const SystemMonitorService&) = delete;
  SystemMonitorService& operator=(const SystemMonitorService&) = delete;

  static constexpr int kHistorySize = 120;

  [[nodiscard]] bool isRunning() const noexcept;
  void applyConfig(const SystemConfig::MonitorConfig& config);
  void setEnabled(bool enabled);
  [[nodiscard]] SystemStats latest() const;
  [[nodiscard]] std::vector<SystemStats> history(int windowSize = kHistorySize) const;
  [[nodiscard]] std::chrono::steady_clock::duration historySampleInterval() const noexcept;
  [[nodiscard]] double netRxBytesPerSec(std::string_view interfaceName = {}) const;
  [[nodiscard]] double netTxBytesPerSec(std::string_view interfaceName = {}) const;
  [[nodiscard]] SystemDetailsSnapshot detailedStats() const;

  // Detailed scans walk /proc and hwmon. Keep them off unless a visible UI
  // explicitly retains the feed.
  void retainDetailedSampling();
  void releaseDetailedSampling();

  void retainCpuTemp();
  void releaseCpuTemp();
  void retainGpuTemp();
  void releaseGpuTemp();
  void retainGpuUsage();
  void releaseGpuUsage();
  void retainGpuVram();
  void releaseGpuVram();
  void retainDiskPath(const std::string& path);
  void releaseDiskPath(const std::string& path);
  [[nodiscard]] float diskUsagePercent(const std::string& path) const;
  [[nodiscard]] std::vector<float> diskHistory(const std::string& path, int windowSize = kHistorySize) const;

private:
  struct NvidiaNvmlReader;
  struct AmdRsmiReader;
  struct IntelGpuReader;

  struct DiskHistory {
    int refs = 0;
    float latestPercent = 0.0f;
    std::array<float, kHistorySize> history{};
  };

  struct CpuTotals {
    std::uint64_t total{0};
    std::uint64_t idle{0};
  };

  struct GpuVramData {
    std::uint64_t usedBytes{0};
    std::uint64_t totalBytes{0};
    std::string source;
  };

  enum class NvidiaDisplayDeviceState { None, InactiveOnly, Active };

  struct GpuTempData {
    std::optional<double> tempC;
    std::string source;
    std::string detail;
  };

  struct GpuUsageData {
    std::optional<double> percent;
    std::string source;
  };

  void start();
  void stop();
  void samplingLoop();
  void logDetectedSources();

  [[nodiscard]] static std::optional<CpuTotals> readCpuTotals();
  struct MemData {
    std::uint64_t totalKb{0};
    std::uint64_t usedKb{0};
    std::uint64_t swapTotalKb{0};
    std::uint64_t swapUsedKb{0};
  };
  [[nodiscard]] static std::optional<MemData> readMemoryKb();
  [[nodiscard]] static std::optional<double> readCpuTempCelsius(const SystemConfig::MonitorConfig& config);
  [[nodiscard]] static NvidiaDisplayDeviceState detectNvidiaPciDisplayDeviceState();
  [[nodiscard]] NvidiaNvmlReader& ensureNvmlReader();
  [[nodiscard]] AmdRsmiReader& ensureAmdRsmiReader();
  [[nodiscard]] IntelGpuReader& ensureIntelGpuReader();
  [[nodiscard]] GpuTempData readGpuTempData(NvidiaDisplayDeviceState nvidiaDisplayState);
  [[nodiscard]] GpuUsageData readGpuUsageData(NvidiaDisplayDeviceState nvidiaDisplayState);
  [[nodiscard]] GpuUsageData readIntelGpuUsageData();
  [[nodiscard]] std::optional<GpuVramData> readIntelGpuVram();
  [[nodiscard]] std::optional<GpuVramData> readGpuVramData(NvidiaDisplayDeviceState nvidiaDisplayState);
  [[nodiscard]] std::optional<double> readGpuTempCelsius();
  [[nodiscard]] std::optional<double> readGpuUsagePercent();
  [[nodiscard]] std::optional<GpuVramData> readGpuVram();
  [[nodiscard]] static float readDiskUsagePercent(const std::string& path);

  struct NetIfaceBytes {
    std::uint64_t rx{0};
    std::uint64_t tx{0};
  };
  [[nodiscard]] static std::optional<std::unordered_map<std::string, NetIfaceBytes>> readNetBytes();
  [[nodiscard]] static std::optional<std::array<double, 3>> readLoadAvg();
  [[nodiscard]] SystemDetailsSnapshot readDetailedStats(
      std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration interval
  );

  [[nodiscard]] SystemConfig::MonitorConfig pollConfig() const;

  std::atomic<bool> m_running{false};
  std::atomic<int> m_cpuTempRefs{0};
  std::atomic<int> m_gpuTempRefs{0};
  std::atomic<int> m_gpuUsageRefs{0};
  std::atomic<int> m_gpuVramRefs{0};
  std::atomic<int> m_detailedStatsRefs{0};
  std::thread m_thread;
  std::mutex m_wakeMutex;
  std::condition_variable m_wakeCv;

  mutable std::mutex m_configMutex;
  SystemConfig::MonitorConfig m_pollConfig;
  std::chrono::steady_clock::duration m_historyInterval{std::chrono::seconds(1)};

  mutable std::mutex m_statsMutex;
  SystemStats m_latest;
  SystemDetailsSnapshot m_details;
  std::array<SystemStats, kHistorySize> m_history{};
  int m_historyHead = 0;
  std::unordered_map<std::string, DiskHistory> m_diskHistories;
  std::unordered_map<std::string, NetIfaceBytes> m_prevNetBytes;
  struct ProcessCounters {
    std::uint64_t ticks{0};
  };
  struct DiskCounters {
    std::uint64_t sectorsRead{0};
    std::uint64_t sectorsWritten{0};
    std::uint64_t ioMilliseconds{0};
  };
  std::unordered_map<std::int32_t, ProcessCounters> m_prevProcessCounters;
  std::unordered_map<std::string, DiskCounters> m_prevDiskCounters;
  std::unique_ptr<NvidiaNvmlReader> m_nvidiaNvmlReader;
  std::unique_ptr<AmdRsmiReader> m_amdRsmiReader;
  std::unique_ptr<IntelGpuReader> m_intelGpuReader;
};
