#include "shell/panel/panel_click_shield.h"

#include <iostream>
#include <string_view>

namespace {
  bool check(bool value, std::string_view message) {
    if (!value) {
      std::cerr << "FAIL: " << message << '\n';
    }
    return value;
  }
} // namespace

int main() {
  bool ok = true;
  using panel_dismissal::usesClickShield;

  ok &= check(usesClickShield("launcher", false), "shortcut launcher needs outside dismissal");
  ok &= check(usesClickShield("session", false), "shortcut session needs outside dismissal");
  ok &= check(usesClickShield("network", false), "standalone content panels need outside dismissal");
  ok &= check(usesClickShield("notifications", false), "sidebar needs outside dismissal");
  ok &= check(usesClickShield("utilities", false), "utilities need outside dismissal");
  ok &= check(usesClickShield("tray-drawer", false), "deep tray drawer needs outside dismissal");
  ok &= check(usesClickShield("clipboard", true), "rail-triggered popout joins the dismissal group");
  ok &= check(usesClickShield("settings", false), "shortcut settings needs outside dismissal");
  ok &= check(usesClickShield("settings", true), "rail-triggered settings needs outside dismissal");
  ok &= check(!usesClickShield("polkit", false), "security prompt is not outside-dismissible");
  ok &= check(!usesClickShield("setup-wizard", false), "persistent flow is not outside-dismissible");

  return ok ? 0 : 1;
}
