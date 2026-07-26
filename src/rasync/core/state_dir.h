#pragma once

/**
 * @file state_dir.h
 * @brief Where rasync keeps its own state — outside the directory it syncs.
 *
 * A synced tree belongs to the user, not to rasync: everything rasync needs to
 * remember between runs (the baseline manifest, the node identity, the DHT
 * routing table) lives under the platform's per-user state location instead of
 * in the tree. Only genuinely transient scratch data may touch the tree, and
 * even that is removed again (see SyncService::clean_temp_dir / prune_temp_dir).
 *
 * The mapping "synced directory → state directory" has to be *stable across
 * runs*, or the second run of the same command would come up with an empty
 * baseline and treat the sync as first-time (no deletes propagate). It is
 * therefore derived only from the directory's absolute path — never from a
 * timestamp, a random id, or anything stored in the tree.
 */

#include <string>

namespace rasync {

/// Per-user root of rasync's state, from the first of these that resolves:
///   `$RASYNC_HOME`                        (any platform — the explicit override)
///   Windows: `%LOCALAPPDATA%\rasync`, else `%APPDATA%\rasync`
///   macOS:   `~/Library/Application Support/rasync`
///   else:    `$XDG_DATA_HOME/rasync`, else `~/.local/share/rasync`
/// Empty when even a home directory cannot be determined.
std::string user_state_root();

/// The directory name a synced tree maps to: `<leaf>-<16 hex>`, where the leaf
/// is the tree's own (sanitised) folder name — so the directory is recognisable
/// by eye — and the hex is a SHA-256 prefix of the full absolute path, which is
/// what actually keeps two different trees with the same leaf name apart.
///
/// The path is normalised first (separators, `.`/`..`, trailing slash; and case
/// on Windows, where two spellings of one path name the same directory), so the
/// same tree keeps its state however the user happened to type it.
std::string state_dir_name(const std::string& sync_root);

/// Full state directory for one synced tree: `<user_state_root>/dirs/<name>`.
/// Falls back to `<sync_root>/.rasync` only when there is no user state root at
/// all — a state directory that cannot be found again is worse than an in-tree one.
std::string state_dir_for(const std::string& sync_root);

/// The *node's* own state — its identity key and DHT routing table — at
/// `<user_state_root>/node`. Empty when there is no user state root; the caller
/// then has to pick somewhere (the daemon falls back to the first folder's).
///
/// This is deliberately not any folder's state directory. One process syncs many
/// folders over one node, so tying the identity to a folder would make the peer
/// id — the thing `--allow` lists and peers recognise us by — depend on which
/// directory happened to be named first on the command line.
std::string node_state_dir();

/// Drop a `directory.txt` next to the state, naming the tree it belongs to.
/// The state directory's own name is hashed and therefore unreadable; this is
/// what makes an orphaned one identifiable. Best-effort: failure is ignored.
void record_state_owner(const std::string& state_dir, const std::string& sync_root);

/// Move a pre-existing in-tree state directory (`<root>/.rasync`) to `target`.
/// Entries already present in `target` win and are left untouched in `legacy`,
/// so nothing is ever overwritten or silently discarded; `legacy` is removed
/// only once it is empty. Returns the number of entries moved.
///
/// This exists so upgrading in place does not silently lose the baseline (and
/// with it, delete propagation) and does not leave the tree cluttered.
size_t migrate_state_dir(const std::string& legacy, const std::string& target);

} // namespace rasync
