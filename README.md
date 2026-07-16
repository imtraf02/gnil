# GNIL

> **Version 0 Beta** — GNIL is under active development and is not yet a stable release.

GNIL is a native Wayland desktop shell for [Niri](https://github.com/YaLTeR/niri), built directly with Wayland and OpenGL ES. It provides a cohesive bar, animated panels, launcher, notifications, system controls, wallpaper management, and desktop utilities without Qt or GTK.

The project takes visual and interaction inspiration from [Caelestia](https://github.com/caelestia-dots/shell) and [Noctalia](https://github.com/noctalia-dev/noctalia). GNIL retains and substantially adapts MIT-licensed renderer and service code originating from Noctalia.

> [!IMPORTANT]
> GNIL currently supports Niri only. It exits when `NIRI_SOCKET` is unavailable, so it must be started from inside a running Niri session.

## Highlights

- Native Wayland and OpenGL ES rendering
- A unified edge bar and smoothly morphing attached panels
- Application, command, emoji, calculator, window, and wallpaper launcher providers
- Notification daemon, animated notification toasts, history, and actions
- NetworkManager, Bluetooth, UPower, power profile, MPRIS, and StatusNotifier integrations
- Audio controls and visualizers through PipeWire/WirePlumber
- Image and video wallpapers, including stable palette generation from live wallpapers
- Wallpaper-driven Material colour schemes with light, dark, and automatic modes
- Clipboard history, tray menus, OSDs, session controls, lock screen, and settings UI
- Multi-monitor-aware bar, panel, wallpaper, notification, and output configuration
- NixOS, Home Manager, and Hjem modules
- `x86_64-linux` and `aarch64-linux` flake packages

The interface currently ships in English only.

## Requirements

- NixOS with flakes enabled
- A running Niri session
- A GPU and driver with EGL/OpenGL ES support
- A working user D-Bus session

The GNIL package includes its runtime assets and wraps the executable with the required `mpv` and `mpvpaper` tools. The NixOS module can also enable the recommended NetworkManager, Bluetooth, UPower, and power profile services.

GNIL bundles its icon font, but normal interface fonts are resolved through Fontconfig. Install the font selected in your configuration or choose one already available on the system.

GNIL is developed against `nixos-unstable`. Other nixpkgs revisions may work, but using the same nixpkgs input is recommended.

## Try it without installing

Run this from a terminal inside Niri:

```console
nix run github:imtraf02/gnil
```

Stop an already running GNIL service first to avoid launching two shell instances:

```console
systemctl --user stop gnil.service
```

## Install on NixOS

Add GNIL to your system flake and import its NixOS module:

```nix
{
  description = "My NixOS configuration";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    gnil = {
      url = "github:imtraf02/gnil";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { nixpkgs, gnil, ... }: {
    nixosConfigurations.my-host = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";

      modules = [
        gnil.nixosModules.default

        ({ ... }: {
          # Configure Niri separately. GNIL does not install or start the
          # compositor itself.
          programs.gnil = {
            enable = true;
            systemd.enable = true;
            recommendedServices.enable = true;
          };
        })
      ];
    };
  };
}
```

Apply the configuration:

```console
sudo nixos-rebuild switch --flake .#my-host
```

Log out and enter a Niri session, or start the user service from an existing Niri session:

```console
systemctl --user enable --now gnil.service
```

Check that it started successfully:

```console
systemctl --user status gnil.service
journalctl --user -u gnil.service -b
```

### Declarative NixOS settings

The NixOS module can generate `/etc/xdg/gnil/settings.toml`:

```nix
programs.gnil = {
  enable = true;
  systemd.enable = true;
  recommendedServices.enable = true;

  settings = {
    schema_version = 1;

    settings = {
      appearance = {
        thickness = 6.0;
        corner_radius = 8.0;
        font.sans = "Rubik";

        theme = {
          mode = "dark";
          dynamic = true;
          live_wallpaper_output = "auto";
        };
      };

      wallpaper = {
        directory = "~/Pictures/Wallpapers";
        directory_light = "";
        directory_dark = "";
      };

      bar = {
        persistent = false;
        show_on_hover = false;
      };
    };
  };
};
```

This file is system-managed and should be changed through Nix. If you prefer to edit settings from GNIL's UI and keep those changes at runtime, leave `programs.gnil.settings = { };` and use the per-user configuration file described below.

## Install with Home Manager

If Niri and your graphical session are already managed per user, the Home Manager module keeps the package, service, and configuration in the same scope:

```nix
{
  imports = [ inputs.gnil.homeModules.default ];

  programs.gnil = {
    enable = true;
    systemd.enable = true;

    settings = {
      schema_version = 1;

      settings = {
        appearance = {
          thickness = 6.0;
          corner_radius = 8.0;
          theme.dynamic = true;
          font.sans = "Rubik";
        };

        wallpaper.directory = "~/Pictures/Wallpapers";

        bar = {
          persistent = false;
          show_on_hover = false;
        };
      };
    };
  };
}
```

The module writes the settings file to `$XDG_CONFIG_HOME/gnil/settings.toml` and validates it during the Home Manager build by default. Set `programs.gnil.validateConfig = false` only when intentionally deploying an externally generated file that cannot be validated at build time.

GNIL also exposes `hjemModules.default` for Hjem users.

## Niri keybindings

Add bindings to the top-level `binds` block in your Niri configuration:

```kdl
binds {
    Mod+Space repeat=false hotkey-overlay-title="GNIL launcher" {
        spawn "gnil" "msg" "panel" "toggle" "launcher";
    }

    Mod+N repeat=false hotkey-overlay-title="GNIL notifications" {
        spawn "gnil" "msg" "panel" "toggle" "notifications";
    }

    Mod+Shift+S repeat=false hotkey-overlay-title="GNIL settings" {
        spawn "gnil" "msg" "panel" "toggle" "settings";
    }

    Mod+Escape repeat=false hotkey-overlay-title="GNIL session" {
        spawn "gnil" "msg" "panel" "toggle" "session";
    }
}
```

After changing Niri bindings, reload the compositor configuration:

```console
niri msg action load-config-file
```

## Configuration

GNIL reads one user-facing configuration document:

```text
$XDG_CONFIG_HOME/gnil/settings.toml
```

When `XDG_CONFIG_HOME` is unset, the path is `~/.config/gnil/settings.toml`. The complete Settings/Style v1 document is available in [example.toml](example.toml).

GNIL has built-in defaults, so the file is optional for the first launch. Settings changed through the UI are persisted to this document when it is user-writable.

Validate a configuration before restarting GNIL:

```console
gnil config validate ~/.config/gnil/settings.toml
```

Inspect the effective configuration:

```console
gnil config export merged
gnil config export full
```

Configuration and wallpaper changes are hot-reloaded where possible.

### Live wallpapers and dynamic colours

Video wallpapers use `mpvpaper`. They can be selected from GNIL's wallpaper UI or configured directly:

```toml
[settings.appearance.theme]
dynamic = true
live_wallpaper_output = "auto"

[settings.wallpaper.video.all]
enabled = true
path = "/home/alice/Videos/wallpaper.mp4"
mute = true
hardware_decode = true
auto_pause = true
keep_last_frame = false
mpv_options = ""
```

With `live_wallpaper_output = "auto"`, GNIL prefers a global video assignment and otherwise uses the first connected output with an active live wallpaper. A connector such as `DP-1` or `eDP-1` can be used instead.

The palette is generated from several representative frames and cached. It remains stable for that video instead of changing colour on every scene.

## Command line and IPC

GNIL exposes a local IPC socket under `$XDG_RUNTIME_DIR`, scoped to the current Wayland display.

Useful commands:

```console
# Panels
gnil msg panel toggle launcher
gnil msg panel toggle notifications
gnil msg panel toggle settings
gnil msg panel toggle wallpaper
gnil msg panel toggle network
gnil msg panel toggle audio
gnil msg panel toggle session

# Configuration
gnil config validate
gnil config export full

# Discover all IPC commands supported by the running version
gnil msg --help
```

In the launcher, `/wallpaper` opens the wallpaper provider and `/emoji` opens the emoji browser.

## Troubleshooting

### `NIRI_SOCKET is not set`

GNIL was started outside Niri, or the systemd user manager did not receive Niri's environment:

```console
echo "$NIRI_SOCKET"
systemctl --user show-environment | grep NIRI_SOCKET
```

From a terminal inside Niri, import the session environment and restart GNIL:

```console
systemctl --user import-environment NIRI_SOCKET WAYLAND_DISPLAY XDG_CURRENT_DESKTOP
systemctl --user restart gnil.service
```

### GNIL starts twice

Use either the systemd service or a compositor startup command, not both:

```console
systemctl --user status gnil.service
pgrep -a gnil
```

### A panel shortcut does nothing

Confirm that GNIL is running and ask the IPC client for the command result:

```console
gnil msg panel toggle launcher
journalctl --user -u gnil.service -b -n 100
```

### Live wallpaper does not start

Check that the configured file exists and that both runtime tools are available:

```console
command -v mpv
command -v mpvpaper
```

The official package and modules provide both tools automatically.

### Notifications are handled by another daemon

Only one process can own `org.freedesktop.Notifications`. Disable another notification daemon such as Mako or SwayNotificationCenter, or disable GNIL notifications in `settings.toml`:

```toml
[settings.notifications]
enabled = false
```

## Development

Clone the repository and enter the development shell:

```console
git clone https://github.com/imtraf02/gnil.git
cd gnil
nix develop
```

Configure, build, and run:

```console
just configure
just build
just run
```

For an incremental edit/build/restart loop:

```console
just dev
```

`just dev` uses `.dev/` for configuration and state, watches native sources and assets, rebuilds with Meson/Ninja, and restarts the source-tree binary. Stop `gnil.service` before using it.

Before submitting changes:

```console
just test
just lint
nix flake check
```

Build the Nix package directly with:

```console
nix build .#
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for architecture, coding style, and source layout notes.

## Project status

GNIL is currently at Version 0 Beta. Configuration keys, visuals, and shell behaviour may change between revisions. Bug reports should include:

- the GNIL revision or `gnil --version` output;
- the Niri version;
- relevant `journalctl --user -u gnil.service` output;
- a minimal configuration that reproduces the issue.

## License and attribution

GNIL is distributed under the [MIT License](LICENSE).

The repository retains upstream Noctalia copyright notices for inherited MIT-licensed code. Caelestia and Noctalia are credited as design and engineering references; their names do not imply endorsement or affiliation.
