#pragma once

#include "shell/bar/widget.h"

class Glyph;
class InputArea;
class GammaService;
class ConfigService;

class NightLightWidget : public Widget {
public:
  NightLightWidget(GammaService* nightLight, ConfigService* config);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncState(Renderer& renderer);

  GammaService* m_nightLight = nullptr;
  ConfigService* m_config = nullptr;
  InputArea* m_area = nullptr;
  Glyph* m_glyph = nullptr;
  bool m_lastEnabled = false;
  bool m_lastActive = false;
  bool m_lastForced = false;
};
