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
#include <string_view>
#include <vector>

struct ext_session_lock_surface_v1;
struct ext_session_lock_v1;
struct wl_output;

class Button;
class Box;
class Flex;
class Glyph;
class Image;
class Input;
class Label;
class ProgressBar;
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
  std::string weatherLocation;
  bool weatherAvailable = false;
  std::string systemIdentity;
  std::string systemDetails;
  std::string distroAssetPath;
  std::string mediaTitle;
  std::string mediaArtist;
  std::string mediaArtworkPath;
  std::int64_t mediaPositionUs = 0;
  std::int64_t mediaLengthUs = 0;
  bool mediaAvailable = false;
  bool mediaPlaying = false;
  bool mediaCanPrevious = false;
  bool mediaCanPlayPause = false;
  bool mediaCanNext = false;
  std::array<LockscreenMetricState, 3> metrics;
  std::vector<int> calendarEventDateKeys;
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
  void setOnSystemAction(std::function<void(std::string_view)> onSystemAction);
  void setOnCycleLayout(std::function<void()> onCycleLayout);
  void setOnPasswordChanged(std::function<void(const std::string&)> onPasswordChanged);
  void setOnMediaPrevious(std::function<void()> callback);
  void setOnMediaPlayPause(std::function<void()> callback);
  void setOnMediaNext(std::function<void()> callback);
  void revealPasswordPrompt();
  void collapsePasswordPrompt();
  [[nodiscard]] bool passwordPromptVisible() const noexcept { return m_passwordPromptVisible; }
  [[nodiscard]] bool dismissTransientUi();
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
  void syncAvatar(Renderer& renderer, float avatarSize);
  void syncHeroArtwork(Renderer& renderer, float artworkSize);
  void syncDashboardCopy();
  void syncCalendarCopy();
  [[nodiscard]] bool systemActionEnabled(std::string_view action) const;
  void setAuthenticationVisual(bool authenticating, bool animate);
  void updateHeroParallax(float sceneX, float sceneY);
  void resetHeroParallax(bool animate);
  void setNotificationPanelOpen(bool open, bool animate = true);
  void setPasswordPromptVisible(bool visible, bool animate = true);
  void changeCalendarMonth(int delta);

  struct MetricView {
    Flex* row = nullptr;
    Glyph* glyph = nullptr;
    Label* label = nullptr;
    Label* value = nullptr;
    ProgressBar* progress = nullptr;
    float displayedProgress = 0.0f;
  };

  struct CalendarCellView {
    Flex* cell = nullptr;
    Label* label = nullptr;
    Box* eventDot = nullptr;
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
  Node* m_leftColumn = nullptr;
  Node* m_centerColumn = nullptr;
  Node* m_rightColumn = nullptr;
  Flex* m_identityCard = nullptr;
  Label* m_identityHostLabel = nullptr;
  Flex* m_clockBlock = nullptr;
  Flex* m_loginPanel = nullptr;
  Node* m_promptHost = nullptr;
  Button* m_unlockButton = nullptr;
  Node* m_heroLayer = nullptr;
  Box* m_heroBackSheetA = nullptr;
  Box* m_heroBackSheetB = nullptr;
  Flex* m_heroCard = nullptr;
  Node* m_heroImageFrame = nullptr;
  Image* m_heroImage = nullptr;
  Glyph* m_heroFallbackGlyph = nullptr;
  Label* m_heroCaptionLabel = nullptr;
  Flex* m_calendarCard = nullptr;
  Flex* m_calendarGrid = nullptr;
  Button* m_calendarPreviousButton = nullptr;
  Button* m_calendarMonthButton = nullptr;
  Button* m_calendarNextButton = nullptr;
  std::array<Label*, 7> m_calendarWeekdayLabels{};
  std::array<CalendarCellView, 42> m_calendarCells{};
  Button* m_notificationButton = nullptr;
  InputArea* m_notificationBackdropArea = nullptr;
  Flex* m_notificationPanel = nullptr;
  Button* m_notificationCloseButton = nullptr;
  Flex* m_weatherCard = nullptr;
  Flex* m_mediaCard = nullptr;
  Box* m_mediaArtworkFrame = nullptr;
  Image* m_mediaArtwork = nullptr;
  Glyph* m_mediaArtworkFallback = nullptr;
  ProgressBar* m_mediaProgress = nullptr;
  Label* m_mediaDurationLabel = nullptr;
  Flex* m_resourcesCard = nullptr;
  Label* m_weatherTitleLabel = nullptr;
  Glyph* m_weatherGlyph = nullptr;
  Label* m_weatherTemperatureLabel = nullptr;
  Label* m_weatherConditionLabel = nullptr;
  Label* m_weatherDetailLabel = nullptr;
  Label* m_mediaHeaderLabel = nullptr;
  Label* m_mediaTitleLabel = nullptr;
  Label* m_mediaArtistLabel = nullptr;
  Button* m_mediaPreviousButton = nullptr;
  Button* m_mediaPlayPauseButton = nullptr;
  Button* m_mediaNextButton = nullptr;
  std::array<MetricView, 3> m_metricViews{};
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
  Node* m_loginActionSlot = nullptr;
  Button* m_loginButton = nullptr;
  Spinner* m_loginSpinner = nullptr;
  Button* m_layoutChip = nullptr;
  Label* m_statusLabel = nullptr;
  Flex* m_systemActionsRow = nullptr;
  Button* m_shutdownButton = nullptr;
  Button* m_rebootButton = nullptr;
  Button* m_suspendButton = nullptr;
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
  std::function<void(std::string_view)> m_onSystemAction;
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
  std::string m_loadedHeroPath;
  int m_loadedHeroSize = 0;
  std::string m_loadedMediaArtworkPath;
  int m_loadedMediaArtworkSize = 0;
  float m_introProgress = 1.0f;
  float m_unlockExitProgress = 0.0f;
  float m_authTransition = 0.0f;
  float m_passwordErrorOffsetX = 0.0f;
  float m_heroParallaxX = 0.0f;
  float m_heroParallaxY = 0.0f;
  float m_heroParallaxRotation = 0.0f;
  float m_heroParallaxScale = 1.0f;
  float m_heroParallaxStartX = 0.0f;
  float m_heroParallaxStartY = 0.0f;
  float m_heroParallaxStartRotation = 0.0f;
  float m_heroParallaxStartScale = 1.0f;
  float m_heroParallaxTargetX = 0.0f;
  float m_heroParallaxTargetY = 0.0f;
  float m_heroParallaxTargetRotation = 0.0f;
  float m_heroParallaxTargetScale = 1.0f;
  bool m_lastPromptWasError = false;
  bool m_introPending = false;
  bool m_introStarted = false;
  bool m_unlocking = false;
  bool m_passwordPromptVisible = false;
  bool m_notificationPanelOpen = false;
  int m_calendarMonthOffset = 0;
  int m_calendarPendingDelta = 0;
  bool m_calendarTransitioning = false;
  float m_calendarSlideOffset = 0.0f;
  float m_notificationPanelOffsetY = 0.0f;
  float m_promptTransition = 0.0f;
  float m_appliedCardPadding = -1.0f;
  float m_appliedContentGap = -1.0f;
  float m_appliedResourceGap = -1.0f;
  float m_appliedCardRadius = -1.0f;
  float m_appliedClockPadding = -1.0f;
  float m_appliedClockGap = -1.0f;
};
