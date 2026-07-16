{
  config,
  pkgs,
  lib,
  ...
}:
let
  cfg = config.programs.gnil;
  jsonFormat = pkgs.formats.json { };
  tomlFormat = pkgs.formats.toml { };

  generateConfig =
    format: name: value:
    if lib.isString value then
      pkgs.writeText name value
    else if builtins.isPath value || lib.isStorePath value then
      value
    else
      format.generate name value;

  generateToml = generateConfig tomlFormat;
  generateJson = generateConfig jsonFormat;
in
{
  options.programs.gnil = {
    enable = lib.mkEnableOption "Whether to enable GNIL, a Niri-native Wayland shell.";

    systemd.enable = lib.mkEnableOption "Enables a systemd user service for GNIL.";

    package = lib.mkOption {
      type = lib.types.nullOr lib.types.package;
      description = "The GNIL package to use.";
    };

    validateConfig = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Validate the configuration file at build time.";
    };

    settings = lib.mkOption {
      type =
        with lib.types;
        oneOf [
          tomlFormat.type
          str
          path
        ];
      default = { };
      description = ''
        Default settings for GNIL. Can be written as:
          - A Nix attrset (converted to TOML via nixpkgs' tomlFormat)
          - A raw TOML string
          - A path to a `.toml` file

        Settings are written to `$XDG_CONFIG_HOME/gnil/settings.toml`.
      '';
      example = lib.literalExpression ''
        schema_version = 1;
        settings = {
          appearance = {
            thickness = 6;
            corner_radius = 8;
            font.sans = "Rubik";
          };
          bar = {
            persistent = false;
            show_on_hover = false;
          };
        };
      '';
    };

    customPalettes = lib.mkOption {
      type =
        with lib.types;
        oneOf [
          jsonFormat.type
          str
          path
        ];
      default = { };
      description = ''
        Custom color pallete options.

        Palette files are written under `$XDG_CONFIG_HOME/gnil/palettes/`.
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    systemd.user.services.gnil = lib.mkIf cfg.systemd.enable {
      Unit = {
        Description = "GNIL - a Niri-native Wayland shell";
        PartOf = [ config.wayland.systemd.target ];
        After = [ config.wayland.systemd.target ];
        X-Restart-Triggers =
          lib.optional (cfg.settings != { }) "${config.xdg.configFile."gnil/settings.toml".source}"
          ++ lib.mapAttrsToList (
            name: _: "${config.xdg.configFile."gnil/palettes/${name}.json".source}"
          ) cfg.customPalettes;
      };

      Service = {
        ExecStart = lib.getExe cfg.package;
        Restart = "on-failure";
      };

      Install.WantedBy = [ config.wayland.systemd.target ];
    };

    home.packages = lib.optional (cfg.package != null) cfg.package ++ [ pkgs.mpvpaper pkgs.mpv ];

    xdg = {
      configFile = lib.mkMerge [
        (lib.mkIf (cfg.settings != { }) {
          "gnil/settings.toml".source =
            let
              rawConfig = generateToml "settings.toml" cfg.settings;
            in
            if cfg.validateConfig && cfg.package != null then
              pkgs.runCommand "gnil-config" { } ''
                ${lib.getExe cfg.package} config validate ${rawConfig}
                cp ${rawConfig} $out
              ''
            else
              rawConfig;
        })
        (lib.mapAttrs' (
          name: palette:
          lib.nameValuePair "gnil/palettes/${name}.json" {
            source = generateJson "${name}-palette.json" palette;
          }
        ) cfg.customPalettes)
        (lib.mkIf (cfg.package != null) {
          "gnil/niri/gnil.kdl".source = "${cfg.package}/share/gnil/assets/niri/gnil.kdl";
        })
      ];
    };

    assertions = [
      {
        assertion = !cfg.systemd.enable || cfg.package != null;
        message = "programs.gnil.package cannot be null when programs.gnil.systemd.enable is true";
      }
    ];
  };

  _class = "homeManager";
}
