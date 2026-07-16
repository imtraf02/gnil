{
  config,
  pkgs,
  lib,
  ...
}:
let
  inherit (lib.modules) mkIf;
  inherit (lib.options) mkEnableOption mkOption;
  inherit (lib.attrsets) mapAttrs mapAttrs' attrValues;
  inherit (lib.lists) optional;

  cfg = config.programs.gnil;
  toml = pkgs.formats.toml { };
  json = pkgs.formats.json { };

  configToml =
    let
      rawConfig = toml.generate "settings.toml" cfg.settings;
    in
    if cfg.package != null && cfg.validateConfig then
      pkgs.runCommand "gnil-config" { } ''
        ${lib.getExe cfg.package} config validate ${rawConfig}
        cp ${rawConfig} $out
      ''
    else
      rawConfig;

  paletteFiles = mapAttrs (
    name: palette: json.generate "${name}-palette.json" palette
  ) cfg.customPalettes;
in
{
  options.programs.gnil = {
    enable = mkEnableOption "Whether to enable GNIL, a Niri-native Wayland shell.";

    systemd = {
      enable = mkEnableOption "Enables a systemd user service for GNIL.";

      target = mkOption {
        type = lib.types.str;
        default = "graphical-session.target";
        example = "graphical-session.target";
        description = "The systemd target for the GNIL service.";
      };
    };

    package = mkOption {
      type = lib.types.nullOr lib.types.package;
      description = "The GNIL package to use.";
    };

    validateConfig = mkOption {
      type = lib.types.bool;
      default = true;
      description = "Validate the configuration file at build time.";
    };

    settings = mkOption {
      type = toml.type;
      default = { };
      description = ''
        Configuration written to {file}`$XDG_CONFIG_HOME/gnil/settings.toml`.
      '';
      example = lib.literalExpression ''
        schema_version = 1;
        settings = {
          appearance.thickness = 6;
          appearance.corner_radius = 8;
          bar.persistent = false;
          bar.show_on_hover = false;
        };
      '';
    };

    customPalettes = mkOption {
      type = json.type;
      default = { };
      description = ''
        Custom color palettes written to {file}`$XDG_CONFIG_HOME/gnil/palettes/<name>.json`.
      '';
      example = lib.literalExpression ''
        cherry-blossom = {
          dark = {
            mPrimary = "#F2C1D4";
            mOnPrimary = "#2A1B21";
            mSecondary = "#FFD6E2";
            ...
          };
        };
      '';
    };
  };

  config = mkIf cfg.enable {
    packages = optional (cfg.package != null) cfg.package ++ [ pkgs.mpvpaper pkgs.mpv ];

    systemd.services.gnil = mkIf (cfg.systemd.enable) {
      description = "GNIL - a Niri-native Wayland shell";
      partOf = [ cfg.systemd.target ];
      after = [ cfg.systemd.target ];
      wantedBy = [ cfg.systemd.target ];
      # without this the service will have the default
      # Environment="PATH=coreutils:…", clobbering the PATH that the DE
      # imported into the user manager.
      enableDefaultPath = false;
      restartTriggers = [
        cfg.package
      ]
      ++ optional (cfg.settings != { }) configToml
      ++ attrValues paletteFiles;
      serviceConfig = {
        ExecStart = lib.getExe cfg.package;
        Restart = "on-failure";
      };
    };

    xdg.config.files = lib.mkMerge [
      (mkIf (cfg.settings != { }) {
        "gnil/settings.toml".source = configToml;
      })
      (mapAttrs' (
        name: source: lib.nameValuePair "gnil/palettes/${name}.json" { inherit source; }
      ) paletteFiles)
    ];

    assertions = [
      {
        assertion = !cfg.systemd.enable || cfg.package != null;
        message = "programs.gnil.package cannot be null when programs.gnil.systemd.enable is true";
      }
    ];
  };

  _class = "hjem";
}
