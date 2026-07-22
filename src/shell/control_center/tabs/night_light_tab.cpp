#include "shell/control_center/tabs/night_light_tab.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "system/day_night_schedule.h"
#include "system/gamma_service.h"
#include "ui/builders.h"
#include "ui/controls/input.h"
#include "ui/controls/segmented.h"
#include "ui/controls/slider.h"
#include "ui/controls/toggle.h"
#include "ui/palette.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>

using namespace control_center;

namespace {

  std::unique_ptr<Flex> sectionHeading(std::string glyph, std::string title, float scale) {
    return ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
        ui::glyph({
            .glyph = std::move(glyph),
            .glyphSize = Style::fontSizeTitle * scale,
            .color = colorSpecFromRole(ColorRole::Primary),
        }),
        ui::label({
            .text = std::move(title),
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0f,
        })
    );
  }

  std::unique_ptr<Flex> temperatureRow(
      std::string glyph, std::string label, Label** valueLabel, Slider** slider, double value, float scale,
      std::function<void(double)> onChanged, std::function<void()> onDragEnd
  ) {
    auto row = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale});
    row->addChild(ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
        ui::glyph({
            .glyph = std::move(glyph),
            .glyphSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::Tertiary),
        }),
        ui::label({
            .text = std::move(label),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0f,
        }),
        ui::label({
            .out = valueLabel,
            .text = std::format("{}K", static_cast<int>(value)),
            .fontSize = Style::fontSizeCaption * scale,
            .fontWeight = FontWeight::SemiBold,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    ));
    row->addChild(ui::slider({
        .out = slider,
        .minValue = NightLightConfig::kTemperatureMin,
        .maxValue = NightLightConfig::kTemperatureMax,
        .step = 100.0,
        .value = value,
        .presentation = SliderPresentation::LevelCompact,
        .trackHeight = 18.0f * scale,
        .thumbSize = 30.0f * scale,
        .controlHeight = 34.0f * scale,
        .onValueChanged = std::move(onChanged),
        .onDragEnd = std::move(onDragEnd),
    }));
    return row;
  }

} // namespace

NightLightTab::NightLightTab(GammaService* nightLight, ConfigService* config, CompositorPlatform* platform)
    : m_nightLight(nightLight), m_config(config), m_platform(platform) {}

std::unique_ptr<Flex> NightLightTab::create() {
  const float scale = contentScale();
  const bool available = m_platform != nullptr && m_platform->hasGammaControl() && m_nightLight != nullptr;
  const auto& night = m_config != nullptr ? m_config->config().nightlight : NightLightConfig{};
  const auto& location = m_config != nullptr ? m_config->config().location : LocationConfig{};
  m_pendingDay = night.dayTemperature;
  m_pendingNight = night.nightTemperature;
  m_pendingSunset = day_night_schedule::normalizedClock(location.sunset).value_or("20:30");
  m_pendingSunrise = day_night_schedule::normalizedClock(location.sunrise).value_or("07:30");

  auto root = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });

  auto hero = ui::row({
      .out = &m_heroCard,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .configure = [this, scale](Flex& card) {
        applySectionCardStyle(card, scale, panelCardOpacity(), panelBordersEnabled());
        card.setDirection(FlexDirection::Horizontal);
        card.setAlign(FlexAlign::Center);
      },
  });
  m_cards.push_back(hero.get());
  hero->addChild(ui::glyph({
      .out = &m_statusGlyph,
      .glyph = "nightlight-on",
      .glyphSize = 28.0f * scale,
      .color = colorSpecFromRole(ColorRole::Primary),
  }));
  hero->addChild(ui::column(
      {.align = FlexAlign::Stretch, .gap = 1.0f * scale, .flexGrow = 1.0f},
      ui::label({
          .text = i18n::tr("control-center.night-light.title"),
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      }),
      ui::label({
          .out = &m_statusLabel,
          .text = i18n::tr("control-center.night-light.status.off"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      }),
      ui::label({
          .out = &m_statusDetail,
          .text = i18n::tr("control-center.night-light.subtitle"),
          .fontSize = Style::fontSizeMini * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.72f),
          .maxLines = 2,
      })
  ));
  hero->addChild(ui::toggle({
      .out = &m_enabledToggle,
      .checked = night.enabled || night.force,
      .enabled = available,
      .toggleSize = ToggleSize::Large,
      .scale = scale,
      .onChange = [this](bool enabled) {
        if (!m_syncing) {
          persistEnabled(enabled);
        }
      },
  }));
  root->addChild(std::move(hero));

  auto unavailableCard = ui::row({
      .out = &m_unavailableCard,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .visible = !available,
      .participatesInLayout = !available,
      .configure = [this, scale](Flex& card) {
        applySectionCardStyle(card, scale, panelCardOpacity(), panelBordersEnabled());
        card.setDirection(FlexDirection::Horizontal);
        card.setAlign(FlexAlign::Center);
      },
  });
  m_cards.push_back(unavailableCard.get());
  unavailableCard->addChild(ui::glyph({
      .glyph = "warning",
      .glyphSize = Style::fontSizeTitle * scale,
      .color = colorSpecFromRole(ColorRole::Error),
  }));
  unavailableCard->addChild(ui::column(
      {.align = FlexAlign::Stretch, .gap = 1.0f * scale, .flexGrow = 1.0f},
      ui::label({
          .text = i18n::tr("control-center.night-light.unavailable-title"),
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
      }),
      ui::label({
          .text = i18n::tr("control-center.night-light.unavailable-body"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
      })
  ));
  root->addChild(std::move(unavailableCard));

  auto controls = ui::column({
      .out = &m_controls,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .visible = available,
      .participatesInLayout = available,
  });

  auto modeCard = ui::column({
      .gap = Style::spaceSm * scale,
      .configure = [this, scale](Flex& card) {
        applySectionCardStyle(card, scale, panelCardOpacity(), panelBordersEnabled());
      },
  });
  m_cards.push_back(modeCard.get());
  modeCard->addChild(sectionHeading("clock", i18n::tr("control-center.night-light.mode.title"), scale));
  modeCard->addChild(ui::segmented({
      .out = &m_modePicker,
      .options = std::vector<ui::SegmentedOption>{
          {.label = i18n::tr("control-center.night-light.mode.scheduled"), .glyph = "clock"},
          {.label = i18n::tr("control-center.night-light.mode.always"), .glyph = "nightlight-forced"},
      },
      .selectedIndex = night.force ? 1U : 0U,
      .scale = scale,
      .enabled = night.enabled || night.force,
      .presentation = SegmentedPresentation::Expressive,
      .surfaceOpacity = 0.45f,
      .equalSegmentWidths = true,
      .onChange = [this](std::size_t index) {
        if (!m_syncing) {
          persistMode(index);
        }
      },
  }));
  controls->addChild(std::move(modeCard));

  auto temperatureCard = ui::column({
      .gap = Style::spaceSm * scale,
      .configure = [this, scale](Flex& card) {
        applySectionCardStyle(card, scale, panelCardOpacity(), panelBordersEnabled());
      },
  });
  m_cards.push_back(temperatureCard.get());
  temperatureCard->addChild(
      sectionHeading("temperature-sun", i18n::tr("control-center.night-light.temperature.title"), scale)
  );
  temperatureCard->addChild(temperatureRow(
      "sun", i18n::tr("control-center.night-light.temperature.day"), &m_dayValue, &m_daySlider, m_pendingDay,
      scale,
      [this](double value) {
        if (m_syncing) {
          return;
        }
        const auto rounded = static_cast<std::int32_t>(std::lround(value / 100.0) * 100);
        m_pendingDay = std::max(rounded, m_pendingNight + NightLightConfig::kTemperatureGap);
        if (m_daySlider != nullptr && m_daySlider->value() != m_pendingDay) {
          m_syncing = true;
          m_daySlider->setValue(m_pendingDay);
          m_syncing = false;
        }
        if (m_dayValue != nullptr) {
          m_dayValue->setText(std::format("{}K", m_pendingDay));
        }
      },
      [this]() { persistTemperatures(); }
  ));
  temperatureCard->addChild(temperatureRow(
      "weather-moon-stars", i18n::tr("control-center.night-light.temperature.night"), &m_nightValue,
      &m_nightSlider, m_pendingNight, scale,
      [this](double value) {
        if (m_syncing) {
          return;
        }
        const auto rounded = static_cast<std::int32_t>(std::lround(value / 100.0) * 100);
        m_pendingNight = std::min(rounded, m_pendingDay - NightLightConfig::kTemperatureGap);
        if (m_nightSlider != nullptr && m_nightSlider->value() != m_pendingNight) {
          m_syncing = true;
          m_nightSlider->setValue(m_pendingNight);
          m_syncing = false;
        }
        if (m_nightValue != nullptr) {
          m_nightValue->setText(std::format("{}K", m_pendingNight));
        }
      },
      [this]() { persistTemperatures(); }
  ));
  controls->addChild(std::move(temperatureCard));

  auto scheduleCard = ui::column({
      .gap = Style::spaceSm * scale,
      .configure = [this, scale](Flex& card) {
        applySectionCardStyle(card, scale, panelCardOpacity(), panelBordersEnabled());
      },
  });
  m_cards.push_back(scheduleCard.get());
  scheduleCard->addChild(sectionHeading("sunset", i18n::tr("control-center.night-light.schedule.title"), scale));
  scheduleCard->addChild(ui::segmented({
      .out = &m_schedulePicker,
      .options = std::vector<ui::SegmentedOption>{
          {.label = i18n::tr("control-center.night-light.schedule.location"), .glyph = "map-pin"},
          {.label = i18n::tr("control-center.night-light.schedule.custom"), .glyph = "clock"},
      },
      .selectedIndex = location.customSchedule ? 1U : 0U,
      .scale = scale,
      .enabled = night.enabled && !night.force,
      .presentation = SegmentedPresentation::Expressive,
      .surfaceOpacity = 0.45f,
      .equalSegmentWidths = true,
      .onChange = [this](std::size_t index) {
        if (!m_syncing) {
          persistScheduleMode(index);
        }
      },
  }));
  scheduleCard->addChild(ui::row(
      {.out = &m_locationDetails,
       .align = FlexAlign::Center,
       .gap = Style::spaceSm * scale,
       .visible = !location.customSchedule,
       .participatesInLayout = !location.customSchedule},
      ui::glyph({
          .glyph = "map-pin",
          .glyphSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::Primary),
      }),
      ui::label({
          .out = &m_locationStatus,
          .text = i18n::tr("control-center.night-light.schedule.location-missing"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 2,
          .flexGrow = 1.0f,
      })
  ));

  auto customDetails = ui::column({
      .out = &m_customDetails,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .visible = location.customSchedule,
      .participatesInLayout = location.customSchedule,
  });
  customDetails->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::column(
          {.align = FlexAlign::Stretch, .gap = 2.0f * scale, .flexGrow = 1.0f},
          ui::label({
              .text = i18n::tr("control-center.night-light.schedule.sunset"),
              .fontSize = Style::fontSizeMini * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          }),
          ui::input({
              .out = &m_sunsetInput,
              .value = m_pendingSunset,
              .placeholder = "20:30",
              .controlHeight = Style::controlHeightSm * scale,
              .clearButtonEnabled = false,
              .textAlign = TextAlign::Center,
              .onChange = [this](const std::string& value) { m_pendingSunset = value; },
              .onSubmit = [this](const std::string&) { commitCustomSchedule(); },
              .onFocusLoss = [this]() { commitCustomSchedule(); },
          })
      ),
      ui::column(
          {.align = FlexAlign::Stretch, .gap = 2.0f * scale, .flexGrow = 1.0f},
          ui::label({
              .text = i18n::tr("control-center.night-light.schedule.sunrise"),
              .fontSize = Style::fontSizeMini * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          }),
          ui::input({
              .out = &m_sunriseInput,
              .value = m_pendingSunrise,
              .placeholder = "07:30",
              .controlHeight = Style::controlHeightSm * scale,
              .clearButtonEnabled = false,
              .textAlign = TextAlign::Center,
              .onChange = [this](const std::string& value) { m_pendingSunrise = value; },
              .onSubmit = [this](const std::string&) { commitCustomSchedule(); },
              .onFocusLoss = [this]() { commitCustomSchedule(); },
          })
      )
  ));
  customDetails->addChild(ui::label({
      .out = &m_scheduleError,
      .text = i18n::tr("control-center.night-light.schedule.invalid"),
      .fontSize = Style::fontSizeMini * scale,
      .color = colorSpecFromRole(ColorRole::Error),
      .visible = false,
      .participatesInLayout = false,
  }));
  scheduleCard->addChild(std::move(customDetails));
  controls->addChild(std::move(scheduleCard));
  root->addChild(std::move(controls));

  syncState();
  return root;
}

void NightLightTab::onClose() {
  m_root = nullptr;
  m_heroCard = nullptr;
  m_unavailableCard = nullptr;
  m_controls = nullptr;
  m_locationDetails = nullptr;
  m_customDetails = nullptr;
  m_statusGlyph = nullptr;
  m_statusLabel = nullptr;
  m_statusDetail = nullptr;
  m_dayValue = nullptr;
  m_nightValue = nullptr;
  m_locationStatus = nullptr;
  m_scheduleError = nullptr;
  m_enabledToggle = nullptr;
  m_modePicker = nullptr;
  m_schedulePicker = nullptr;
  m_daySlider = nullptr;
  m_nightSlider = nullptr;
  m_sunsetInput = nullptr;
  m_sunriseInput = nullptr;
  m_cards.clear();
}

bool NightLightTab::dragging() const noexcept {
  return (m_daySlider != nullptr && m_daySlider->dragging())
      || (m_nightSlider != nullptr && m_nightSlider->dragging());
}

void NightLightTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_root == nullptr) {
    return;
  }
  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);
}

void NightLightTab::doUpdate(Renderer&) { syncState(); }

void NightLightTab::onPanelCardOpacityChanged(float) { refreshCardStyles(); }

void NightLightTab::onPanelBordersChanged(bool) { refreshCardStyles(); }

void NightLightTab::refreshCardStyles() {
  const float scale = contentScale();
  for (auto* card : m_cards) {
    if (card != nullptr) {
      applySectionCardStyle(*card, scale, panelCardOpacity(), panelBordersEnabled());
      if (card == m_heroCard || card == m_unavailableCard) {
        card->setDirection(FlexDirection::Horizontal);
        card->setAlign(FlexAlign::Center);
      }
    }
  }
}

void NightLightTab::syncState() {
  if (m_config == nullptr || m_enabledToggle == nullptr) {
    return;
  }
  const auto& night = m_config->config().nightlight;
  const auto& location = m_config->config().location;
  const bool available = m_platform != nullptr && m_platform->hasGammaControl() && m_nightLight != nullptr;
  const bool enabled = night.enabled || night.force;

  m_syncing = true;
  m_enabledToggle->setChecked(enabled);
  m_enabledToggle->setEnabled(available);
  if (m_modePicker != nullptr) {
    m_modePicker->setSelectedIndex(night.force ? 1U : 0U);
    m_modePicker->setEnabled(enabled);
  }
  if (m_schedulePicker != nullptr) {
    m_schedulePicker->setSelectedIndex(location.customSchedule ? 1U : 0U);
    m_schedulePicker->setEnabled(night.enabled && !night.force);
  }
  if (m_daySlider != nullptr) {
    m_daySlider->setEnabled(enabled);
    if (!m_daySlider->dragging()) {
      m_pendingDay = night.dayTemperature;
      m_daySlider->setValue(m_pendingDay);
    }
  }
  if (m_nightSlider != nullptr) {
    m_nightSlider->setEnabled(enabled);
    if (!m_nightSlider->dragging()) {
      m_pendingNight = night.nightTemperature;
      m_nightSlider->setValue(m_pendingNight);
    }
  }
  if (m_dayValue != nullptr) {
    m_dayValue->setText(std::format("{}K", m_pendingDay));
  }
  if (m_nightValue != nullptr) {
    m_nightValue->setText(std::format("{}K", m_pendingNight));
  }
  if (m_locationDetails != nullptr) {
    m_locationDetails->setVisible(!location.customSchedule);
    m_locationDetails->setParticipatesInLayout(!location.customSchedule);
  }
  if (m_customDetails != nullptr) {
    m_customDetails->setVisible(location.customSchedule);
    m_customDetails->setParticipatesInLayout(location.customSchedule);
  }
  const bool sunsetFocused = m_sunsetInput != nullptr && m_sunsetInput->inputArea() != nullptr
      && m_sunsetInput->inputArea()->focused();
  const bool sunriseFocused = m_sunriseInput != nullptr && m_sunriseInput->inputArea() != nullptr
      && m_sunriseInput->inputArea()->focused();
  if (!sunsetFocused && !sunriseFocused) {
    m_pendingSunset = day_night_schedule::normalizedClock(location.sunset).value_or("20:30");
    m_pendingSunrise = day_night_schedule::normalizedClock(location.sunrise).value_or("07:30");
    if (m_sunsetInput != nullptr) {
      m_sunsetInput->setValue(m_pendingSunset);
    }
    if (m_sunriseInput != nullptr) {
      m_sunriseInput->setValue(m_pendingSunrise);
    }
  }
  m_syncing = false;

  const bool scheduleReady = m_nightLight != nullptr && m_nightLight->scheduleAvailable();
  if (m_locationStatus != nullptr) {
    if (m_nightLight != nullptr && m_nightLight->locationResolving()) {
      m_locationStatus->setText(i18n::tr("control-center.night-light.schedule.location-resolving"));
    } else {
      m_locationStatus->setText(i18n::tr(
          scheduleReady ? "control-center.night-light.schedule.location-ready"
                        : "control-center.night-light.schedule.location-missing"
      ));
    }
  }

  if (m_statusGlyph != nullptr && m_statusLabel != nullptr && m_statusDetail != nullptr) {
    if (!enabled) {
      m_statusGlyph->setGlyph("nightlight-off");
      m_statusLabel->setText(i18n::tr("control-center.night-light.status.off"));
      m_statusDetail->setText(i18n::tr("control-center.night-light.subtitle"));
    } else if (night.force) {
      m_statusGlyph->setGlyph("nightlight-forced");
      m_statusLabel->setText(i18n::tr("control-center.night-light.status.always"));
      const int kelvin = m_nightLight != nullptr ? m_nightLight->currentKelvin() : night.nightTemperature;
      m_statusDetail->setText(std::format("{}K", kelvin > 0 ? kelvin : night.nightTemperature));
    } else if (!scheduleReady) {
      m_statusGlyph->setGlyph("warning");
      m_statusLabel->setText(i18n::tr("control-center.night-light.status.needs-schedule"));
      m_statusDetail->setText(i18n::tr("control-center.night-light.schedule.location-missing"));
    } else if (m_nightLight != nullptr && m_nightLight->active()) {
      m_statusGlyph->setGlyph("nightlight-on");
      m_statusLabel->setText(i18n::tr("control-center.night-light.status.active"));
      const int kelvin = m_nightLight->currentKelvin();
      m_statusDetail->setText(std::format("{}K", kelvin > 0 ? kelvin : night.nightTemperature));
    } else {
      m_statusGlyph->setGlyph("nightlight-on");
      m_statusLabel->setText(i18n::tr("control-center.night-light.status.scheduled"));
      m_statusDetail->setText(i18n::tr("control-center.night-light.status.waiting"));
    }
  }
}

void NightLightTab::persistEnabled(bool enabled) {
  if (m_config != nullptr) {
    (void)m_config->setOverrides({
        {{"nightlight", "enabled"}, enabled},
        {{"nightlight", "force"}, enabled && m_config->config().nightlight.force},
    });
  } else if (m_nightLight != nullptr) {
    m_nightLight->setEnabled(enabled);
    if (!enabled) {
      m_nightLight->setForceEnabled(false);
    }
  }
}

void NightLightTab::persistMode(std::size_t index) {
  if (m_config != nullptr) {
    (void)m_config->setOverrides({
        {{"nightlight", "enabled"}, true},
        {{"nightlight", "force"}, index == 1U},
    });
  } else if (m_nightLight != nullptr) {
    m_nightLight->setEnabled(true);
    m_nightLight->setForceEnabled(index == 1U);
  }
}

void NightLightTab::persistTemperatures() {
  if (m_config == nullptr) {
    return;
  }
  m_pendingDay = std::clamp(
      m_pendingDay, NightLightConfig::kTemperatureMin + NightLightConfig::kTemperatureGap,
      NightLightConfig::kTemperatureMax
  );
  m_pendingNight = std::clamp(
      m_pendingNight, NightLightConfig::kTemperatureMin, m_pendingDay - NightLightConfig::kTemperatureGap
  );
  (void)m_config->setOverrides({
      {{"nightlight", "temperature_day"}, static_cast<std::int64_t>(m_pendingDay)},
      {{"nightlight", "temperature_night"}, static_cast<std::int64_t>(m_pendingNight)},
  });
}

void NightLightTab::persistScheduleMode(std::size_t index) {
  if (m_config == nullptr) {
    return;
  }
  if (index == 1U) {
    const auto sunset = day_night_schedule::normalizedClock(m_pendingSunset).value_or("20:30");
    const auto sunrise = day_night_schedule::normalizedClock(m_pendingSunrise).value_or("07:30");
    (void)m_config->setOverrides({
        {{"location", "custom_schedule"}, true},
        {{"location", "sunset"}, sunset},
        {{"location", "sunrise"}, sunrise},
    });
  } else {
    (void)m_config->setOverride({"location", "custom_schedule"}, false);
  }
}

void NightLightTab::commitCustomSchedule() {
  const auto sunset = day_night_schedule::normalizedClock(m_pendingSunset);
  const auto sunrise = day_night_schedule::normalizedClock(m_pendingSunrise);
  const bool valid = sunset.has_value() && sunrise.has_value();
  if (m_sunsetInput != nullptr) {
    m_sunsetInput->setInvalid(!sunset.has_value());
  }
  if (m_sunriseInput != nullptr) {
    m_sunriseInput->setInvalid(!sunrise.has_value());
  }
  if (m_scheduleError != nullptr) {
    m_scheduleError->setVisible(!valid);
    m_scheduleError->setParticipatesInLayout(!valid);
  }
  if (!valid || m_config == nullptr) {
    return;
  }
  m_pendingSunset = *sunset;
  m_pendingSunrise = *sunrise;
  (void)m_config->setOverrides({
      {{"location", "custom_schedule"}, true},
      {{"location", "sunset"}, m_pendingSunset},
      {{"location", "sunrise"}, m_pendingSunrise},
  });
}
