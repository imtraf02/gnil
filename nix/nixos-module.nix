{
  config,
  pkgs,
  lib,
  ...
}:
let
  cfg = config.programs.gnil;
  toml = pkgs.formats.toml { };
  json = pkgs.formats.json { };
  configToml = toml.generate "gnil-settings.toml" cfg.settings;
  paletteFiles = lib.mapAttrs (
    name: palette: json.generate "${name}-palette.json" palette
  ) cfg.customPalettes;
in
{
  options.programs.gnil = {
    enable = lib.mkEnableOption "Whether to enable GNIL, a Niri-native Wayland shell.";

    package = lib.mkOption {
      type = lib.types.nullOr lib.types.package;
      default = null;
      description = "The GNIL package to install.";
    };

    settings = lib.mkOption {
      type = toml.type;
      default = { };
      description = ''
        GNIL configuration written to `/etc/xdg/gnil/settings.toml` for the
        system-managed GNIL service.
      '';
      example = lib.literalExpression ''
        {
          schema_version = 1;
          settings.wallpaper.directory = "/home/alice/Pictures/Wallpapers";
          settings.bar.persistent = false;
          settings.bar.show_on_hover = false;
        }
      '';
    };

    customPalettes = lib.mkOption {
      type = json.type;
      default = { };
      description = "Custom GNIL palettes written to /etc/xdg/gnil/palettes/.";
    };

    systemd = {
      enable = lib.mkEnableOption "Enables a systemd user service for GNIL.";

      target = lib.mkOption {
        type = lib.types.str;
        default = "graphical-session.target";
        example = "graphical-session.target";
        description = "The systemd user target for the GNIL service.";
      };
    };

    recommendedServices.enable = lib.mkEnableOption ''
      NixOS services used by GNIL integrations, including NetworkManager,
      Bluetooth, UPower, and a power profile service.
    '';
  };

  config = lib.mkIf cfg.enable (
    lib.mkMerge [
      {
        environment.systemPackages = lib.optional (cfg.package != null) cfg.package ++ [ pkgs.mpvpaper pkgs.mpv ];

        systemd.user.services.gnil = lib.mkIf cfg.systemd.enable {
          description = "GNIL - a Niri-native Wayland shell";
          partOf = [ cfg.systemd.target ];
          after = [ cfg.systemd.target ];
          wantedBy = [ cfg.systemd.target ];
          restartTriggers = [ cfg.package ];

          environment.PATH = lib.mkForce null;
          path = [ pkgs.mpvpaper pkgs.mpv ];

          serviceConfig = {
            ExecStart = lib.getExe cfg.package;
            Restart = "on-failure";
            Environment = lib.optional (cfg.settings != { } || cfg.customPalettes != { }) "GNIL_CONFIG_HOME=/etc/xdg";
          };
        };

        environment.etc = lib.mkMerge [
          (lib.mkIf (cfg.settings != { }) {
            "xdg/gnil/settings.toml".source = configToml;
          })
          (lib.mapAttrs' (
            name: source:
            lib.nameValuePair "xdg/gnil/palettes/${name}.json" { inherit source; }
          ) paletteFiles)
          (lib.mkIf (cfg.package != null) {
            "gnil/niri.kdl" = {
              source = "${cfg.package}/share/gnil/assets/niri/gnil.kdl";
              mode = "0444";
            };
          })
        ];

        assertions = [
          {
            assertion = !cfg.systemd.enable || cfg.package != null;
            message = "programs.gnil.package cannot be null when programs.gnil.systemd.enable is true";
          }
        ];
      }

      (lib.mkIf cfg.recommendedServices.enable {
        networking.networkmanager.enable = lib.mkDefault true;
        hardware.bluetooth.enable = lib.mkDefault true;
        services.upower.enable = lib.mkDefault true;
        services.power-profiles-daemon.enable = lib.mkIf (!config.services.tuned.enable) (
          lib.mkDefault true
        );
      })
    ]
  );
}
