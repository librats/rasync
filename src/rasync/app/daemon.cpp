#include "app/daemon.h"

#include "app/dial_retry.h"

#include "app/terminal.h"
#include "core/scanner.h"
#include "core/state_dir.h"
#include "net/auth.h"
#include "net/sync_router.h"
#include "net/sync_service.h"
#include "version.h"

#include "librats/core/types.h"   // CloseReason, to_string
#include "librats/node/node.h"
#include "librats/subsystems/dht_discovery.h"
#include "librats/subsystems/mdns_discovery.h"
#include "librats/util/logger.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono;

namespace rasync {

namespace {

std::atomic<bool> g_stop{false};
std::mutex        g_print;

void line(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_print);
    std::cout << s << '\n';
    std::cout.flush();
}

bool parse_hostport(const std::string& s, std::string& host, uint16_t& port) {
    std::string h;
    std::string p;
    if (!s.empty() && s[0] == '[') {                 // [ipv6]:port
        auto close = s.find(']');
        if (close == std::string::npos) return false;
        h = s.substr(1, close - 1);
        auto colon = s.find(':', close);
        if (colon == std::string::npos) return false;
        p = s.substr(colon + 1);
    } else {
        auto colon = s.rfind(':');
        if (colon == std::string::npos) return false;
        h = s.substr(0, colon);
        p = s.substr(colon + 1);
    }
    char* end = nullptr;
    unsigned long v = std::strtoul(p.c_str(), &end, 10);
    if (h.empty() || !end || *end != '\0' || v == 0 || v > 65535) return false;
    host = h;
    port = static_cast<uint16_t>(v);
    return true;
}

std::string short_id(const librats::PeerId& id) { return id.short_hex(); }

/// One absolute path in the single spelling two paths must share to be compared.
/// Case is folded on Windows, where two spellings name one directory — the same
/// rule state_dir.cpp uses, so "the same tree" means the same thing in both places.
std::string path_key(const fs::path& p) {
    std::string s = p.lexically_normal().generic_string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
#ifdef _WIN32
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
#endif
    return s;
}

/// Does `child` lie inside `parent` (or equal it)? Compared segment-wise, so
/// `/srv/dataset` is not "inside" `/srv/data`.
bool path_within(const std::string& child_key, const std::string& parent_key) {
    if (child_key == parent_key) return true;
    if (child_key.size() <= parent_key.size()) return false;
    if (child_key.compare(0, parent_key.size(), parent_key) != 0) return false;
    // The parent already ends in '/' when it is a filesystem root ("c:/", "/").
    return parent_key.back() == '/' || child_key[parent_key.size()] == '/';
}

/// Everything the daemon tracks for one synced directory. Held by pointer so the
/// event lambdas can capture it: the vector must never move one out from under a
/// callback already running on a reactor thread.
struct Folder {
    std::string       name;
    std::string       root;       ///< absolute
    std::string       root_key;   ///< path_key(root), for the overlap checks
    std::string       data_dir;
    Scanner           scanner;
    Manifest          current;
    SyncService*      svc = nullptr;
    std::atomic<bool> synced_once{false};
};

/// Where the node's identity and each folder's baseline live.
struct StateLayout {
    std::string              node;      ///< identity.key, DHT routing table
    std::vector<std::string> folders;   ///< one per folder, in order
};

/// `--data-dir` is the root of the same layout the per-user default uses:
/// `node/` plus one `dirs/<hash>` per folder.
///
/// Deliberately *not* special-cased for a single folder — a flat layout there
/// would read better, but it would also make the layout depend on how many
/// directories are on the command line. Adding a second folder to a working
/// `--data-dir` setup would then move both the identity key (a new peer id, so
/// every peer's `--allow` goes stale) and the baseline (which reads as "never
/// synced", and silently stops deletes propagating). One shape, always.
StateLayout resolve_state(const std::string& data_dir_opt,
                          const std::vector<std::unique_ptr<Folder>>& folders) {
    StateLayout out;
    if (data_dir_opt.empty()) {
        out.node = node_state_dir();
        for (const auto& f : folders) out.folders.push_back(state_dir_for(f->root));
        // No per-user location at all (no HOME, no LOCALAPPDATA): state_dir_for has
        // already fallen back into the trees, and the node still has to live
        // somewhere it can be found again — the first folder's directory is at
        // least stable across runs of the same command.
        if (out.node.empty() && !out.folders.empty()) out.node = out.folders.front();
        return out;
    }
    out.node = (fs::path(data_dir_opt) / "node").string();
    for (const auto& f : folders)
        out.folders.push_back((fs::path(data_dir_opt) / "dirs" / state_dir_name(f->root)).string());
    return out;
}

} // namespace

void request_stop() { g_stop.store(true); }

int run_daemon(const Options& opt) {
    term::init(opt.no_color);

    // Keep librats' internal logging out of the way: silent unless the user asked
    // for -v, where we surface the full DEBUG stream for troubleshooting.
    auto& logger = librats::Logger::getInstance();
    if (opt.verbosity >= 2) logger.set_log_level(librats::LogLevel::DEBUG);
    else                    logger.set_console_logging_enabled(false);

    const int  verbosity = opt.verbosity;
    const bool multi     = opt.folders.size() > 1;

    auto fail = [](const std::string& msg) {
        line(term::red("error: ") + msg);
        return 2;
    };

    // ── resolve directories ──────────────────────────────────────────────────
    std::vector<std::unique_ptr<Folder>> folders;
    for (const auto& fo : opt.folders) {
        std::error_code ec;
        fs::path root = fs::absolute(fo.directory, ec);
        if (ec) return fail("bad directory: " + fo.directory);
        fs::create_directories(root, ec);
        if (!fs::is_directory(root)) return fail("not a directory: " + root.string());

        auto f = std::make_unique<Folder>();
        f->root = root.string();
        f->root_key = path_key(root);
        f->name = fo.name.empty() ? default_folder_name(f->root) : fo.name;
        folders.push_back(std::move(f));
    }

    // Overlapping roots are not two folders but one folder synced twice: the outer
    // scan sees the inner tree (and its `.rasync-tmp` mid-transfer), so every file
    // inside it would be advertised under two names and written through two paths.
    // Checked before the names, so a directory listed twice is reported as exactly
    // that rather than as the name collision it also is.
    for (size_t i = 0; i < folders.size(); ++i) {
        for (size_t j = 0; j < folders.size(); ++j) {
            if (i == j) continue;
            if (!path_within(folders[i]->root_key, folders[j]->root_key)) continue;
            const bool same = folders[i]->root_key == folders[j]->root_key;
            if (same && i > j) continue;   // report the pair once
            return fail(same ? "the same directory is listed twice: " + folders[i]->root
                             : folders[i]->root + " is inside " + folders[j]->root +
                                   " — a synced folder cannot contain another");
        }
    }

    // Two folders under one name would route both peers' traffic for that name
    // into whichever one registered first. Explicit names were already checked by
    // the parser; this is the case it could not see — two distinct directories
    // whose *leaf names* happen to match ("~/work/src" and "~/play/src").
    {
        std::unordered_map<std::string, const Folder*> seen;
        for (const auto& f : folders) {
            auto [it, fresh] = seen.emplace(f->name, f.get());
            if (!fresh)
                return fail("both " + it->second->root + " and " + f->root +
                            " would be the folder '" + f->name +
                            "' — give one a different --name");
        }
    }

    // ── state directories ────────────────────────────────────────────────────
    StateLayout state = resolve_state(opt.data_dir, folders);
    {
        std::error_code ec;
        fs::create_directories(state.node, ec);
    }
    for (size_t i = 0; i < folders.size(); ++i) {
        std::error_code ec;
        folders[i]->data_dir = state.folders[i];
        fs::create_directories(folders[i]->data_dir, ec);
        record_state_owner(folders[i]->data_dir, folders[i]->root);

        // Older versions kept state in <dir>/.rasync. Move it rather than start
        // from an empty baseline — that would read as "nothing was ever synced"
        // and stop deletes propagating — and leave the tree clean afterwards.
        const std::string legacy = (fs::path(folders[i]->root) / ".rasync").string();
        if (size_t moved = migrate_state_dir(legacy, folders[i]->data_dir))
            line(term::dim("moved ") + std::to_string(moved) +
                 term::dim(" state file(s) out of ") + folders[i]->root +
                 term::dim(" into ") + folders[i]->data_dir);
    }

    // ── build the node ───────────────────────────────────────────────────────
    librats::NodeConfig ncfg;
    ncfg.listen_port = opt.port;
    ncfg.enable_listen = true;
    ncfg.bind_address = "::";
    // The shared key is folded into the protocol id, which librats binds into the
    // Noise prologue — that, not discovery, is what stops an uninvited peer: it
    // cannot finish the handshake however it found us. It is node-wide: one
    // process is one trust domain, however many folders it carries.
    ncfg.protocol = protocol_id(opt.key);
    ncfg.data_dir = state.node;

    librats::Node node(ncfg);

    // ── node-level events ────────────────────────────────────────────────────
    // Declared before the router so they outlive any event still running during
    // teardown.
    std::atomic<int>     peers{0};
    std::atomic<int64_t> last_sync_ms{0};

    RouterEvents rev;
    rev.peer_up = [&peers](const librats::PeerId& id) {
        peers.fetch_add(1);
        line(term::green("●") + " connected to peer " + term::bold(short_id(id)));
    };
    rev.peer_down = [&peers](const librats::PeerId& id, librats::CloseReason reason) {
        peers.fetch_sub(1);
        // A slow-consumer drop is the one reason that is our own doing: librats
        // closed the peer because we kept sending with its queue already past the
        // high-water mark. It reads as an ordinary disconnect and it is not one —
        // the link was fine — so it is called out rather than folded in with the
        // peer simply leaving. Anything else is the peer's or the network's, and
        // reconnecting is the whole answer.
        if (reason == librats::CloseReason::SlowConsumer) {
            line(term::red("○") + " peer " + term::bold(short_id(id)) +
                 " dropped us as a slow consumer — sent faster than the link drained");
        } else {
            line(term::yellow("○") + " peer " + term::bold(short_id(id)) + " disconnected" +
                 term::dim(" (" + std::string(librats::to_string(reason)) + ")"));
        }
    };
    rev.log = [verbosity](int level, const std::string& msg) {
        if (level >= 2) { if (verbosity >= 1) line(term::yellow("! ") + msg); }
        else            { if (verbosity >= 2) line(term::dim("  " + msg)); }
    };

    SyncRouter router(node, rev);

    // ── one service per folder ───────────────────────────────────────────────
    for (size_t i = 0; i < folders.size(); ++i) {
        Folder& f = *folders[i];
        const FolderOptions& fo = opt.folders[i];

        IgnoreList ignore;
        // Still excluded by name: a peer on an older version, or a --data-dir
        // pointed back inside the tree, can put one of these there.
        ignore.add(".rasync");
        ignore.add(kTempDirName);
        const std::string ignore_file =
            fo.ignore_file.empty() ? (fs::path(f.root) / ".rasyncignore").string()
                                   : fo.ignore_file;
        ignore.add_file(ignore_file);
        for (const auto& p : fo.ignores) ignore.add(p);
        f.scanner = Scanner(std::move(ignore), fo.follow_symlinks);

        SyncConfig cfg;
        cfg.root = f.root;
        cfg.data_dir = f.data_dir;
        cfg.folder = f.name;
        cfg.mode = fo.mode;
        cfg.conflict = fo.conflict;
        cfg.source = fo.source;
        cfg.propagate_deletes = !fo.no_delete;
        cfg.use_delta = !fo.no_delta;
        // Both halves of the same flag: what the scanner reads through a link, a
        // transfer must write back through it.
        cfg.follow_symlinks = fo.follow_symlinks;
        for (const auto& hex : opt.allow) {
            auto id = librats::PeerId::from_hex(hex);
            if (!id) return fail("bad --allow peer id: " + hex);
            cfg.allowed_peers.insert(*id);
        }

        // A folder's lines carry its name only when there is more than one to tell
        // apart, so a single-folder run reads exactly as it always did.
        const std::string label = multi ? term::gray("[" + f.name + "] ") : std::string();

        SyncEvents ev;
        ev.round = [verbosity, label](const librats::PeerId&, const SyncPlan& plan) {
            if (verbosity < 2 || plan.empty()) return;
            line(label + term::dim("  reconcile: ") +
                 std::to_string(plan.pull.size()) + "↓  " +
                 std::to_string(plan.push.size()) + "↑  " +
                 std::to_string(plan.delete_local.size() + plan.delete_remote.size()) + "✗  " +
                 (plan.conflicts.empty()
                      ? ""
                      : term::yellow(std::to_string(plan.conflicts.size()) + " conflicts")));
        };
        ev.file_done = [verbosity, label](const TransferInfo& t) {
            if (verbosity < 1) return;
            const bool recv = t.direction == TransferInfo::Recv;
            std::string arrow = recv ? term::cyan("↓") : term::blue("↑");
            std::string detail = term::gray("(" + term::bytes(t.total));
            if (t.delta) detail += ", delta " + term::bytes(t.on_wire) + " on wire";
            detail += ")";
            line(label + "  " + arrow + " " + t.path + " " + detail);
        };
        ev.synced = [&f, &last_sync_ms, label](const librats::PeerId& id, uint64_t files,
                                               uint64_t bytes) {
            f.synced_once.store(true);
            last_sync_ms.store(
                duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
            line(label + term::green("✓ in sync") +
                 term::gray(" with " + short_id(id) + " — " + std::to_string(files) +
                            " files, " + term::bytes(bytes)));
        };
        ev.log = [verbosity, label](int level, const std::string& msg) {
            if (level >= 2) { if (verbosity >= 1) line(label + term::yellow("! ") + msg); }
            else            { if (verbosity >= 2) line(label + term::dim("  " + msg)); }
        };

        f.svc = router.add_folder(std::move(cfg), std::move(ev));
        if (!f.svc)
            return fail("folder '" + f.name + "' collides with another on the wire — "
                        "give one a different --name");
    }

    router.attach();         // before node.start(): the channel must be registered first
    router.start_folders();  // baselines + stale temp files, while nothing can be live

    librats::DhtDiscovery* dht = nullptr;
    if (opt.discover && !opt.lan_only) {
        librats::DhtDiscovery::Config dcfg;
        dcfg.data_dir = state.node;
        dcfg.discovery_key = discovery_id(opt.key);
        dht = node.add_subsystem(std::make_unique<librats::DhtDiscovery>(dcfg));
    }
    if (opt.discover) {
        node.add_subsystem(std::make_unique<librats::MdnsDiscovery>());
    }

    // ── initial scan (before start, so the first exchange is meaningful) ──────
    for (auto& f : folders) {
        line(term::dim("scanning ") + f->root + term::dim(" ..."));
        auto scan_t0 = steady_clock::now();
        ScanStats stats;
        f->current = f->scanner.scan(f->root, nullptr, &stats);
        f->svc->set_local_manifest(f->current);
        auto scan_ms = duration_cast<milliseconds>(steady_clock::now() - scan_t0).count();
        // Links are the one thing a scan silently drops from a tree the user can
        // see, so say so: "why is that file not syncing?" should be answerable
        // from the first screen, whether or not --follow-symlinks is on. "link"
        // rather than "symlink" because a Windows junction is counted here too,
        // and being told a symlink was skipped in a tree holding none is its own
        // small mystery.
        std::string links;
        if (stats.symlinks_followed)
            links += ", " + std::to_string(stats.symlinks_followed) + " link(s) followed";
        if (stats.symlinks_skipped)
            links += ", " + std::to_string(stats.symlinks_skipped) + " link(s) skipped";
        line(term::green("indexed ") + std::to_string(f->current.size()) + " files (" +
             term::bytes(f->current.total_bytes()) + ") in " + std::to_string(scan_ms) + "ms" +
             term::gray(links) + (multi ? term::gray("  " + f->name) : std::string()));
    }

    if (!node.start()) return fail("failed to start network (port in use?)");

    // ── banner ────────────────────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lk(g_print);
        std::cout << "\n" << term::bold(term::cyan("rasync")) << " " << kVersion << "\n";
        // Full id, not the short form used in logs: this is the string a peer
        // pastes into its --allow, so it has to be copyable from here.
        std::cout << term::gray("  id       ") << node.local_id().to_hex() << "\n";
        for (size_t i = 0; i < folders.size(); ++i) {
            const Folder& f = *folders[i];
            const FolderOptions& fo = opt.folders[i];
            std::cout << term::gray(i == 0 ? "  folder   " : "           ")
                      << term::bold(f.name) << term::gray("  " + f.root) << "\n";
            std::cout << "             "
                      << (fo.mode == SyncMode::Mirror
                              ? std::string("mirror (") + (fo.source ? "source" : "replica") + ")"
                              : "two-way")
                      << (fo.mode == SyncMode::Mirror
                              ? std::string()
                              : term::dim("  conflict ") + conflict_policy_name(fo.conflict))
                      << (fo.no_delta ? term::dim("  no-delta") : "")
                      << (fo.no_delete ? term::dim("  no-delete") : "")
                      << (fo.follow_symlinks ? term::dim("  follow-symlinks") : "")
                      << "\n";
        }
        // Worth printing: state is deliberately not next to the data, and the
        // folder half is what a user deletes to reset a tree's sync history. The
        // node half is separate (it is the identity, shared by every folder), so
        // both are named — unless --data-dir has collapsed them into one.
        std::cout << term::gray("  state    ") << state.node << term::gray("  (node)") << "\n";
        const std::string folder_state =
            folders.size() == 1
                ? folders.front()->data_dir
                : fs::path(state.folders.front()).parent_path().string();
        if (folder_state != state.node)
            std::cout << term::gray("           ") << folder_state
                      << term::gray(folders.size() == 1 ? "  (this folder)"
                                                        : "  (one per folder)")
                      << "\n";
        // Never echo the key itself — it is a password, and this line is what ends
        // up in screenshots and CI logs.
        const std::string listed = std::to_string(opt.allow.size()) + " allowed peer(s)";
        std::string access;
        if (!opt.key.empty())
            access = term::green("shared key") +
                     (opt.allow.empty() ? "" : term::gray(" · " + listed));
        else if (!opt.allow.empty())
            access = term::green("allow-list") + term::gray(" · " + listed + ", no shared key");
        else
            access = term::yellow("open — any rasync peer that reaches this port can sync");
        std::cout << term::gray("  access   ") << access << "\n";
        std::cout << term::gray("  listen   ") << "port " << node.listen_port() << "\n";
        if (opt.discover)
            std::cout << term::gray("  discover ")
                      << (opt.lan_only ? "mDNS (LAN)" : "DHT + mDNS") << "\n";
        if (dht)
            std::cout << term::gray("  dht      ") << "port " << dht->dht_port() << "\n";
        std::cout << "\n";
        std::cout.flush();
    }

    // ── dial explicit peers ──────────────────────────────────────────────────
    // Kept, not just used once: every way a connection can end leaves an explicit
    // peer with nothing to re-establish it, so the watch loop dials these again
    // while nothing is connected (see app/dial_retry.h).
    struct DialTarget { std::string spec, host; uint16_t port = 0; };
    std::vector<DialTarget> dial_targets;
    for (const auto& spec : opt.peers) {
        DialTarget t;
        t.spec = spec;
        if (!parse_hostport(spec, t.host, t.port)) {
            line(term::yellow("! ") + "ignoring bad --peer " + spec);
            continue;
        }
        dial_targets.push_back(std::move(t));
    }
    for (const auto& t : dial_targets) {
        line(term::dim("dialing ") + t.spec + term::dim(" ..."));
        node.connect(t.host, t.port);
    }
    DialRetry dial_retry;

    if (!opt.discover && opt.peers.empty())
        line(term::yellow("note: ") + "no --peer and no --discover — waiting for inbound peers on port " +
             std::to_string(node.listen_port()));

    // ── watch loop ───────────────────────────────────────────────────────────
    if (opt.once) {
        line(term::dim("running once; will exit when idle after syncing..."));
    } else {
        line(term::dim("watching for changes (Ctrl-C to stop)"));
    }

    auto all_synced = [&folders] {
        for (const auto& f : folders)
            if (!f->synced_once.load()) return false;
        return true;
    };

    auto last_rescan = steady_clock::now();
    auto started = steady_clock::now();
    const auto interval = seconds(opt.interval);
    while (!g_stop.load()) {
        std::this_thread::sleep_for(milliseconds(200));
        auto now = steady_clock::now();

        // Reconnect. Only while *nothing* is connected: librats resolves a second
        // connection to a peer we already hold by keeping the newer one, so
        // dialing a live peer would drop its session and whatever it was
        // transferring. Nobody connected cannot describe a connection worth
        // keeping, which makes this safe as well as sufficient.
        if (!dial_targets.empty() && dial_retry.due(now, peers.load())) {
            for (const auto& t : dial_targets) {
                // Shown at the same volume as the startup dial: it is the same
                // event, and a user watching a sync that stopped needs to see that
                // something is trying. The backoff is what keeps it from being
                // noise — one line per address every 8 s at first, then rarer.
                line(term::dim("re-dialing ") + t.spec + term::dim(" ..."));
                node.connect(t.host, t.port);
            }
        }

        if (opt.once) {
            // Exit once *every* folder has synced with a peer and things have
            // stayed quiet briefly; or bail if no peer ever shows up.
            int64_t now_ms = duration_cast<milliseconds>(now.time_since_epoch()).count();
            if (all_synced() && now_ms - last_sync_ms.load() > 1500) {
                line(term::green("✓ done") + term::dim(" (--once)"));
                break;
            }
            if (peers.load() == 0 && now - started > seconds(30)) {
                line(term::yellow("! ") + "no peers connected within 30s — exiting");
                break;
            }
        }

        if (now - last_rescan < interval) continue;
        last_rescan = now;

        // Folders are scanned in turn on this one thread. Hashing dominates only
        // where something actually changed, so a steady-state pass over several
        // trees is metadata-only and cheap; a folder in the middle of a large
        // import does delay its neighbours' next scan by its own scan time.
        for (auto& f : folders) {
            // Scan against the service's view, not a private copy: it already holds
            // the files the sessions received since the last pass, so their hashes
            // are reused instead of the whole download being read back off disk.
            Manifest before = f->svc->local_manifest();
            ScanStats s;
            Manifest fresh = f->scanner.scan(f->root, &before, &s);

            // Publish the difference rather than the snapshot. A file that landed
            // while this scan was walking appears in neither endpoint, so it keeps
            // the entry its transfer recorded instead of being dropped and re-pulled.
            ManifestPatch patch = diff_manifests(before, fresh);
            if (patch.empty()) continue;
            if (verbosity >= 2)
                line((multi ? term::gray("[" + f->name + "] ") : std::string()) +
                     term::dim("rescan: ") + std::to_string(fresh.size()) + " files, " +
                     std::to_string(patch.size()) + " changed, " +
                     std::to_string(s.files_hashed) + " (re)hashed");
            f->svc->apply_local_patch(patch);
        }
    }

    line("\n" + term::dim("shutting down..."));
    router.shutdown();
    node.stop();
    line(term::green("stopped."));
    return 0;
}

} // namespace rasync
