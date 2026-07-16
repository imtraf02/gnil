#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace launcher_command {

  enum class Mode { None, Catalog, Calculator, Scheme, Wallpaper, Variant };

  enum class ActionId {
    Calculator,
    Scheme,
    Wallpaper,
    Variant,
    Random,
    Light,
    Dark,
    Shutdown,
    Reboot,
    Logout,
    Lock,
    Sleep,
    Settings,
    Window,
    Emoji,
    Session,
  };

  struct Action {
    ActionId id;
    std::string_view title;
    std::string_view command;
    std::string_view glyph;
    bool dangerous = false;
    bool available = true;

    bool operator==(const Action&) const = default;
  };

  struct Route {
    Mode mode = Mode::None;
    std::string query;

    [[nodiscard]] bool active() const noexcept { return mode != Mode::None; }
    bool operator==(const Route&) const = default;
  };

  enum class ActivationKind { RewriteQuery, Confirm, Execute, Unavailable };

  struct Activation {
    ActivationKind kind = ActivationKind::Unavailable;
    std::string query;
    ActionId action = ActionId::Calculator;
  };

  class Router {
  public:
    void configure(bool dangerousActionsEnabled, bool lockscreenEnabled) noexcept;

    [[nodiscard]] Route route(std::string_view input) const;
    [[nodiscard]] std::vector<Action> catalog(std::string_view filter = {}) const;
    [[nodiscard]] std::string autocomplete(ActionId action) const;
    [[nodiscard]] Activation activate(ActionId action);

    // Escape first dismisses a pending destructive confirmation. Returns true
    // when it consumed the key and the launcher should remain open.
    bool escape() noexcept;
    [[nodiscard]] std::optional<ActionId> pendingConfirmation() const noexcept { return m_pendingConfirmation; }

  private:
    bool m_dangerousActionsEnabled = false;
    bool m_lockscreenEnabled = true;
    std::optional<ActionId> m_pendingConfirmation;
  };

  [[nodiscard]] std::string_view modeCommand(Mode mode) noexcept;

} // namespace launcher_command
