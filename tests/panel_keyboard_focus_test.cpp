#include "shell/panel/panel_keyboard_focus.h"

#include <iostream>
#include <string_view>

namespace {
  bool check(bool value, std::string_view message) {
    if (!value) {
      std::cerr << "FAIL: " << message << '\n';
    }
    return value;
  }
}

int main() {
  using Action = PanelKeyboardFocusTracker::Action;
  bool ok = true;
  PanelKeyboardFocusTracker focus;

  focus.beginOpen();
  ok &= check(focus.needsBootstrap(), "unfocused open panel requests focus bootstrap");
  ok &= check(focus.observe(false, true) == Action::None, "foreign focus before map is ignored");
  ok &= check(!focus.shouldClose(false), "outside dismissal is not armed before panel focus");
  ok &= check(focus.needsBootstrap(), "foreign focus does not satisfy bootstrap");

  ok &= check(focus.observe(true, true) == Action::None, "panel focus arms tracker");
  ok &= check(focus.acquired(), "panel focus was acquired");
  ok &= check(!focus.needsBootstrap(), "owned focus completes bootstrap");
  ok &= check(focus.observe(true, false) == Action::CheckAfterDispatch, "owned leave is checked later");
  ok &= check(!focus.shouldClose(true), "panel to owned popup remains open");

  ok &= check(focus.observe(true, true) == Action::None, "owned popup enter remains armed");
  ok &= check(focus.observe(false, true) == Action::CheckAfterDispatch, "foreign enter requests close check");
  ok &= check(focus.shouldClose(false), "panel to application closes");

  focus.endOpen();
  ok &= check(!focus.shouldClose(false), "destroyed panel ignores stale deferred close");
  ok &= check(!focus.needsBootstrap(), "closed panel cannot request bootstrap");
  focus.beginOpen();
  ok &= check(!focus.acquired(), "reopen starts with a fresh focus acquisition");

  ok &= check(
      shouldDeferFocusDismissToTriggerToggle(true, true, 10, 11),
      "a new pointer press on the opening trigger defers dismissal to toggle"
  );
  ok &= check(
      !shouldDeferFocusDismissToTriggerToggle(true, false, 10, 11),
      "keyboard activity over the trigger remains an outside focus transition"
  );
  ok &= check(
      !shouldDeferFocusDismissToTriggerToggle(true, true, 10, 10),
      "the original opening serial is not mistaken for a second toggle"
  );
  ok &= check(shouldKeepClosingOnToggle(true, true, true), "same-panel toggle cannot revive a closing surface");
  ok &= check(!shouldKeepClosingOnToggle(true, false, true), "open same-panel toggle is still allowed to close");

  return ok ? 0 : 1;
}
