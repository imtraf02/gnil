#include "shell/settings/nexus_route.h"

#include <print>
#include <string>
#include <string_view>

namespace {
  int failures = 0;
  void expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "nexus_route_test: FAIL: {}", message);
      ++failures;
    }
  }
}

int main() {
  NexusRoute route;
  expect(nexusPages().size() == 9, "Nexus does not expose exactly nine pages");
  route.setScrollOffset(42.0f);
  route.setPage(NexusPage::Network);
  route.setScrollOffset(91.0f);
  route.pushSubpage({"wifi", "Wi-Fi"});
  route.setSelectedEntity("home-network");
  route.setScrollOffset(17.0f);
  expect(route.currentSubpage() != nullptr && route.currentSubpage()->id == "wifi", "subpage push failed");
  expect(route.scrollOffset() == 17.0f, "subpage scroll was not retained");
  expect(route.popSubpage(), "subpage pop failed");
  expect(route.scrollOffset() == 91.0f, "parent scroll was not restored");
  route.setPage(NexusPage::WallpaperStyle);
  expect(route.scrollOffset() == 42.0f, "page scroll was not restored");
  expect(route.deepLink("audio/applications"), "search deep-link was rejected");
  expect(route.page() == NexusPage::Audio && route.pendingControl() == "applications", "deep-link target was lost");
  expect(route.deepLinkKey() == "audio/applications", "handoff route lost the selected control");
  expect(route.deepLink("panels/frame"), "legacy panels deep-link was rejected");
  expect(route.page() == NexusPage::Panels, "legacy panels deep-link did not resolve to Desktop & Chrome");
  expect(route.deepLink("wallpaper-style/appearance"), "legacy appearance deep-link was rejected");
  expect(route.page() == NexusPage::WallpaperStyle, "legacy appearance deep-link selected the wrong page");
  route.setQuery("volume");
  route.setFocusKey("audio.output");
  expect(route.query() == "volume" && route.focusKey() == "audio.output", "shared search/focus state was lost");
  for (const auto& page : nexusPages()) {
    const std::string target = std::string(page.id) + "/probe-control";
    expect(route.deepLink(target), "public page deep-link was rejected");
    expect(route.page() == page.page, "public page deep-link selected the wrong page");
    expect(route.selectedControl() == "probe-control", "public control deep-link lost its selection");
    expect(route.pendingControl() == "probe-control", "public control deep-link lost its scroll target");
  }
  expect(!route.deepLink("plugins/store"), "removed plugin page is still exposed");
  expect(!route.deepLink("dock/position"), "removed Dock page is still exposed");

  NexusHostCoordinator coordinator;
  expect(coordinator.activate(NexusHost::Panel, reinterpret_cast<void*>(1)) == NexusHost::None, "first host was not exclusive");
  expect(coordinator.activate(NexusHost::Window, reinterpret_cast<void*>(1)) == NexusHost::Panel, "PiP handoff lost prior host");
  coordinator.release(NexusHost::Panel);
  expect(coordinator.host() == NexusHost::Window, "stale panel release closed the window host");
  coordinator.release(NexusHost::Window);
  expect(coordinator.host() == NexusHost::None, "window release did not clear host");
  return failures == 0 ? 0 : 1;
}
