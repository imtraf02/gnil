#pragma once

#include "capture/screencopy_capture.h"
#include "config/config_service.h"
#include "render/core/blur_cache.h"
#include "render/core/color.h"
#include "render/core/texture_manager.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "wayland/surface.h"

#include <cstdint>
#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>

struct ext_session_lock_surface_v1;
struct ext_session_lock_v1;
struct wl_output;

class Button;
class Box;
class CountdownRingNode;
class Flex;
class Glyph;
class Image;
class Input;
class Label;
class Renderer;
class SharedTextureCache;
class Spinner;
class WallpaperNode;
struct KeyboardEvent;
struct PointerEvent;

// Snapshot intentionally contains presentation-ready text.  The session-lock
// surface never reaches into DBus services while rendering; LockScreen owns
// service reads and hands every output the same stable state.
struct LockscreenMetricState {
  std::string glyph;
  std::string value;
  float progress = 0.0f;
  bool available = false;

  bool operator==(const LockscreenMetricState&) const = default;
};

struct LockscreenNotificationPreview {
  std::string app;
  std::string summary;

  bool operator==(const LockscreenNotificationPreview&) const = default;
};

struct LockscreenDashboardState {
  std::string weatherGlyph = "weather-cloud-off";
  std::string weatherTemperature;
  std::string weatherCondition;
  std::string weatherDetail;
  bool weatherAvailable = false;
  std::string systemIdentity;
  std::string systemDetails;
  std::string distroAssetPath;
  std::string mediaTitle;
  std::string mediaArtist;
  bool mediaAvailable = false;
  bool mediaPlaying = false;
  bool mediaCanPrevious = false;
  bool mediaCanPlayPause = false;
  bool mediaCanNext = false;
  std::array<LockscreenMetricState, 4> metrics;
  std::array<LockscreenNotificationPreview, 3> notificationPreviews;
  std::size_t notificationCount = 0;
  std::string avatarPath;
  bool showNotifications = true;

  bool operator==(const LockscreenDashboardState&) const = default;
};

class LockSurface : public Surface {
public:
  explicit LockSurface(WaylandConnection& connection, ConfigService* config = nullptr);
  ~LockSurface() override;

  using Surface::initialize;
  bool initialize() override { return false; }
  bool initialize(ext_session_lock_v1* lock, wl_output* output, std::int32_t scale);
  void setLockedState(bool locked);
  void setPromptState(std::string user, std::string password, std::string status, bool error, bool authenticating);
  void setKeyboardIndicators(bool capsLock, bool hasMultipleLayouts, bool layoutSwitchable, std::string layoutLabel);
  void setDashboardState(LockscreenDashboardState state);
  void setTextureCache(SharedTextureCache* cache) noexcept { m_textureCache = cache; }
  void setWallpaperPath(std::string wallpaperPath);
  void setWallpaperFillMode(WallpaperFillMode fillMode);
  void setWallpaperFillColor(Color fillColor);
  void setDesktopCapture(std::optional<ScreencopyImage> capture);
  void setBackgroundStyle(float blurIntensity, float tintIntensity);
  void setBlackout(bool blackout);
  [[nodiscard]] bool isBlackout() const noexcept { return m_blackout; }
  void setOnLogin(std::function<void()> onLogin);
  void setOnCycleLayout(std::function<void()> onCycleLayout);
  void setOnPasswordChanged(std::function<void(const std::string&)> onPasswordChanged);
  void setOnMediaPrevious(std::function<void()> callback);
  void setOnMediaPlayPause(std::function<void()> callback);
  void setOnMediaNext(std::function<void()> callback);
  void selectAllPassword();
  void clearPasswordSelection();
  void onThemeChanged();
  void onSecondTick();
  void beginUnlockAnimation(std::function<void()> finished);
  void onGpuResourcesInvalidated();
  void prepareForGraphicsReset() noexcept;
  void onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);
  [[nodiscard]] wl_output* output() const noexcept { return m_output; }
  [[nodiscard]] bool hasDesktopCapture() const noexcept;
  [[nodiscard]] bool firstFrameRendered() const noexcept { return m_firstFrameRendered; }
  void setRenderCallback(std::function<void()> callback) { m_renderCallback = std::move(callback); }

  static void handleConfigure(
      void* data, ext_session_lock_surface_v1* lockSurface, std::uint32_t serial, std::uint32_t width,
      std::uint32_t height
  );

protected:
  void render() override;

private:
  void prepareFrame(bool needsUpdate, bool needsLayout);
  void applyWallpaperTexture();
  void applyBlurredDesktopTexture();
  void releaseWallpaperTextureRef(const std::string& path);
  void releaseCaptureTextures();
  void layoutScene(std::uint32_t width, std::uint32_t height);
  void updateCopy();
  [[nodiscard]] std::string resolveStatusText(bool& isError) const;
  [[nodiscard]] bool passwordFieldContainsPoint(float sceneX, float sceneY) const;
  void focusPasswordField();
  void startIntroAnimation();
  void startPasswordErrorAnimation();
  void applyLockscreenPalette();
  void layoutMetricViews(Renderer& renderer, float width, float height, float scale, float gap);
  void syncAvatar(Renderer& renderer, float avatarSize);
  void syncDashboardCopy();

  struct MetricView {
    Flex* card = nullptr;
    CountdownRingNode* track = nullptr;
    CountdownRingNode* progress = nullptr;
    Glyph* glyph = nullptr;
    Label* value = nullptr;
    float displayedProgress = 0.0f;
  };

  struct NotificationView {
    Flex* row = nullptr;
    Label* app = nullptr;
    Label* summary = nullptr;
  };

  ext_session_lock_surface_v1* m_lockSurface = nullptr;
  wl_output* m_output = nullptr;
  ConfigService* m_config = nullptr;
  Node m_root;
  Node* m_backgroundLayer = nullptr;
  WallpaperNode* m_wallpaper = nullptr;
  Box* m_tintOverlay = nullptr;
  Box* m_backdrop = nullptr;
  Box* m_island = nullptr;
  Node* m_leftColumn = nullptr;
  Node* m_rightColumn = nullptr;
  Flex* m_loginPanel = nullptr;
  Flex* m_weatherCard = nullptr;
  Flex* m_fetchCard = nullptr;
  Flex* m_mediaCard = nullptr;
  Flex* m_resourcesCard = nullptr;
  Flex* m_notificationsCard = nullptr;
  Label* m_weatherTitleLabel = nullptr;
  Glyph* m_weatherGlyph = nullptr;
  Label* m_weatherTemperatureLabel = nullptr;
  Label* m_weatherConditionLabel = nullptr;
  Label* m_weatherDetailLabel = nullptr;
  Image* m_distroImage = nullptr;
  Glyph* m_distroGlyph = nullptr;
  Label* m_systemIdentityLabel = nullptr;
  Label* m_fetchLabel = nullptr;
  std::array<Box*, 8> m_paletteDots{};
  Label* m_mediaHeaderLabel = nullptr;
  Label* m_mediaTitleLabel = nullptr;
  Label* m_mediaArtistLabel = nullptr;
  Button* m_mediaPreviousButton = nullptr;
  Button* m_mediaPlayPauseButton = nullptr;
  Button* m_mediaNextButton = nullptr;
  std::array<MetricView, 4> m_metricViews{};
  Label* m_notificationsHeaderLabel = nullptr;
  Label* m_notificationsLabel = nullptr;
  Flex* m_notificationsEmpty = nullptr;
  Glyph* m_notificationsEmptyGlyph = nullptr;
  std::array<NotificationView, 3> m_notificationViews{};
  Label* m_timeLabel = nullptr;
  Label* m_dateLabel = nullptr;
  Box* m_avatarFrame = nullptr;
  Image* m_avatarImage = nullptr;
  Glyph* m_avatarGlyph = nullptr;
  Label* m_userLabel = nullptr;
  Flex* m_passwordCapsule = nullptr;
  Glyph* m_passwordLockGlyph = nullptr;
  Flex* m_loginContentRow = nullptr;
  Input* m_passwordField = nullptr;
  Button* m_loginButton = nullptr;
  Spinner* m_loginSpinner = nullptr;
  Button* m_layoutChip = nullptr;
  Label* m_statusLabel = nullptr;
  SharedTextureCache* m_textureCache = nullptr;
  TextureHandle m_wallpaperTexture{};
  TextureHandle m_blurredWallpaperTexture{};
  TextureHandle m_captureSourceTexture{};
  TextureHandle m_blurredDesktopTexture{};
  BlurCache m_blurCache;
  BlurCache m_wallpaperBlurCache;
  std::optional<ScreencopyImage> m_desktopCapture;
  float m_blurIntensity = 0.5f;
  float m_tintIntensity = 0.3f;
  bool m_blackout = false;
  bool m_captureDirty = true;
  std::string m_wallpaperPath;
  std::string m_textureWallpaperPath;
  WallpaperFillMode m_wallpaperFillMode = WallpaperFillMode::Crop;
  Color m_wallpaperFillColor = rgba(0.0f, 0.0f, 0.0f, 0.0f);
  bool m_wallpaperDirty = false;
  InputDispatcher m_inputDispatcher;
  std::function<void()> m_onLogin;
  std::function<void()> m_onCycleLayout;
  std::function<void(const std::string&)> m_onPasswordChanged;
  std::function<void()> m_onMediaPrevious;
  std::function<void()> m_onMediaPlayPause;
  std::function<void()> m_onMediaNext;
  bool m_locked = false;
  std::string m_user;
  std::string m_password;
  std::string m_status;
  bool m_error = false;
  bool m_authenticating = false;
  bool m_capsLock = false;
  bool m_hasMultipleLayouts = false;
  bool m_layoutSwitchable = false;
  std::string m_layoutLabel;
  LockscreenDashboardState m_dashboard;
  bool m_firstFrameRendered = false;
  std::function<void()> m_renderCallback;
  AnimationManager m_animations;
  std::string m_loadedAvatarPath;
  int m_loadedAvatarSize = 0;
  std::string m_loadedDistroPath;
  int m_loadedDistroSize = 0;
  float m_introOffsetY = 0.0f;
  float m_introSideOffset = 0.0f;
  float m_introOpacity = 1.0f;
  float m_loginBaseX = 0.0f;
  float m_loginBaseY = 0.0f;
  float m_passwordErrorOffsetX = 0.0f;
  bool m_lastPromptWasError = false;
  bool m_introPending = false;
  bool m_introStarted = false;
  bool m_unlocking = false;
};
