#pragma once

#include "shell/control_center/tab.h"

#include <cstdint>
#include <string>
#include <vector>

class CompositorPlatform;
class ConfigService;
class Flex;
class GammaService;
class Glyph;
class Input;
class Label;
class Renderer;
class Segmented;
class Slider;
class Toggle;

class NightLightTab : public Tab {
public:
  NightLightTab(GammaService* nightLight, ConfigService* config, CompositorPlatform* platform);

  std::unique_ptr<Flex> create() override;
  void onClose() override;
  [[nodiscard]] bool dragging() const noexcept;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void onPanelBordersChanged(bool enabled) override;
  void syncState();
  void persistEnabled(bool enabled);
  void persistMode(std::size_t index);
  void persistTemperatures();
  void persistScheduleMode(std::size_t index);
  void commitCustomSchedule();
  void refreshCardStyles();

  GammaService* m_nightLight = nullptr;
  ConfigService* m_config = nullptr;
  CompositorPlatform* m_platform = nullptr;

  Flex* m_root = nullptr;
  Flex* m_heroCard = nullptr;
  Flex* m_unavailableCard = nullptr;
  Flex* m_controls = nullptr;
  Flex* m_locationDetails = nullptr;
  Flex* m_customDetails = nullptr;
  Glyph* m_statusGlyph = nullptr;
  Label* m_statusLabel = nullptr;
  Label* m_statusDetail = nullptr;
  Label* m_dayValue = nullptr;
  Label* m_nightValue = nullptr;
  Label* m_locationStatus = nullptr;
  Label* m_scheduleError = nullptr;
  Toggle* m_enabledToggle = nullptr;
  Segmented* m_modePicker = nullptr;
  Segmented* m_schedulePicker = nullptr;
  Slider* m_daySlider = nullptr;
  Slider* m_nightSlider = nullptr;
  Input* m_sunsetInput = nullptr;
  Input* m_sunriseInput = nullptr;
  std::vector<Flex*> m_cards;

  std::int32_t m_pendingDay = 6500;
  std::int32_t m_pendingNight = 4000;
  std::string m_pendingSunset;
  std::string m_pendingSunrise;
  bool m_syncing = false;
};
