#include "config/settings_document.h"

#include "config/config_migrations.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <utility>
#include <vector>

namespace gnil::settings_document {
  namespace {
    constexpr std::string_view kDefaults = R"toml(
schema_version = 1

[settings.general]
avatar_image = ""

[settings.appearance]
thickness = 6.0
corner_radius = 8.0

[settings.appearance.theme]
mode = "light"
light = "Ling Light"
dark = "Ling Dark"
dynamic = false
matugen_type = "scheme-tonal-spot"
live_wallpaper_output = "auto"

[settings.appearance.font]
sans = "Rubik"
mono = "CaskaydiaCove NF"
clock = "Rubik"

[settings.bar]
persistent = false
show_on_hover = false
monitors = []
top = ["launcher", "workspaces", "media"]
center = ["clock"]
bottom = ["tray", "volume", "brightness", "network", "battery"]

[settings.bar.workspaces]
shown = 5
show_number = true
show_icons = true

[settings.bar.tray]
blacklist = []
favourites = []
colourize = false

[settings.delay]
pill = 200

[settings.brightness]
step = 5
enforce_minimum = true
enable_ddc_support = false

[settings.wallpaper]
enabled = true
overview_enabled = true
directory = ""
directory_light = ""
directory_dark = ""
per_monitor_directories = false
recursive = false
set_for_all_monitors = true
default = ""
fill_mode = "crop"
fill_color = "#000000"
monitors = []
transition_duration = 500.0
edge_smoothness = 0.05

[settings.wallpaper.video.all]
enabled = true
path = ""
mute = true
hardware_decode = true
auto_pause = true
keep_last_frame = false
mpv_options = ""

[settings.network]
wifi_enabled = true

[settings.audio]
step = 5
overdrive = false
cava = true
visualizer = true
mpris_blacklist = []
preferred_player = ""

[settings.launcher]
max_shown = 7
special_prefix = "@"
action_prefix = ">"
hidden_apps = []
max_wallpapers = 5

[settings.session]
gif = ""
drag_threshold = 30
vim_keybinds = false

[settings.calendar]
show_events_card = true

[settings.lock]
recolour_logo = false
fingerprint = true
max_fingerprint_tries = 3

[settings.notifications]
enabled = true
show_app_name = true
show_actions = true
collapse_on_dismiss = true
clear_threshold = 0.3
expand_threshold = 20
group_preview_count = 3
open_expanded = false

[settings.system_monitor]
enabled = true
cpu_poll_seconds = 2.0
gpu_poll_seconds = 0.0
memory_poll_seconds = 2.0
network_poll_seconds = 3.0
disk_poll_seconds = 10.0
external_monitor = ""

[style.rounding]
small = 9.0
normal = 17.0
large = 25.0
full = 1000.0

[style.spacing]
small = 7.0
smaller = 10.0
normal = 12.0
larger = 15.0
large = 20.0

[style.padding]
small = 5.0
smaller = 7.0
normal = 10.0
larger = 12.0
large = 15.0

[style.font]
small = 11.0
smaller = 12.0
normal = 13.0
larger = 15.0
large = 18.0
extra_large = 28.0

[style.animation]
small = 200
normal = 400
large = 600
extra_large = 1000
expressive_fast_spatial = 350
expressive_default_spatial = 500
expressive_effects = 200
settings_save_debounce = 1000

[style.widget]
size = 33.0
slider_width = 200.0

[style.bar]
inner_height = 33.0
tray_menu_width = 300.0

[style.shadow]
opacity = 0.85
blur = 1.0
blur_max = 22.0
horizontal_offset = 2.0
vertical_offset = 3.0

[style.launcher]
item_width = 600.0
item_height = 52.0
wallpaper_width = 280.0
wallpaper_height = 200.0

[style.lock]
height_mult = 0.7
ratio = 1.7777777778
center_width = 560.0

[style.notifications]
width = 400.0
image = 40.0
badge = 20.0

[style.session]
button = 80.0

[style.settings]
width = 1280.0
height = 720.0
)toml";

    using Path = std::initializer_list<std::string_view>;

    const toml::node* nodeAt(const toml::table& root, Path path) {
      const toml::table* table = &root;
      const toml::node* node = nullptr;
      std::size_t index = 0;
      for (const auto part : path) {
        node = table->get(part);
        if (node == nullptr) {
          return nullptr;
        }
        if (++index < path.size()) {
          table = node->as_table();
          if (table == nullptr) {
            return nullptr;
          }
        }
      }
      return node;
    }

    toml::table* ensureTable(toml::table& root, Path path) {
      toml::table* table = &root;
      for (const auto part : path) {
        if (auto* child = table->get_as<toml::table>(part)) {
          table = child;
          continue;
        }
        auto [it, _] = table->insert_or_assign(part, toml::table{});
        table = it->second.as_table();
      }
      return table;
    }

    void setNode(toml::table& root, Path parent, std::string_view key, const toml::node& value) {
      ensureTable(root, parent)->insert_or_assign(key, value);
    }

    void copyNode(
        const toml::table& source, Path sourcePath, toml::table& destination, Path destinationParent,
        std::string_view destinationKey
    ) {
      if (const auto* value = nodeAt(source, sourcePath)) {
        setNode(destination, destinationParent, destinationKey, *value);
      }
    }

    template <typename T> std::optional<T> valueAt(const toml::table& root, Path path) {
      if (const auto* value = nodeAt(root, path)) {
        return value->value<T>();
      }
      return std::nullopt;
    }

    void copyCommonToRuntime(const toml::table& document, toml::table& runtime) {
      copyNode(document, {"settings", "general", "avatar_image"}, runtime, {"shell"}, "avatar_path");
      copyNode(document, {"settings", "appearance", "font", "sans"}, runtime, {"shell"}, "font_family");
      copyNode(document, {"settings", "appearance", "thickness"}, runtime, {"shell", "chrome"}, "frame_thickness");
      copyNode(document, {"settings", "appearance", "corner_radius"}, runtime, {"shell", "chrome"}, "rounding");

      copyNode(document, {"settings", "wallpaper", "enabled"}, runtime, {"wallpaper"}, "enabled");
      copyNode(document, {"settings", "wallpaper", "directory"}, runtime, {"wallpaper"}, "directory");
      copyNode(document, {"settings", "wallpaper", "directory_light"}, runtime, {"wallpaper"}, "directory_light");
      copyNode(document, {"settings", "wallpaper", "directory_dark"}, runtime, {"wallpaper"}, "directory_dark");
      copyNode(document, {"settings", "wallpaper", "per_monitor_directories"}, runtime, {"wallpaper"}, "per_monitor_directories");
      copyNode(document, {"settings", "wallpaper", "fill_mode"}, runtime, {"wallpaper"}, "fill_mode");
      copyNode(document, {"settings", "wallpaper", "fill_color"}, runtime, {"wallpaper"}, "fill_color");
      copyNode(document, {"settings", "wallpaper", "transition_duration"}, runtime, {"wallpaper"}, "transition_duration");
      copyNode(document, {"settings", "wallpaper", "edge_smoothness"}, runtime, {"wallpaper"}, "edge_smoothness");
      copyNode(document, {"settings", "wallpaper", "recursive"}, runtime, {"wallpaper", "automation"}, "recursive");
      if (const auto path = valueAt<std::string>(document, {"settings", "wallpaper", "default"}); path && !path->empty()) {
        ensureTable(runtime, {"wallpaper", "default"})->insert_or_assign("path", *path);
      }
      if (const auto path = valueAt<std::string>(document, {"settings", "wallpaper", "last"}); path && !path->empty()) {
        ensureTable(runtime, {"wallpaper", "last"})->insert_or_assign("path", *path);
      }
      copyNode(document, {"settings", "wallpaper", "assignments"}, runtime, {"wallpaper"}, "monitors");
      copyNode(document, {"settings", "wallpaper", "favourites"}, runtime, {"wallpaper"}, "favorite");
      copyNode(document, {"settings", "wallpaper", "video"}, runtime, {"wallpaper"}, "video");

      copyNode(document, {"settings", "brightness", "enable_ddc_support"}, runtime, {"brightness"}, "enable_ddcutil");
      if (const auto enforce = valueAt<bool>(document, {"settings", "brightness", "enforce_minimum"})) {
        ensureTable(runtime, {"brightness"})->insert_or_assign("minimum_brightness", *enforce ? 0.01 : 0.0);
      }
      copyNode(document, {"settings", "audio", "overdrive"}, runtime, {"audio"}, "enable_overdrive");
      copyNode(document, {"settings", "audio", "mpris_blacklist"}, runtime, {"shell", "mpris"}, "blacklist");
      copyNode(document, {"settings", "launcher", "max_shown"}, runtime, {"shell", "launcher"}, "max_shown");
      copyNode(document, {"settings", "launcher", "max_wallpapers"}, runtime, {"shell", "launcher"}, "max_wallpapers");
      copyNode(document, {"settings", "launcher", "hidden_apps"}, runtime, {"shell", "launcher"}, "hidden_apps");
      copyNode(document, {"settings", "session", "drag_threshold"}, runtime, {"shell", "launcher"}, "drag_threshold");
      copyNode(document, {"settings", "session", "vim_keybinds"}, runtime, {"shell", "launcher"}, "vim_keybinds");
      copyNode(document, {"settings", "calendar", "show_events_card"}, runtime, {"calendar"}, "show_events_card");
      copyNode(document, {"settings", "lock", "fingerprint"}, runtime, {"lockscreen"}, "fingerprint");

      if (const auto* panels = nodeAt(document, {"settings", "panels"}); panels != nullptr) {
        if (const auto* panelTable = panels->as_table(); panelTable != nullptr) {
          auto* runtimeSizes = ensureTable(runtime, {"shell", "panel", "size"});
          for (const auto& [id, node] : *panelTable) {
            const auto* configured = node.as_table();
            if (configured == nullptr) continue;
            toml::table size;
            if (const auto width = (*configured)["width"].value<std::int64_t>(); width.has_value()) {
              size.insert_or_assign("width", *width);
            }
            if (!size.empty()) {
              runtimeSizes->insert_or_assign(id, std::move(size));
            }
          }
        }
      }

      copyNode(document, {"settings", "notifications", "enabled"}, runtime, {"notification"}, "enable_daemon");
      copyNode(document, {"settings", "notifications", "show_app_name"}, runtime, {"notification"}, "show_app_name");
      copyNode(document, {"settings", "notifications", "show_actions"}, runtime, {"notification"}, "show_actions");
      copyNode(document, {"settings", "notifications", "collapse_on_dismiss"}, runtime, {"notification"}, "collapse_on_dismiss");
      copyNode(document, {"settings", "notifications", "clear_threshold"}, runtime, {"notification"}, "clear_threshold");
      copyNode(document, {"settings", "notifications", "expand_threshold"}, runtime, {"notification"}, "expand_threshold");
      copyNode(document, {"settings", "notifications", "group_preview_count"}, runtime, {"notification"}, "group_preview_count");
      copyNode(document, {"settings", "notifications", "open_expanded"}, runtime, {"notification"}, "open_expanded");

      copyNode(document, {"settings", "system_monitor", "enabled"}, runtime, {"system", "monitor"}, "enabled");
      copyNode(document, {"settings", "system_monitor", "cpu_poll_seconds"}, runtime, {"system", "monitor"}, "cpu_poll_seconds");
      copyNode(document, {"settings", "system_monitor", "gpu_poll_seconds"}, runtime, {"system", "monitor"}, "gpu_poll_seconds");
      copyNode(document, {"settings", "system_monitor", "memory_poll_seconds"}, runtime, {"system", "monitor"}, "memory_poll_seconds");
      copyNode(document, {"settings", "system_monitor", "network_poll_seconds"}, runtime, {"system", "monitor"}, "network_poll_seconds");
      copyNode(document, {"settings", "system_monitor", "disk_poll_seconds"}, runtime, {"system", "monitor"}, "disk_poll_seconds");
    }

    void copyCommonFromRuntime(const toml::table& runtime, toml::table& document) {
      copyNode(runtime, {"shell", "avatar_path"}, document, {"settings", "general"}, "avatar_image");
      copyNode(runtime, {"shell", "font_family"}, document, {"settings", "appearance", "font"}, "sans");
      copyNode(runtime, {"shell", "chrome", "frame_thickness"}, document, {"settings", "appearance"}, "thickness");
      copyNode(runtime, {"shell", "chrome", "rounding"}, document, {"settings", "appearance"}, "corner_radius");

      copyNode(runtime, {"wallpaper", "enabled"}, document, {"settings", "wallpaper"}, "enabled");
      copyNode(runtime, {"wallpaper", "directory"}, document, {"settings", "wallpaper"}, "directory");
      copyNode(runtime, {"wallpaper", "directory_light"}, document, {"settings", "wallpaper"}, "directory_light");
      copyNode(runtime, {"wallpaper", "directory_dark"}, document, {"settings", "wallpaper"}, "directory_dark");
      copyNode(runtime, {"wallpaper", "per_monitor_directories"}, document, {"settings", "wallpaper"}, "per_monitor_directories");
      copyNode(runtime, {"wallpaper", "fill_mode"}, document, {"settings", "wallpaper"}, "fill_mode");
      copyNode(runtime, {"wallpaper", "fill_color"}, document, {"settings", "wallpaper"}, "fill_color");
      copyNode(runtime, {"wallpaper", "transition_duration"}, document, {"settings", "wallpaper"}, "transition_duration");
      copyNode(runtime, {"wallpaper", "edge_smoothness"}, document, {"settings", "wallpaper"}, "edge_smoothness");
      copyNode(runtime, {"wallpaper", "automation", "recursive"}, document, {"settings", "wallpaper"}, "recursive");

      copyNode(runtime, {"brightness", "enable_ddcutil"}, document, {"settings", "brightness"}, "enable_ddc_support");
      if (const auto minimum = valueAt<double>(runtime, {"brightness", "minimum_brightness"})) {
        ensureTable(document, {"settings", "brightness"})->insert_or_assign("enforce_minimum", *minimum > 0.0);
      }
      copyNode(runtime, {"audio", "enable_overdrive"}, document, {"settings", "audio"}, "overdrive");
      copyNode(runtime, {"shell", "mpris", "blacklist"}, document, {"settings", "audio"}, "mpris_blacklist");
      copyNode(runtime, {"shell", "launcher", "max_shown"}, document, {"settings", "launcher"}, "max_shown");
      copyNode(runtime, {"shell", "launcher", "max_wallpapers"}, document, {"settings", "launcher"}, "max_wallpapers");
      copyNode(runtime, {"shell", "launcher", "hidden_apps"}, document, {"settings", "launcher"}, "hidden_apps");
      copyNode(runtime, {"shell", "launcher", "drag_threshold"}, document, {"settings", "session"}, "drag_threshold");
      copyNode(runtime, {"shell", "launcher", "vim_keybinds"}, document, {"settings", "session"}, "vim_keybinds");
      copyNode(runtime, {"calendar", "show_events_card"}, document, {"settings", "calendar"}, "show_events_card");
      copyNode(runtime, {"lockscreen", "fingerprint"}, document, {"settings", "lock"}, "fingerprint");

      auto* settings = ensureTable(document, {"settings"});
      settings->erase("panels");
      const auto* runtimeSizes = nodeAt(runtime, {"shell", "panel", "size"});
      if (runtimeSizes != nullptr) {
        if (const auto* sizes = runtimeSizes->as_table(); sizes != nullptr) {
          toml::table panels;
          for (const auto& [id, node] : *sizes) {
            const auto* configured = node.as_table();
            if (configured == nullptr) continue;
            toml::table size;
            if (const auto width = (*configured)["width"].value<std::int64_t>(); width.has_value()) {
              size.insert_or_assign("width", *width);
            }
            if (!size.empty()) {
              panels.insert_or_assign(id, std::move(size));
            }
          }
          if (!panels.empty()) {
            settings->insert_or_assign("panels", std::move(panels));
          }
        }
      }

      copyNode(runtime, {"notification", "enable_daemon"}, document, {"settings", "notifications"}, "enabled");
      copyNode(runtime, {"notification", "show_app_name"}, document, {"settings", "notifications"}, "show_app_name");
      copyNode(runtime, {"notification", "show_actions"}, document, {"settings", "notifications"}, "show_actions");
      copyNode(runtime, {"notification", "collapse_on_dismiss"}, document, {"settings", "notifications"}, "collapse_on_dismiss");
      copyNode(runtime, {"notification", "clear_threshold"}, document, {"settings", "notifications"}, "clear_threshold");
      copyNode(runtime, {"notification", "expand_threshold"}, document, {"settings", "notifications"}, "expand_threshold");
      copyNode(runtime, {"notification", "group_preview_count"}, document, {"settings", "notifications"}, "group_preview_count");
      copyNode(runtime, {"notification", "open_expanded"}, document, {"settings", "notifications"}, "open_expanded");

      copyNode(runtime, {"system", "monitor", "enabled"}, document, {"settings", "system_monitor"}, "enabled");
      copyNode(runtime, {"system", "monitor", "cpu_poll_seconds"}, document, {"settings", "system_monitor"}, "cpu_poll_seconds");
      copyNode(runtime, {"system", "monitor", "gpu_poll_seconds"}, document, {"settings", "system_monitor"}, "gpu_poll_seconds");
      copyNode(runtime, {"system", "monitor", "memory_poll_seconds"}, document, {"settings", "system_monitor"}, "memory_poll_seconds");
      copyNode(runtime, {"system", "monitor", "network_poll_seconds"}, document, {"settings", "system_monitor"}, "network_poll_seconds");
      copyNode(runtime, {"system", "monitor", "disk_poll_seconds"}, document, {"settings", "system_monitor"}, "disk_poll_seconds");
    }

    std::vector<std::string> stringArrayAt(const toml::table& root, Path path) {
      std::vector<std::string> result;
      const auto* node = nodeAt(root, path);
      const auto* array = node != nullptr ? node->as_array() : nullptr;
      if (array == nullptr) {
        return result;
      }
      for (const auto& item : *array) {
        if (auto value = item.value<std::string>()) {
          result.push_back(std::move(*value));
        } else if (const auto* table = item.as_table()) {
          if (auto id = (*table)["id"].value<std::string>()) {
            result.push_back(std::move(*id));
          }
        }
      }
      return result;
    }

    toml::array stringArray(const std::vector<std::string>& values) {
      toml::array out;
      for (const auto& value : values) {
        out.push_back(value);
      }
      return out;
    }
  } // namespace

  toml::table defaults() {
    return toml::parse(kDefaults);
  }

  std::optional<toml::table> toRuntimeOverrides(const toml::table& document, std::string* error) {
    const auto version = document["schema_version"].value<std::int64_t>();
    if (!version.has_value() || *version != kSchemaVersion) {
      if (error != nullptr) {
        *error = "settings.toml: schema_version must be 1";
      }
      return std::nullopt;
    }

    toml::table merged = defaults();
    // Preserve forward-compatible keys while applying defaults for missing fields.
    const auto merge = [](auto&& self, toml::table& base, const toml::table& overlay) -> void {
      for (const auto& [key, value] : overlay) {
        if (const auto* overlayTable = value.as_table()) {
          if (auto* baseTable = base.get_as<toml::table>(key)) {
            self(self, *baseTable, *overlayTable);
            continue;
          }
        }
        base.insert_or_assign(key, value);
      }
    };
    merge(merge, merged, document);

    toml::table runtime;
    runtime.insert_or_assign(
        gnil::config::kConfigVersionKey, static_cast<std::int64_t>(gnil::config::currentConfigVersion())
    );
    copyCommonToRuntime(merged, runtime);

    auto* bar = ensureTable(runtime, {"bar", "default"});
    const bool persistent = valueAt<bool>(merged, {"settings", "bar", "persistent"}).value_or(false);
    bar->insert_or_assign("auto_hide", !persistent);
    bar->insert_or_assign(
        "show_on_hover", valueAt<bool>(merged, {"settings", "bar", "show_on_hover"}).value_or(false)
    );
    const double frame = valueAt<double>(merged, {"settings", "appearance", "thickness"}).value_or(6.0);
    const double inner = valueAt<double>(merged, {"style", "bar", "inner_height"}).value_or(33.0);
    const double padding = valueAt<double>(merged, {"style", "padding", "small"}).value_or(5.0);
    bar->insert_or_assign(
        "thickness", static_cast<std::int64_t>(std::lround(inner + 2.0 * std::max(frame, padding)))
    );
    bar->insert_or_assign("auto_hide_collapsed_thickness", static_cast<std::int64_t>(std::lround(frame)));
    bar->insert_or_assign("padding", static_cast<std::int64_t>(std::lround(padding)));
    bar->insert_or_assign(
        "widget_spacing",
        static_cast<std::int64_t>(
            std::lround(valueAt<double>(merged, {"style", "spacing", "small"}).value_or(7.0))
        )
    );
    bar->insert_or_assign("start", stringArray(stringArrayAt(merged, {"settings", "bar", "top"})));
    bar->insert_or_assign("center", stringArray(stringArrayAt(merged, {"settings", "bar", "center"})));
    bar->insert_or_assign("end", stringArray(stringArrayAt(merged, {"settings", "bar", "bottom"})));

    const auto mode = valueAt<std::string>(merged, {"settings", "appearance", "theme", "mode"}).value_or("dark");
    auto* theme = ensureTable(runtime, {"theme"});
    theme->insert_or_assign("mode", mode);
    theme->insert_or_assign(
        "source", valueAt<bool>(merged, {"settings", "appearance", "theme", "dynamic"}).value_or(false)
            ? "wallpaper"
            : "builtin"
    );
    theme->insert_or_assign(
        "builtin",
        valueAt<std::string>(merged, {"settings", "appearance", "theme", mode == "light" ? "light" : "dark"})
            .value_or(mode == "light" ? "Ling Light" : "Ling Dark")
    );
    theme->insert_or_assign(
        "live_wallpaper_output",
        valueAt<std::string>(merged, {"settings", "appearance", "theme", "live_wallpaper_output"}).value_or("auto")
    );

    if (error != nullptr) {
      error->clear();
    }
    return runtime;
  }

  void syncFromRuntimeOverrides(toml::table& document, const toml::table& overrides) {
    if (document.empty()) {
      document = defaults();
    }
    document.insert_or_assign("schema_version", kSchemaVersion);
    copyCommonFromRuntime(overrides, document);

    if (const auto autoHide = valueAt<bool>(overrides, {"bar", "default", "auto_hide"})) {
      ensureTable(document, {"settings", "bar"})->insert_or_assign("persistent", !*autoHide);
    }
    copyNode(overrides, {"bar", "default", "show_on_hover"}, document, {"settings", "bar"}, "show_on_hover");
    copyNode(overrides, {"bar", "default", "start"}, document, {"settings", "bar"}, "top");
    copyNode(overrides, {"bar", "default", "center"}, document, {"settings", "bar"}, "center");
    copyNode(overrides, {"bar", "default", "end"}, document, {"settings", "bar"}, "bottom");
    copyNode(overrides, {"bar", "default", "padding"}, document, {"style", "padding"}, "small");
    copyNode(overrides, {"bar", "default", "widget_spacing"}, document, {"style", "spacing"}, "small");

    if (const auto* wallpaper = overrides.get_as<toml::table>("wallpaper")) {
      // These are app-owned wallpaper selections, not appearance side effects.
      if (const auto* table = wallpaper->get_as<toml::table>("default")) {
        if (const auto path = (*table)["path"].value<std::string>()) {
          ensureTable(document, {"settings", "wallpaper"})->insert_or_assign("default", *path);
        }
      }
      if (const auto* table = wallpaper->get_as<toml::table>("last")) {
        if (const auto path = (*table)["path"].value<std::string>()) {
          ensureTable(document, {"settings", "wallpaper"})->insert_or_assign("last", *path);
        }
      }
      if (const auto* value = wallpaper->get("monitors")) {
        setNode(document, {"settings", "wallpaper"}, "assignments", *value);
      }
      if (const auto* value = wallpaper->get("favorite")) {
        setNode(document, {"settings", "wallpaper"}, "favourites", *value);
      }
      if (const auto* value = wallpaper->get("video")) {
        setNode(document, {"settings", "wallpaper"}, "video", *value);
      }
    }

    if (const auto mode = valueAt<std::string>(overrides, {"theme", "mode"})) {
      ensureTable(document, {"settings", "appearance", "theme"})->insert_or_assign("mode", *mode);
    }
    if (const auto source = valueAt<std::string>(overrides, {"theme", "source"})) {
      ensureTable(document, {"settings", "appearance", "theme"})->insert_or_assign("dynamic", *source == "wallpaper");
    }
    if (const auto builtin = valueAt<std::string>(overrides, {"theme", "builtin"})) {
      const auto mode = valueAt<std::string>(document, {"settings", "appearance", "theme", "mode"}).value_or("dark");
      ensureTable(document, {"settings", "appearance", "theme"})
          ->insert_or_assign(mode == "light" ? "light" : "dark", *builtin);
    }
    copyNode(
        overrides, {"theme", "live_wallpaper_output"}, document,
        {"settings", "appearance", "theme"}, "live_wallpaper_output"
    );
  }

} // namespace gnil::settings_document
