#pragma once

/**
 * @file daemon.h
 * @brief The long-running sync process: node + discovery + scan loop + UI.
 *
 * Daemon turns a parsed `Options` into a running system: it builds a librats Node
 * (optionally with DHT/mDNS discovery), wires a SyncService with terminal-printing
 * event handlers, dials any explicit peers, and then loops — rescanning the tree
 * on an interval and feeding fresh manifests to the sessions, which do the actual
 * reconciling and transferring. Ctrl-C stops it cleanly.
 */

#include "app/cli.h"

namespace rasync {

/// Run the sync daemon to completion. Returns a process exit code.
int run_daemon(const Options& options);

/// Ask a running daemon to stop (installed as the SIGINT/SIGTERM handler target).
void request_stop();

} // namespace rasync
