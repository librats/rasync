#pragma once

/**
 * @file cli.h
 * @brief Command-line parsing for rasync.
 *
 * Parses argv into an `Options` struct and produces the `--help` text. Kept free
 * of any networking so it can be unit-tested on its own.
 *
 * One run syncs one or more directories over a single connection per peer, so
 * the options split in two. Node-level ones (`--port`, `--key`, `--peer`,
 * `--allow`, `--discover`, `--interval`, …) are global; the rest describe *one*
 * folder and are positional — they apply to the directory they follow, and any
 * given before the first directory become the default for every folder:
 *
 *     rasync --conflict newer ./docs ./photos --mirror --source
 *             \_ default for both _/         \_ photos only _/
 *
 * That rule is the whole of the syntax: read left to right, a folder-scoped
 * option modifies whatever directory is currently open.
 */

#include <string>
#include <vector>

#include "core/diff.h"  // SyncMode, ConflictPolicy

namespace rasync {

/// Everything that describes a single synced directory.
struct FolderOptions {
    std::string    directory;              ///< the tree to keep in sync, as typed
    /// The name both peers know this folder by (`--name`). Empty means "the
    /// directory's own leaf name" — right whenever both machines happen to call
    /// the tree the same thing, and the reason `--name` exists when they don't.
    std::string    name;

    SyncMode       mode = SyncMode::TwoWay;
    bool           source = false;         ///< mirror: this side is authoritative
    bool           replica = false;        ///< mirror: this side mirrors the source
    ConflictPolicy conflict = ConflictPolicy::Newer;
    bool           no_delete = false;      ///< don't propagate deletions
    bool           no_delta = false;       ///< disable rsync-style delta transfer

    std::vector<std::string> ignores;      ///< extra ignore patterns
    std::string              ignore_file;  ///< ignore file (default <dir>/.rasyncignore)
};

struct Options {
    /// At least one; in command-line order, which is also display order.
    std::vector<FolderOptions> folders;

    // — node-level: one connection carries every folder, so these are not per-folder —
    uint16_t                 port = 0;               ///< listen port (0 = ephemeral)
    std::vector<std::string> peers;                  ///< explicit "host:port" peers to dial
    std::string              key;                    ///< shared secret: gates the handshake, keys discovery
    std::vector<std::string> allow;                  ///< peer ids (64 hex) allowed to sync; empty = any
    bool                     discover = false;       ///< enable DHT + mDNS discovery
    bool                     lan_only = false;       ///< restrict discovery to mDNS (LAN)

    int                      interval = 3;           ///< rescan cadence, seconds
    bool                     once = false;           ///< sync once, then exit
    /// State location: a root holding `node/` and one `dirs/<folder>/` per synced
    /// directory, whatever the folder count — the layout must not shift when a
    /// second directory joins the command line. Empty = the per-user default.
    std::string              data_dir;

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

/// Is `name` usable as a folder name? Names are compared byte for byte between
/// peers and travel in Hello, so anything invisible in one (a stray space, a
/// control character) becomes a mismatch nobody can see. Rejected at parse time
/// instead. `why` receives the reason when the answer is false.
bool valid_folder_name(const std::string& name, std::string& why);

} // namespace rasync
