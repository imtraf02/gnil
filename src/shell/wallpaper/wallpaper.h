#pragma once

#include "config/config_types.h"
#include "theme/live_wallpaper_palette.h"
#include "ui/signal.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ConfigService;
class IpcService;
class RenderContext;
class SharedTextureCache;
class WaylandConnection;
enum class WallpaperTransitionDirection;
struct TextureHandle;
struct WallpaperInstance;
struct PointerEvent;
struct WaylandOutput;
struct wl_surface;

struct WallpaperChange {
  std::string path;
  std::string connector;
};

class Wallpaper {
public:
  Wallpaper();
  ~Wallpaper();

  bool initialize(
      WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext, SharedTextureCache* textureCache
  );
  void onOutputChange();
  // Mark an output as driven by an external wallpaper source (for example a third-party provider):
  // its Background surface is torn down so the external surface shows through. Runtime-only
  // (not persisted) — it clears on restart and is re-asserted by the owner.
  void setOutputExternallyManaged(const std::string& connector, bool managed);
  [[nodiscard]] std::vector<WallpaperChange> onStateChange();
  void onSecondTick();
  void onGpuResourcesInvalidated();
  void registerIpc(IpcService& ipc);
  // Apply and persist a wallpaper image. nullopt connector targets all connected
  // outputs plus the default. Returns false if the path does not exist or the
  // connector is unknown. Shared by the wallpaper-set IPC handler.
  bool applyWallpaperImage(const std::optional<std::string>& connector, const std::string& path);
  // Start a persisted video wallpaper on one output, or on every connected
  // output when connector is omitted. Playback is delegated to the packaged
  // mpvpaper executable; image wallpaper rendering is restored on failure.
  bool applyVideoWallpaper(const std::optional<std::string>& connector, const std::string& path);
  bool clearVideoWallpaper(const std::optional<std::string>& connector);
  [[nodiscard]] bool videoWallpaperAvailable() const noexcept;
  // Display an image without mutating configuration or triggering palette
  // side effects. A missing connector previews it on every managed output.
  bool previewWallpaperImage(const std::optional<std::string>& connector, const std::string& path);
  // Restore the persisted wallpaper after a cancelled preview.
  void clearPreview();
  // Forget preview bookkeeping after the caller persisted the same image through
  // its normal configuration path. This deliberately does not redraw.
  void commitPreview();
  void setAutomationGate(std::function<bool()> gate);
  [[nodiscard]] bool ownsSurface(wl_surface* surface) const noexcept;
  bool onPointerEvent(const PointerEvent& event);

  [[nodiscard]] TextureHandle currentTexture() const;
  [[nodiscard]] std::string currentPath() const;

  // Emits whenever the displayed wallpaper (path/texture) changes, including
  // automation rotation and transition completion. Lets consumers pre-warm
  // previews while their UI is closed.
  [[nodiscard]] Signal<>& changed() noexcept { return m_changed; }
  [[nodiscard]] Signal<>& livePaletteChanged() noexcept { return m_livePaletteChanged; }
  [[nodiscard]] std::string liveWallpaperFallbackFrame() const;
  [[nodiscard]] std::optional<gnil::theme::LiveWallpaperPaletteSource>
  livePaletteSource(std::string_view outputSelector);

private:
  enum class TransitionRedirect {
    Unrelated,
    AlreadyTargeting,
    Redirected,
  };

  enum class PickWallpaper {
    Random,
    Previous,
    Next,
  };

  enum class SwitchOutcome {
    Changed,     // at least one output was switched to a new wallpaper
    NoChange,    // wallpapers were found but there was nothing new to switch to
    Unavailable, // wallpaper disabled, target unknown, or no candidate images
  };

  [[nodiscard]] bool isConnectorKnown(std::string_view connector) const;
  // Persist a resolved image path to a single connector, or to every connected
  // output plus the default when no connector is given.
  void applyResolvedWallpaper(const std::optional<std::string>& connector, const std::string& resolvedPath);
  void reload();
  void syncInstances();
  void applyStartupAutomation(std::int64_t secondStamp);
  void resetAutomationState();
  void runAutomation(std::int64_t secondStamp);
  [[nodiscard]] bool automationAllowed() const noexcept;
  [[nodiscard]] SwitchOutcome
  switchWallpaperTo(PickWallpaper action, std::optional<std::string_view> connector = std::nullopt);
  void createInstance(const WaylandOutput& output);
  [[nodiscard]] TextureHandle acquireTexture(const std::string& path);
  void releaseTexture(TextureHandle& handle, const std::string& path);
  void loadWallpaper(WallpaperInstance& instance, const std::string& path);
  TransitionRedirect redirectActiveTransition(WallpaperInstance& instance, const std::string& path);
  void startTransition(WallpaperInstance& instance);
  void startTransitionAnimation(WallpaperInstance& instance, float fromTime, WallpaperTransitionDirection direction);
  void finishTransition(WallpaperInstance& instance);
  void promotePendingWallpaper(WallpaperInstance& instance);
  void discardPendingWallpaper(WallpaperInstance& instance);
  void runQueuedWallpaper(WallpaperInstance& instance);
  void updateRendererState(WallpaperInstance& instance);
  void releaseInstanceTextures(WallpaperInstance& inst);
  void restorePersistedWallpaper(WallpaperInstance& instance);
  void syncVideoWallpapers();
  void syncLiveWallpaperPalette();
  void ensureLiveWallpaperPalette(std::string_view outputSelector);
  [[nodiscard]] std::optional<std::pair<std::string, std::string>>
  selectLiveWallpaperVideo(std::string_view outputSelector) const;
  void completeLivePaletteExtraction(
      std::uint64_t generation, std::string identity, std::vector<std::string> framePaths
  );
  void stopVideoWallpaper(const std::string& connector, bool restoreImage);

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  SharedTextureCache* m_textureCache = nullptr;
  bool m_wallpaperEnabled = false;
  WallpaperConfig m_lastWallpaperConfig{};
  std::int64_t m_lastAutomationSecondStamp = -1;
  std::int64_t m_lastAutomationSwitchSecond = -1;
  std::function<bool()> m_automationGate;
  Signal<>::ScopedConnection m_paletteConn;
  Signal<> m_changed;
  Signal<> m_livePaletteChanged;
  std::vector<std::unique_ptr<WallpaperInstance>> m_instances;
  std::unordered_set<std::string> m_externallyManagedOutputs;
  struct VideoProcess {
    int pid = -1;
    std::string path;
    std::string options;
  };
  std::unordered_map<std::string, VideoProcess> m_videoProcesses;
  struct LivePaletteAsyncGuard;
  std::shared_ptr<LivePaletteAsyncGuard> m_livePaletteAsyncGuard;
  std::optional<gnil::theme::LiveWallpaperPaletteSource> m_livePaletteSource;
  std::string m_livePaletteDesiredIdentity;
  std::uint64_t m_livePaletteGeneration = 0;
  bool m_previewActive = false;
  std::unordered_set<std::string> m_previewedConnectors;
};
