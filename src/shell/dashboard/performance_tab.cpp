#include "shell/dashboard/performance_tab.h"

#include "config/config_service.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/power/power_profiles_service.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel_manager.h"
#include "system/distro_info.h"
#include "system/format_units.h"
#include "system/hardware_info.h"
#include "system/system_monitor_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/graph.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <ranges>
#include <string>

using namespace control_center;

namespace {
  constexpr float kCardGap = Style::spaceSm;
  constexpr float kCardPaddingH = Style::spaceMd;
  constexpr float kCardPaddingV = Style::spaceSm;

  std::unique_ptr<Flex> performanceCard(Flex** out, float scale, float opacity, bool borders, float grow) {
    return ui::column({
        .out = out,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
        .paddingV = kCardPaddingV * scale,
        .paddingH = kCardPaddingH * scale,
        .flexGrow = grow,
        .configure = [scale, opacity, borders](Flex& card) { applySectionCardStyle(card, scale, opacity, borders); },
    });
  }

  void addCardTitle(Flex& card, std::string title, std::string glyph, float scale) {
    card.addChild(ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
        ui::glyph({
            .glyph = std::move(glyph),
            .glyphSize = Style::fontSizeCaption * 1.05f * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        }),
        ui::label({
            .text = std::move(title),
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0f,
        })
    ));
  }

  DashboardPerformanceTab::DetailRow addDetailRow(Flex& parent, float scale, bool withSecondary = true) {
    DashboardPerformanceTab::DetailRow result;
    auto row = ui::row(
        {.out = &result.row, .align = FlexAlign::Center, .gap = Style::spaceXs * scale},
        ui::label({
            .out = &result.name,
            .fontSize = Style::fontSizeMini * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
            .ellipsize = TextEllipsize::End,
            .flexGrow = 1.0f,
        }),
        ui::label({
            .out = &result.secondary,
            .fontSize = Style::fontSizeMini * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.7f),
            .maxLines = 1,
            .visible = withSecondary,
            .participatesInLayout = withSecondary,
        }),
        ui::label({
            .out = &result.value,
            .fontSize = Style::fontSizeMini * scale,
            .fontWeight = FontWeight::Medium,
            .color = colorSpecFromRole(ColorRole::Primary),
            .maxLines = 1,
        })
    );
    parent.addChild(std::move(row));
    return result;
  }

  std::string joinDns(const std::vector<std::string>& servers) {
    std::string result;
    for (const auto& server : servers) {
      if (!result.empty()) {
        result += ", ";
      }
      result += server;
    }
    return result;
  }

  std::string connectivityLabel(const NetworkState& state) {
    if (!state.connected) {
      return i18n::tr("dashboard.performance.network.disconnected");
    }
    if (state.kind == NetworkConnectivity::Wireless && !state.ssid.empty()) {
      return state.ssid;
    }
    return state.kind == NetworkConnectivity::Wired ? i18n::tr("dashboard.performance.network.wired")
                                                     : i18n::tr("dashboard.performance.network.connected");
  }

  float normalizedPercent(double value) { return static_cast<float>(std::clamp(value / 100.0, 0.0, 1.0)); }
} // namespace

DashboardPerformanceTab::DashboardPerformanceTab(const ControlCenterServices& services)
    : m_services(services), m_monitor(services.sysmon), m_config(services.config) {}

DashboardPerformanceTab::~DashboardPerformanceTab() {
  if (m_detailsRetained && m_monitor != nullptr) {
    m_monitor->releaseDetailedSampling();
    m_monitor->releaseCpuTemp();
    m_monitor->releaseGpuTemp();
  }
}

std::unique_ptr<Flex> DashboardPerformanceTab::create() {
  const float scale = contentScale();
  auto root = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = kCardGap * scale,
      .clipChildren = true,
  });

  auto top = ui::row({
      .out = &m_topRow, .align = FlexAlign::Stretch, .gap = kCardGap * scale, .flexGrow = 1.05f,
  });
  {
    auto card = performanceCard(&m_cpuCard, scale, panelCardOpacity(), panelBordersEnabled(), 1.45f);
    addCardTitle(*card, i18n::tr("control-center.system.titles.cpu"), "cpu-usage", scale);
    card->addChild(ui::row(
        {.align = FlexAlign::End, .gap = Style::spaceSm * scale},
        ui::label({
            .out = &m_cpuValue,
            .text = "—",
            .fontSize = Style::fontSizeTitle * 1.35f * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::Primary),
        }),
        ui::label({
            .out = &m_cpuTemp,
            .text = "—",
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::Error),
        }),
        ui::label({
            .out = &m_cpuDetails,
            .text = "",
            .fontSize = Style::fontSizeMini * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 2,
            .flexGrow = 1.0f,
        })
    ));
    auto graph = std::make_unique<Graph>();
    m_cpuGraph = graph.get();
    graph->setColor(colorSpecFromRole(ColorRole::Primary));
    graph->setColor2(colorSpecFromRole(ColorRole::Error));
    graph->setFillOpacity(0.10f);
    graph->setFlexGrow(1.0f);
    card->addChild(std::move(graph));
    top->addChild(std::move(card));
  }
  {
    auto card = performanceCard(&m_memoryCard, scale, panelCardOpacity(), panelBordersEnabled(), 1.1f);
    addCardTitle(*card, i18n::tr("control-center.system.titles.memory"), "memory", scale);
    card->addChild(ui::label({
        .out = &m_memoryValue,
        .text = "—",
        .fontSize = Style::fontSizeTitle * 1.35f * scale,
        .fontWeight = FontWeight::Bold,
        .color = colorSpecFromRole(ColorRole::Primary),
    }));
    card->addChild(ui::label({
        .out = &m_memoryDetails,
        .text = "",
        .fontSize = Style::fontSizeMini * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .maxLines = 2,
    }));
    auto graph = std::make_unique<Graph>();
    m_memoryGraph = graph.get();
    graph->setColor(colorSpecFromRole(ColorRole::Primary));
    graph->setFillOpacity(0.12f);
    graph->setFlexGrow(1.0f);
    card->addChild(std::move(graph));
    top->addChild(std::move(card));
  }
  {
    auto card = performanceCard(&m_batteryCard, scale, panelCardOpacity(), panelBordersEnabled(), 0.92f);
    addCardTitle(*card, i18n::tr("dashboard.performance.battery.title"), "battery", scale);
    card->addChild(ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
        ui::label({
            .out = &m_batteryValue,
            .text = "—",
            .fontSize = Style::fontSizeTitle * 1.55f * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::Primary),
        }),
        ui::label({
            .out = &m_batteryState,
            .text = "",
            .fontSize = Style::fontSizeMini * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 2,
            .flexGrow = 1.0f,
        })
    ));
    card->addChild(ui::progressBar({
        .out = &m_batteryLevel,
        .fill = colorSpecFromRole(ColorRole::Primary),
        .track = colorSpecFromRole(ColorRole::Outline, 0.18f),
        .progress = 0.0f,
        .height = 5.0f * scale,
    }));
    if (m_services.powerProfiles != nullptr && !m_services.powerProfiles->profiles().empty()) {
      std::vector<ui::SegmentedOption> options;
      for (const auto& preferred : powerProfileOrder()) {
        if (std::ranges::find(m_services.powerProfiles->profiles(), preferred)
            == m_services.powerProfiles->profiles().end()) {
          continue;
        }
        m_profileOrder.emplace_back(preferred);
        options.push_back({.label = "", .glyph = std::string(profileGlyphName(preferred)), .tooltip = profileLabel(preferred)});
      }
      card->addChild(ui::segmented({
          .out = &m_powerProfiles,
          .options = std::move(options),
          .scale = scale,
          .compact = true,
          .presentation = SegmentedPresentation::Expressive,
          .surfaceOpacity = 0.22f,
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t index) {
            if (m_services.powerProfiles != nullptr && index < m_profileOrder.size()) {
              (void)m_services.powerProfiles->setActiveProfile(m_profileOrder[index]);
            }
          },
      }));
    }
    card->addChild(ui::label({
        .out = &m_batteryElectrical,
        .text = "",
        .fontSize = Style::fontSizeMini * scale,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .maxLines = 1,
        .textAlign = TextAlign::Center,
    }));
    top->addChild(std::move(card));
  }
  root->addChild(std::move(top));

  auto middle = ui::row({
      .out = &m_middleRow, .align = FlexAlign::Stretch, .gap = kCardGap * scale, .flexGrow = 0.92f,
  });
  {
    auto card = performanceCard(&m_networkCard, scale, panelCardOpacity(), panelBordersEnabled(), 1.75f);
    addCardTitle(*card, i18n::tr("control-center.system.titles.network"), "network", scale);
    card->addChild(ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceMd * scale},
        ui::column(
            {.align = FlexAlign::Stretch, .gap = Style::spaceXs * 0.5f * scale, .width = 165.0f * scale},
            ui::label({
                .out = &m_networkState,
                .text = "—",
                .fontSize = Style::fontSizeCaption * scale,
                .fontWeight = FontWeight::Bold,
                .color = colorSpecFromRole(ColorRole::Primary),
                .maxLines = 1,
            }),
            ui::label({
                .out = &m_networkAddress,
                .text = "",
                .fontSize = Style::fontSizeMini * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .maxLines = 3,
            })
        ),
        ui::column(
            {.align = FlexAlign::Stretch, .gap = Style::spaceXs * 0.5f * scale, .flexGrow = 1.0f},
            ui::label({
                .out = &m_networkTotals,
                .text = "",
                .fontSize = Style::fontSizeMini * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .maxLines = 1,
                .textAlign = TextAlign::End,
            }),
            ui::label({
                .out = &m_networkRates,
                .text = "",
                .fontSize = Style::fontSizeMini * scale,
                .color = colorSpecFromRole(ColorRole::Primary),
                .maxLines = 1,
                .textAlign = TextAlign::End,
            })
        )
    ));
    auto graph = std::make_unique<Graph>();
    m_networkGraph = graph.get();
    graph->setColor(colorSpecFromRole(ColorRole::Primary));
    graph->setColor2(colorSpecFromRole(ColorRole::Secondary));
    graph->setFillOpacity(0.08f);
    graph->setFlexGrow(1.0f);
    card->addChild(std::move(graph));
    middle->addChild(std::move(card));
  }
  {
    auto card = performanceCard(&m_diskCard, scale, panelCardOpacity(), panelBordersEnabled(), 0.95f);
    addCardTitle(*card, i18n::tr("dashboard.performance.disk.title"), "storage", scale);
    card->addChild(ui::label({
        .out = &m_diskValue,
        .text = "—",
        .fontSize = Style::fontSizeTitle * 1.55f * scale,
        .fontWeight = FontWeight::Bold,
        .color = colorSpecFromRole(ColorRole::Primary),
        .textAlign = TextAlign::Center,
    }));
    card->addChild(ui::progressBar({
        .out = &m_diskLevel,
        .fill = colorSpecFromRole(ColorRole::Primary),
        .track = colorSpecFromRole(ColorRole::Outline, 0.18f),
        .progress = 0.0f,
        .height = 7.0f * scale,
    }));
    for (Label** target : {&m_diskName, &m_diskUsage, &m_diskIo}) {
      card->addChild(ui::label({
          .out = target,
          .text = "",
          .fontSize = Style::fontSizeMini * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .textAlign = TextAlign::Center,
      }));
    }
    middle->addChild(std::move(card));
  }
  root->addChild(std::move(middle));

  auto bottom = ui::row({
      .out = &m_bottomRow, .align = FlexAlign::Stretch, .gap = kCardGap * scale, .flexGrow = 0.86f,
  });
  {
    auto card = performanceCard(&m_systemCard, scale, panelCardOpacity(), panelBordersEnabled(), 1.3f);
    addCardTitle(*card, i18n::tr("control-center.system.titles.system"), "device-desktop", scale);
    for (auto& label : m_systemLines) {
      card->addChild(ui::label({
          .out = &label,
          .text = "",
          .fontSize = Style::fontSizeMini * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      }));
    }
    bottom->addChild(std::move(card));
  }
  {
    auto card = performanceCard(&m_processCard, scale, panelCardOpacity(), panelBordersEnabled(), 1.05f);
    addCardTitle(*card, i18n::tr("dashboard.performance.processes.title"), "apps", scale);
    for (auto& row : m_processSummary) {
      row = addDetailRow(*card, scale);
    }
    card->addChild(ui::button({
        .text = i18n::tr("dashboard.performance.processes.view-all"),
        .glyph = "arrow-right",
        .controlHeight = Style::controlHeightSm * scale,
        .variant = ButtonVariant::Ghost,
        .onClick = [this]() { openTray(Tray::Processes); },
    }));
    bottom->addChild(std::move(card));
  }
  {
    auto card = performanceCard(&m_sensorCard, scale, panelCardOpacity(), panelBordersEnabled(), 0.95f);
    addCardTitle(*card, i18n::tr("dashboard.performance.sensors.title"), "temperature", scale);
    for (auto& row : m_sensorSummary) {
      row = addDetailRow(*card, scale, false);
    }
    card->addChild(ui::button({
        .text = i18n::tr("dashboard.performance.sensors.view-all"),
        .glyph = "arrow-right",
        .controlHeight = Style::controlHeightSm * scale,
        .variant = ButtonVariant::Ghost,
        .onClick = [this]() { openTray(Tray::Sensors); },
    }));
    bottom->addChild(std::move(card));
  }
  root->addChild(std::move(bottom));

  auto overlay = ui::column({
      .out = &m_overlay,
      .align = FlexAlign::Stretch,
      .justify = FlexJustify::End,
      .padding = Style::spaceSm * scale,
      .fill = colorSpecFromRole(ColorRole::Shadow, 0.55f),
      .clipChildren = true,
      .visible = false,
      .participatesInLayout = false,
      .configure = [](Flex& node) { node.setZIndex(30); },
  });
  overlay->addChild(ui::inputArea({
      .participatesInLayout = false,
      .onClick = [this](const InputArea::PointerData&) { closeTray(true); },
      .configure = [](InputArea& area) { area.setZIndex(0); },
  }));

  const auto buildTray = [this, scale](Flex** out, std::string title, std::string glyph, auto& rows) {
    auto tray = ui::column({
        .out = out,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .padding = Style::spaceMd * scale,
        .fill = colorSpecFromRole(ColorRole::Surface),
        .radius = Style::scaledRadiusXl(scale),
        .border = colorSpecFromRole(ColorRole::Outline, 0.35f),
        .borderWidth = Style::borderWidth,
        .visible = false,
        .configure = [](Flex& node) {
          node.setZIndex(2);
          node.setShadow(colorSpecFromRole(ColorRole::Shadow, 0.35f), 24.0f, 0.0f, -4.0f);
        },
    });
    tray->addChild(ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
        ui::glyph({
            .glyph = std::move(glyph),
            .glyphSize = Style::fontSizeTitle * scale,
            .color = colorSpecFromRole(ColorRole::Primary),
        }),
        ui::label({
            .text = std::move(title),
            .fontSize = Style::fontSizeTitle * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0f,
        }),
        ui::button({
            .glyph = "x",
            .controlHeight = Style::controlHeightSm * scale,
            .variant = ButtonVariant::Ghost,
            .onClick = [this]() { closeTray(true); },
        })
    ));
    auto scroll = ui::scrollView({
        .scrollbarVisible = true,
        .flexGrow = 1.0f,
        .configure = [](ScrollView& view) {
          view.clearFill();
          view.clearBorder();
        },
    });
    scroll->content()->setDirection(FlexDirection::Vertical);
    scroll->content()->setAlign(FlexAlign::Stretch);
    scroll->content()->setGap(Style::spaceXs * scale);
    for (auto& row : rows) {
      row = addDetailRow(*scroll->content(), scale);
      row.row->setVisible(false);
      row.row->setParticipatesInLayout(false);
    }
    tray->addChild(std::move(scroll));
    return tray;
  };
  overlay->addChild(buildTray(
      &m_processTray, i18n::tr("dashboard.performance.processes.all-title"), "apps", m_processDetails
  ));
  overlay->addChild(buildTray(
      &m_sensorTray, i18n::tr("dashboard.performance.sensors.all-title"), "temperature", m_sensorDetails
  ));
  root->addChild(std::move(overlay));

  syncVisibility();
  return root;
}

void DashboardPerformanceTab::setActive(bool active) {
  m_active = active;
  if (m_monitor != nullptr && active != m_detailsRetained) {
    if (active) {
      m_monitor->retainDetailedSampling();
      m_monitor->retainCpuTemp();
      m_monitor->retainGpuTemp();
    } else {
      m_monitor->releaseDetailedSampling();
      m_monitor->releaseCpuTemp();
      m_monitor->releaseGpuTemp();
    }
    m_detailsRetained = active;
  }
  if (!active) {
    m_redrawLimiter.reset();
    closeTray(false);
  }
}

void DashboardPerformanceTab::onFrameTick(float /*deltaMs*/) {
  if (!m_active || m_monitor == nullptr || !m_monitor->isRunning()) {
    return;
  }
  if (!m_redrawLimiter.shouldStep([]() { PanelManager::instance().requestRedraw(); })) {
    return;
  }
  const auto sampleAt = m_monitor->latest().sampledAt;
  const auto detailsAt = m_monitor->detailedStats().sampledAt;
  if (sampleAt != m_lastSampleAt || detailsAt != m_lastDetailsAt) {
    PanelManager::instance().requestUpdateOnly();
  }
}

void DashboardPerformanceTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_root == nullptr) {
    return;
  }
  m_layoutWidth = contentWidth;
  m_layoutHeight = bodyHeight;
  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);
  const float scale = contentScale();
  const auto sizeGraph = [scale](Graph* graph, Flex* card) {
    if (graph == nullptr || card == nullptr || !card->visible()) {
      return;
    }
    graph->setSize(
        std::max(1.0f, card->width() - card->paddingLeft() - card->paddingRight()),
        std::max(34.0f * scale, graph->height())
    );
    graph->setLineWidth(1.0f * scale);
  };
  sizeGraph(m_cpuGraph, m_cpuCard);
  sizeGraph(m_memoryGraph, m_memoryCard);
  sizeGraph(m_networkGraph, m_networkCard);
  m_root->layout(renderer);
  layoutOverlay();
  if (m_overlay != nullptr && m_overlay->visible()) {
    m_overlay->layout(renderer);
    layoutOverlay();
  }
}

void DashboardPerformanceTab::doUpdate(Renderer& renderer) {
  if (!m_active || m_monitor == nullptr) {
    return;
  }
  syncVisibility();
  syncGraphs(renderer);
  syncLabels();
}

void DashboardPerformanceTab::syncVisibility() {
  if (m_config == nullptr) {
    return;
  }
  const auto& config = m_config->config().dashboard.performance;
  const auto visible = [](Flex* card, bool show) {
    if (card != nullptr) {
      card->setVisible(show);
      card->setParticipatesInLayout(show);
    }
  };
  visible(m_cpuCard, config.showCpu);
  visible(m_memoryCard, config.showMemory);
  visible(m_networkCard, config.showNetwork);
  visible(m_diskCard, config.showStorage);
  const bool batteryPresent = m_services.upower != nullptr && m_services.upower->state().isPresent;
  visible(m_batteryCard, config.showBattery && batteryPresent);
}

void DashboardPerformanceTab::syncGraphs(Renderer& renderer) {
  const auto history = m_monitor->history();
  std::vector<float> cpu;
  std::vector<float> temp;
  std::vector<float> memory;
  std::vector<float> rx;
  std::vector<float> tx;
  cpu.reserve(history.size());
  temp.reserve(history.size());
  memory.reserve(history.size());
  double peak = m_networkPeak;
  for (const auto& sample : history) {
    peak = std::max({peak, sample.netRxBytesPerSec, sample.netTxBytesPerSec});
  }
  m_networkPeak = std::max(10'000.0, peak * 0.96);
  for (const auto& sample : history) {
    cpu.push_back(normalizedPercent(sample.cpuUsagePercent));
    temp.push_back(sample.cpuTempC.has_value() ? normalizedPercent(*sample.cpuTempC) : 0.0f);
    memory.push_back(normalizedPercent(sample.ramUsagePercent));
    rx.push_back(static_cast<float>(std::clamp(sample.netRxBytesPerSec / m_networkPeak, 0.0, 1.0)));
    tx.push_back(static_cast<float>(std::clamp(sample.netTxBytesPerSec / m_networkPeak, 0.0, 1.0)));
  }
  if (m_cpuGraph != nullptr) {
    m_cpuGraph->setValues(std::move(cpu));
    m_cpuGraph->setValues2(std::move(temp));
    m_cpuGraph->sync(renderer);
  }
  if (m_memoryGraph != nullptr) {
    m_memoryGraph->setValues(std::move(memory));
    m_memoryGraph->sync(renderer);
  }
  if (m_networkGraph != nullptr) {
    m_networkGraph->setValues(std::move(rx));
    m_networkGraph->setValues2(std::move(tx));
    m_networkGraph->sync(renderer);
  }
  const auto interval = m_monitor->historySampleInterval();
  float progress = 1.0f;
  const auto latest = m_monitor->latest();
  if (latest.sampledAt != std::chrono::steady_clock::time_point{} && interval > decltype(interval)::zero()) {
    progress = static_cast<float>(std::clamp(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - latest.sampledAt).count()
            / std::chrono::duration<double>(interval).count(),
        0.0, 1.0
    ));
  }
  for (Graph* graph : {m_cpuGraph, m_memoryGraph, m_networkGraph}) {
    if (graph != nullptr) {
      graph->setScroll(progress);
    }
  }
  m_lastSampleAt = latest.sampledAt;
}

void DashboardPerformanceTab::syncLabels() {
  const SystemStats stats = m_monitor->latest();
  const SystemDetailsSnapshot details = m_monitor->detailedStats();
  m_lastDetailsAt = details.sampledAt;
  if (m_cpuValue != nullptr) {
    m_cpuValue->setText(std::format("{:.0f}%", stats.cpuUsagePercent));
  }
  if (m_cpuTemp != nullptr) {
    m_cpuTemp->setText(stats.cpuTempC.has_value() ? std::format("{:.0f}°C", *stats.cpuTempC) : "—");
  }
  if (m_cpuDetails != nullptr) {
    m_cpuDetails->setText(std::format(
        "{:.2f} GHz / {:.2f} GHz base\n{}S · {}C · {}T · {}", details.cpuFrequencyGhz,
        details.cpuBaseFrequencyGhz, details.cpuSockets, details.cpuCores, details.cpuThreads,
        i18n::tr("dashboard.performance.process-count", "count", details.processCount)
    ));
  }
  if (m_memoryValue != nullptr) {
    m_memoryValue->setText(FormatUnits::formatBinaryMibAsGib(stats.ramUsedMb));
  }
  if (m_memoryDetails != nullptr) {
    m_memoryDetails->setText(std::format(
        "{} · {} {}\n{} {} · {} {} · Swap {}", FormatUnits::formatBinaryMibUsageAsGib(stats.ramUsedMb, stats.ramTotalMb),
        i18n::tr("dashboard.performance.memory.available"), FormatUnits::formatBinaryMibAsGib(details.memoryAvailableMb),
        i18n::tr("dashboard.performance.memory.committed"), FormatUnits::formatBinaryMibAsGib(details.memoryCommittedMb),
        i18n::tr("dashboard.performance.memory.cache"), FormatUnits::formatBinaryMibAsGib(details.memoryCacheMb),
        FormatUnits::formatBinaryMibAsGib(stats.swapUsedMb)
    ));
  }

  if (m_services.upower != nullptr && m_batteryCard != nullptr && m_batteryCard->visible()) {
    const auto& battery = m_services.upower->state();
    m_batteryValue->setText(std::format("{:.0f}%", battery.percentage));
    std::string batteryStatus = batteryStateLabel(battery.state);
    const auto remaining = battery.state == BatteryState::Charging ? battery.timeToFull : battery.timeToEmpty;
    if (remaining > 0) {
      batteryStatus += "\n" + formatDuration(std::chrono::seconds{remaining});
    }
    m_batteryState->setText(batteryStatus);
    m_batteryLevel->setProgress(static_cast<float>(std::clamp(battery.percentage / 100.0, 0.0, 1.0)));
    const double current = battery.voltage > 0.0 ? battery.energyRate / battery.voltage : 0.0;
    m_batteryElectrical->setText(std::format(
        "{} · {} · {}", battery.voltage > 0.0 ? std::format("{:.1f} V", battery.voltage) : "— V",
        battery.energyRate > 0.0 ? std::format("{:.1f} W", battery.energyRate) : "— W",
        current > 0.0 ? std::format("{:.2f} A", current) : "— A"
    ));
    if (m_powerProfiles != nullptr && m_services.powerProfiles != nullptr) {
      const auto active = m_services.powerProfiles->activeProfile();
      if (const auto it = std::ranges::find(m_profileOrder, active); it != m_profileOrder.end()) {
        m_powerProfiles->setSelectedIndex(static_cast<std::size_t>(std::distance(m_profileOrder.begin(), it)));
      }
    }
  }

  if (m_services.network != nullptr) {
    const auto& state = m_services.network->state();
    m_networkState->setText(connectivityLabel(state));
    m_networkAddress->setText(std::format(
        "IP  {}\nGW  {}\nDNS {}", state.ipv4.empty() ? "—" : state.ipv4,
        details.network.gateway.empty() ? "—" : details.network.gateway,
        details.network.dnsServers.empty() ? "—" : joinDns(details.network.dnsServers)
    ));
  }
  m_networkTotals->setText(std::format(
      "↓ {}  ↑ {}", FormatUnits::formatDecimalBytesAsGb(static_cast<double>(details.network.receivedBytes)),
      FormatUnits::formatDecimalBytesAsGb(static_cast<double>(details.network.sentBytes))
  ));
  m_networkRates->setText(std::format(
      "↓ {}  ↑ {}", FormatUnits::formatDecimalBytesPerSecond(stats.netRxBytesPerSec),
      FormatUnits::formatDecimalBytesPerSecond(stats.netTxBytesPerSec)
  ));

  if (!details.disks.empty()) {
    const auto& disk = details.disks.front();
    const double usage = disk.totalBytes > 0 ? static_cast<double>(disk.usedBytes) / static_cast<double>(disk.totalBytes) : 0.0;
    m_diskValue->setText(std::format("{:.0f}%", usage * 100.0));
    m_diskLevel->setProgress(static_cast<float>(std::clamp(usage, 0.0, 1.0)));
    m_diskName->setText(disk.model.empty() ? disk.device : disk.model);
    m_diskUsage->setText(FormatUnits::formatDecimalBytesUsage(
        static_cast<double>(disk.usedBytes), static_cast<double>(disk.totalBytes)
    ));
    m_diskIo->setText(std::format(
        "R {} · W {} · {:.0f}%", FormatUnits::formatDecimalBytesPerSecond(disk.readBytesPerSec),
        FormatUnits::formatDecimalBytesPerSecond(disk.writeBytesPerSec), disk.activePercent
    ));
  } else {
    m_diskValue->setText("—");
    m_diskLevel->setProgress(0.0f);
    m_diskName->setText(i18n::tr("dashboard.performance.unavailable"));
    m_diskUsage->setText("");
    m_diskIo->setText("");
  }

  const auto uptime = systemUptime();
  const std::array<std::string, 6> systemValues{
      "CPU  " + (details.cpuModel.empty() ? cpuModelName() : details.cpuModel),
      "GPU  " + gpuLabel(),
      "Board  " + motherboardLabel(),
      "OS  " + distroLabel(),
      "Kernel  " + kernelLabel(),
      "Uptime  " + (uptime.has_value() ? formatDuration(*uptime) : "—"),
  };
  for (std::size_t i = 0; i < m_systemLines.size(); ++i) {
    if (m_systemLines[i] != nullptr) {
      m_systemLines[i]->setText(systemValues[i]);
    }
  }

  const auto syncProcessRow = [](DetailRow& row, const ProcessStats* process) {
    const bool show = process != nullptr;
    row.row->setVisible(show);
    row.row->setParticipatesInLayout(show);
    if (!show) {
      return;
    }
    row.name->setText(process->name);
    row.secondary->setText(std::format("PID {}", process->pid));
    row.value->setText(std::format(
        "{:.1f}% · {}", process->cpuUsagePercent, FormatUnits::formatBinaryBytesAsGib(process->residentBytes)
    ));
  };
  for (std::size_t i = 0; i < m_processSummary.size(); ++i) {
    syncProcessRow(m_processSummary[i], i < details.processes.size() ? &details.processes[i] : nullptr);
  }
  for (std::size_t i = 0; i < m_processDetails.size(); ++i) {
    syncProcessRow(m_processDetails[i], i < details.processes.size() ? &details.processes[i] : nullptr);
  }

  const auto showGpu = m_config == nullptr || m_config->config().dashboard.performance.showGpu;
  std::vector<const TemperatureReading*> visibleSensors;
  for (const auto& sensor : details.sensors) {
    if (showGpu || sensor.group != "GPU") {
      visibleSensors.push_back(&sensor);
    }
  }
  const auto syncSensorRow = [](DetailRow& row, const TemperatureReading* sensor) {
    const bool show = sensor != nullptr;
    row.row->setVisible(show);
    row.row->setParticipatesInLayout(show);
    if (!show) {
      return;
    }
    row.name->setText(sensor->name);
    row.secondary->setText(sensor->group);
    row.value->setText(std::format("{:.0f}°C", sensor->temperatureC));
    const double warning = sensor->maximumC.value_or(80.0);
    const double critical = sensor->criticalC.value_or(90.0);
    row.value->setColor(colorSpecFromRole(
        sensor->temperatureC >= critical ? ColorRole::Error
                                         : (sensor->temperatureC >= warning ? ColorRole::Tertiary : ColorRole::Primary)
    ));
  };
  for (std::size_t i = 0; i < m_sensorSummary.size(); ++i) {
    syncSensorRow(m_sensorSummary[i], i < visibleSensors.size() ? visibleSensors[i] : nullptr);
  }
  for (std::size_t i = 0; i < m_sensorDetails.size(); ++i) {
    syncSensorRow(m_sensorDetails[i], i < visibleSensors.size() ? visibleSensors[i] : nullptr);
  }
}

void DashboardPerformanceTab::openTray(Tray tray) {
  if (tray == Tray::None || m_overlay == nullptr) {
    return;
  }
  m_openTray = tray;
  m_overlay->setVisible(true);
  setTrayProgress(m_trayProgress);
  m_processTray->setVisible(tray == Tray::Processes);
  m_processTray->setParticipatesInLayout(tray == Tray::Processes);
  m_sensorTray->setVisible(tray == Tray::Sensors);
  m_sensorTray->setParticipatesInLayout(tray == Tray::Sensors);
  if (m_trayAnimation != 0 && m_root != nullptr && m_root->animationManager() != nullptr) {
    m_root->animationManager()->cancel(m_trayAnimation);
  }
  if (m_root == nullptr || m_root->animationManager() == nullptr) {
    setTrayProgress(1.0f);
    return;
  }
  m_trayAnimation = m_root->animationManager()->animate(
      m_trayProgress, 1.0f, static_cast<float>(Style::animNormal), Easing::FluidSpatial,
      [this](float value) { setTrayProgress(value); }, [this]() { m_trayAnimation = 0; }, m_overlay
  );
  PanelManager::instance().requestFrameTick();
}

void DashboardPerformanceTab::closeTray(bool animated) {
  if (m_openTray == Tray::None || m_overlay == nullptr) {
    return;
  }
  if (m_trayAnimation != 0 && m_root != nullptr && m_root->animationManager() != nullptr) {
    m_root->animationManager()->cancel(m_trayAnimation);
    m_trayAnimation = 0;
  }
  const auto finish = [this]() {
    m_trayAnimation = 0;
    m_openTray = Tray::None;
    m_trayProgress = 0.0f;
    if (m_overlay != nullptr) {
      m_overlay->setVisible(false);
    }
  };
  if (!animated || m_root == nullptr || m_root->animationManager() == nullptr) {
    finish();
    return;
  }
  m_trayAnimation = m_root->animationManager()->animate(
      m_trayProgress, 0.0f, static_cast<float>(Style::animFast), Easing::EaseInCubic,
      [this](float value) { setTrayProgress(value); }, finish, m_overlay
  );
  PanelManager::instance().requestFrameTick();
}

void DashboardPerformanceTab::setTrayProgress(float progress) {
  m_trayProgress = std::clamp(progress, 0.0f, 1.0f);
  if (m_overlay != nullptr) {
    m_overlay->setOpacity(m_trayProgress);
  }
  layoutOverlay();
  PanelManager::instance().requestRedraw();
}

void DashboardPerformanceTab::layoutOverlay() {
  if (m_overlay == nullptr) {
    return;
  }
  m_overlay->setPosition(0.0f, 0.0f);
  m_overlay->setSize(m_layoutWidth, m_layoutHeight);
  if (auto* scrim = m_overlay->children().empty() ? nullptr : m_overlay->children().front().get(); scrim != nullptr) {
    scrim->setPosition(0.0f, 0.0f);
    scrim->setSize(m_layoutWidth, m_layoutHeight);
  }
  Flex* tray = m_openTray == Tray::Processes ? m_processTray : m_sensorTray;
  if (tray == nullptr || m_openTray == Tray::None) {
    return;
  }
  const float desiredHeight = m_layoutHeight * (m_openTray == Tray::Processes ? 0.75f : 0.62f);
  tray->setSize(std::max(1.0f, m_layoutWidth - 2.0f * Style::spaceSm * contentScale()), desiredHeight);
  tray->setPosition(
      Style::spaceSm * contentScale(),
      m_layoutHeight - desiredHeight - Style::spaceSm * contentScale() + (1.0f - m_trayProgress) * desiredHeight
  );
}

bool DashboardPerformanceTab::dismissTransientUi() {
  if (m_openTray == Tray::None) {
    return false;
  }
  closeTray(true);
  return true;
}

void DashboardPerformanceTab::onClose() {
  setActive(false);
  m_root = nullptr;
  m_topRow = nullptr;
  m_middleRow = nullptr;
  m_bottomRow = nullptr;
  m_cpuCard = nullptr;
  m_memoryCard = nullptr;
  m_batteryCard = nullptr;
  m_networkCard = nullptr;
  m_diskCard = nullptr;
  m_systemCard = nullptr;
  m_processCard = nullptr;
  m_sensorCard = nullptr;
  m_cpuGraph = nullptr;
  m_memoryGraph = nullptr;
  m_networkGraph = nullptr;
  m_overlay = nullptr;
  m_processTray = nullptr;
  m_sensorTray = nullptr;
  m_powerProfiles = nullptr;
  m_trayAnimation = 0;
  m_openTray = Tray::None;
  m_trayProgress = 0.0f;
}
