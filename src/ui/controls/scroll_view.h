#pragma once

#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/signal.h"
#include "ui/style.h"

#include <chrono>
#include <cstdint>
#include <functional>

class InputArea;
class RectNode;
class Scrollbar;

struct ScrollViewState {
  float offset = 0.0f;
};

class ScrollView : public Flex {
public:
  ScrollView();

  [[nodiscard]] Flex* content() noexcept { return m_content; }
  [[nodiscard]] const Flex* content() const noexcept { return m_content; }

  void setScrollOffset(float offset);
  void scrollToOffset(
      float offset, bool animated, float durationMs = -1.0f, std::function<void()> onComplete = {}
  );
  void scrollBy(float delta);
  void setScrollbarVisible(bool visible);
  void setDragScrollingEnabled(bool enabled) noexcept;
  // Return true to consume a vertical wheel step instead of applying pixel scrolling.
  void setWheelHandler(std::function<bool(float)> callback);
  void setViewportPaddingH(float padding);
  void setViewportPaddingV(float padding);
  void setFill(const ColorSpec& fill);
  void setFill(const Color& fill);
  void clearFill();
  void setBorder(const ColorSpec& border, float width);
  void setBorder(const Color& border, float width);
  void clearBorder();
  void setRadius(float radius);
  void setSoftness(float softness);
  void setCardStyle(float scale = 1.0f, float fillOpacity = 1.0f, bool showBorder = true);
  void bindState(ScrollViewState* state);
  void setOnScrollChanged(std::function<void(float)> callback);
  // Called when a wheel event arrives but the list is already at the scroll
  // boundary. Positive delta = at the bottom; negative delta = at the top.
  void setOnWheelBoundary(std::function<void(float)> callback);

  [[nodiscard]] float scrollOffset() const noexcept { return m_scrollOffset; }
  [[nodiscard]] float maxScrollOffset() const noexcept { return m_maxScrollOffset; }
  [[nodiscard]] bool scrollable() const noexcept { return m_maxScrollOffset > 0.0f; }
  [[nodiscard]] float contentViewportWidth() const noexcept;
  [[nodiscard]] float contentViewportHeight() const noexcept;
  [[nodiscard]] float viewportPaddingH() const noexcept { return m_viewportPaddingH; }
  [[nodiscard]] float viewportPaddingV() const noexcept { return m_viewportPaddingV; }

private:
  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;
  void applyPalette();
  void applyScrollOffset();
  void applyScrollOffsetValue(float offset);
  void stopScrollAnimation();
  void animateScrollTo(float target, float durationMs = -1.0f, std::function<void()> onComplete = {});
  void startFling();
  [[nodiscard]] float clampOffset(float offset) const noexcept;

  RectNode* m_background = nullptr;
  InputArea* m_viewportArea = nullptr;
  Flex* m_content = nullptr;
  Scrollbar* m_scrollbar = nullptr;

  ScrollViewState* m_boundState = nullptr;
  std::function<void(float)> m_onScrollChanged;
  std::function<void(float)> m_onWheelBoundary;
  std::function<bool(float)> m_wheelHandler;
  ColorSpec m_backgroundFill = clearColorSpec();
  ColorSpec m_backgroundBorder = clearColorSpec();
  Signal<>::ScopedConnection m_paletteConn;

  float m_viewportPaddingH = Style::spaceXs;
  float m_viewportPaddingV = Style::spaceSm;
  float m_scrollOffset = 0.0f;
  float m_targetScrollOffset = 0.0f;
  float m_maxScrollOffset = 0.0f;
  float m_scrollWheelStep = Style::scrollWheelStep;
  float m_dragStartLocalY = 0.0f;
  float m_dragStartOffset = 0.0f;
  float m_lastDragLocalY = 0.0f;
  float m_dragVelocity = 0.0f;
  std::chrono::steady_clock::time_point m_lastDragSampleAt;
  std::uint32_t m_scrollAnimId = 0;
  float m_viewportHeight = 0.0f;
  float m_viewportWidth = 0.0f;
  float m_backgroundBorderWidth = 0.0f;
  float m_backgroundRadius = Style::scaledRadiusMd();
  float m_backgroundSoftness = 1.0f;
  bool m_scrollbarShown = false;
  bool m_showScrollbar = true;
  bool m_dragScrollingEnabled = true;
  bool m_dragging = false;
};
