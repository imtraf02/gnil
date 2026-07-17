#pragma once

namespace gnil::config {

  // Entry point for `gnil config <command> [options]`. Returns a process
  // exit code. Pure CLI helper; does not start Application or mutate live config.
  int runCli(int argc, char* argv[]);

} // namespace gnil::config
