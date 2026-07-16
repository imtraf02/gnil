#include "config/config_types.h"
#include "scripting/plugin_source_paths.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::fprintf(stderr, "plugin_source_paths_test: %s\n", message);
    }
    return condition;
  }

  bool expectPath(const std::filesystem::path& actual, const std::filesystem::path& expected, const char* message) {
    if (actual != expected) {
      std::fprintf(
          stderr, "plugin_source_paths_test: %s\n  actual:   %s\n  expected: %s\n", message, actual.string().c_str(),
          expected.string().c_str()
      );
      return false;
    }
    return true;
  }

} // namespace

int main() {
  ::setenv("GNIL_STATE_HOME", "/tmp/gnil-path-test-state", 1);
  ::setenv("GNIL_DATA_HOME", "/tmp/gnil-path-test-data", 1);

  const PluginSourceConfig gitSource{
      .kind = PluginSourceKind::Git,
      .name = "official",
      .location = "https://example.invalid/plugins.git",
      .autoUpdate = false,
  };
  const PluginSourceConfig pathSource{
      .kind = PluginSourceKind::Path,
      .name = "dev",
      .location = "~/dev/noctalia-plugins",
      .autoUpdate = false,
  };

  const std::filesystem::path stateRoot = "/tmp/gnil-path-test-state/gnil";
  bool ok = true;
  ok = expectPath(
           scripting::plugin_paths::localSourceRoot(), "/tmp/gnil-path-test-data/gnil/plugins",
           "local source root"
       )
      && ok;
  ok = expectPath(
           scripting::plugin_paths::sourceStorageRoot(gitSource), stateRoot / "plugins/sources/official",
           "git source storage root"
       )
      && ok;
  ok = expectPath(
           scripting::plugin_paths::gitRepoRoot(gitSource), stateRoot / "plugins/sources/official/repo", "git repo root"
       )
      && ok;
  ok = expectPath(
           scripting::plugin_paths::gitMaterializedRoot(gitSource), stateRoot / "plugins/materialized/official",
           "git materialized root"
       )
      && ok;
  ok = expectPath(
           scripting::plugin_paths::registryRoot(gitSource), stateRoot / "plugins/materialized/official",
           "git registry root"
       )
      && ok;
  ok = expect(isDefaultPluginSourceName("official"), "official source should be protected as a default source") && ok;
  ok = expect(isDefaultPluginSourceName("community"), "community source should be protected as a default source") && ok;
  ok = expect(!isDefaultPluginSourceName("dev"), "custom source should not be protected as a default source") && ok;
  ok = expect(
           scripting::plugin_paths::registryRoot(pathSource).string().ends_with("/dev/noctalia-plugins"),
           "path source registry root expands user path"
       )
      && ok;
  ok = expect(
           scripting::plugin_paths::pathIsInside("/tmp/noctalia/a/b", "/tmp/noctalia"),
           "child path should be inside parent"
       )
      && ok;
  ok = expect(
           !scripting::plugin_paths::pathIsInside("/tmp/noctalia", "/tmp/noctalia"),
           "parent path must not count as inside itself"
       )
      && ok;
  ok = expect(
           !scripting::plugin_paths::pathIsInside("/tmp/noctalia-other/a", "/tmp/noctalia"),
           "sibling prefix must not count as inside parent"
       )
      && ok;

  return ok ? 0 : 1;
}
