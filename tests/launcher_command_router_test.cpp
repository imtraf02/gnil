#include "launcher/command_router.h"

#include <print>
#include <string_view>

namespace {
  int failures = 0;
  void expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "launcher_command_router_test: FAIL: {}", message);
      ++failures;
    }
  }
}

int main() {
  using namespace launcher_command;
  Router router;
  router.configure(false, false);

  expect(!router.route("firefox").active(), "plain query entered command mode");
  expect(router.route("/").mode == Mode::Catalog, "slash did not open catalog");
  expect(router.route("/cal").mode == Mode::Catalog, "partial calc entered mode too early");
  expect(router.route("/calc ").mode == Mode::Calculator, "calc mode not routed");
  expect(router.route("/wallpaper ocean").mode == Mode::Wallpaper, "longest wallpaper mode not routed");
  expect(router.route("/variant vibrant").query == "vibrant", "variant query not extracted");
  expect(!router.route("/win ").active(), "window provider prefix was swallowed by command catalog");
  expect(!router.route("/emoji smile").active(), "emoji provider prefix was swallowed by command catalog");
  expect(!router.route("/session ").active(), "session provider prefix was swallowed by command catalog");

  const auto all = router.catalog();
  expect(all.size() == 16, "catalog size/order contract changed");
  expect(all[0].id == ActionId::Calculator && all[12].id == ActionId::Settings, "Caelestia action order changed");
  expect(all[13].id == ActionId::Window && all[15].id == ActionId::Session, "GNIL actions are not last");
  expect(!all[7].available && !all[8].available && !all[9].available, "dangerous actions were enabled");
  expect(!all[10].available, "lock action available without lockscreen");
  expect(router.catalog("cal").size() == 1, "catalog filtering is not deterministic");
  expect(router.autocomplete(ActionId::Calculator) == "/calc ", "mode autocomplete missing separator");

  expect(router.activate(ActionId::Shutdown).kind == ActivationKind::Unavailable, "disabled shutdown activated");
  router.configure(true, true);
  expect(router.activate(ActionId::Shutdown).kind == ActivationKind::Confirm, "shutdown skipped confirmation");
  expect(router.pendingConfirmation() == ActionId::Shutdown, "confirmation state not retained");
  expect(router.escape(), "Escape did not dismiss confirmation");
  expect(!router.pendingConfirmation().has_value(), "Escape left confirmation active");
  expect(router.activate(ActionId::Reboot).kind == ActivationKind::Confirm, "reboot skipped confirmation");
  expect(router.activate(ActionId::Reboot).kind == ActivationKind::Execute, "second reboot activation did not execute");
  expect(
      router.activate(ActionId::Wallpaper).query == "/wallpaper ",
      "specialized action did not rewrite to provider mode"
  );

  return failures == 0 ? 0 : 1;
}
