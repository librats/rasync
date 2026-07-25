#pragma once

/**
 * @file cli.h
 * @brief Command-line parsing for rasync.
 *
 * Parses argv into an `Options` struct and produces the `--help` text. Kept free
 * of any networking so it can be unit-tested on its own.
 */

#include <string>
#include <vector>

#include "core/diff.h"  // SyncMode, ConflictPolicy

namespace rasync {

struct Options {
    std::string              directory;              ///< the tree to keep in sync (required)
    uint16_t                 port = 0;               ///< listen port (0 = ephemeral)
    std::vector<std::string> peers;                  ///< explicit "host:port" peers to dial
    std::string              key;                    ///< shared secret: gates the handshake, keys discovery
    std::vector<std::string> allow;                  ///< peer ids (64 hex) allowed to sync; empty = any
    bool                     discover = false;       ///< enable DHT + mDNS discovery
    bool                     lan_only = false;       ///< restrict discovery to mDNS (LAN)

    SyncMode                 mode = SyncMode::TwoWay;
    bool                     source = false;         ///< mirror: this side is authoritative
    bool                     replica = false;        ///< mirror: this side mirrors the source
    ConflictPolicy           conflict = ConflictPolicy::Newer;
    bool                     no_delete = false;      ///< don't propagate deletions
    bool                     no_delta = false;       ///< disable rsync-style delta transfer

    std::vector<std::string> ignores;                ///< extra ignore patterns
    std::string              ignore_file;            ///< ignore file (default <dir>/.rasyncignore)

    int                      interval = 3;           ///< rescan cadence, seconds
    bool                     once = false;           ///< sync once, then exit
    std::string              data_dir;               ///< state dir (default: per-user, see core/state_dir.h)

    bool                     no_color = false;
    int                      verbosity = 1;          ///< 0 quiet · 1 normal · 2 verbose
};

struct ParseResult {
    enum Action { Run, Help, Version, Error } action = Run;
    std::string error;
    Options     options;
};

ParseResult parse_args(int argc, char** argv);

std::string help_text(const std::string& prog);

} // namespace rasync
