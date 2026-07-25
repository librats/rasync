#include "app/daemon.h"

#include "app/terminal.h"
#include "core/scanner.h"
#include "net/auth.h"
#include "net/sync_service.h"
#include "version.h"

#include "node/node.h"
#include "subsystems/dht_discovery.h"
#include "subsystems/mdns_discovery.h"
#include "util/logger.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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

} // namespace

void request_stop() { g_stop.store(true); }

int run_daemon(const Options& opt) {
    term::init(opt.no_color);

    // Keep librats' internal logging out of the way: silent unless the user asked
    // for -v, where we surface the full DEBUG stream for troubleshooting.
    auto& logger = librats::Logger::getInstance();
    if (opt.verbosity >= 2) logger.set_log_level(librats::LogLevel::DEBUG);
    else                    logger.set_console_logging_enabled(false);

    // ── resolve paths ────────────────────────────────────────────────────────
    std::error_code ec;
    fs::path root = fs::absolute(opt.directory, ec);
    if (ec) { line(term::red("error: ") + "bad directory: " + opt.directory); return 2; }
    fs::create_directories(root, ec);
    if (!fs::is_directory(root)) {
        line(term::red("error: ") + "not a directory: " + root.string());
        return 2;
    }
    std::string root_str = root.string();
    std::string data_dir = opt.data_dir.empty() ? (root / ".rasync").string() : opt.data_dir;
    fs::create_directories(data_dir, ec);

    // ── ignore rules ─────────────────────────────────────────────────────────
    IgnoreList ignore;
    ignore.add(".rasync");       // our own state + temp dirs never sync
    ignore.add(kTempDirName);
    std::string ignore_file = opt.ignore_file.empty() ? (root / ".rasyncignore").string()
                                                      : opt.ignore_file;
    ignore.add_file(ignore_file);
    for (const auto& p : opt.ignores) ignore.add(p);

    // ── sync config ──────────────────────────────────────────────────────────
    SyncConfig cfg;
    cfg.root = root_str;
    cfg.data_dir = data_dir;
    cfg.mode = opt.mode;
    cfg.conflict = opt.conflict;
    cfg.source = opt.source;
    cfg.propagate_deletes = !opt.no_delete;
    cfg.use_delta = !opt.no_delta;
    for (const auto& hex : opt.allow) {
        auto id = librats::PeerId::from_hex(hex);
        if (!id) { line(term::red("error: ") + "bad --allow peer id: " + hex); return 2; }
        cfg.allowed_peers.insert(*id);
    }

    // ── terminal event handlers ──────────────────────────────────────────────
    // Shared liveness counters, read by the --once exit check. Declared before the
    // node so they outlive any event that fires during teardown.
    std::atomic<int>      peers{0};
    std::atomic<bool>     synced_once{false};
    std::atomic<int64_t>  last_sync_ms{0};

    SyncEvents ev;
    const int verbosity = opt.verbosity;
    ev.peer_up = [&peers](const librats::PeerId& id) {
        peers.fetch_add(1);
        line(term::green("●") + " connected to peer " + term::bold(short_id(id)));
    };
    ev.peer_down = [&peers](const librats::PeerId& id) {
        peers.fetch_sub(1);
        line(term::yellow("○") + " peer " + term::bold(short_id(id)) + " disconnected");
    };
    ev.round = [verbosity](const librats::PeerId&, const SyncPlan& plan) {
        if (verbosity < 2) return;
        if (plan.empty()) return;
        line(term::dim("  reconcile: ") +
             std::to_string(plan.pull.size()) + "↓  " +
             std::to_string(plan.push.size()) + "↑  " +
             std::to_string(plan.delete_local.size() + plan.delete_remote.size()) + "✗  " +
             (plan.conflicts.empty() ? "" : (term::yellow(std::to_string(plan.conflicts.size()) + " conflicts"))));
    };
    ev.file_done = [verbosity](const TransferInfo& t) {
        if (verbosity < 1) return;
        const bool recv = t.direction == TransferInfo::Recv;
        std::string arrow = recv ? term::cyan("↓") : term::blue("↑");
        std::string detail = term::gray("(" + term::bytes(t.total));
        if (t.delta)
            detail += ", delta " + term::bytes(t.on_wire) + " on wire";
        detail += ")";
        line("  " + arrow + " " + t.path + " " + detail);
    };
    ev.synced = [&](const librats::PeerId& id, uint64_t files, uint64_t bytes) {
        synced_once.store(true);
        last_sync_ms.store(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
        line(term::green("✓ in sync") + term::gray(" with " + short_id(id) + " — " +
             std::to_string(files) + " files, " + term::bytes(bytes)));
    };
    ev.log = [verbosity](int level, const std::string& msg) {
        if (level >= 2) { if (verbosity >= 1) line(term::yellow("! ") + msg); }
        else            { if (verbosity >= 2) line(term::dim("  " + msg)); }
    };

    // ── build the node ───────────────────────────────────────────────────────
    librats::NodeConfig ncfg;
    ncfg.listen_port = opt.port;
    ncfg.enable_listen = true;
    ncfg.bind_address = "::";
    // The shared key is folded into the protocol id, which librats binds into the
    // Noise prologue — that, not discovery, is what stops an uninvited peer: it
    // cannot finish the handshake however it found us.
    ncfg.protocol = protocol_id(opt.key);
    ncfg.data_dir = data_dir;

    librats::Node node(ncfg);
    SyncService service(node, cfg, ev);
    service.attach();
    service.load_baseline();
    service.clean_temp_dir();  // before the node starts: no transfer can be live yet

    librats::DhtDiscovery* dht = nullptr;
    if (opt.discover && !opt.lan_only) {
        librats::DhtDiscovery::Config dcfg;
        dcfg.data_dir = data_dir;
        dcfg.discovery_key = discovery_id(opt.key);
        dht = node.add_subsystem(std::make_unique<librats::DhtDiscovery>(dcfg));
    }
    if (opt.discover) {
        node.add_subsystem(std::make_unique<librats::MdnsDiscovery>());
    }

    // ── initial scan (before start, so the first exchange is meaningful) ──────
    Scanner scanner(ignore);
    line(term::dim("scanning ") + root_str + term::dim(" ..."));
    auto scan_t0 = steady_clock::now();
    ScanStats stats;
    Manifest current = scanner.scan(root_str, nullptr, &stats);
    service.set_local_manifest(current);
    auto scan_ms = duration_cast<milliseconds>(steady_clock::now() - scan_t0).count();
    line(term::green("indexed ") + std::to_string(current.size()) + " files (" +
         term::bytes(current.total_bytes()) + ") in " + std::to_string(scan_ms) + "ms");

    if (!node.start()) {
        line(term::red("error: ") + "failed to start network (port in use?)");
        return 1;
    }

    // ── banner ────────────────────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lk(g_print);
        std::cout << "\n" << term::bold(term::cyan("rasync")) << " " << kVersion << "\n";
        // Full id, not the short form used in logs: this is the string a peer
        // pastes into its --allow, so it has to be copyable from here.
        std::cout << term::gray("  id       ") << node.local_id().to_hex() << "\n";
        std::cout << term::gray("  dir      ") << root_str << "\n";
        std::cout << term::gray("  mode     ")
                  << (opt.mode == SyncMode::Mirror
                          ? std::string("mirror (") + (opt.source ? "source" : "replica") + ")"
                          : "two-way")
                  << (cfg.use_delta ? "" : term::dim("  no-delta"))
                  << (cfg.propagate_deletes ? "" : term::dim("  no-delete")) << "\n";
        // Never echo the key itself — it is a password, and this line is what ends
        // up in screenshots and CI logs.
        const std::string listed = std::to_string(cfg.allowed_peers.size()) + " allowed peer(s)";
        std::string access;
        if (!opt.key.empty())
            access = term::green("shared key") +
                     (cfg.allowed_peers.empty() ? "" : term::gray(" · " + listed));
        else if (!cfg.allowed_peers.empty())
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
    for (const auto& spec : opt.peers) {
        std::string host; uint16_t port = 0;
        if (!parse_hostport(spec, host, port)) {
            line(term::yellow("! ") + "ignoring bad --peer " + spec);
            continue;
        }
        line(term::dim("dialing ") + spec + term::dim(" ..."));
        node.connect(host, port);
    }

    if (!opt.discover && opt.peers.empty())
        line(term::yellow("note: ") + "no --peer and no --discover — waiting for inbound peers on port " +
             std::to_string(node.listen_port()));

    // ── watch loop ───────────────────────────────────────────────────────────
    if (opt.once) {
        line(term::dim("running once; will exit when idle after syncing..."));
    } else {
        line(term::dim("watching for changes (Ctrl-C to stop)"));
    }

    auto last_rescan = steady_clock::now();
    auto started = steady_clock::now();
    const auto interval = seconds(opt.interval);
    while (!g_stop.load()) {
        std::this_thread::sleep_for(milliseconds(200));
        auto now = steady_clock::now();

        if (opt.once) {
            // Exit once we've synced with a peer and stayed quiet briefly; or bail
            // if no peer ever shows up.
            int64_t now_ms = duration_cast<milliseconds>(now.time_since_epoch()).count();
            if (synced_once.load() && now_ms - last_sync_ms.load() > 1500) {
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

        // Scan against the service's view, not a private copy: it already holds
        // the files the sessions received since the last pass, so their hashes
        // are reused instead of the whole download being read back off disk.
        Manifest before = service.local_manifest();
        ScanStats s;
        Manifest fresh = scanner.scan(root_str, &before, &s);

        // Publish the difference rather than the snapshot. A file that landed
        // while this scan was walking appears in neither endpoint, so it keeps
        // the entry its transfer recorded instead of being dropped and re-pulled.
        ManifestPatch patch = diff_manifests(before, fresh);
        if (!patch.empty()) {
            if (verbosity >= 2)
                line(term::dim("rescan: ") + std::to_string(fresh.size()) + " files, " +
                     std::to_string(patch.size()) + " changed, " +
                     std::to_string(s.files_hashed) + " (re)hashed");
            service.apply_local_patch(patch);
        }
    }

    line("\n" + term::dim("shutting down..."));
    service.shutdown();
    node.stop();
    line(term::green("stopped."));
    return 0;
}

} // namespace rasync
