#pragma once

#include "launcher/launcher_provider.h"
#include "launcher/command_router.h"
#include "launcher/usage_tracker.h"
#include "render/core/thumbnail_service.h"
#include "shell/panel/panel.h"
#include "system/icon_resolver.h"
#include "ui/signal.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ContextMenuPopup;
class Flex;
class Input;
class Label;
class LauncherResultAdapter;
class LauncherAppGridAdapter;
class Renderer;
class Segmented;
class ScrollView;
class VirtualGridView;
class WallpaperCarousel;
class ConfigService;
class AsyncTextureCache;
class Wallpaper;

class LauncherPanel : public Panel {
public:
  LauncherPanel(
      ConfigService* config, AsyncTextureCache* asyncTextures, ThumbnailService* thumbnails, Wallpaper* wallpaper
  );
  ~LauncherPanel() override;

  void addProvider(std::unique_ptr<LauncherProvider> provider);
  // Drop providers whose stable id starts with `prefix` (e.g. config-driven "dmenu.").
  void clearProvidersWithIdPrefix(std::string_view prefix);
  // Restrict the next open to a single provider (stdin/dmenu session). When set,
  // onInputChanged queries only that provider and skips prefix routing/overview.
  // Cleared on close.
  void setScopedProvider(std::string_view providerId, std::string_view placeholder = {});

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  void onIconThemeChanged() override;

  void clearUsage();
  // Drops persisted usage data when sort-by-usage is off (including after config reload).
  void syncUsageTrackingState();

  [[nodiscard]] float preferredWidth() const override { return scaled(630.0f); }
  [[nodiscard]] float preferredHeight() const override { return scaled(420.0f); }
  // The launcher is a bottom drawer body, never a full output-sized card.
  [[nodiscard]] bool fillsWidth() const noexcept override { return false; }
  [[nodiscard]] bool fillsHeight() const noexcept override { return false; }
  [[nodiscard]] bool usesDynamicVisualSize() const noexcept override { return true; }
  [[nodiscard]] float initialVisualWidth() const override { return scaled(630.0f); }
  [[nodiscard]] float initialVisualHeight() const override { return scaled(420.0f); }
  [[nodiscard]] ChromeEdge chromeEdge() const noexcept override { return ChromeEdge::Bottom; }
  [[nodiscard]] LayerShellLayer layer() const override { return LayerShellLayer::Overlay; }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::OnDemand; }
  [[nodiscard]] InputArea* initialFocusArea() const override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;
  [[nodiscard]] bool dismissTransientUi() override;
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override;

private:
  enum class Presentation { Applications, ProviderOverview, Wallpaper, LiveWallpaper, Emoji, Detail };

  void onPanelCardOpacityChanged(float opacity) override;
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void onInputChanged(const std::string& text);
  void setQuery(std::string query);
  // Re-gather the current query, preserving the selected result by identity.
  void reapplyCurrentQuery();
  // A provider delivered fresh async results — re-gather if the panel is open.
  void onProviderResultsChanged();
  void refreshResults();
  [[nodiscard]] bool routeCommandQuery(std::string_view text);
  [[nodiscard]] std::vector<LauncherResult> commandCatalogResults(std::string_view filter);
  [[nodiscard]] std::vector<LauncherResult> commandSchemeResults(std::string_view filter) const;
  [[nodiscard]] std::vector<LauncherResult> commandVariantResults(std::string_view filter) const;
  bool activateCommandResult(const LauncherResult& result);
  void activateAt(std::size_t index);
  void activateSelected();
  void selectWallpaperAt(std::size_t index);
  bool previewWallpaperAt(std::size_t index);
  bool applyWallpaperAt(std::size_t index, bool closePanel);
  bool handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers);
  void applyEmptyState();
  void bindDetailResult();
  [[nodiscard]] bool shouldUseDetailPresentation() const;
  [[nodiscard]] std::vector<LauncherResult> providerOverviewResults(std::string_view text) const;
  void openAppActionsMenu(std::size_t index, float anchorX, float anchorY);
  void publishResults();
  void syncLauncherListStyle();
  void syncLauncherViewLayout(Renderer* renderer = nullptr);
  [[nodiscard]] bool shouldUseAppGrid() const;
  [[nodiscard]] bool isWallpaperBrowse() const;
  [[nodiscard]] bool isLiveWallpaperBrowse() const;
  [[nodiscard]] Presentation currentPresentation() const;
  void syncDynamicVisualSize(bool animate = true);
  void refreshLauncherAppIconColorization();
  void updateLauncherGridMetrics(Renderer& renderer);
  [[nodiscard]] bool shouldTrackUsage() const;

  std::vector<std::unique_ptr<LauncherProvider>> m_providers;
  std::vector<LauncherResult> m_results;
  std::vector<LauncherResult> m_allResults;
  UsageTracker m_usageTracker;
  launcher_command::Router m_commandRouter;
  IconResolver m_iconResolver;

  Flex* m_container = nullptr;
  Input* m_input = nullptr;
  Segmented* m_wallpaperModeTab = nullptr;
  Flex* m_body = nullptr;
  VirtualGridView* m_grid = nullptr;
  WallpaperCarousel* m_wallpaperCarousel = nullptr;
  ScrollView* m_detailScroll = nullptr;
  Label* m_detailSubtitle = nullptr;
  Label* m_detailBody = nullptr;
  Label* m_emptyLabel = nullptr;
  std::unique_ptr<LauncherResultAdapter> m_listAdapter;
  std::unique_ptr<LauncherAppGridAdapter> m_gridAdapter;

  std::string m_query;
  std::string m_scopedProviderId;
  std::string m_scopedPlaceholder;
  std::size_t m_selectedIndex = 0;
  std::optional<std::size_t> m_pendingWallpaperSelection;
  std::uint64_t m_wallpaperPreviewSerial = 0;
  bool m_launcherShowIcons = true;
  bool m_launcherCompact = false;
  bool m_launcherAppGrid = false;
  bool m_usingAppGrid = false;
  bool m_usingWallpaperGrid = false;
  Presentation m_presentation = Presentation::Applications;
  std::uint32_t m_modeTransitionAnimation = 0;
  std::size_t m_wallpaperModeIndex = 0;
  float m_launcherRowHeight = 0.0f;
  ConfigService* m_config = nullptr;
  AsyncTextureCache* m_asyncTextures = nullptr;
  ThumbnailService* m_thumbnails = nullptr;
  Wallpaper* m_wallpaper = nullptr;
  ThumbnailService::Subscription m_thumbnailPendingSub;
  bool m_thumbnailRefreshPending = false;
  std::unique_ptr<ContextMenuPopup> m_actionsMenu;
  Signal<>::ScopedConnection m_appIconColorizeConn;
};
