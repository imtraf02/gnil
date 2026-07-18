#include "launcher/command_router.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace launcher_command {
  namespace {

    constexpr std::array<Action, 16> kActions = {{
        {ActionId::Calculator, "Calculator", "calc", "calculate"},
        {ActionId::Scheme, "Scheme", "scheme", "palette"},
        {ActionId::Wallpaper, "Wallpaper", "wallpaper", "wallpaper"},
        {ActionId::Variant, "Variant", "variant", "colors"},
        {ActionId::Random, "Random", "random", "shuffle"},
        {ActionId::Light, "Light", "light", "light_mode"},
        {ActionId::Dark, "Dark", "dark", "dark_mode"},
        {ActionId::Shutdown, "Shutdown", "shutdown", "power_settings_new", true},
        {ActionId::Reboot, "Reboot", "reboot", "restart_alt", true},
        {ActionId::Logout, "Logout", "logout", "logout", true},
        {ActionId::Lock, "Lock", "lock", "lock"},
        {ActionId::Sleep, "Sleep", "sleep", "bedtime"},
        {ActionId::Settings, "Settings", "settings", "settings"},
        {ActionId::Window, "Window", "win", "select_window"},
        {ActionId::Emoji, "Emoji", "emoji", "emoji_emotions"},
        {ActionId::Session, "Session", "session", "power"},
    }};

    std::string lower(std::string_view text) {
      std::string result(text);
      std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return result;
    }

    std::string_view trimLeft(std::string_view text) {
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
      }
      return text;
    }

    const Action* find(ActionId id) {
      const auto it = std::ranges::find(kActions, id, &Action::id);
      return it == kActions.end() ? nullptr : &*it;
    }

    bool isModeAction(ActionId id) {
      return id == ActionId::Calculator || id == ActionId::Scheme || id == ActionId::Wallpaper
          || id == ActionId::Variant;
    }

  } // namespace

  void Router::configure(bool dangerousActionsEnabled, bool lockscreenEnabled) noexcept {
    m_dangerousActionsEnabled = dangerousActionsEnabled;
    m_lockscreenEnabled = lockscreenEnabled;
    if (!m_dangerousActionsEnabled && m_pendingConfirmation.has_value()) {
      m_pendingConfirmation.reset();
    }
  }

  Route Router::route(std::string_view input) const {
    if (input.empty() || input.front() != '/') {
      return {};
    }

    input.remove_prefix(1);
    struct PrefixMode {
      std::string_view command;
      Mode mode;
    };
    static constexpr PrefixMode kModes[] = {
        {"wallpaper", Mode::Wallpaper},
        {"variant", Mode::Variant},
        {"scheme", Mode::Scheme},
        {"calc", Mode::Calculator},
    };
    for (const auto& entry : kModes) {
      if (input.size() > entry.command.size() && input.starts_with(entry.command)
          && std::isspace(static_cast<unsigned char>(input[entry.command.size()]))) {
        return {.mode = entry.mode, .query = std::string(trimLeft(input.substr(entry.command.size() + 1)))};
      }
    }
    // These commands deliberately hand back to GNIL's existing prefixed
    // providers after catalog autocomplete has inserted the separating space.
    static constexpr std::string_view kProviderCommands[] = {"win", "emoji", "session"};
    for (const auto command : kProviderCommands) {
      if (input.size() > command.size() && input.starts_with(command)
          && std::isspace(static_cast<unsigned char>(input[command.size()]))) {
        return {};
      }
    }
    return {.mode = Mode::Catalog, .query = std::string(input)};
  }

  std::vector<Action> Router::catalog(std::string_view filter) const {
    const std::string needle = lower(filter);
    std::vector<Action> result;
    result.reserve(kActions.size());
    for (Action action : kActions) {
      const std::string haystack = lower(std::string(action.title) + " " + std::string(action.command));
      if (!needle.empty() && !haystack.contains(needle)) {
        continue;
      }
      if (action.dangerous && !m_dangerousActionsEnabled) {
        action.available = false;
      }
      if (action.id == ActionId::Lock && !m_lockscreenEnabled) {
        action.available = false;
      }
      result.push_back(action);
    }
    return result;
  }

  std::string Router::autocomplete(ActionId action) const {
    const Action* entry = find(action);
    if (entry == nullptr) {
      return "/";
    }
    std::string result = "/" + std::string(entry->command);
    if (isModeAction(action) || action == ActionId::Window || action == ActionId::Emoji || action == ActionId::Session) {
      result.push_back(' ');
    }
    return result;
  }

  Activation Router::activate(ActionId action) {
    const Action* entry = find(action);
    if (entry == nullptr || (entry->dangerous && !m_dangerousActionsEnabled)
        || (action == ActionId::Lock && !m_lockscreenEnabled)) {
      return {.kind = ActivationKind::Unavailable, .action = action};
    }
    if (isModeAction(action) || action == ActionId::Window || action == ActionId::Emoji || action == ActionId::Session) {
      m_pendingConfirmation.reset();
      return {.kind = ActivationKind::RewriteQuery, .query = autocomplete(action), .action = action};
    }
    if (entry->dangerous && m_pendingConfirmation != action) {
      m_pendingConfirmation = action;
      return {.kind = ActivationKind::Confirm, .action = action};
    }
    m_pendingConfirmation.reset();
    return {.kind = ActivationKind::Execute, .action = action};
  }

  bool Router::escape() noexcept {
    if (!m_pendingConfirmation.has_value()) {
      return false;
    }
    m_pendingConfirmation.reset();
    return true;
  }

  std::string_view modeCommand(Mode mode) noexcept {
    switch (mode) {
    case Mode::Calculator:
      return "calc";
    case Mode::Scheme:
      return "scheme";
    case Mode::Wallpaper:
      return "wallpaper";
    case Mode::Variant:
      return "variant";
    case Mode::None:
    case Mode::Catalog:
      return {};
    }
    return {};
  }

} // namespace launcher_command
