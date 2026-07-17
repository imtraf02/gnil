#pragma once

namespace gnil::ipc {

  // Entry point for `gnil msg <command> [args...]`. Returns a process exit
  // code. Forwards the command to the running instance over the IPC socket.
  int runCli(int argc, char* argv[]);

} // namespace gnil::ipc
