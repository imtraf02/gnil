#pragma once

#include "core/timer_manager.h"
#include "notification/notification.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "system/icon_resolver.h"
#include "shell/chrome/chrome_geometry.h"

#include <memory>
#include <functional>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ConfigService;
class Box;
class HttpClient;
class Input;
class InputArea;
class LayerSurface;
class NotificationManager;
class Node;
class ProgressBar;
class RenderContext;
class WaylandConnection;
enum class NotificationEvent;
struct KeyboardEvent;
struct PointerEvent;
struct WaylandOutput;
struct wl_output;

class NotificationToast {
public:
  enum class RevealDirection { FromLeft, FromRight, FromTop, FromBottom };

  NotificationToast();
  ~NotificationToast();

  NotificationToast(const NotificationToast&) = delete;
  NotificationToast& operator=(const NotificationToast&) = delete;

  void initialize(
      WaylandConnection& wayland, ConfigService* config, NotificationManager* notifications,
      RenderContext* renderContext, HttpClient* httpClient = nullptr
  );
  void onConfigReload();
  void onOutputChange();
  void requestLayout();
  void requestRedraw();
  // Hides current toast surfaces without closing their notifications. Adds received
  // while the sidebar is open are consumed by the sidebar and never replayed later.
  void setSidebarOpen(bool open);
  bool onPointerEvent(const PointerEvent& event);
  bool onKeyboardEvent(const KeyboardEvent& event);
  // Called with output-local toast bodies. A true return selects the shared
  // chrome path; false retains the standalone card material fallback.
  void setChromeToastStateCallback(std::function<bool(wl_output*, std::vector<ChromePanelState>)> callback);

  [[nodiscard]] float horizontalInnerPad(float scale) const;

private:
  // Per-notification visual state (shared across all instances)
  struct PopupEntry {
    uint32_t notificationId = 0;
    std::string appName;
    std::string summary;
    std::string body;
    std::vector<std::string> actions;
    std::optional<std::string> icon;
    std::optional<NotificationImageData> imageData;
    TimePoint receivedTime{};
    std::optional<WallTimePoint> receivedWallClock;
    Urgency urgency = Urgency::Normal;
    int displayDurationMs = 0; // -1 = persistent (no auto-dismiss)
    int32_t rawTimeoutMs = 0;  // raw DBus timeout; >0 means manager has an auto-expire timer we must coordinate with
    float remainingProgress = 1.0f;
    float y = -1.0f; // stable top position while visible; negative = queued/off-screen
    float height = 0.0f;
    // Planned toast chrome (refreshEntryGeometry); buildCard must match these for placement vs paint.
    int toastBodyLines = 0;
    bool expanded = false;
    bool gestureActive = false;
    bool suppressNextClick = false;
    float gestureStartX = 0.0f;
    float gestureStartY = 0.0f;
    bool exiting = false;
    bool hovered = false; // pointer is currently over the card on some instance
    int hoverOwners = 0;
    std::uint64_t hoverResetToken = 0;
    bool hoverResetPending = false;
    bool replyInputFocused = false;
    Timer exitFallbackTimer;
  };

  // Per-output instance (each has its own surface, scene, animations)
  struct Instance {
    wl_output* output = nullptr;

    std::unique_ptr<LayerSurface> surface;
    // Declaration order matters: sceneRoot must be destroyed before `animations`,
    // because ~Node() calls cancelForOwner() on its AnimationManager.
    AnimationManager animations;
    std::unique_ptr<Node> sceneRoot;
    InputDispatcher inputDispatcher;
    bool pointerInside = false;
    bool rebuildRequested = false;
    bool chromeHosted = false;
    std::optional<ChromePanelState> toastPanelState;
    std::optional<ChromePanelState> toastPanelTarget;
    AnimationManager::Id toastPanelAnimId = 0;

    // Per-entry visual nodes for this instance
    struct CardState {
      Node* cardNode = nullptr;
      Node* cardContent = nullptr;
      Node* cardForeground = nullptr;
      Box* cardBackground = nullptr;
      ProgressBar* progressBar = nullptr;
      Node* actionsRowNode = nullptr;
      Node* inlineReplyRowNode = nullptr;
      Input* inlineReplyInput = nullptr;
      // Real laid-out card height for this instance, measured at this surface's render
      // scale in buildCard(). The reveal clip uses this, not the shared entry.height,
      // which is measured once at whatever scale was current on arrival.
      float clipHeight = 0.0f;
      float targetHeight = 0.0f;
      AnimationManager::Id countdownAnimId = 0;
      AnimationManager::Id entryAnimId = 0;
      AnimationManager::Id slideAnimId = 0;
      AnimationManager::Id exitAnimId = 0;
      AnimationManager::Id heightAnimId = 0;
      bool replyMode = false;
    };
    std::vector<CardState> cards;
    float lastPointerX = 0.0f;
    float lastPointerY = 0.0f;
  };

  void onNotificationEvent(const Notification& n, NotificationEvent event);
  void schedulePendingAdds();
  void flushPendingAdds();
  void addPopup(const Notification& n);
  void dismissPopup(std::size_t index);
  void requestClose(uint32_t notificationId, CloseReason reason);
  void removePopup(uint32_t notificationId);
  void finishRemoval(uint32_t notificationId);
  void finishExitingEntryIfOrphaned(uint32_t notificationId);
  void updateInputRegion(Instance& inst);
  void syncChromeHosting(Instance& inst);
  void publishChromeStates(Instance& inst);
  void enterInlineReplyMode(uint32_t notificationId);
  bool exitInlineReplyMode(uint32_t notificationId);
  void submitInlineReply(uint32_t notificationId, const std::string& replyText);
  void setExpanded(uint32_t notificationId, bool expanded);
  void syncKeyboardInteractivity(Instance& inst) const;
  static void clearInlineReplyFocus(Instance& inst);
  [[nodiscard]] static bool isInlineReplyInputArea(const Instance& inst, const InputArea* area);
  [[nodiscard]] static bool pointerHitsInlineReplyInput(const Instance& inst, const Node* hit);
  [[nodiscard]] static bool inputAreaBelongsToCard(const Instance::CardState& card, const InputArea* area);

  void ensureSurfaces();
  void destroySurfaces();
  void prepareFrame(Instance& inst, bool needsUpdate, bool needsLayout);
  void buildScene(Instance& inst, uint32_t width, uint32_t height);
  InputArea* buildCard(
      const PopupEntry& entry, Node** outCardContent, Node** outCardForeground, Box** outCardBackground,
      ProgressBar** outProgress, Node** outActionsRow, Node** outInlineReplyRow, Input** outInlineReplyInput,
      bool chromeHosted
  );
  void setCardVisualHeight(Instance::CardState& card, float height) const;
  void packCardsForDisplayedHeights(Instance& inst) const;
  void applyCardReveal(Instance::CardState& cs, float reveal, float y, float cardHeight) const;
  [[nodiscard]] float cardReveal(const Instance::CardState& cs, float cardHeight) const;
  void addCardToInstance(Instance& inst, std::size_t entryIndex);
  void removeCardFromInstance(Instance& inst, std::size_t entryIndex);
  void syncEntryVisibility(std::size_t entryIndex);
  void dismissCardFromInstance(Instance& inst, std::size_t entryIndex);

  PopupEntry* findEntry(uint32_t notificationId);
  Instance::CardState* findCardState(Instance& inst, uint32_t notificationId);
  void beginPopupHover(uint32_t notificationId, const ProgressBar* progressBar = nullptr);
  void endPopupHover(uint32_t notificationId, int totalDuration, const ProgressBar* progressBar = nullptr);
  void resetPopupHover(uint32_t notificationId, int totalDuration, bool resumeTimer);
  void resetInstanceHover(Instance& inst, bool resumeTimers);
  void pauseTimeout(uint32_t notificationId, const ProgressBar* progressBar = nullptr);
  void resumeTimeout(uint32_t notificationId, int totalDuration);
  void pauseCountdowns(uint32_t notificationId);
  void resumeCountdowns(uint32_t notificationId);
  void revealQueuedEntries();
  void collapseStack();
  void evictOverlappingEntries(std::size_t anchorIndex);
  [[nodiscard]] bool hasPlacement(const PopupEntry& entry) const;
  [[nodiscard]] bool
  canKeepPlacement(const PopupEntry& entry, std::optional<uint32_t> ignoreNotificationId = std::nullopt) const;
  [[nodiscard]] bool fitsOnSurface(const PopupEntry& entry, float surfaceHeight) const;
  [[nodiscard]] std::string notificationPosition() const;
  [[nodiscard]] std::string notificationLayer() const;
  [[nodiscard]] std::vector<std::string> notificationMonitors() const;
  [[nodiscard]] bool shouldRenderOnOutput(const WaylandOutput& output) const;
  [[nodiscard]] bool isBottomStacking() const;
  [[nodiscard]] RevealDirection revealDirection() const;
  void refreshEntryGeometry(PopupEntry& entry) const;
  [[nodiscard]] float layoutBottomForSurfaceHeight(float surfaceHeight) const;
  [[nodiscard]] float maxPlacementBottom() const;
  [[nodiscard]] float entryOffsetFromPlacementBottom(const PopupEntry& entry) const;
  // Resting surface Y for one instance's card, packed from the stacking edge using this
  // instance's real per-card heights (CardState::clipHeight). Inter-card gaps are taken
  // from the shared placement skeleton, so hover spacing and dismiss gaps are preserved,
  // but heights are per-monitor real values so cards never overlap or leave height-mismatch gaps.
  [[nodiscard]] float cardSurfaceY(const Instance& inst, std::size_t entryIndex) const;
  void alignBottomStackToPlacementBottom();
  [[nodiscard]] std::optional<float>
  findPlacementY(float entryHeight, std::optional<uint32_t> ignoreNotificationId = std::nullopt) const;
  [[nodiscard]] uint32_t surfaceHeightForOutput(wl_output* output) const;
  [[nodiscard]] std::string resolveNotificationIconPath(const PopupEntry& entry);

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  NotificationManager* m_notifications = nullptr;
  RenderContext* m_renderContext = nullptr;
  HttpClient* m_httpClient = nullptr;

  std::vector<PopupEntry> m_entries;
  std::vector<Notification> m_pendingAdds;
  bool m_pendingAddsScheduled = false;
  std::vector<std::unique_ptr<Instance>> m_instances;
  int m_callbackToken = -1;
  IconResolver m_iconResolver;
  std::unordered_map<std::string, std::string> m_remoteIconCache;
  std::unordered_set<std::string> m_pendingRemoteIconDownloads;
  std::unordered_set<std::string> m_failedRemoteIconDownloads;
  std::string m_lastPosition;
  std::string m_lastLayer;
  std::vector<std::string> m_lastMonitorSelectors;
  bool m_sidebarOpen = false;
  std::function<bool(wl_output*, std::vector<ChromePanelState>)> m_chromeToastStateCallback;
};
