#pragma once

#include <cstdint>

[[nodiscard]] inline bool shouldDeferFocusDismissToTriggerToggle(
    bool pointerOnTrigger, bool lastInputWasPointer, std::uint32_t openingSerial, std::uint32_t currentSerial
) noexcept {
  return pointerOnTrigger && lastInputWasPointer && currentSerial != openingSerial;
}

[[nodiscard]] inline bool shouldKeepClosingOnToggle(bool open, bool closing, bool samePanel) noexcept {
  return open && closing && samePanel;
}

// Tracks the semantic focus hand-off without depending on Wayland object
// types.  A newly opened on-demand layer may briefly leave a foreign client
// focused; outside-dismiss is armed only after one of the panel's own surfaces
// has actually received focus.
class PanelKeyboardFocusTracker {
public:
  enum class Action : std::uint8_t {
    None,
    CheckAfterDispatch,
  };

  void beginOpen() noexcept {
    m_open = true;
    m_acquired = false;
  }

  void endOpen() noexcept {
    m_open = false;
    m_acquired = false;
  }

  [[nodiscard]] Action observe(bool owned, bool entered) noexcept {
    if (!m_open) {
      return Action::None;
    }
    if (entered && owned) {
      m_acquired = true;
      return Action::None;
    }
    if (m_acquired && (!entered ? owned : !owned)) {
      return Action::CheckAfterDispatch;
    }
    return Action::None;
  }

  [[nodiscard]] bool shouldClose(bool finalSurfaceOwned) const noexcept {
    return m_open && m_acquired && !finalSurfaceOwned;
  }

  // Niri can leave the previously focused application active when an
  // OnDemand layer is opened from a keyboard-inert bar. In that case there is
  // no later panel -> application focus transition to drive outside-dismiss.
  // The host may briefly request Exclusive focus, but only until this tracker
  // observes the panel's first keyboard enter.
  [[nodiscard]] bool needsBootstrap() const noexcept { return m_open && !m_acquired; }

  [[nodiscard]] bool acquired() const noexcept { return m_acquired; }

private:
  bool m_open = false;
  bool m_acquired = false;
};
