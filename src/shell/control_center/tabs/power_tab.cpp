#include "shell/control_center/tabs/power_tab.h"

#include "dbus/power/power_profiles_service.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/palette.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <string>

using namespace control_center;

namespace {

  ColorRole healthRole(double health) {
    if (health >= 80.0) {
      return ColorRole::Primary;
    }
    if (health >= 50.0) {
      return ColorRole::Tertiary;
    }
    return ColorRole::Error;
  }

  std::string deviceDisplayName(const UPowerDeviceInfo& info) {
    if (!info.model.empty()) {
      return info.model;
    }
    if (!info.vendor.empty()) {
      return info.vendor;
    }
    return i18n::tr("control-center.power.unknown-device");
  }

  void applyProfileButtonPalette(Button* btn, std::string_view profile, bool active = true) {
    if (btn == nullptr) {
      return;
    }
    if (active) {
      ColorRole role = ColorRole::Primary;
      if (profile == "performance") {
        role = ColorRole::Error;
      } else if (profile == "power-saver") {
        role = ColorRole::Tertiary;
      }
      auto palette = Button::defaultPalette(ButtonVariant::TabActive);
      palette.normal.bg = colorSpecFromRole(role);
      palette.normal.label = colorSpecFromRole(ColorRole::OnPrimary);
      palette.hover.bg = colorSpecFromRole(role, 0.9f);
      palette.hover.label = colorSpecFromRole(ColorRole::OnPrimary);
      palette.pressed.bg = colorSpecFromRole(role, 0.8f);
      palette.pressed.label = colorSpecFromRole(ColorRole::OnPrimary);
      btn->setCustomPalette(palette);
    } else {
      auto palette = Button::defaultPalette(ButtonVariant::Tab);
      palette.normal.bg = colorSpecFromRole(ColorRole::SurfaceVariant, 0.35f);
      palette.normal.label = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.75f);
      palette.hover.bg = colorSpecFromRole(ColorRole::SurfaceVariant, 0.6f);
      palette.hover.label = colorSpecFromRole(ColorRole::OnSurface, 1.0f);
      palette.pressed.bg = colorSpecFromRole(ColorRole::SurfaceVariant, 0.8f);
      palette.pressed.label = colorSpecFromRole(ColorRole::OnSurface, 1.0f);
      btn->setCustomPalette(palette);
    }
  }

} // namespace

PowerTab::PowerTab(UPowerService* upower, PowerProfilesService* powerProfiles, bool scrollbarVisible)
    : m_upower(upower), m_powerProfiles(powerProfiles), m_scrollbarVisible(scrollbarVisible) {}

std::unique_ptr<Flex> PowerTab::create() {
  const float scale = contentScale();

  auto tab = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });

  auto scroll = ui::scrollView({
      .scrollbarVisible = m_scrollbarVisible,
      .flexGrow = 1.0f,
      .configure = [](ScrollView& scrollView) {
        scrollView.clearFill();
        scrollView.clearBorder();
      },
  });

  auto* content = scroll->content();
  content->setDirection(FlexDirection::Vertical);
  content->setAlign(FlexAlign::Stretch);
  content->setGap(Style::spaceSm * scale);

  buildStatusCard(*content, scale);
  buildProfilesCard(*content, scale);
  syncPowerProfiles();
  buildHealthCard(*content, scale);
  buildPeripheralsCard(*content, scale);

  m_root->addChild(std::move(scroll));

  return tab;
}

void PowerTab::buildStatusCard(Flex& root, float scale) {
  if (m_upower == nullptr) {
    return;
  }

  auto card = ui::column({
      .gap = Style::spaceXs * scale,
      .configure = [](Flex& section) {
        section.clearFill();
        section.clearBorder();
        section.setDirection(FlexDirection::Vertical);
        section.setAlign(FlexAlign::Stretch);
      },
  });
  m_statusCard = card.get();

  auto topRow = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::glyph({
          .out = &m_statusGlyph,
          .glyph = batteryGlyphName(0.0, BatteryState::Unknown),
          .glyphSize = 24.0f * scale,
          .color = colorSpecFromRole(ColorRole::Primary),
      }),
      ui::label({
          .out = &m_percentLabel,
          .text = "--",
          .fontSize = 28.0f * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      }),
      ui::column(
          {.align = FlexAlign::Start, .justify = FlexJustify::Center, .gap = 0.0f, .flexGrow = 1.0f},
          ui::label({
              .out = &m_stateLabel,
              .text = "",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          }),
          ui::row(
              {.out = &m_timeRow, .align = FlexAlign::Center, .gap = Style::spaceXs * scale, .visible = false},
              ui::label({
                  .out = &m_timeLabel,
                  .text = "",
                  .fontSize = Style::fontSizeCaption * scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.7f),
              })
          )
      ),
      ui::row(
          {.out = &m_rateRow, .align = FlexAlign::Center, .gap = Style::spaceXs * scale, .visible = false},
          ui::glyph({
              .glyph = "bolt",
              .glyphSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.6f),
          }),
          ui::label({
              .out = &m_rateLabel,
              .text = "",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.8f),
          })
      )
  );
  card->addChild(std::move(topRow));

  card->addChild(
      ui::progressBar({
          .out = &m_levelBar,
          .fill = colorSpecFromRole(ColorRole::Primary),
          .track = colorSpecFromRole(ColorRole::Outline, 0.15f),
          .radius = 2.5f * scale,
          .progress = 0.0f,
          .height = 5.0f * scale,
      })
  );

  root.addChild(std::move(card));
}

void PowerTab::buildProfilesCard(Flex& root, float scale) {
  m_profileOrder.clear();
  if (m_powerProfiles != nullptr && !m_powerProfiles->profiles().empty()) {
    const auto& available = m_powerProfiles->profiles();
    for (const auto& candidate : powerProfileOrder()) {
      if (std::ranges::find(available, candidate) != available.end()) {
        m_profileOrder.emplace_back(candidate);
      }
    }
  }
  if (m_profileOrder.empty()) {
    for (const auto& candidate : powerProfileOrder()) {
      m_profileOrder.emplace_back(candidate);
    }
  }

  auto card = ui::column({
      .gap = Style::spaceXs * scale,
      .configure = [](Flex& section) {
        section.clearFill();
        section.clearBorder();
        section.setDirection(FlexDirection::Vertical);
        section.setAlign(FlexAlign::Stretch);
      },
  });
  m_profilesCard = card.get();

  std::vector<ui::SegmentedOption> options;
  options.reserve(m_profileOrder.size());
  for (const auto& profile : m_profileOrder) {
    options.push_back({.label = "", .glyph = std::string(profileGlyphName(profile))});
  }

  card->addChild(
      ui::segmented({
          .out = &m_profiles,
          .options = std::move(options),
          .fontSize = Style::fontSizeTitle * scale,
          .scale = scale,
          .surfaceOpacity = 0.2f,
          .surfaceRole = ColorRole::SurfaceVariant,
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t index) {
            if (m_syncingProfiles || index >= m_profileOrder.size()) {
              return;
            }
            if (m_powerProfiles != nullptr) {
              (void)m_powerProfiles->setActiveProfile(m_profileOrder[index]);
            }
            if (m_profiles != nullptr) {
              for (std::size_t i = 0; i < m_profileOrder.size(); ++i) {
                if (auto* btn = m_profiles->optionButton(i)) {
                  applyProfileButtonPalette(btn, m_profileOrder[i], i == index);
                }
              }
            }
          },
      })
  );

  if (m_profiles != nullptr) {
    for (std::size_t i = 0; i < m_profileOrder.size(); ++i) {
      m_profiles->setOptionTooltip(i, profileLabel(m_profileOrder[i]));
      if (auto* btn = m_profiles->optionButton(i)) {
        applyProfileButtonPalette(btn, m_profileOrder[i], i == 0);
      }
    }
  }

  auto inhibitedRow = ui::row(
      {.out = &m_inhibitedRow, .align = FlexAlign::Center, .gap = Style::spaceXs * scale, .visible = false},
      ui::glyph({
          .glyph = "alert-triangle",
          .glyphSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Error),
      }),
      ui::label({
          .out = &m_inhibitedLabel,
          .text = i18n::tr("control-center.power.performance-inhibited"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .flexGrow = 1.0f,
      })
  );
  card->addChild(std::move(inhibitedRow));

  root.addChild(std::move(card));
}

void PowerTab::buildHealthCard(Flex& root, float scale) {
  if (m_upower == nullptr) {
    return;
  }

  auto card = ui::row({
      .out = &m_healthCard,
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceSm * scale,
      .visible = false,
      .configure = [](Flex& section) {
        section.clearFill();
        section.clearBorder();
      },
  });

  card->addChild(
      ui::glyph({
          .glyph = "heart",
          .glyphSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::Tertiary),
      })
  );

  card->addChild(
      ui::progressBar({
          .out = &m_healthBar,
          .fill = colorSpecFromRole(ColorRole::Primary),
          .track = colorSpecFromRole(ColorRole::Outline, 0.15f),
          .radius = 2.0f * scale,
          .progress = 0.0f,
          .width = 80.0f * scale,
          .height = 4.0f * scale,
      })
  );

  card->addChild(
      ui::label({
          .out = &m_healthLabel,
          .text = "--",
          .fontSize = Style::fontSizeCaption * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      })
  );

  root.addChild(std::move(card));
}

void PowerTab::buildPeripheralsCard(Flex& root, float scale) {
  if (m_upower == nullptr) {
    return;
  }

  auto card = ui::column({
      .gap = Style::spaceXs * scale,
      .visible = false,
      .configure = [](Flex& section) {
        section.clearFill();
        section.clearBorder();
        section.setDirection(FlexDirection::Vertical);
        section.setAlign(FlexAlign::Stretch);
      },
  });
  m_peripheralsCard = card.get();

  auto list = ui::column({.out = &m_peripheralsList, .align = FlexAlign::Stretch, .gap = Style::spaceXs * scale});
  card->addChild(std::move(list));

  root.addChild(std::move(card));
}

void PowerTab::onClose() {
  m_root = nullptr;
  m_statusCard = nullptr;
  m_statusGlyph = nullptr;
  m_percentLabel = nullptr;
  m_stateLabel = nullptr;
  m_levelBar = nullptr;
  m_timeRow = nullptr;
  m_timeLabel = nullptr;
  m_rateRow = nullptr;
  m_rateLabel = nullptr;
  m_profilesCard = nullptr;
  m_profiles = nullptr;
  m_inhibitedRow = nullptr;
  m_inhibitedLabel = nullptr;
  m_healthCard = nullptr;
  m_healthLabel = nullptr;
  m_healthBar = nullptr;
  m_peripheralsCard = nullptr;
  m_peripheralsList = nullptr;
  m_peripheralRows.clear();
  m_lastPeripheralKey.clear();
}

void PowerTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_root == nullptr) {
    return;
  }
  rebuildPeripherals();
  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);
}

void PowerTab::doUpdate(Renderer& /*renderer*/) {
  syncBatteryStatus();
  syncPowerProfiles();
  syncBatteryHealth();
  rebuildPeripherals();
}

void PowerTab::syncBatteryStatus() {
  if (m_statusCard == nullptr || m_upower == nullptr) {
    return;
  }

  const UPowerState& state = m_upower->state();
  m_statusCard->setVisible(state.isPresent);
  if (!state.isPresent) {
    return;
  }

  if (m_statusGlyph != nullptr) {
    m_statusGlyph->setGlyph(batteryGlyphName(state.percentage, state.state));
  }
  if (m_percentLabel != nullptr) {
    m_percentLabel->setText(std::format("{:.0f}%", state.percentage));
  }
  if (m_stateLabel != nullptr) {
    m_stateLabel->setText(batteryStateLabel(state.state));
  }
  if (m_levelBar != nullptr) {
    m_levelBar->setProgress(static_cast<float>(std::clamp(state.percentage / 100.0, 0.0, 1.0)));
    const bool low = state.state == BatteryState::Discharging && state.percentage <= 20.0;
    const bool charging = state.state == BatteryState::Charging || state.state == BatteryState::PendingCharge;
    if (low) {
      m_levelBar->setFillGradient(colorSpecFromRole(ColorRole::Error), fixedColorSpec(rgba(0.95f, 0.35f, 0.1f, 1.0f)));
    } else if (charging) {
      m_levelBar->setFillGradient(colorSpecFromRole(ColorRole::Tertiary), colorSpecFromRole(ColorRole::Primary));
    } else {
      m_levelBar->setFillGradient(colorSpecFromRole(ColorRole::Primary), colorSpecFromRole(ColorRole::Secondary));
    }
  }

  const bool charging = state.state == BatteryState::Charging || state.state == BatteryState::PendingCharge;
  const std::int64_t seconds = charging ? state.timeToFull : state.timeToEmpty;
  if (m_timeRow != nullptr) {
    const bool show = seconds > 0;
    m_timeRow->setVisible(show);
    if (show && m_timeLabel != nullptr) {
      const std::string duration = formatDuration(std::chrono::seconds{seconds});
      m_timeLabel->setText(
          i18n::tr(
              charging ? "control-center.power.time-to-full" : "control-center.power.time-to-empty", "time", duration
          )
      );
    }
  }

  if (m_rateRow != nullptr) {
    const bool show = state.energyRate > 0.0;
    m_rateRow->setVisible(show);
    if (show && m_rateLabel != nullptr) {
      m_rateLabel->setText(std::format("{:.1f} W", state.energyRate));
    }
  }
}

void PowerTab::syncPowerProfiles() {
  if (m_profiles == nullptr || m_profileOrder.empty()) {
    return;
  }

  std::size_t index = m_profiles->selectedIndex();
  if (m_powerProfiles != nullptr) {
    const auto& active = m_powerProfiles->activeProfile();
    const auto it = std::ranges::find(m_profileOrder, active);
    if (it != m_profileOrder.end()) {
      index = static_cast<std::size_t>(std::distance(m_profileOrder.begin(), it));
      if (index != m_profiles->selectedIndex()) {
        m_syncingProfiles = true;
        m_profiles->setSelectedIndex(index);
        m_syncingProfiles = false;
      }
    }
  }

  for (std::size_t i = 0; i < m_profileOrder.size(); ++i) {
    if (auto* btn = m_profiles->optionButton(i)) {
      applyProfileButtonPalette(btn, m_profileOrder[i], i == index);
    }
  }

  if (m_inhibitedRow != nullptr && m_powerProfiles != nullptr) {
    m_inhibitedRow->setVisible(!m_powerProfiles->state().performanceInhibited.empty());
  }
}

void PowerTab::syncBatteryHealth() {
  if (m_healthCard == nullptr || m_upower == nullptr) {
    return;
  }

  const UPowerDeviceInfo* battery = m_upower->defaultSystemBattery();
  const bool hasHealth = battery != nullptr && battery->energyFullDesign > 0.0 && battery->energyFull > 0.0;
  m_healthCard->setVisible(hasHealth);
  if (!hasHealth) {
    return;
  }

  const double health = std::clamp(battery->energyFull / battery->energyFullDesign * 100.0, 0.0, 100.0);
  if (m_healthLabel != nullptr) {
    m_healthLabel->setText(std::format("{:.0f}%", health));
  }
  if (m_healthBar != nullptr) {
    m_healthBar->setProgress(static_cast<float>(health / 100.0));
    m_healthBar->setFill(colorSpecFromRole(healthRole(health)));
  }
}

void PowerTab::rebuildPeripherals() {
  if (m_peripheralsCard == nullptr || m_peripheralsList == nullptr || m_upower == nullptr) {
    return;
  }

  std::vector<UPowerDeviceInfo> peripherals;
  for (auto& device : m_upower->batteryDevices()) {
    if (!device.isLaptopBattery() && device.isPresent) {
      peripherals.push_back(std::move(device));
    }
  }

  std::string key;
  for (const auto& device : peripherals) {
    key += device.path;
    key += ';';
  }

  const bool structuralChange = key != m_lastPeripheralKey;
  if (structuralChange) {
    m_lastPeripheralKey = key;

    for (auto& entry : m_peripheralRows) {
      if (entry.row != nullptr) {
        m_peripheralsList->removeChild(entry.row);
      }
    }
    m_peripheralRows.clear();

    const float scale = contentScale();
    for (const auto& device : peripherals) {
      PeripheralRow entry;
      entry.path = device.path;
      auto row = ui::row(
          {.out = &entry.row, .align = FlexAlign::Center, .gap = Style::spaceSm * scale},
          ui::glyph({
              .glyph = batteryDeviceGlyphName(device.type),
              .glyphSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          }),
          ui::label({
              .out = &entry.nameLabel,
              .text = deviceDisplayName(device),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
              .flexGrow = 1.0f,
          }),
          ui::progressBar({
              .out = &entry.bar,
              .fill = colorSpecFromRole(ColorRole::Primary),
              .track = colorSpecFromRole(ColorRole::Surface),
              .radius = Style::sliderTrackHeight * scale * 0.4f * 0.5f,
              .progress = 0.0f,
              .width = 60.0f * scale,
              .height = Style::sliderTrackHeight * scale * 0.4f,
          }),
          ui::label({
              .out = &entry.pctLabel,
              .text = "",
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .minWidth = Style::controlHeightSm * scale,
              .textAlign = TextAlign::End,
          })
      );
      m_peripheralsList->addChild(std::move(row));
      m_peripheralRows.push_back(std::move(entry));
    }
  }

  m_peripheralsCard->setVisible(!peripherals.empty());

  for (std::size_t i = 0; i < m_peripheralRows.size() && i < peripherals.size(); ++i) {
    const auto pct = peripherals[i].state.percentage;
    if (m_peripheralRows[i].pctLabel != nullptr) {
      m_peripheralRows[i].pctLabel->setText(std::format("{:.0f}%", pct));
    }
    if (m_peripheralRows[i].bar != nullptr) {
      m_peripheralRows[i].bar->setProgress(static_cast<float>(pct / 100.0));
      const bool low = pct <= 20.0;
      m_peripheralRows[i].bar->setFill(colorSpecFromRole(low ? ColorRole::Error : ColorRole::Primary));
    }
  }
}
