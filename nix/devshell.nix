{ pkgs }:
let
  # Nixpkgs' stable stb can lag the resize2 header required by GNIL.  Keep the
  # interactive shell aligned with the package derivation so configure never
  # succeeds in one environment and fails in the other.
  stb' = pkgs.stb.overrideAttrs (_: {
    version = "unstable-2025-10-26";
    src = pkgs.fetchFromGitHub {
      owner = "nothings";
      repo = "stb";
      rev = "f1c79c02822848a9bed4315b12c8c8f3761e1296";
      hash = "sha256-BlyXJtAI7WqXCTT3ylww8zoG0hBxaojJnQDvdQOXJPE=";
    };
  });
in
pkgs.mkShell {
  # Keep this explicit instead of inputsFrom = [ package ]. Entering the shell
  # must provision headers/tools only; GNIL itself stays a mutable Meson build.
  nativeBuildInputs = with pkgs; [
    just
    lefthook
    meson
    ninja
    pkg-config
    wayland-scanner
    llvmPackages_22.clang-tools
    llvmPackages_22.libclang
    gnugrep
    gnused
    findutils
    python3
    gdb
    watchexec
  ];

  buildInputs = with pkgs; [
    wayland
    wayland-protocols
    libGL
    libglvnd
    freetype
    fontconfig
    cairo
    pango
    harfbuzz
    libxkbcommon
    sdbus-cpp_2
    systemd
    pipewire
    wireplumber
    pam
    curl
    libwebp
    glib
    polkit
    librsvg
    libqalculate
    libxml2
    md4c
    stb'
    nlohmann_json
    tomlplusplus
    jemalloc
    mpv
    mpvpaper
  ];

  shellHook = ''
    export GNIL_ASSETS_DIR="$PWD/assets"
    echo " GNIL dev-shell | 'just --list' to see available tasks"
  '';
}
