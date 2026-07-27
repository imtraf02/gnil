set positional-arguments

mode := "debug"
build-dir := "build-" + mode
prefix := "/usr/local"
cpp-std := "c++23"

default:
    @just --list

_profile-signature m:
    #!/usr/bin/env bash
    set -euo pipefail
    case "{{m}}" in
        ui)
            linker="bfd"
            command -v mold >/dev/null 2>&1 && linker="mold"
            compiler_cache="false"
            command -v ccache >/dev/null 2>&1 && compiler_cache="true"
            echo "v3;mode=ui;buildtype=debug;debug=false;optimization=0;tests=disabled;lto=false;unity=off;sanitize=none;linker=$linker;linker_threads=1;ccache=$compiler_cache;cpp_std={{cpp-std}}"
            ;;
        debug)
            echo "v2;mode=debug;buildtype=debug;debug=true;optimization=0;tests=enabled;lto=false;unity=off;sanitize=none;cpp_std={{cpp-std}}"
            ;;
        release)
            echo "v2;mode=release;buildtype=release;debug=false;optimization=3;tests=disabled;lto=true;unity=off;sanitize=none;cpp_std={{cpp-std}}"
            ;;
        asan)
            echo "v2;mode=asan;buildtype=debug;debug=true;optimization=0;tests=disabled;lto=false;unity=off;sanitize=address,undefined;cpp_std={{cpp-std}}"
            ;;
        *)
            echo "error: unknown build profile '{{m}}' (expected ui, debug, release, or asan)" >&2
            exit 2
            ;;
    esac

configure m=mode install_prefix=prefix:
    #!/usr/bin/env bash
    set -euo pipefail
    native_args=()
    case "{{m}}" in
        ui)
            args=(
                --buildtype=debug
                -Ddebug=false
                -Doptimization=0
                -Dtests=disabled
                -Db_lto=false
                -Dunity=off
                -Db_sanitize=none
                -Dcpp_std={{cpp-std}}
            )
            if command -v mold >/dev/null 2>&1; then
                # A single mold worker avoids an intermittent "unknown file type"
                # failure on the large custom_schemes object while remaining much
                # faster than ld.bfd for UI iteration links.
                args+=("-Dcpp_link_args=-fuse-ld=mold -Wl,--threads=1")
            else
                echo "warning: mold is unavailable; the ui profile will use the slower default linker" >&2
                args+=(-Dcpp_link_args=)
            fi
            if command -v ccache >/dev/null 2>&1; then
                native_args+=(--native-file=tools/meson/ccache.ini)
            else
                echo "warning: ccache is unavailable; clean and branch-switch rebuilds will be slower" >&2
            fi
            ;;
        debug)
            args=(
                --buildtype=debug
                -Ddebug=true
                -Doptimization=0
                -Dtests=enabled
                -Db_lto=false
                -Dunity=off
                -Db_sanitize=none
                -Dcpp_std={{cpp-std}}
            )
            ;;
        release)
            args=(
                --buildtype=release
                -Ddebug=false
                -Doptimization=3
                -Dtests=disabled
                -Db_lto=true
                -Dunity=off
                -Db_sanitize=none
                -Dcpp_std={{cpp-std}}
            )
            ;;
        asan)
            args=(
                --buildtype=debug
                -Ddebug=true
                -Doptimization=0
                -Dtests=disabled
                -Db_lto=false
                -Dunity=off
                -Db_sanitize=address,undefined
                -Dcpp_std={{cpp-std}}
            )
            ;;
        *)
            echo "error: unknown build profile '{{m}}' (expected ui, debug, release, or asan)" >&2
            exit 2
            ;;
    esac

    build_dir="build-{{m}}"
    expected="$(just --quiet _profile-signature "{{m}}")"
    actual=""
    [[ -f "$build_dir/.gnil-profile" ]] && actual="$(<"$build_dir/.gnil-profile")"
    if [[ -f "$build_dir/build.ninja" && -n "$actual" && "$actual" != "$expected" ]]; then
        echo "Build profile changed; recreating $build_dir metadata and objects."
        meson setup "$build_dir" "${args[@]}" "${native_args[@]}" --prefix "{{install_prefix}}" --wipe
    elif [[ -f "$build_dir/build.ninja" ]]; then
        meson setup "$build_dir" "${args[@]}" "${native_args[@]}" --prefix "{{install_prefix}}" --reconfigure
    else
        meson setup "$build_dir" "${args[@]}" "${native_args[@]}" --prefix "{{install_prefix}}"
    fi
    printf '%s\n' "$expected" >"$build_dir/.gnil-profile"
    ln -sfn "build-{{m}}/compile_commands.json" compile_commands.json

build m=mode: (_ensure-configured m)
    meson compile -C build-{{m}}

build-app m="ui": (_ensure-configured m)
    meson compile -C build-{{m}} gnil

_ensure-configured m=mode:
    #!/usr/bin/env bash
    set -euo pipefail
    expected="$(just --quiet _profile-signature "{{m}}")"
    actual=""
    [[ -f "build-{{m}}/.gnil-profile" ]] && actual="$(<"build-{{m}}/.gnil-profile")"
    if [[ ! -f "build-{{m}}/build.ninja" || "$actual" != "$expected" ]]; then
        just configure {{m}}
    fi

run m="ui": (build-app m)
    ./build-{{m}}/gnil

# Code and asset changes get an incremental Meson/Ninja build and a process
# restart. GNIL's config watcher handles TOML edits in-process.
dev m="ui":
    #!/usr/bin/env bash
    set -euo pipefail
    if systemctl --user --quiet is-active gnil.service; then
        echo "error: gnil.service is active; stop it before running the source-tree shell" >&2
        exit 1
    fi
    mkdir -p .dev/config .dev/state
    export GNIL_ASSETS_DIR="$PWD/assets"
    export GNIL_CONFIG_HOME="$PWD/.dev/config"
    export GNIL_STATE_HOME="$PWD/.dev/state"
    export NINJA_STATUS='[%f/%t %es] '
    watchexec \
        --restart \
        --debounce 300ms \
        --stop-signal SIGINT \
        --stop-timeout 2s \
        --watch src \
        --watch assets \
        --watch meson.build \
        --watch meson_options.txt \
        --watch justfile \
        -- just run {{m}}

build-stats m="ui": (_ensure-configured m)
    #!/usr/bin/env bash
    set -euo pipefail
    echo "profile: $(<"build-{{m}}/.gnil-profile")"
    [[ -f "build-{{m}}/gnil" ]] && du -h "build-{{m}}/gnil"
    [[ -d "build-{{m}}/gnil.p" ]] && du -sh "build-{{m}}/gnil.p"
    if command -v ccache >/dev/null 2>&1; then
        ccache --show-stats
    fi

# Build (forcing tests on, even for release) and run the unit tests.
test m=mode *args: (_ensure-configured m)
    #!/usr/bin/env bash
    set -euo pipefail
    # Plain reconfigure first so build dirs predating the 'tests' option learn it,
    # then force it on (covers release, where it defaults off).
    meson setup "build-{{m}}" --reconfigure >/dev/null
    meson setup "build-{{m}}" -Dtests=enabled --reconfigure >/dev/null
    if [[ "{{m}}" != "debug" ]]; then
        trap 'rm -f "build-{{m}}/.gnil-profile"' EXIT
    fi
    meson test -C build-{{m}} {{args}}

install m:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -x "build-{{m}}/gnil" ]]; then
        echo "error: build-{{m}}/gnil is missing; run 'just build {{m}}' before installing" >&2
        exit 1
    fi
    meson install --no-rebuild -C build-{{m}}

uninstall m:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -f "build-{{m}}/build.ninja" ]]; then
        echo "error: build-{{m}} is missing or was not configured with the Ninja backend; nothing to uninstall" >&2
        exit 1
    fi
    ninja -C build-{{m}} uninstall

format:
    find src \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 clang-format -i
    find src \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 grep -ZlP '\s+$' | xargs -0 -r sed -i 's/[[:space:]]*$//'

_clang_tidy m=mode *args:
    #!/usr/bin/env bash
    set -euo pipefail
    src_root="$(realpath src)"
    # meson emits one compile_commands.json entry per (file, target); sources shared with
    # unit-test executables appear many times (core/log.cpp 14x), so clang-tidy re-lints
    # them once per entry. Dedupe to one entry per file (preferring the main app target)
    # so each file is linted once — faster, and clang-tidy's per-file progress spam
    # disappears (it only prints that when a file has multiple compile commands).
    cdb_dir="$(mktemp -d)"
    trap 'rm -rf "$cdb_dir"' EXIT
    # sort main-app (gnil.p) entries first, then keep the first entry per file
    python3 -c "import json, sys; e = sorted(json.load(open(sys.argv[1])), key=lambda x: not x.get('output', '').startswith('gnil.p/')); b = {}; [b.setdefault(x['file'], x) for x in e]; json.dump(list(b.values()), open(sys.argv[2], 'w'))" "build-{{m}}/compile_commands.json" "$cdb_dir/compile_commands.json"
    # compile_commands.json stores build-relative paths, so clang-tidy emits header
    # diagnostics as ../src/...; the header-filter must match that form (an absolute
    # ^${src_root} anchor never matches, silently dropping every header diagnostic).
    # ../src/ also excludes vendored third_party/*/src/* headers.
    run-clang-tidy -quiet -use-color -p "$cdb_dir" -j "$(nproc)" -header-filter='\.\./src/.*' {{args}} "^${src_root}/.*"

lint m=mode: (_ensure-configured m)
    just _clang_tidy {{m}} '-warnings-as-errors=*'

fix m=mode: (_ensure-configured m)
    just _clang_tidy {{m}} -fix
    just format

clean m=mode:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ -L compile_commands.json && "$(readlink compile_commands.json)" == "build-{{m}}/compile_commands.json" ]]; then
        rm -f compile_commands.json
    fi
    rm -rf build-{{m}}

rebuild m=mode: (clean m) (build m)
