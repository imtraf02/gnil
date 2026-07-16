#pragma once

#include "config/config_types.h"
#include "ui/palette.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class WaylandConnection;

namespace desktop_settings {
  enum class DesktopWidgetSettingsScope;
}

namespace lockscreen_login_box {

  constexpr std::string_view kWidgetType = "login_box";
  constexpr std::string_view kWidgetIdPrefix = "lockscreen-login-box@";

  constexpr std::string_view kInputOpacityKey = "input_opacity";
  constexpr std::string_view kInputRadiusKey = "input_radius";
  constexpr std::string_view kCenterPasswordTextKey = "center_password_text";
  constexpr std::string_view kShowLoginButtonKey = "show_login_button";
  constexpr std::string_view kShowPasswordHintKey = "show_password_hint";
  constexpr std::string_view kShowCapsLockKey = "show_caps_lock";
  constexpr std::string_view kShowKeyboardLayoutKey = "show_keyboard_layout";

  struct LoginBoxStyle {
    ColorSpec panelFill = colorSpecFromRole(ColorRole::Surface, 0.82f);
    float panelOpacity = 0.82f;
    float panelRadius = 36.0f;
    float inputOpacity = 0.92f;
    float inputRadius = 22.0f;
    bool centerPasswordText = true;
    bool showLoginButton = true;
    bool showPasswordHint = true;
    bool showCapsLock = true;
    bool showKeyboardLayout = true;
  };

  [[nodiscard]] bool isLoginBoxWidget(const DesktopWidgetState& state);
  [[nodiscard]] bool isLoginBoxWidgetType(std::string_view type);
  [[nodiscard]] bool isLoginBoxWidgetId(std::string_view id);
  [[nodiscard]] std::string widgetIdForOutput(std::string_view outputKey);

  // The login box is the central lock card, not just the password row.  These
  // bounds keep the clock/profile/password composition intact at every output
  // scale while still allowing the lockscreen editor to reposition the card.
  constexpr float kDefaultPanelWidthCap = 480.0f;
  constexpr float kMinPanelWidth = 360.0f;
  constexpr float kMinPanelHeight = 500.0f;
  constexpr float kMaxPanelHeight = 720.0f;

  struct PanelContentLayout {
    float contentLeft = 0.0f;
    float contentTop = 0.0f;
    float inputWidth = 0.0f;
    float buttonX = 0.0f;
    float controlHeight = 0.0f;
  };

  [[nodiscard]] float defaultPanelWidth(float screenWidth);
  [[nodiscard]] float defaultPanelHeight();
  [[nodiscard]] float panelWidth(float screenWidth);
  [[nodiscard]] float panelHeight();
  [[nodiscard]] float resolvePanelWidth(float screenWidth, float boxWidth);
  [[nodiscard]] float resolvePanelHeight(float boxHeight);
  void defaultPanelSize(float screenWidth, float& boxWidth, float& boxHeight);
  void clampPanelSize(float screenWidth, float& boxWidth, float& boxHeight);
  [[nodiscard]] PanelContentLayout panelContentLayout(float panelWidth, float panelHeight, bool showLoginButton);
  void defaultPanelCenter(float screenWidth, float screenHeight, float& cx, float& cy);
  void panelOriginFromCenter(
      float cx, float cy, float screenWidth, float boxWidth, float boxHeight, float& panelX, float& panelY,
      float& panelWidthOut, float& panelHeightOut
  );

  [[nodiscard]] const DesktopWidgetState*
  findForOutput(const std::vector<DesktopWidgetState>& widgets, std::string_view outputKey);

  [[nodiscard]] LoginBoxStyle resolveStyle(const std::unordered_map<std::string, WidgetSettingValue>& settings);
  void applyDefaultSettings(
      std::unordered_map<std::string, WidgetSettingValue>& settings, desktop_settings::DesktopWidgetSettingsScope scope
  );
  void applyAllDefaultSettings(std::unordered_map<std::string, WidgetSettingValue>& settings);
  void normalizeSettings(std::unordered_map<std::string, WidgetSettingValue>& settings);

  void ensureWidgets(std::vector<DesktopWidgetState>& widgets, const WaylandConnection& wayland);

} // namespace lockscreen_login_box
