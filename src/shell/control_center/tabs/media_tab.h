#pragma once

#include "core/timer_manager.h"
#include "dbus/mpris/mpris_service.h"
#include "render/core/texture_handle.h"
#include "shell/control_center/tab.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

class Button;
class ContextMenuPopup;
class Glyph;
class HttpClient;
class Image;
class Label;
class MprisService;
class PipeWireSpectrum;
class RenderContext;
class Slider;
class AudioVisualizer;
class ConfigService;
class WaylandConnection;
class LyricsService;

enum class MediaTabPresentation : std::uint8_t {
  Dashboard,
  ReferencePanel,
};

class MediaTab : public Tab {
public:
  MediaTab(
      MprisService* mpris, HttpClient* httpClient, PipeWireSpectrum* spectrum, ConfigService* config,
      WaylandConnection* wayland, RenderContext* renderContext,
      MediaTabPresentation presentation = MediaTabPresentation::Dashboard
  );
  ~MediaTab() override;

  std::unique_ptr<Flex> create() override;
  void onFrameTick(float deltaMs) override;
  void setActive(bool active) override;
  void onClose() override;
  bool dismissTransientUi() override;

private:
  struct BongoFrame {
    TextureHandle texture{};
    std::chrono::milliseconds duration{0};
  };

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doPrepareIntrinsicLayout(Renderer& renderer, float contentWidth, float maxBodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void refresh(Renderer& renderer);
  void clearArt(Renderer& renderer);
  void ensureBongoLoaded(Renderer& renderer);
  void unloadBongoFrames();
  void syncBongoPlayback(bool playing);
  void scheduleNextBongoFrame();
  void advanceBongoFrame();
  void commitPendingSeek(double valueSeconds);
  void syncLyrics(const std::optional<MprisPlayerInfo>& active);

  void openPlayerMenu();

  // Guard token for deferred callbacks that run on the next main-loop tick.
  // Callbacks capture a weak_ptr so they can detect destruction without
  // relying on a raw this pointer staying valid.
  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);

  MprisService* m_mpris = nullptr;
  HttpClient* m_httpClient = nullptr;
  PipeWireSpectrum* m_spectrum = nullptr;
  ConfigService* m_config = nullptr;
  WaylandConnection* m_wayland = nullptr;
  RenderContext* m_renderContext = nullptr;
  MediaTabPresentation m_presentation = MediaTabPresentation::Dashboard;
  std::unique_ptr<LyricsService> m_lyricsService;
  std::uint64_t m_spectrumListenerId = 0;
  bool m_active = false;

  Flex* m_rootLayout = nullptr;
  Flex* m_mediaColumn = nullptr;
  Flex* m_visualizerColumn = nullptr;
  Flex* m_visualizerBody = nullptr;
  Flex* m_lyricsCard = nullptr;
  Flex* m_lyricsEmptyState = nullptr;
  Flex* m_lyricsLines = nullptr;
  AudioVisualizer* m_visualizerSpectrum = nullptr;
  Image* m_bongoCat = nullptr;
  Image* m_artwork = nullptr;
  Glyph* m_artFallbackGlyph = nullptr;
  Flex* m_artContainer = nullptr;
  Flex* m_artworkRow = nullptr;
  Flex* m_nowCard = nullptr;
  Flex* m_mediaStack = nullptr;
  Button* m_playerMenuButton = nullptr;
  std::unique_ptr<ContextMenuPopup> m_playerMenuPopup;
  Label* m_trackTitle = nullptr;
  Label* m_trackArtist = nullptr;
  Label* m_trackAlbum = nullptr;
  Label* m_lyricsPrevious = nullptr;
  Label* m_lyricsCurrent = nullptr;
  Label* m_lyricsNext = nullptr;
  Label* m_visualizerStatus = nullptr;
  Label* m_timeElapsedLabel = nullptr;
  Label* m_timeRemainingLabel = nullptr;
  Slider* m_progressSlider = nullptr;
  Button* m_prevButton = nullptr;
  Button* m_playPauseButton = nullptr;
  Button* m_nextButton = nullptr;
  Button* m_repeatButton = nullptr;
  Button* m_shuffleButton = nullptr;
  Slider* m_volumeSlider = nullptr;
  Label* m_volumeLabel = nullptr;

  std::string m_lastArtPath;
  std::string m_lastBusName;
  std::string m_lastPlaybackStatus;
  std::string m_lastLoopStatus;
  bool m_lastShuffle = false;
  bool m_syncingProgress = false;
  std::int64_t m_pendingSeekUs = -1;
  std::string m_pendingSeekBusName;
  std::chrono::steady_clock::time_point m_pendingSeekUntil;
  std::chrono::steady_clock::time_point m_progressSettleUntil;
  bool m_playerMenuOpen = false;
  bool m_bongoLoadAttempted = false;
  bool m_bongoPlaying = false;
  std::vector<BongoFrame> m_bongoFrames;
  std::size_t m_bongoFrame = 0;
  Timer m_bongoTimer;
  Renderer* m_bongoRenderer = nullptr;
  std::vector<std::string> m_playerBusNames;
  std::unordered_set<std::string> m_pendingArtDownloads;
  std::string m_positionBusName;
  std::string m_positionTrackId;
  std::string m_positionTrackSignature;
  std::int64_t m_positionUs = 0;
  std::int64_t m_lastTrackLengthUs = 0;
  std::chrono::steady_clock::time_point m_positionSampleAt;
  std::optional<MprisPlayerInfo> m_lastActiveSnapshot;
  std::chrono::steady_clock::time_point m_lastActiveSeenAt;
  std::chrono::steady_clock::time_point m_nextRealtimeUpdateAt;
  std::chrono::steady_clock::time_point m_lastRealtimeMprisPollAt;
  Timer m_progressTimer;
};
