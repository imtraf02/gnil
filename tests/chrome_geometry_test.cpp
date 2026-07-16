#include "shell/bar/bar_reserved_zone.h"
#include "shell/chrome/chrome_geometry.h"

#include <array>
#include <cmath>
#include <iostream>
#include <string_view>

// BarConfig's default member initializer references the palette helper, while
// this geometry-only test intentionally does not link the complete theme
// stack. A local deterministic stub keeps the test focused on geometry.
ColorSpec colorSpecFromRole(ColorRole role, float alpha) noexcept {
  return ColorSpec{.role = role, .alpha = alpha};
}

namespace {

  bool check(bool condition, std::string_view message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
  }

  bool near(float lhs, float rhs, float epsilon = 0.001f) { return std::abs(lhs - rhs) <= epsilon; }

} // namespace

int main() {
  bool ok = true;
  const ChromeGeometrySettings settings{
      .frameThickness = 10.0f,
      .rounding = 25.0f,
      .smoothing = 0.0f,
      .deformScale = 0.0f,
  };
  const ChromeGeometryModel model(settings);

  const auto inner = model.innerFrame(1920.0f, 1080.0f);
  ok &= check(inner == ChromeRect{10.0f, 10.0f, 1900.0f, 1060.0f}, "inner frame uses canonical thickness");
  const ChromeGeometryModel expandedLeft(
      ChromeGeometrySettings{
          .frameThickness = 10.0f,
          .rounding = 25.0f,
          .apertureInsets = ChromeInsets{60.0f, 10.0f, 10.0f, 10.0f},
      }
  );
  ok &= check(
      expandedLeft.innerFrame(1920.0f, 1080.0f) == ChromeRect{60.0f, 10.0f, 1850.0f, 1060.0f},
      "left bar is the left inset of the same rounded aperture"
  );
  ShellConfig shell;
  shell.chrome.frameThickness = 10.0f;
  shell.chrome.rounding = 25.0f;
  shell.chrome.smoothing = 0.0f;
  BarConfig leftBar;
  leftBar.position = "left";
  leftBar.thickness = 60;
  const auto leftContext = resolveChromeLayoutContext(leftBar, shell, 1920, 1080);
  ok &= check(leftContext.barEdge == ChromeEdge::Left, "layout context resolves the left bar edge once");
  ok &= check(
      leftContext.barRect == BarVisibleRect{0, 10, 60, 1070},
      "left bar uses frame thickness as its Y margin"
  );
  ok &= check(near(resolvedBarMainAxisPadding(leftBar, shell), 10.0f), "left bar content uses thickness on Y");
  leftBar.padding = 14;
  ok &= check(near(resolvedBarMainAxisPadding(leftBar, shell), 14.0f), "explicit larger bar padding is preserved");
  leftBar.padding = 5;
  auto thickerShell = shell;
  thickerShell.chrome.frameThickness = 16.0f;
  const auto thickerLeftContext = resolveChromeLayoutContext(leftBar, thickerShell, 1920, 1080);
  ok &= check(
      thickerLeftContext.barRect == BarVisibleRect{0, 16, 60, 1064},
      "changing thickness moves both Y ends of the left bar"
  );
  ok &= check(
      near(resolvedBarMainAxisPadding(leftBar, thickerShell), 16.0f),
      "changing thickness moves the visible bar content by the same amount"
  );
  ok &= check(
      leftContext.geometry.apertureInsets == ChromeInsets{60.0f, 10.0f, 10.0f, 10.0f},
      "layout context shares the expanded left aperture"
  );

  const ChromeRect loose{-50.0f, -20.0f, 300.0f, 240.0f};
  const std::array edges = {ChromeEdge::Top, ChromeEdge::Right, ChromeEdge::Bottom, ChromeEdge::Left};
  for (const auto edge : edges) {
    const auto panel = model.clampPanel(loose, edge, 1920.0f, 1080.0f);
    ok &= check(panel.x >= inner.x && panel.y >= inner.y, "edge panel stays inside inner frame");
    ok &= check(panel.right() <= inner.right() && panel.bottom() <= inner.bottom(), "edge panel is fully clamped");
    if (edge == ChromeEdge::Top)
      ok &= check(near(panel.y, inner.y), "top panel touches frame");
    if (edge == ChromeEdge::Right)
      ok &= check(near(panel.right(), inner.right()), "right panel touches frame");
    if (edge == ChromeEdge::Bottom)
      ok &= check(near(panel.bottom(), inner.bottom()), "bottom panel touches frame");
    if (edge == ChromeEdge::Left)
      ok &= check(near(panel.x, inner.x), "left panel touches frame");
  }

  const std::array anchoredCases = {
      std::pair{ChromeEdge::Left, ChromeRect{0.0f, 10.0f, 60.0f, 1060.0f}},
      std::pair{ChromeEdge::Top, ChromeRect{10.0f, 0.0f, 1900.0f, 60.0f}},
      std::pair{ChromeEdge::Right, ChromeRect{1860.0f, 10.0f, 60.0f, 1060.0f}},
      std::pair{ChromeEdge::Bottom, ChromeRect{10.0f, 1020.0f, 1900.0f, 60.0f}},
  };
  for (const auto& [edge, bar] : anchoredCases) {
    ChromePanelState anchored{
        .rect = ChromeRect{50.0f, 15.0f, 360.0f, 420.0f},
        .triggerAnchor =
            ChromePoint{
                .x = edge == ChromeEdge::Top || edge == ChromeEdge::Bottom ? 80.0f : 30.0f,
                .y = edge == ChromeEdge::Left || edge == ChromeEdge::Right ? 80.0f : 30.0f
            },
        .radius = settings.rounding,
        .edge = edge,
        .hasTriggerAnchor = true,
        .visible = true,
    };
    if (edge == ChromeEdge::Right) {
      anchored.rect.x = 1510.0f;
    } else if (edge == ChromeEdge::Bottom) {
      anchored.rect.y = 650.0f;
    }
    const auto resolved = model.resolveAnchoredPanel(anchored, bar, 1920.0f, 1080.0f);
    ok &= check(resolved.connectorVisible, "anchored panel publishes a connector");
    ok &= check(resolved.rect.x >= inner.x && resolved.rect.y >= inner.y, "anchored body remains inside frame");
    ok &= check(
        resolved.rect.right() <= inner.right() && resolved.rect.bottom() <= inner.bottom(),
        "anchored body remains inside opposite frame"
    );
    if (edge == ChromeEdge::Left || edge == ChromeEdge::Right) {
      ok &=
          check(near(resolved.connector.y + resolved.connector.height * 0.5f, 80.0f), "vertical connector tracks icon");
    } else {
      ok &= check(
          near(resolved.connector.x + resolved.connector.width * 0.5f, 80.0f), "horizontal connector tracks icon"
      );
    }
  }

  const ChromeRect leftBarRect{0.0f, 0.0f, 60.0f, 1080.0f};
  ChromePanelState flushAnchored{
      .rect = chromePlaceAttachedBody(ChromeRect{0.0f, 100.0f, 360.0f, 420.0f}, leftBarRect, ChromeEdge::Left),
      .triggerAnchor = ChromePoint{.x = 30.0f, .y = 310.0f},
      .radius = settings.rounding,
      .edge = ChromeEdge::Left,
      .hasTriggerAnchor = true,
      .attached = true,
      .visible = true,
  };
  flushAnchored = expandedLeft.resolveAnchoredPanel(flushAnchored, leftBarRect, 1920.0f, 1080.0f);
  ok &= check(near(flushAnchored.rect.x, 60.0f), "left popout stays flush with the shared aperture");
  ok &= check(!flushAnchored.connectorVisible, "flush popout does not add a redundant connector primitive");
  flushAnchored.rect.y = 10.0f;
  flushAnchored.rect.height = 1060.0f;
  flushAnchored = expandedLeft.resolveJoinedShape(flushAnchored, 1920.0f, 1080.0f);
  ok &= check(
      flushAnchored.joinedEdges == ChromeJoinEdge::Left,
      "anchored bar popout joins only its source edge when clamped against top and bottom"
  );

  // The logical/input body never overlaps the bar and stays unchanged. The
  // shared host derives explicit concave wings from its classified joins.
  for (const auto& [edge, bar] : anchoredCases) {
    const ChromeRect source{100.0f, 100.0f, 300.0f, 200.0f};
    const auto openBody = chromePlaceAttachedBody(source, bar, edge);
    if (edge == ChromeEdge::Top)
      ok &= check(near(openBody.y, bar.bottom()), "top body begins after bar");
    if (edge == ChromeEdge::Right)
      ok &= check(near(openBody.right(), bar.x), "right body ends before bar");
    if (edge == ChromeEdge::Bottom)
      ok &= check(near(openBody.bottom(), bar.y), "bottom body ends before bar");
    if (edge == ChromeEdge::Left)
      ok &= check(near(openBody.x, bar.right()), "left body begins after bar");

    auto joined = model.resolveJoinedShape(
        ChromePanelState{
            .rect = openBody,
            .radius = settings.rounding,
            .edge = edge,
            .attached = true,
            .visible = true,
        },
        1920.0f, 1080.0f
    );
    const std::uint8_t expectedJoin = edge == ChromeEdge::Top ? ChromeJoinEdge::Top
        : edge == ChromeEdge::Right                          ? ChromeJoinEdge::Right
        : edge == ChromeEdge::Bottom                         ? ChromeJoinEdge::Bottom
                                                              : ChromeJoinEdge::Left;
    ok &= check((joined.joinedEdges & expectedJoin) != 0, "attached body records its structural join");
    ok &= check(joined.rect == openBody, "join classification never expands the logical/render rect");

    const ChromeRect hidden = chromeAnchoredRevealRect(openBody, edge, 0.0f);
    const ChromeRect quarter = chromeAnchoredRevealRect(openBody, edge, 0.25f);
    const ChromeRect half = chromeAnchoredRevealRect(openBody, edge, 0.5f);
    const ChromeRect openReveal = chromeAnchoredRevealRect(openBody, edge, 1.0f);

    const auto hiddenClip = chromeAttachedRevealClip(hidden, openBody, edge);
    const auto quarterClip = chromeAttachedRevealClip(quarter, openBody, edge);
    const auto openClip = chromeAttachedRevealClip(openBody, openBody, edge);
    ok &= check(
        hiddenClip.width <= 1.0f || hiddenClip.height <= 1.0f,
        "fully hidden attached content is only the structural contact strip"
    );
    ok &= check(quarterClip.width > 0.0f && quarterClip.height > 0.0f, "quarter reveal has visible content");
    ok &= check(openClip == openBody, "fully revealed content restores the complete logical body");
    ok &= check(openReveal == openBody, "anchored reveal ends at the exact open body");
    if (edge == ChromeEdge::Top || edge == ChromeEdge::Left) {
      ok &= check(quarter.x == openBody.x && quarter.y == openBody.y, "near contact origin stays fixed");
    }
    if (edge == ChromeEdge::Right) {
      ok &= check(near(quarter.right(), openBody.right()), "right contact stays fixed");
      ok &= check(half.width > quarter.width, "right reveal extent grows monotonically");
    }
    if (edge == ChromeEdge::Bottom) {
      ok &= check(near(quarter.bottom(), openBody.bottom()), "bottom contact stays fixed");
      ok &= check(half.height > quarter.height, "bottom reveal extent grows monotonically");
    }
    if (edge == ChromeEdge::Top)
      ok &= check(quarterClip.y >= openBody.y, "top reveal never enters bar lane");
    if (edge == ChromeEdge::Right)
      ok &= check(quarterClip.right() <= openBody.right(), "right reveal never enters bar lane");
    if (edge == ChromeEdge::Bottom)
      ok &= check(quarterClip.bottom() <= openBody.bottom(), "bottom reveal never enters bar lane");
    if (edge == ChromeEdge::Left)
      ok &= check(quarterClip.x >= openBody.x, "left reveal never enters bar lane");
  }

  const float frameAtEdge = chromeFrameDistance(5.0f, 540.0f, 1920.0f, 1080.0f, settings);
  const float frameAtCenter = chromeFrameDistance(960.0f, 540.0f, 1920.0f, 1080.0f, settings);
  ok &= check(frameAtEdge < 0.0f, "inverted rounded rectangle contains the frame");
  ok &= check(frameAtCenter > 0.0f, "inverted rounded rectangle excludes application center");

  const ChromeRect attached{10.0f, 200.0f, 320.0f, 420.0f};
  const ChromeRect nearFrame{18.0f, 200.0f, 320.0f, 420.0f};
  const ChromeRect detached{700.0f, 250.0f, 420.0f, 360.0f};
  const float frameD = chromeFrameDistance(12.0f, 400.0f, 1920.0f, 1080.0f, settings);
  ok &= check(
      chromeSmoothUnion(frameD, chromeRoundedRectDistance(12.0f, 400.0f, attached, 25.0f), settings.smoothing) < 0.0f,
      "panel touching frame is one union"
  );
  ok &= check(chromeRoundedRectDistance(25.0f, 400.0f, nearFrame, 25.0f) < 0.0f, "near-edge panel stays rounded");
  ok &= check(chromeRoundedRectDistance(910.0f, 430.0f, detached, 25.0f) < 0.0f, "detached island has own SDF");
  ok &= check(chromeRoundedRectDistance(700.0f, 250.0f, detached, 25.0f) > 0.0f, "detached corner is rounded");

  const ChromeRect logicalJoin{60.0f, 180.0f, 360.0f, 420.0f};
  const auto leftBridge = chromeJoinedRenderRect(logicalJoin, ChromeJoinEdge::Left);
  ok &= check(logicalJoin.x == 60.0f, "render bridge does not mutate logical panel geometry");
  ok &= check(near(leftBridge.x, -12.0f), "left join extends twenty percent underneath the bar");
  ok &= check(near(leftBridge.right(), logicalJoin.right()), "left bridge preserves the visible far edge");
  const auto topRightBridge = chromeJoinedRenderRect(
      logicalJoin, static_cast<std::uint8_t>(ChromeJoinEdge::Top | ChromeJoinEdge::Right)
  );
  ok &= check(near(topRightBridge.y, 96.0f), "top join extends underneath the top frame");
  ok &= check(near(topRightBridge.right(), 492.0f), "corner attachment bridges both joined edges");
  ok &= check(
      chromeCircularSmoothUnion(2.0f, 2.0f, 8.0f) < 0.0f,
      "circular union closes the positive-distance contact gap"
  );
  ok &= check(
      chromeCircularSmoothUnion(0.0f, 0.0f, 8.0f) < 0.0f
          && chromeCircularSmoothUnion(4.0f, 0.0f, 8.0f) < 0.0f
          && chromeCircularSmoothUnion(8.0f, 8.0f, 8.0f) > 0.0f,
      "joined shoulder forms a concave circular arc instead of a square corner"
  );

  // Default GNIL layout: wallpaper touches the expanded left aperture and the
  // top frame. Every touching corner receives an explicit concavity axis.
  ChromePanelState wallpaper{
      .rect = ChromeRect{60.0f, 10.0f, 980.0f, 700.0f},
      .radius = settings.rounding,
      .edge = ChromeEdge::Left,
      .attached = true,
      .visible = true,
  };
  wallpaper = expandedLeft.resolveJoinedShape(wallpaper, 1920.0f, 1080.0f);
  ok &= check(
      (wallpaper.joinedEdges & (ChromeJoinEdge::Left | ChromeJoinEdge::Top))
          == (ChromeJoinEdge::Left | ChromeJoinEdge::Top),
      "wallpaper records both structural joins"
  );
  ok &= check(near(wallpaper.radius, settings.rounding), "wallpaper join does not change radius");
  const auto wallpaperShape = chromeResolvePanelShape(wallpaper);
  ok &= check(
      wallpaperShape.corners == ChromeCornerModes{
          .topLeft = ChromeCornerMode::ConcaveVertical,
          .topRight = ChromeCornerMode::ConcaveHorizontal,
          .bottomRight = ChromeCornerMode::Convex,
          .bottomLeft = ChromeCornerMode::ConcaveVertical,
      },
      "left wallpaper uses dock-axis corner precedence"
  );
  ok &= check(
      wallpaperShape.bodyInsets == ChromeInsets{0.0f, 25.0f, 25.0f, 25.0f},
      "wallpaper grows only the wings required by its joined edges"
  );
  ok &= check(
      wallpaperShape.visualRect == ChromeRect{59.0f, -16.0f, 1006.0f, 751.0f},
      "wallpaper visual bounds include concave wings and visual-only contact overlap"
  );
  ok &= check(near(wallpaperShape.effectiveRadius, 25.0f), "wallpaper wings use the effective radius");

  const std::array frameCornerCases = {
      std::pair{static_cast<std::uint8_t>(ChromeJoinEdge::Top | ChromeJoinEdge::Left),
                ChromeRect{10.0f, 10.0f, 320.0f, 240.0f}},
      std::pair{static_cast<std::uint8_t>(ChromeJoinEdge::Top | ChromeJoinEdge::Right),
                ChromeRect{1590.0f, 10.0f, 320.0f, 240.0f}},
      std::pair{static_cast<std::uint8_t>(ChromeJoinEdge::Right | ChromeJoinEdge::Bottom),
                ChromeRect{1590.0f, 830.0f, 320.0f, 240.0f}},
      std::pair{static_cast<std::uint8_t>(ChromeJoinEdge::Bottom | ChromeJoinEdge::Left),
                ChromeRect{10.0f, 830.0f, 320.0f, 240.0f}},
  };
  for (const auto& [expectedEdges, rect] : frameCornerCases) {
    ChromePanelState corner{
        .rect = rect,
        .radius = settings.rounding,
        .visible = true,
    };
    corner = model.resolveJoinedShape(corner, 1920.0f, 1080.0f);
    ok &= check((corner.joinedEdges & expectedEdges) == expectedEdges, "each frame corner records both joined edges");
    ok &= check(near(corner.radius, settings.rounding), "frame corner join preserves the shared radius");
  }

  ChromePanelState topRightToast{
      .rect = ChromeRect{1590.0f, 10.0f, 320.0f, 240.0f},
      .radius = settings.rounding,
      .edge = ChromeEdge::Top,
      .attached = true,
      .visible = true,
  };
  topRightToast = model.resolveJoinedShape(topRightToast, 1920.0f, 1080.0f);
  ok &= check(
      (topRightToast.joinedEdges & (ChromeJoinEdge::Top | ChromeJoinEdge::Right))
          == (ChromeJoinEdge::Top | ChromeJoinEdge::Right),
      "top-right toast touches both aperture edges"
  );
  const auto topRightToastShape = chromeResolvePanelShape(topRightToast);
  ok &= check(
      topRightToastShape.corners.topRight == ChromeCornerMode::ConcaveHorizontal,
      "top-right toast follows the top attachment corner precedence"
  );
  ok &= check(topRightToast.rect == ChromeRect{1590.0f, 10.0f, 320.0f, 240.0f},
              "toast contact never expands logical/input geometry");

  ChromePanelState launcher{
      .rect = ChromeRect{645.0f, 962.0f, 630.0f, 108.0f},
      .radius = settings.rounding,
      .edge = ChromeEdge::Bottom,
      .attached = true,
      .visible = true,
  };
  launcher = model.resolveJoinedShape(launcher, 1920.0f, 1080.0f);
  ok &= check((launcher.joinedEdges & ChromeJoinEdge::Bottom) != 0, "launcher joins bottom frame");
  const auto launcherShape = chromeResolvePanelShape(launcher);
  ok &= check(
      launcherShape.corners == ChromeCornerModes{
          .topLeft = ChromeCornerMode::Convex,
          .topRight = ChromeCornerMode::Convex,
          .bottomRight = ChromeCornerMode::ConcaveHorizontal,
          .bottomLeft = ChromeCornerMode::ConcaveHorizontal,
      },
      "bottom launcher opens horizontal wings at both contacts"
  );
  ok &= check(
      launcherShape.bodyInsets == ChromeInsets{25.0f, 0.0f, 25.0f, 0.0f},
      "launcher wing width equals the configured corner radius"
  );

  ChromePanelState shortPanel{
      .rect = ChromeRect{700.0f, 1060.0f, 520.0f, 10.0f},
      .radius = settings.rounding,
      .edge = ChromeEdge::Bottom,
      .joinedEdges = ChromeJoinEdge::Bottom,
      .attached = true,
      .visible = true,
  };
  const auto shortShape = chromeResolvePanelShape(shortPanel);
  ok &= check(near(shortShape.effectiveRadius, 5.0f), "short panel flattens its effective radius");
  ok &= check(
      shortShape.bodyInsets == ChromeInsets{5.0f, 0.0f, 5.0f, 0.0f},
      "short panel wings use the flattened radius"
  );

  ChromePanelState closed{
      .rect = chromeAnchoredRevealRect(ChromeRect{10.0f, 200.0f, 320.0f, 420.0f}, ChromeEdge::Left, 0.0f),
      .radius = 25.0f,
      .opacity = 0.0f,
      .progress = 0.0f,
      .edge = ChromeEdge::Left,
      .attached = true,
      .visible = false,
      .inputEnabled = false,
  };
  ChromePanelState opened = closed;
  opened.rect = ChromeRect{10.0f, 200.0f, 320.0f, 420.0f};
  opened.opacity = 1.0f;
  opened.progress = 1.0f;
  opened.visible = true;
  opened.inputEnabled = true;
  opened.connector = ChromeRect{0.0f, 360.0f, 40.0f, 50.0f};
  opened.connectorVisible = true;

  ChromeTransitionState transition;
  transition.reset(closed);
  transition.setTarget(opened);
  for (const float progress : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
    const auto state = transition.sample(progress, settings.deformScale);
    ok &= check(state.radius >= 1.0f, "transition radius never becomes rectangular");
    ok &= check(state.rect.width > 0.0f && state.rect.height > 0.0f, "transition silhouette never collapses");
    ok &= check(near(state.rect.x, opened.rect.x), "left reveal keeps frame contact while animating");
  }

  transition.reset(closed);
  transition.setTarget(opened);
  const auto midOpen = transition.sample(0.5f, settings.deformScale);
  ChromePanelState closingTarget = closed;
  transition.setTarget(closingTarget);
  const auto reversalStart = transition.sample(0.0f, settings.deformScale);
  ok &= check(reversalStart.rect == midOpen.rect, "close reversal starts at displayed geometry");
  ok &= check(!reversalStart.inputEnabled, "input clears immediately when close starts");
  const auto midClose = transition.sample(0.25f, settings.deformScale);
  ok &= check(midClose.radius >= 1.0f, "mid-close keeps rounded silhouette");
  transition.setTarget(opened);
  const auto reopenStart = transition.sample(0.0f, settings.deformScale);
  ok &= check(reopenStart.radius >= 1.0f, "reopen reversal preserves rounded silhouette");

  ChromePanelState panelB = opened;
  panelB.rect = ChromeRect{420.0f, 60.0f, 440.0f, 510.0f};
  panelB.radius = 32.0f;
  transition.reset(opened);
  transition.setTarget(panelB);
  const auto displayedAB = transition.sample(0.37f, settings.deformScale);
  ChromePanelState panelC = opened;
  panelC.rect = ChromeRect{930.0f, 90.0f, 280.0f, 300.0f};
  panelC.radius = 18.0f;
  transition.setTarget(panelC);
  const auto retargetStart = transition.sample(0.0f, settings.deformScale);
  ok &= check(retargetStart.rect == displayedAB.rect, "rapid A-B-C retarget starts at displayed rect");
  ok &= check(near(retargetStart.radius, displayedAB.radius), "rapid retarget starts at displayed radius");
  const auto retargetMiddle = transition.sample(0.5f, settings.deformScale);
  ok &= check(retargetMiddle.rect.x != retargetStart.rect.x, "retarget interpolates position");
  ok &= check(retargetMiddle.rect.width != retargetStart.rect.width, "retarget interpolates width in the same state");
  ok &= check(retargetMiddle.rect.height != retargetStart.rect.height, "retarget interpolates height in the same state");
  const auto overshotSample = transition.sample(1.2f, settings.deformScale);
  ok &= check(overshotSample.rect == panelC.rect, "geometry clamps an overshooting easing sample to its target");
  ok &= check(near(overshotSample.radius, panelC.radius), "corner morph never overshoots its target radius");

  ok &= check(model.physicalFrameThickness(1.0f) == 10, "scale 1 frame");
  ok &= check(model.physicalFrameThickness(1.25f) == 13, "scale 1.25 frame rounds once");
  ok &= check(model.physicalFrameThickness(1.5f) == 15, "scale 1.5 frame");
  ok &= check(model.physicalFrameThickness(2.0f) == 20, "scale 2 frame");

  const auto leftZones = model.exclusiveZones(ChromeEdge::Left, 60.0f, true);
  const auto topZones = model.exclusiveZones(ChromeEdge::Top, 60.0f, true);
  const auto rightZones = model.exclusiveZones(ChromeEdge::Right, 60.0f, true);
  const auto bottomZones = model.exclusiveZones(ChromeEdge::Bottom, 60.0f, true);
  ok &= check(leftZones == std::array<std::int32_t, 4>{60, 10, 10, 10}, "left exclusive zone matches rail width");
  ok &= check(topZones == std::array<std::int32_t, 4>{10, 60, 10, 10}, "top exclusive zone matches rail width");
  ok &= check(rightZones == std::array<std::int32_t, 4>{10, 10, 60, 10}, "right exclusive zone matches rail width");
  ok &= check(bottomZones == std::array<std::int32_t, 4>{10, 10, 10, 60}, "bottom exclusive zone matches rail width");

  ok &= check(
      resolveBarVisibleRect("left", 52, 1920, 1080) == BarVisibleRect{0, 0, 52, 1080},
      "left tray join uses the structural edge rail"
  );
  ok &= check(
      resolveBarVisibleRect("right", 52, 1920, 1080) == BarVisibleRect{1868, 0, 1920, 1080},
      "right tray join uses the structural edge rail"
  );
  ok &= check(
      resolveBarVisibleRect("top", 52, 1920, 1080) == BarVisibleRect{0, 0, 1920, 52},
      "top tray join uses the structural edge rail"
  );
  ok &= check(
      resolveBarVisibleRect("bottom", 52, 1920, 1080) == BarVisibleRect{0, 1028, 1920, 1080},
      "bottom tray join uses the structural edge rail"
  );
  ok &= check(
      resolveBarVisibleRect("top", 52, 1920, 1080, 10) == BarVisibleRect{10, 0, 1910, 52},
      "top rail retains the old frame-sized end margins"
  );
  ok &= check(
      resolveBarVisibleRect("right", 52, 1920, 1080, 10) == BarVisibleRect{1868, 10, 1920, 1070},
      "right rail retains the old frame-sized Y margins"
  );

  ok &= check(
      chromeFrameDistance(20.0f, 540.0f, 1920.0f, 1080.0f, expandedLeft.settings()) < 0.0f,
      "expanded bar remains inside the structural frame fill"
  );
  ok &= check(
      chromeFrameDistance(80.0f, 540.0f, 1920.0f, 1080.0f, expandedLeft.settings()) > 0.0f,
      "expanded aperture begins after the bar without a second rounded primitive"
  );

  return ok ? 0 : 1;
}
