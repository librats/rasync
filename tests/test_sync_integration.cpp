#include <gtest/gtest.h>

#include "core/scanner.h"
#include "net/auth.h"
#include "net/sync_router.h"
#include "net/sync_service.h"
#include "version.h"
#include "test_util.h"

#include "librats/node/node.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace rasync;
using namespace std::chrono;

namespace {

IgnoreList make_ignore() {
    IgnoreList ig;
    ig.add(".rasync");
    ig.add(kTempDirName);
    return ig;
}

Manifest disk_state(const std::string& root, bool follow_symlinks = false) {
    return Scanner(make_ignore(), follow_symlinks).scan(root);
}

template <typename Pred>
bool wait_until(Pred pred, milliseconds timeout = milliseconds(25000)) {
    auto deadline = steady_clock::now() + timeout;
    while (steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(milliseconds(50));
    }
    return pred();
}

/// The folder name every endpoint's primary directory carries. Fixed rather than
/// derived from the (unique) temp path so a RawPeer can compute the same tag.
constexpr const char* kFolder = "test";

/// One synced directory belonging to an Endpoint: its tree, its state, and the
/// service that drives it.
struct Folder {
    test::TempDir dir;
    test::TempDir state;   ///< out of the synced tree, as in a real run
    std::string   name;
    std::string   root;
    Scanner       scanner;
    Manifest      current;
    SyncService*  svc = nullptr;   ///< owned by the endpoint's router
    std::atomic<int> synced_events{0};

    explicit Folder(std::string folder_name, bool follow_symlinks = false)
        : name(std::move(folder_name)), root(dir.str()),
          scanner(make_ignore(), follow_symlinks) {}

    void publish() {
        current = scanner.scan(root, &current, nullptr);
        svc->set_local_manifest(current);
    }
    std::string sub(const std::string& rel) const { return dir.sub(rel); }
};

// One node + router + folders rooted at fresh temp dirs, driven directly (no
// daemon). The first folder is the one the single-folder tests use, and is
// reachable through the members that name it directly (`dir`, `root`, `svc`, …).
struct Endpoint {
    test::TempDir                  node_state;  ///< identity: node-wide, not per folder
    std::unique_ptr<librats::Node> node;
    std::unique_ptr<SyncRouter>    router;
    std::vector<std::unique_ptr<Folder>> folders;
    SyncConfig                     cfg;   ///< the template every folder is built from

    /// `key` and `allow` are the two access controls: the first gates the librats
    /// handshake (via the derived protocol id), the second gates syncing once the
    /// handshake is through. An allow-list entry has to be a peer id we already
    /// hold, which is why the endpoint whose id is listed must be built first.
    /// `send_queue_limit` shrinks librats' per-connection send high-water mark
    /// (0 = the 8 MiB default). Small values reproduce, in a couple of megabytes,
    /// the overrun that took gigabytes and several folders to provoke in the
    /// field — the peer is dropped as a slow consumer the moment the queue is
    /// offered more than it holds.
    explicit Endpoint(SyncMode mode = SyncMode::TwoWay, bool source = false,
                      size_t max_update_bytes = 0,
                      ConflictPolicy conflict = ConflictPolicy::Newer,
                      const std::string& key = {},
                      const std::vector<librats::PeerId>& allow = {},
                      bool follow_symlinks = false,
                      size_t send_queue_limit = 0) {
        cfg.mode = mode;
        cfg.source = source;
        cfg.conflict = conflict;
        cfg.follow_symlinks = follow_symlinks;
        cfg.allowed_peers.insert(allow.begin(), allow.end());
        if (max_update_bytes) cfg.max_update_bytes = max_update_bytes;

        librats::NodeConfig ncfg;
        ncfg.listen_port = 0;
        ncfg.bind_address = "127.0.0.1";
        ncfg.protocol = protocol_id(key);
        ncfg.data_dir = node_state.str();
        ncfg.enable_network_monitor = false;
        ncfg.send_queue_limit = send_queue_limit;
        node = std::make_unique<librats::Node>(ncfg);

        RouterEvents rev;
        rev.peer_up   = [this](const librats::PeerId&) { peer_ups.fetch_add(1); };
        rev.peer_down = [this](const librats::PeerId&, librats::CloseReason reason) {
            peer_downs.fetch_add(1);
            if (reason == librats::CloseReason::SlowConsumer) slow_consumer_downs.fetch_add(1);
        };
        rev.log = [this](int, const std::string& msg) {
            std::lock_guard<std::mutex> lk(log_mtx);
            logs.push_back(msg);
        };
        router = std::make_unique<SyncRouter>(*node, std::move(rev));

        add_folder(kFolder);
    }

    ~Endpoint() {
        if (router) router->shutdown();
        if (node) node->stop();
    }

    /// Add a folder. Before start(), like a real run's command line.
    Folder& add_folder(const std::string& name) {
        // The scanner reads through links exactly when the service writes back
        // through them: one flag, both halves (SyncConfig::follow_symlinks).
        auto f = std::make_unique<Folder>(name, cfg.follow_symlinks);
        SyncConfig c = cfg;
        c.root = f->root;
        c.data_dir = f->state.str();
        c.folder = name;

        // Fires from reactor/session threads once a peer's tree matches ours.
        Folder* raw = f.get();
        SyncEvents ev;
        ev.synced = [raw](const librats::PeerId&, uint64_t, uint64_t) {
            raw->synced_events.fetch_add(1);
        };
        raw->svc = router->add_folder(std::move(c), std::move(ev));
        EXPECT_NE(raw->svc, nullptr) << "folder '" << name << "' collided with another";
        folders.push_back(std::move(f));
        return *raw;
    }

    void publish() { folders.front()->publish(); }

    void start() {
        router->attach();
        router->start_folders();
        for (auto& f : folders) f->publish();
        ASSERT_TRUE(node->start());
    }

    uint16_t port() const { return node->listen_port(); }

    /// The folder a single-folder test means when it says "this endpoint's tree".
    Folder&       first()       { return *folders.front(); }
    const Folder& first() const { return *folders.front(); }

    /// Connections seen. A drop mid-transfer is invisible in the synced tree — the
    /// sync simply takes longer, or never finishes — so a test that cares has to
    /// watch the count rather than the files.
    std::atomic<int> peer_ups{0};
    std::atomic<int> peer_downs{0};
    /// Of those drops, the ones librats blamed on us: we kept sending with the
    /// peer's queue already past its high-water mark. Counted apart because it is
    /// the only close reason the pacing tests are actually about — every other one
    /// says something about the peer or the link, and none of them would be
    /// evidence that the gate failed.
    std::atomic<int> slow_consumer_downs{0};

    /// Node-level diagnostics the router produced (folders a peer offered that we
    /// do not have, and the like).
    mutable std::mutex       log_mtx;
    std::vector<std::string> logs;
    bool logged(const std::string& needle) const {
        std::lock_guard<std::mutex> lk(log_mtx);
        for (const auto& l : logs)
            if (l.find(needle) != std::string::npos) return true;
        return false;
    }
};

void connect(Endpoint& from, Endpoint& to) {
    from.node->connect("127.0.0.1", to.port());
}

// A bare librats node that speaks the rasync channel by hand. An Endpoint can
// only ever behave itself; these tests need a peer that lies — about the protocol
// version it speaks, about the folder it is in, or about the paths its tree contains.
struct RawPeer {
    test::TempDir                  dir;
    std::unique_ptr<librats::Node> node;
    std::string                    folder = kFolder;
    uint32_t                       tag    = proto::folder_tag(kFolder);

    explicit RawPeer(const std::string& folder_name = kFolder)
        : folder(folder_name), tag(proto::folder_tag(folder_name)) {
        librats::NodeConfig ncfg;
        ncfg.listen_port = 0;
        ncfg.bind_address = "127.0.0.1";
        ncfg.protocol = kProtocolName;
        ncfg.data_dir = dir.str();
        ncfg.enable_network_monitor = false;
        node = std::make_unique<librats::Node>(ncfg);
    }
    ~RawPeer() { if (node) node->stop(); }

    /// Handle inbound messages for *our* folder, header already peeled off.
    /// Register before start().
    template <typename Fn>
    void on_message(Fn fn) {
        const uint32_t want = tag;
        node->on(proto::kChannel, [fn, want](const librats::Peer& peer, librats::ByteView payload) {
            BinaryReader r(payload.data(), payload.size());
            const uint32_t got = r.u32();
            auto op = static_cast<proto::Op>(r.u8());
            if (r.ok() && got == want) fn(peer.id(), op, r);
        });
    }

    /// Handle every inbound message, whatever folder it is tagged for.
    template <typename Fn>
    void on_any(Fn fn) {
        node->on(proto::kChannel, [fn](const librats::Peer& peer, librats::ByteView payload) {
            BinaryReader r(payload.data(), payload.size());
            const uint32_t got = r.u32();
            auto op = static_cast<proto::Op>(r.u8());
            if (r.ok()) fn(peer.id(), got, op, r);
        });
    }

    void send(const librats::PeerId& to, BinaryWriter& w) {
        node->send(to, proto::kChannel, librats::ByteView(w.buffer()));
    }

    BinaryWriter message(proto::Op op) const { return proto::message(tag, op); }

    /// A Hello claiming `version`, `mode`, `conflict` and a folder name. Each is
    /// something a real misconfiguration can get wrong, and only a peer we control
    /// can get it wrong on purpose.
    void say_hello(const librats::PeerId& to, uint8_t version,
                   SyncMode mode = SyncMode::TwoWay,
                   ConflictPolicy conflict = ConflictPolicy::Newer) {
        say_hello_as(to, version, mode, conflict, folder);
    }

    void say_hello_as(const librats::PeerId& to, uint8_t version, SyncMode mode,
                      ConflictPolicy conflict, const std::string& claimed_folder) {
        auto w = message(proto::Op::Hello);
        w.u8(version);
        w.u8(static_cast<uint8_t>(mode));
        w.u8(static_cast<uint8_t>(conflict));
        w.str16(claimed_folder);
        send(to, w);
    }
};

FileMeta meta_of(const std::string& content, int64_t mtime = 1000) {
    FileMeta m;
    m.size = content.size();
    m.mtime = mtime;
    m.hash = sha256(content);
    return m;
}

} // namespace

TEST(SyncIntegration, InitialPopulateReplica) {
    Endpoint a, b;
    test::write_file(a.first().sub("a.txt"), "hello world");
    test::write_file(a.first().sub("sub/b.txt"), "nested content");

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] {
        Manifest da = disk_state(a.first().root), db = disk_state(b.first().root);
        return db.size() == 2 && da.fingerprint() == db.fingerprint();
    })) << "B never converged to A";

    EXPECT_EQ(test::read_file(b.first().sub("a.txt")), "hello world");
    EXPECT_EQ(test::read_file(b.first().sub("sub/b.txt")), "nested content");
}

TEST(SyncIntegration, TwoWayMergesBothDirections) {
    Endpoint a, b;
    test::write_file(a.first().sub("from_a.txt"), "content A");
    test::write_file(b.first().sub("from_b.txt"), "content B");

    a.start();
    b.start();
    connect(a, b);

    ASSERT_TRUE(wait_until([&] {
        Manifest da = disk_state(a.first().root), db = disk_state(b.first().root);
        return da.size() == 2 && db.size() == 2 && da.fingerprint() == db.fingerprint();
    })) << "two-way merge did not converge";

    EXPECT_EQ(test::read_file(a.first().sub("from_b.txt")), "content B");
    EXPECT_EQ(test::read_file(b.first().sub("from_a.txt")), "content A");
}

TEST(SyncIntegration, ModificationPropagatesAsDelta) {
    Endpoint a, b;
    // A sizable file so the change is a small fraction (delta path).
    auto original = test::random_bytes(400 * 1024, 123);
    test::write_file(a.first().sub("big.bin"), original);

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] {
        return disk_state(b.first().root).contains("big.bin") &&
               disk_state(a.first().root).fingerprint() == disk_state(b.first().root).fingerprint();
    })) << "initial big file did not sync";

    // Modify a small middle region on A and re-publish. Also change the length so
    // the size+mtime quick-check reliably flags it (mtime has 1-second resolution,
    // like rsync — a same-size edit within the same second would otherwise hide).
    auto modified = original;
    for (size_t i = 200000; i < 200100; ++i) modified[i] ^= 0xaa;
    auto extra = test::random_bytes(37, 999);
    modified.insert(modified.end(), extra.begin(), extra.end());
    test::write_file(a.first().sub("big.bin"), modified);
    a.publish();

    ASSERT_TRUE(wait_until([&] {
        return disk_state(a.first().root).fingerprint() == disk_state(b.first().root).fingerprint() &&
               test::read_file(b.first().sub("big.bin")).size() == modified.size();
    })) << "modification did not propagate";

    std::string got = test::read_file(b.first().sub("big.bin"));
    ASSERT_EQ(got.size(), modified.size());
    EXPECT_EQ(0, std::memcmp(got.data(), modified.data(), modified.size()));
}

// A delta whose literal run exceeds the send window (SyncConfig::window_bytes)
// makes the sender genuinely block on FileAck, which the small-file delta test
// above never does. The receiver must ack *wire* bytes: acking the reconstructed
// size instead counts COPY bytes the sender never sent, so its window counter
// runs backwards (unsigned) and the sender thread wedges for good.
TEST(SyncIntegration, LargeDeltaWithCopiesDrainsTheSendWindow) {
    Endpoint a, b;
    // The COPY run must outweigh the literals: only then does the receiver's byte
    // count stay ahead of the sender's for the whole transfer, which is what turns
    // a mis-scoped ack into a permanently unsatisfiable window.
    constexpr size_t kPrefix = 8 * 1024 * 1024;  // untouched → one large COPY
    constexpr size_t kTail   = 6 * 1024 * 1024;  // rewritten → literals past the window
    const std::string rel = "huge.bin";
    auto original = test::random_bytes(kPrefix + kTail, 7);
    test::write_file(a.first().sub(rel), original);

    // Hashing a 14 MiB tree on every poll would dominate the test, and the final
    // path only ever gets the new content after verification + atomic rename — so
    // its size is a sound (and cheap) convergence signal.
    auto b_size = [&](size_t want) {
        std::error_code ec;
        return std::filesystem::file_size(b.first().sub(rel), ec) == want;
    };

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] { return b_size(original.size()); }))
        << "initial whole-file transfer did not converge";

    // Replace everything past the first MiB, and change the length so the
    // size+mtime quick-check reliably flags the edit.
    auto modified = original;
    modified.resize(kPrefix);
    auto rewritten = test::random_bytes(kTail + 11, 99);
    modified.insert(modified.end(), rewritten.begin(), rewritten.end());
    test::write_file(a.first().sub(rel), modified);
    a.publish();

    ASSERT_TRUE(wait_until([&] { return b_size(modified.size()); }))
        << "large delta stalled — the sender never drained its send window";

    std::string got = test::read_file(b.first().sub(rel));
    ASSERT_EQ(got.size(), modified.size());
    EXPECT_EQ(0, std::memcmp(got.data(), modified.data(), modified.size()));
}

// ── --follow-symlinks, end to end ────────────────────────────────────────────
//
// The point of the option is a peer that has no links of its own — a Windows host
// receiving a Linux tree published through them. So what these check is that the
// *content* crosses as ordinary files, and that an update coming back lands on the
// data the link points at instead of overwriting the link with a copy.

TEST(SyncIntegration, FollowedLinksReachThePeerAsOrdinaryFilesAndDirectories) {
    test::TempDir outside;  // the real data, deliberately not inside the tree
    test::write_file(outside.sub("data.txt"), "linked file content");
    test::write_file(outside.sub("sub/deep.txt"), "linked directory content");

    Endpoint a(SyncMode::TwoWay, /*source=*/false, /*max_update_bytes=*/0,
               ConflictPolicy::Newer, /*key=*/{}, /*allow=*/{}, /*follow_symlinks=*/true);
    Endpoint b;  // the other machine follows nothing — it has no links to follow
    test::write_file(a.first().sub("plain.txt"), "an ordinary file");
    if (!test::make_symlink(outside.path() / "data.txt", a.first().sub("link.txt"),
                            /*directory=*/false) ||
        !test::make_symlink(outside.path(), a.first().sub("linked-dir"),
                            /*directory=*/true))
        GTEST_SKIP() << "symlinks unavailable on this host";

    a.start();
    b.start();
    connect(b, a);

    // Wait for the whole round, not just for the linked entries: plain.txt sorts
    // last of the four, so "the links arrived" is true one file before the tree is
    // complete — and the fingerprint below would then compare 3 entries against 4.
    ASSERT_TRUE(wait_until([&] {
        Manifest db = disk_state(b.first().root);
        return db.contains("link.txt") && db.contains("linked-dir/sub/deep.txt") &&
               disk_state(a.first().root, /*follow_symlinks=*/true).fingerprint() ==
                   db.fingerprint();
    })) << "the linked content never reached the peer";

    EXPECT_EQ(test::read_file(b.first().sub("link.txt")), "linked file content");
    EXPECT_EQ(test::read_file(b.first().sub("linked-dir/data.txt")), "linked file content");
    EXPECT_EQ(test::read_file(b.first().sub("linked-dir/sub/deep.txt")),
              "linked directory content");
    // Real files and a real directory on the receiving side: nothing about the
    // link survives the wire, which is what makes this work on a host without them.
    std::error_code ec;
    EXPECT_FALSE(std::filesystem::is_symlink(
        std::filesystem::symlink_status(b.first().sub("link.txt"), ec)));
    EXPECT_FALSE(std::filesystem::is_symlink(
        std::filesystem::symlink_status(b.first().sub("linked-dir"), ec)));

    // A follows links, B does not — and the two trees still agree, each read the
    // way its own side reads it. Nothing about following is negotiated.
    EXPECT_EQ(disk_state(a.first().root, /*follow_symlinks=*/true).fingerprint(),
              disk_state(b.first().root).fingerprint());
}

TEST(SyncIntegration, AnUpdateToAFollowedLinkIsWrittenThroughToItsTarget) {
    test::TempDir outside;
    test::write_file(outside.sub("data.txt"), "first version");

    Endpoint a(SyncMode::TwoWay, /*source=*/false, /*max_update_bytes=*/0,
               ConflictPolicy::Newer, /*key=*/{}, /*allow=*/{}, /*follow_symlinks=*/true);
    Endpoint b;
    if (!test::make_symlink(outside.path() / "data.txt", a.first().sub("link.txt"),
                            /*directory=*/false))
        GTEST_SKIP() << "symlinks unavailable on this host";

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] {
        return test::read_file(b.first().sub("link.txt")) == "first version";
    })) << "the linked file never reached the peer";

    // B edits its ordinary copy and wins the round (mtime resolution is one
    // second, so the new version is pushed clear of a tie rather than raced).
    test::write_file(b.first().sub("link.txt"), "second version, edited on the peer");
    std::error_code ec;
    std::filesystem::last_write_time(
        b.first().sub("link.txt"),
        std::filesystem::last_write_time(b.first().sub("link.txt"), ec) + seconds(10), ec);
    b.publish();

    // The bytes belong to the file the link points at — which is outside the
    // synced tree, exactly where the user put it.
    ASSERT_TRUE(wait_until([&] {
        return test::read_file(outside.sub("data.txt")) == "second version, edited on the peer";
    })) << "the update never reached the link's target";

    // And the link is still a link: replacing it with a copy would leave the tree
    // holding data that no longer tracks what it was pointed at.
    EXPECT_TRUE(std::filesystem::is_symlink(
        std::filesystem::symlink_status(a.first().sub("link.txt"), ec)));
    EXPECT_EQ(test::read_file(a.first().sub("link.txt")),
              "second version, edited on the peer");
}

TEST(SyncIntegration, DeletionPropagates) {
    Endpoint a, b;
    test::write_file(a.first().sub("keep.txt"), "keep");
    test::write_file(a.first().sub("remove.txt"), "remove me");

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] { return disk_state(b.first().root).size() == 2; }))
        << "initial files did not sync";

    // Delete on A, re-publish; the deletion should reach B.
    std::filesystem::remove(a.first().sub("remove.txt"));
    a.publish();

    ASSERT_TRUE(wait_until([&] {
        Manifest db = disk_state(b.first().root);
        return !db.contains("remove.txt") && db.contains("keep.txt");
    })) << "deletion did not propagate";
}

TEST(SyncIntegration, PropagatedDeletionPrunesEmptyDirectories) {
    Endpoint a, b;
    // A transfer creates parent directories on the way in, so B ends up holding
    // nested/deep/ purely because gone.txt arrived through it.
    test::write_file(a.first().sub("nested/deep/gone.txt"), "x");
    test::write_file(a.first().sub("nested/keep.txt"), "y");

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] { return disk_state(b.first().root).size() == 2; }))
        << "initial files did not sync";
    ASSERT_TRUE(std::filesystem::exists(b.first().sub("nested/deep")));

    std::filesystem::remove(a.first().sub("nested/deep/gone.txt"));
    a.publish();

    ASSERT_TRUE(wait_until([&] { return !disk_state(b.first().root).contains("nested/deep/gone.txt"); }))
        << "deletion did not propagate";

    // Poll for the pruning rather than asserting straight away: it happens just
    // after the unlink, so the file being gone does not yet prove it has run.
    ASSERT_TRUE(wait_until([&] { return !std::filesystem::exists(b.first().sub("nested/deep")); }))
        << "the directory the deletion emptied was not pruned";

    // Pruning stops at the first ancestor that still holds something.
    EXPECT_TRUE(std::filesystem::exists(b.first().sub("nested/keep.txt")));
    EXPECT_TRUE(std::filesystem::exists(b.first().root));
}

TEST(SyncIntegration, StaleTempFilesAreClearedAtStartup) {
    Endpoint a;
    // A leftover from a killed run: the name a session would pick again, since
    // xids restart at 1. If it survived, open_write would append into it.
    std::string stale = a.first().sub(std::string(kTempDirName) + "/dead-1.part");
    test::write_file(stale, std::string(4096, 'x'));
    ASSERT_TRUE(std::filesystem::exists(stale));

    a.first().svc->clean_temp_dir();

    EXPECT_FALSE(std::filesystem::exists(stale));
    EXPECT_FALSE(std::filesystem::exists(a.first().sub(kTempDirName)));
}

TEST(SyncIntegration, TempFilesDoNotOutliveTheTransfer) {
    Endpoint a, b;
    test::write_file(a.first().sub("payload.bin"), test::random_bytes(300 * 1024, 7));

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] {
        return disk_state(a.first().root).fingerprint() == disk_state(b.first().root).fingerprint() &&
               disk_state(b.first().root).contains("payload.bin");
    })) << "file did not sync";

    // A converged receiver must leave no .part behind: every temp file is either
    // renamed into place or dropped by ~Incoming.
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(b.first().sub(kTempDirName), ec))
        ADD_FAILURE() << "leftover temp file: " << e.path().string();
}

TEST(SyncIntegration, ManifestTrafficStaysProportionalToChanges) {
    Endpoint a, b;
    constexpr int kFiles = 120;
    for (int i = 0; i < kFiles; ++i)
        test::write_file(a.first().sub("f" + std::to_string(i) + ".txt"),
                         "payload " + std::to_string(i));

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] {
        return disk_state(b.first().root).size() == kFiles &&
               disk_state(a.first().root).fingerprint() == disk_state(b.first().root).fingerprint();
    })) << "files did not sync";

    // B pulled 120 files. Describing its tree once per received file — the
    // behaviour incremental updates replaced — costs ~121 messages and ~435 KB.
    // Two are enough: the opening reset, and one coalesced update once the
    // inbound queue drained (measured: 2 messages, ~7 KB).
    EXPECT_LE(b.first().svc->advert_messages(), 4u)
        << "B re-described its tree far more often than it changed";
    EXPECT_LT(b.first().svc->advert_bytes(), 32u * 1024)
        << "manifest bookkeeping grew with the tree, not with the changes";

    // A's tree never changed, so after the opening description it says nothing.
    EXPECT_LE(a.first().svc->advert_messages(), 2u);
}

TEST(SyncIntegration, ConvergenceIsAnnouncedAndBaselinePersisted) {
    // Convergence is now decided behind an O(1) "did anything move?" gate, and
    // `--once` plus two-way delete propagation both hang off this event and the
    // baseline it writes — so both are worth pinning down.
    Endpoint a, b;
    test::write_file(a.first().sub("one.txt"), "1");
    test::write_file(a.first().sub("two.txt"), "2");

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] {
        return a.first().synced_events.load() > 0 && b.first().synced_events.load() > 0;
    })) << "neither side ever announced convergence";

    EXPECT_TRUE(std::filesystem::exists(b.first().state.sub("baseline.man")))
        << "no baseline persisted — deletes could not propagate on the next run";

    // Nothing changed afterwards, so nothing re-announces it.
    const int seen = b.first().synced_events.load();
    std::this_thread::sleep_for(milliseconds(400));
    EXPECT_EQ(b.first().synced_events.load(), seen) << "convergence announced repeatedly";
}

TEST(SyncIntegration, SyncedDirectoryHoldsNothingButTheSyncedFiles) {
    // rasync's own state lives outside the tree; the scratch directory is the one
    // thing it may create inside it, and it must not survive the transfer that
    // needed it — let alone the process.
    Endpoint a, b;
    for (int i = 0; i < 4; ++i)
        test::write_file(a.first().sub("f" + std::to_string(i) + ".bin"),
                         test::random_bytes(64 * 1024, static_cast<uint32_t>(i + 1)));

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] { return disk_state(b.first().root).size() == 4; })) << "files did not sync";

    const std::filesystem::path scratch = std::filesystem::path(b.first().root) / kTempDirName;
    EXPECT_TRUE(wait_until([&] { return !std::filesystem::exists(scratch); }, milliseconds(5000)))
        << "scratch directory outlived the transfers that needed it";

    b.first().svc->shutdown();
    std::vector<std::string> left;
    for (const auto& e : std::filesystem::directory_iterator(b.first().root))
        left.push_back(e.path().filename().string());
    std::sort(left.begin(), left.end());
    EXPECT_EQ(left, (std::vector<std::string>{"f0.bin", "f1.bin", "f2.bin", "f3.bin"}))
        << "rasync left something of its own in the synced directory";
}

TEST(SyncIntegration, LargeManifestUpdatesAreChunkedAndReassembled) {
    // A's updates are capped absurdly low, so even a handful of files has to go
    // out as several kUpdateMore chunks. The receiver must hold them all back and
    // apply them at once — a half-applied tree reads as "the peer deleted the
    // rest", which would propagate as deletions.
    Endpoint a(SyncMode::TwoWay, false, /*max_update_bytes=*/80);
    Endpoint b;
    for (int i = 0; i < 6; ++i)
        test::write_file(a.first().sub("chunk" + std::to_string(i) + ".txt"), "c");
    test::write_file(b.first().sub("only_on_b.txt"), "b");

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] {
        Manifest da = disk_state(a.first().root), db = disk_state(b.first().root);
        return da.size() == 7 && db.size() == 7 && da.fingerprint() == db.fingerprint();
    })) << "chunked manifest update did not converge";

    // The cap really did split the description into several messages.
    EXPECT_GE(a.first().svc->advert_messages(), 3u);
    // Nothing was lost to a partially applied view.
    EXPECT_TRUE(disk_state(b.first().root).contains("only_on_b.txt"));
    EXPECT_TRUE(disk_state(a.first().root).contains("only_on_b.txt"));
}

TEST(SyncIntegration, PeerWithAnIncompatibleProtocolVersionIsRefused) {
    // v1 and v2 describe a tree differently, so a mismatched pair that keeps
    // talking just logs a malformed message per advertisement and never
    // converges. The session must say Bye once and go inert instead.
    Endpoint a;
    test::write_file(a.first().sub("a.txt"), "content");
    a.start();

    std::atomic<int> byes{0};
    std::atomic<int> updates{0};
    RawPeer peer;
    peer.on_message([&](const librats::PeerId& from, proto::Op op, BinaryReader&) {
        switch (op) {
            case proto::Op::Hello:          peer.say_hello(from, /*version=*/1); break;
            case proto::Op::Bye:            byes.fetch_add(1); break;
            case proto::Op::ManifestUpdate: updates.fetch_add(1); break;
            default: break;
        }
    });
    ASSERT_TRUE(peer.node->start());
    peer.node->connect("127.0.0.1", a.port());

    ASSERT_TRUE(wait_until([&] { return byes.load() > 0; }, milliseconds(10000)))
        << "the mismatched peer was never told the session is off";

    // And A must not describe a tree the peer provably cannot read.
    std::this_thread::sleep_for(milliseconds(300));
    EXPECT_EQ(updates.load(), 0) << "A advertised to an incompatible peer";
    EXPECT_EQ(a.first().svc->advert_messages(), 0u);
    EXPECT_EQ(byes.load(), 1) << "refusal announced more than once";
}

TEST(SyncIntegration, PeerWithANonComplementaryConflictPolicyIsRefused) {
    // Two peers converge only because their plans are complements, which holds only
    // while they resolve conflicts complementarily. A mismatched pair either stalls
    // forever (both sides think they won) or swaps the two versions round after round
    // (both think they lost) — and neither says so. Catch it at the handshake.
    Endpoint a;  // default policy: newer, whose complement is newer
    test::write_file(a.first().sub("a.txt"), "content");
    a.start();

    std::atomic<int> byes{0};
    std::atomic<int> updates{0};
    RawPeer peer;
    peer.on_message([&](const librats::PeerId& from, proto::Op op, BinaryReader&) {
        switch (op) {
            case proto::Op::Hello:
                peer.say_hello(from, proto::kVersion, SyncMode::TwoWay,
                               ConflictPolicy::PreferLocal);  // wants to always win
                break;
            case proto::Op::Bye:            byes.fetch_add(1); break;
            case proto::Op::ManifestUpdate: updates.fetch_add(1); break;
            default: break;
        }
    });
    ASSERT_TRUE(peer.node->start());
    peer.node->connect("127.0.0.1", a.port());

    ASSERT_TRUE(wait_until([&] { return byes.load() > 0; }, milliseconds(10000)))
        << "the mismatched peer was never told the session is off";

    std::this_thread::sleep_for(milliseconds(300));
    EXPECT_EQ(updates.load(), 0) << "A synced with a peer it can never converge with";
    EXPECT_EQ(a.first().svc->advert_messages(), 0u);
    EXPECT_EQ(byes.load(), 1) << "refusal announced more than once";
}

TEST(SyncIntegration, ComplementaryConflictPoliciesArePairedUp) {
    // The other half of the rule: "local" is meant to pair with "remote", and that
    // pairing must sail straight through — the check has to reject a bad pair, not
    // every pair that isn't identical.
    Endpoint a(SyncMode::TwoWay, /*source=*/false, /*max_update_bytes=*/0,
               ConflictPolicy::PreferLocal);
    test::write_file(a.first().sub("a.txt"), "content");
    a.start();

    std::atomic<int> byes{0};
    std::atomic<int> updates{0};
    RawPeer peer;
    peer.on_message([&](const librats::PeerId& from, proto::Op op, BinaryReader&) {
        switch (op) {
            case proto::Op::Hello:
                peer.say_hello(from, proto::kVersion, SyncMode::TwoWay,
                               ConflictPolicy::PreferRemote);  // agrees to lose
                break;
            case proto::Op::Bye:            byes.fetch_add(1); break;
            case proto::Op::ManifestUpdate: updates.fetch_add(1); break;
            default: break;
        }
    });
    ASSERT_TRUE(peer.node->start());
    peer.node->connect("127.0.0.1", a.port());

    ASSERT_TRUE(wait_until([&] { return updates.load() > 0; }, milliseconds(10000)))
        << "a complementary peer was not synced with";
    EXPECT_EQ(byes.load(), 0) << "a complementary peer was refused";
}

TEST(SyncIntegration, MirrorIgnoresTheConflictPolicyPairing) {
    // Mirror mode never consults the policy — one side is authoritative by
    // construction — so a policy that would be refused in two-way mode must not
    // cost a mirror pair its session.
    Endpoint src(SyncMode::Mirror, /*source=*/true);
    test::write_file(src.first().sub("doc.txt"), "authoritative");
    src.start();

    std::atomic<int> byes{0};
    std::atomic<int> updates{0};
    RawPeer peer;
    peer.on_message([&](const librats::PeerId& from, proto::Op op, BinaryReader&) {
        switch (op) {
            case proto::Op::Hello:
                peer.say_hello(from, proto::kVersion, SyncMode::Mirror,
                               ConflictPolicy::PreferLocal);
                break;
            case proto::Op::Bye:            byes.fetch_add(1); break;
            case proto::Op::ManifestUpdate: updates.fetch_add(1); break;
            default: break;
        }
    });
    ASSERT_TRUE(peer.node->start());
    peer.node->connect("127.0.0.1", src.port());

    ASSERT_TRUE(wait_until([&] { return updates.load() > 0; }, milliseconds(10000)))
        << "a mirror peer was refused over a policy mirror mode does not use";
    EXPECT_EQ(byes.load(), 0);
}

TEST(SyncIntegration, UnsafePeerPathsNeverEnterThePullWindow) {
    // A path that would escape the sync root has to be dropped where the peer's
    // tree is applied. Reaching the pull queue is already too late: we would
    // request a file we then refuse to write, and the reservation would hold one
    // of the few pull-window slots for the rest of the session.
    Endpoint a;
    a.start();

    std::mutex               mtx;
    std::vector<std::string> requested;
    RawPeer peer;
    peer.on_message([&](const librats::PeerId& from, proto::Op op, BinaryReader& r) {
        if (op == proto::Op::Hello) {
            peer.say_hello(from, proto::kVersion);

            ManifestPatch patch;
            patch.set.emplace_back("../rasync-escaped-a.txt", meta_of("escaped"));
            patch.set.emplace_back("sub/../../rasync-escaped-b.txt", meta_of("escaped too"));
            patch.set.emplace_back("ok.txt", meta_of("legitimate"));
            auto w = peer.message(proto::Op::ManifestUpdate);
            w.u8(proto::kUpdateReset);
            patch.encode(w);
            peer.send(from, w);
        } else if (op == proto::Op::Request) {
            std::lock_guard<std::mutex> lk(mtx);
            requested.push_back(r.str16());
        }
    });
    ASSERT_TRUE(peer.node->start());
    peer.node->connect("127.0.0.1", a.port());

    ASSERT_TRUE(wait_until([&] {
        std::lock_guard<std::mutex> lk(mtx);
        return !requested.empty();
    }, milliseconds(10000))) << "the legitimate file was never requested";

    // Give any further request time to show up before concluding there is none.
    std::this_thread::sleep_for(milliseconds(300));
    std::lock_guard<std::mutex> lk(mtx);
    EXPECT_EQ(requested.size(), 1u);
    EXPECT_EQ(requested.front(), "ok.txt");
    EXPECT_FALSE(std::filesystem::exists(a.first().sub("../rasync-escaped-a.txt")));
    EXPECT_FALSE(std::filesystem::exists(a.first().sub("../rasync-escaped-b.txt")));
}

TEST(SyncIntegration, PeersSharingAKeySync) {
    Endpoint a(SyncMode::TwoWay, false, 0, ConflictPolicy::Newer, "shared-secret");
    Endpoint b(SyncMode::TwoWay, false, 0, ConflictPolicy::Newer, "shared-secret");
    test::write_file(a.first().sub("doc.txt"), "keyed content");

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] {
        return disk_state(b.first().root).contains("doc.txt");
    })) << "peers with the same key never synced";
    EXPECT_EQ(test::read_file(b.first().sub("doc.txt")), "keyed content");
}

TEST(SyncIntegration, PeerWithTheWrongKeyGetsNothing) {
    // The key is folded into the protocol id, which librats binds into the Noise
    // prologue: the wrong key cannot finish the handshake at all, so this holds
    // however the peer reached us — dialed by hand, as here, or found over DHT.
    Endpoint a(SyncMode::TwoWay, false, 0, ConflictPolicy::Newer, "right-key");
    Endpoint b(SyncMode::TwoWay, false, 0, ConflictPolicy::Newer, "wrong-key");
    test::write_file(a.first().sub("private.txt"), "not for you");
    test::write_file(b.first().sub("theirs.txt"), "not for them either");

    a.start();
    b.start();
    connect(b, a);

    EXPECT_FALSE(wait_until([&] {
        return disk_state(b.first().root).contains("private.txt") ||
               disk_state(a.first().root).contains("theirs.txt");
    }, milliseconds(3000))) << "the tree crossed a key boundary";
    EXPECT_EQ(a.first().synced_events.load(), 0);
    EXPECT_EQ(b.first().synced_events.load(), 0);
}

TEST(SyncIntegration, AllowListIgnoresAnUnlistedPeer) {
    // Same key (none here), so the handshake succeeds — the allow-list is the
    // layer that still has to refuse, and it must refuse in both directions.
    auto stranger = librats::PeerId::from_hex(std::string(64, 'a'));
    ASSERT_TRUE(stranger.has_value());
    Endpoint a(SyncMode::TwoWay, false, 0, ConflictPolicy::Newer, {}, {*stranger});
    Endpoint b;
    test::write_file(a.first().sub("private.txt"), "not for you");
    test::write_file(b.first().sub("theirs.txt"), "not for them either");

    a.start();
    b.start();
    connect(b, a);

    EXPECT_FALSE(wait_until([&] {
        return disk_state(b.first().root).contains("private.txt") ||
               disk_state(a.first().root).contains("theirs.txt");
    }, milliseconds(3000))) << "an unlisted peer was served";
    EXPECT_EQ(a.first().synced_events.load(), 0);
}

TEST(SyncIntegration, AllowListAdmitsAListedPeer) {
    Endpoint b;  // built first: A can only list an id that already exists
    Endpoint a(SyncMode::TwoWay, false, 0, ConflictPolicy::Newer, {}, {b.node->local_id()});
    test::write_file(a.first().sub("doc.txt"), "allowed content");

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] {
        return disk_state(b.first().root).contains("doc.txt");
    })) << "a listed peer was refused";
    EXPECT_EQ(test::read_file(b.first().sub("doc.txt")), "allowed content");
}

TEST(SyncIntegration, MirrorReplicaFollowsSource) {
    Endpoint src(SyncMode::Mirror, /*source=*/true);
    Endpoint rep(SyncMode::Mirror, /*source=*/false);

    test::write_file(src.first().sub("doc.txt"), "authoritative");
    test::write_file(rep.first().sub("stale.txt"), "should be removed");

    src.start();
    rep.start();
    connect(rep, src);

    ASSERT_TRUE(wait_until([&] {
        Manifest dr = disk_state(rep.first().root);
        return dr.contains("doc.txt") && !dr.contains("stale.txt");
    })) << "replica did not mirror the source";

    EXPECT_EQ(test::read_file(rep.first().sub("doc.txt")), "authoritative");
    // The source must be untouched.
    EXPECT_FALSE(disk_state(src.first().root).contains("stale.txt"));
}

// ── many folders over one connection ─────────────────────────────────────────
//
// Every test below runs endpoints holding more than one folder. What they check
// is that the folder tag both *reaches* the right tree and *never* reaches the
// wrong one — the two halves of the only new claim the multi-folder protocol makes.

TEST(SyncIntegration, TwoFoldersSyncIndependentlyOverOneConnection) {
    Endpoint a, b;
    Folder& a2 = a.add_folder("second");
    Folder& b2 = b.add_folder("second");

    test::write_file(a.first().sub("one.txt"), "from the first folder");
    test::write_file(a2.sub("two.txt"), "from the second folder");
    // Each direction in a different folder, so neither can be carrying the other.
    test::write_file(b2.sub("three.txt"), "sent back the other way");

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] {
        return disk_state(b.first().root).contains("one.txt") &&
               disk_state(b2.root).contains("two.txt") &&
               disk_state(a2.root).contains("three.txt");
    })) << "the two folders did not both converge";

    EXPECT_EQ(test::read_file(b.first().sub("one.txt")), "from the first folder");
    EXPECT_EQ(test::read_file(b2.sub("two.txt")), "from the second folder");
    EXPECT_EQ(test::read_file(a2.sub("three.txt")), "sent back the other way");

    // One TCP connection carried all of it.
    EXPECT_EQ(a.node->connected_peers().size(), 1u);
}

TEST(SyncIntegration, AFileNeverLandsInTheWrongFolder) {
    // The failure this protocol most has to avoid: routing by a 32-bit tag and
    // writing one of a peer's trees into another of them.
    Endpoint a, b;
    Folder& a2 = a.add_folder("second");
    Folder& b2 = b.add_folder("second");

    test::write_file(a.first().sub("only-in-first.txt"), "x");
    test::write_file(a2.sub("only-in-second.txt"), "y");

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] {
        return disk_state(b.first().root).contains("only-in-first.txt") &&
               disk_state(b2.root).contains("only-in-second.txt");
    })) << "the folders never converged";

    // Give anything mis-routed time to arrive before concluding nothing was.
    std::this_thread::sleep_for(milliseconds(400));
    Manifest one = disk_state(b.first().root), two = disk_state(b2.root);
    EXPECT_EQ(one.size(), 1u);
    EXPECT_EQ(two.size(), 1u);
    EXPECT_FALSE(one.contains("only-in-second.txt"));
    EXPECT_FALSE(two.contains("only-in-first.txt"));
}

TEST(SyncIntegration, EachFolderKeepsItsOwnBaseline) {
    // Convergence is per folder, and so is the state recording it — a shared
    // baseline would make one folder's deletes look like the other's.
    Endpoint a, b;
    Folder& a2 = a.add_folder("second");
    Folder& b2 = b.add_folder("second");
    test::write_file(a.first().sub("one.txt"), "x");
    test::write_file(a2.sub("two.txt"), "y");

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] {
        return b.first().synced_events.load() > 0 && b2.synced_events.load() > 0;
    })) << "not every folder announced convergence";

    ASSERT_TRUE(wait_until([&] {
        return std::filesystem::exists(b.first().state.sub("baseline.man")) &&
               std::filesystem::exists(b2.state.sub("baseline.man"));
    })) << "a folder did not persist its own baseline";
}

TEST(SyncIntegration, AFolderThePeerDoesNotHaveCostsNothingButAHello) {
    // A folder the peer lacks must not open a session: a session owns threads and
    // per-transfer state, and a node with many folders would otherwise pay for all
    // of them against every peer. The observable proof is that the lonely folder
    // never describes its tree, because only a session ever advertises.
    Endpoint a, b;
    Folder& lonely = a.add_folder("nobody-elses");
    test::write_file(a.first().sub("shared.txt"), "carried");
    test::write_file(lonely.sub("private.txt"), "never described");

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] {
        return disk_state(b.first().root).contains("shared.txt");
    })) << "the shared folder never converged";

    std::this_thread::sleep_for(milliseconds(400));
    EXPECT_EQ(lonely.svc->advert_messages(), 0u)
        << "a folder the peer does not have described its tree anyway";
    EXPECT_GT(a.first().svc->advert_messages(), 0u);
    // And B learns why, instead of the folder silently doing nothing.
    EXPECT_TRUE(b.logged("nobody-elses"))
        << "B was never told about a folder A offered that it does not have";
}

TEST(SyncIntegration, FoldersNamedDifferentlyOnEachSideNeverPairAndSaySo) {
    // The mistake this design makes easiest: one tree called "docs" here and
    // "documents" there. Nothing can pair them — but the user has to be told, or
    // it is indistinguishable from a stalled sync.
    Endpoint a, b;
    Folder& mine   = a.add_folder("docs");
    Folder& theirs = b.add_folder("documents");
    test::write_file(a.first().sub("shared.txt"), "this folder pairs");
    test::write_file(mine.sub("ours.txt"), "this one does not");

    a.start();
    b.start();
    connect(b, a);

    // The correctly-named folder still works, so this is not a broken connection.
    ASSERT_TRUE(wait_until([&] {
        return disk_state(b.first().root).contains("shared.txt");
    })) << "the matching folder never converged";

    ASSERT_TRUE(wait_until([&] {
        return b.logged("docs") && a.logged("documents");
    }, milliseconds(10000))) << "neither side reported the unpaired folder";

    std::this_thread::sleep_for(milliseconds(300));
    EXPECT_TRUE(disk_state(theirs.root).empty())
        << "an unpaired folder received files anyway";
    EXPECT_EQ(mine.svc->advert_messages(), 0u);
}

TEST(SyncIntegration, PerFolderModeAndPolicyAreIndependent) {
    // Mode and conflict policy are per folder, so one pair of trees can be a
    // two-way merge while another is a one-way mirror over the same connection.
    Endpoint src(SyncMode::Mirror, /*source=*/true);
    Endpoint rep(SyncMode::Mirror, /*source=*/false);

    // The mirrored pair is the endpoints' primary folder; put a two-way pair
    // beside it by retuning the template extra folders are built from.
    src.cfg.mode = SyncMode::TwoWay;
    src.cfg.source = false;
    rep.cfg.mode = SyncMode::TwoWay;
    Folder& shared_a = src.add_folder("both-ways");
    Folder& shared_b = rep.add_folder("both-ways");

    test::write_file(src.first().sub("pushed.txt"), "mirrored down");
    test::write_file(rep.first().sub("stale.txt"), "removed by the mirror");
    test::write_file(shared_b.sub("upstream.txt"), "travels up, two-way");

    src.start();
    rep.start();
    connect(rep, src);

    ASSERT_TRUE(wait_until([&] {
        Manifest mirrored = disk_state(rep.first().root);
        return mirrored.contains("pushed.txt") && !mirrored.contains("stale.txt") &&
               disk_state(shared_a.root).contains("upstream.txt");
    })) << "the mirror and the two-way folder did not both behave as configured";

    // The mirror source stays authoritative; the two-way folder does not.
    EXPECT_FALSE(disk_state(src.first().root).contains("stale.txt"));
    EXPECT_EQ(test::read_file(shared_a.sub("upstream.txt")), "travels up, two-way");
}

TEST(SyncIntegration, APeerClaimingAnotherFolderUnderOurTagIsRefused) {
    // Routing is by a 32-bit tag, so two unrelated names could in principle land
    // on one. Hello carries the name in full precisely so that case is caught —
    // here a peer forges it, which is the only way to reach the check on demand.
    Endpoint a;
    test::write_file(a.first().sub("secret.txt"), "must not be described");
    a.start();

    std::atomic<int> byes{0};
    std::atomic<int> updates{0};
    RawPeer peer;   // tagged for kFolder…
    peer.on_message([&](const librats::PeerId& from, proto::Op op, BinaryReader&) {
        switch (op) {
            // …but naming a different folder behind that tag.
            case proto::Op::Hello:
                peer.say_hello_as(from, proto::kVersion, SyncMode::TwoWay,
                                  ConflictPolicy::Newer, "a-different-folder");
                break;
            case proto::Op::Bye:            byes.fetch_add(1); break;
            case proto::Op::ManifestUpdate: updates.fetch_add(1); break;
            default: break;
        }
    });
    ASSERT_TRUE(peer.node->start());
    peer.node->connect("127.0.0.1", a.port());

    ASSERT_TRUE(wait_until([&] { return byes.load() > 0; }, milliseconds(10000)))
        << "the colliding peer was never refused";

    std::this_thread::sleep_for(milliseconds(300));
    EXPECT_EQ(updates.load(), 0) << "the tree was described to a peer in another folder";
    EXPECT_EQ(a.first().svc->advert_messages(), 0u);
    EXPECT_EQ(byes.load(), 1) << "refusal announced more than once";
}

TEST(SyncIntegration, MessagesForAnUnknownFolderAreDroppedNotMisrouted) {
    // Anything tagged for a folder we do not have must never reach one we do,
    // whatever the opcode. A ManifestUpdate is the dangerous case: acted on, it
    // would have us pull files into the wrong tree.
    Endpoint a;
    test::write_file(a.first().sub("mine.txt"), "unchanged");
    a.start();

    RawPeer peer("a-folder-a-does-not-have");
    std::atomic<int> requests{0};
    peer.on_any([&](const librats::PeerId& from, uint32_t, proto::Op op, BinaryReader&) {
        if (op == proto::Op::Hello) {
            peer.say_hello(from, proto::kVersion);
            ManifestPatch patch;
            patch.set.emplace_back("intruder.txt", meta_of("should never be pulled"));
            auto w = peer.message(proto::Op::ManifestUpdate);
            w.u8(proto::kUpdateReset);
            patch.encode(w);
            peer.send(from, w);
        } else if (op == proto::Op::Request) {
            requests.fetch_add(1);
        }
    });
    ASSERT_TRUE(peer.node->start());
    peer.node->connect("127.0.0.1", a.port());

    ASSERT_TRUE(wait_until([&] { return a.logged("a-folder-a-does-not-have"); },
                           milliseconds(10000)))
        << "A never reported the folder it was offered";

    std::this_thread::sleep_for(milliseconds(400));
    EXPECT_EQ(requests.load(), 0) << "A acted on a manifest for a folder it does not have";
    EXPECT_FALSE(disk_state(a.first().root).contains("intruder.txt"));
    EXPECT_EQ(disk_state(a.first().root).size(), 1u);
}

TEST(SyncIntegration, TheRouterRefusesTwoFoldersWithTheSameName) {
    // Two services on one tag would mean a peer's traffic for that name reaching
    // whichever registered first — silently, and for both trees.
    Endpoint a;
    EXPECT_EQ(a.router->folder_for(proto::folder_tag(kFolder)), a.first().svc);
    EXPECT_EQ(a.router->folder_named(kFolder), a.first().svc);

    SyncConfig dup;
    test::TempDir other_root, other_state;
    dup.root = other_root.str();
    dup.data_dir = other_state.str();
    dup.folder = kFolder;                       // the name is already taken
    EXPECT_EQ(a.router->add_folder(dup), nullptr);
    EXPECT_EQ(a.router->size(), 1u);
}

TEST(SyncIntegration, DeletesPropagateWithinTheirOwnFolder) {
    // Deletion needs a persisted baseline, and the baseline is per folder. A file
    // removed from one folder must vanish from that folder on the peer — and leave
    // an identically named file in the *other* folder alone.
    Endpoint a, b;
    Folder& a2 = a.add_folder("second");
    Folder& b2 = b.add_folder("second");
    test::write_file(a.first().sub("same-name.txt"), "in folder one");
    test::write_file(a2.sub("same-name.txt"), "in folder two");

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] {
        return disk_state(b.first().root).contains("same-name.txt") &&
               disk_state(b2.root).contains("same-name.txt") &&
               b.first().synced_events.load() > 0 && b2.synced_events.load() > 0;
    })) << "both folders never converged";

    std::filesystem::remove(a.first().sub("same-name.txt"));
    a.first().publish();

    ASSERT_TRUE(wait_until([&] {
        return !disk_state(b.first().root).contains("same-name.txt");
    })) << "the delete did not propagate";

    EXPECT_TRUE(disk_state(b2.root).contains("same-name.txt"))
        << "a delete in one folder removed the other folder's file of the same name";
    EXPECT_EQ(test::read_file(b2.sub("same-name.txt")), "in folder two");
}

// ── transport backpressure ───────────────────────────────────────────────────

namespace {

/// An endpoint whose connection queue is deliberately tiny. librats' send
/// high-water mark is per *connection*, and a small one reproduces, in a couple of
/// megabytes, the overrun that needed several folders and gigabytes to provoke in
/// the field: past the mark the peer is closed as a slow consumer, mid-transfer,
/// on a link that was never unhealthy.
std::unique_ptr<Endpoint> narrow_endpoint(size_t queue_bytes) {
    return std::make_unique<Endpoint>(SyncMode::TwoWay, /*source=*/false,
                                      /*max_update_bytes=*/0, ConflictPolicy::Newer,
                                      /*key=*/std::string{},
                                      /*allow=*/std::vector<librats::PeerId>{},
                                      /*follow_symlinks=*/false, queue_bytes);
}

bool file_is(const std::string& path, const std::vector<uint8_t>& want) {
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) != want.size()) return false;
    const std::string got = test::read_file(path);
    return got.size() == want.size() &&
           std::memcmp(got.data(), want.data(), want.size()) == 0;
}

} // namespace

// The positive control for the two tests below. They both assert that nothing was
// dropped as a slow consumer, and an assertion that something did *not* happen is
// only worth as much as the proof that it can happen at all and would be seen: if
// the close reason ever stopped reaching RouterEvents::peer_down, those tests
// would keep passing while measuring nothing.
//
// So this drives the failure deliberately. It sends straight at the node, with no
// gate in the path and the answer thrown away — which is precisely what librats
// now defines a slow consumer as, since the mark is weighed on the backlog as it
// stood *before* a message: one message is always queued whatever its size, and
// it is offering another on top of a backlog already past the mark that closes the
// connection.
TEST(SyncIntegration, AnUnpacedSenderIsReportedAsASlowConsumer) {
    constexpr size_t kQueue = 64 * 1024;
    auto a = narrow_endpoint(kQueue);
    Endpoint b(SyncMode::TwoWay, /*source=*/false, /*max_update_bytes=*/0,
               ConflictPolicy::Newer, /*key=*/std::string{}, /*allow=*/{},
               /*follow_symlinks=*/false, kQueue);
    a->start();
    b.start();
    connect(*a, b);
    ASSERT_TRUE(wait_until([&] { return a->peer_ups.load() > 0; })) << "never connected";

    // Each blob is four times the whole queue, so the backlog is past the mark
    // after the first one and every later send is the offence itself. The loop
    // stops as soon as it has provoked the drop rather than sending a fixed
    // amount, so a slower machine does not queue megabytes it never needed.
    const std::vector<uint8_t> blob(4 * kQueue, 0xAB);
    for (int i = 0; i < 500 && a->slow_consumer_downs.load() == 0; ++i)
        (void)a->node->send(b.node->local_id(), proto::kChannel, librats::ByteView(blob));

    ASSERT_TRUE(wait_until([&] { return a->slow_consumer_downs.load() > 0; },
                           milliseconds(5000)))
        << "an unpaced sender was never dropped, or the close reason never arrived "
           "(peer downs: " << a->peer_downs.load() << ")";
}

// The failure this whole mechanism exists for. One connection carries every
// folder, so the send high-water mark is shared — but a SyncSession is per
// (folder, peer) and used to pace itself against nothing but its own FileAck
// window. Four folders transferring at once therefore offered four windows' worth
// of bytes to a queue with room for one, and librats closed the peer as a slow
// consumer part-way through. Nothing here is unhealthy about the link: it is
// loopback, lossless and idle. The overrun is entirely the sender's own making,
// which is why no amount of network tuning ever helped it.
TEST(SyncIntegration, ManyFoldersDoNotOverrunOneSendQueue) {
    // The queue has to leave room for one chunk per concurrent sender: pacing acts
    // *after* a message is handed over, so four sessions can always be one chunk
    // ahead each (4 x 64 KiB). What it must not leave room for is four *windows* —
    // 16 MiB, which is what the sessions offered before the gate existed.
    constexpr size_t kQueue   = 1024 * 1024;      // high-water; low-water is a quarter
    constexpr size_t kPayload = 2 * 1024 * 1024;  // per folder, well past the queue
    const char* kNames[] = {"one", "two", "three", "four"};

    auto a = narrow_endpoint(kQueue);
    auto b = narrow_endpoint(kQueue);
    for (int i = 1; i < 4; ++i) { a->add_folder(kNames[i]); b->add_folder(kNames[i]); }

    std::vector<std::vector<uint8_t>> payloads;
    for (int i = 0; i < 4; ++i) {
        payloads.push_back(test::random_bytes(kPayload, 4242 + i));
        test::write_file(a->folders[i]->sub("bulk.bin"), payloads.back());
    }

    a->start();
    b->start();
    connect(*b, *a);

    ASSERT_TRUE(wait_until([&] {
        for (int i = 0; i < 4; ++i)
            if (!file_is(b->folders[i]->sub("bulk.bin"), payloads[i])) return false;
        return true;
    }, milliseconds(90000)))
        << "four folders never all converged — the connection was almost certainly "
           "closed as a slow consumer (peer downs: " << b->peer_downs.load() << ")";

    // The point of the test. A drop is not fatal to the *data* — the next round
    // retries — but it restarts every transfer from zero, which is how a large
    // file over a real link never finishes at all.
    // Named exactly, now that the close reason reaches us: this test is about one
    // of the ways a connection can end and not the others, and a drop the peer or
    // the link caused would be a different bug wearing this one's failure message.
    EXPECT_EQ(a->slow_consumer_downs.load(), 0)
        << "librats dropped us as a slow consumer — the gate did not hold the "
           "shared queue under the mark";
    EXPECT_EQ(a->peer_downs.load(), 0) << "sender's connection was dropped mid-transfer";
    EXPECT_EQ(b->peer_downs.load(), 0) << "receiver's connection was dropped mid-transfer";

    // …and it stayed under the mark by waiting, not by luck. Without this the run
    // above proves only that loopback happened to keep up.
    EXPECT_GT(a->router->send_gate().waits(), 0u) << "the gate never engaged";
    EXPECT_EQ(a->router->send_gate().timeouts(), 0u)
        << "a sender gave up on a peer that was draining perfectly well";
}

// A delta made of many non-contiguous COPY instructions carries almost no
// payload, so the FileAck window never engages: it counts literal bytes only,
// because COPY bytes are read from the receiver's own base and never cross the
// wire. The transport's backpressure is the only thing that bounds such a stream,
// and this test is what says so — remove the pacing from the COPY path and the
// gate stops engaging here.
//
// Swapping adjacent blocks is what produces the stream: every block still matches
// the base, so there is nothing to send, but no two consecutive matches are
// contiguous *in the base*, so none of them coalesce into one instruction.
//
// Both sides are seeded with the same file so that no whole-file transfer ever
// happens: what crosses the wire is a few thousand 25-byte COPY instructions and
// almost nothing else.
//
// What is asserted is that they went out *through* the pacing path, not that the
// pacing had to wait. Whether a wait happens is a race between this sender and
// the reactor draining what it is handed — a hundred kilobytes of COPY messages
// reach a 16 KiB mark only if the sender gets far enough ahead — and a test that
// asserts on winning that race passes on a quiet machine and fails on a busy
// one. Offering them to the gate is what the code must do; waiting is what the
// transport decides.
TEST(SyncIntegration, CopyOnlyDeltaIsPacedByTheTransport) {
    constexpr size_t kQueue = 64 * 1024;         // low-water: 16 KiB
    constexpr size_t kSize  = 8 * 1024 * 1024;   // ⇒ ~4k COPY instructions at the
    const std::string rel   = "shuffled.bin";    //   default 2 KiB block size

    auto a = narrow_endpoint(kQueue);            // serves: sends the COPY stream
    Endpoint b;                                  // requests: sends the signature

    const auto original = test::random_bytes(kSize, 31337);
    test::write_file(a->first().sub(rel), original);
    test::write_file(b.first().sub(rel), original);

    a->start();
    b.start();
    connect(b, *a);

    // Identical trees: they converge with nothing on the wire but manifests.
    ASSERT_TRUE(wait_until([&] {
        return a->first().synced_events.load() > 0 && b.first().synced_events.load() > 0;
    })) << "the two identical trees never agreed";
    const uint64_t offers_before = a->router->send_gate().offers();

    // Swap every adjacent pair of blocks: the same bytes, all of them findable in
    // the base, none in an order the delta can merge.
    auto shuffled = original;
    constexpr size_t kBlock = kDefaultBlockSize;
    for (size_t off = 0; off + 2 * kBlock <= kSize; off += 2 * kBlock)
        std::swap_ranges(shuffled.begin() + off, shuffled.begin() + off + kBlock,
                         shuffled.begin() + off + kBlock);
    // A permutation keeps the size, and mtime has one-second resolution — so the
    // scanner's size+mtime quick-check would not re-hash the file at all and the
    // edit would simply never be noticed. Change the length too, exactly as the
    // other delta tests do.
    const auto tail = test::random_bytes(29, 5);
    shuffled.insert(shuffled.end(), tail.begin(), tail.end());
    ASSERT_NE(shuffled, original);
    test::write_file(a->first().sub(rel), shuffled);
    a->publish();

    ASSERT_TRUE(wait_until([&] { return file_is(b.first().sub(rel), shuffled); },
                           milliseconds(90000)))
        << "the copy-only delta never landed (peer downs: " << b.peer_downs.load() << ")";

    EXPECT_EQ(a->slow_consumer_downs.load(), 0)
        << "the COPY stream outran the queue and librats dropped us for it";
    EXPECT_EQ(a->peer_downs.load(), 0) << "a stream of COPY messages closed the connection";
    EXPECT_EQ(b.peer_downs.load(), 0);

    // One offer per COPY instruction, and the file is 4k blocks long. A threshold
    // well under that says "the COPY stream went through the gate" while leaving
    // room for however the delta happens to coalesce; take the pacing off that
    // path and the count barely moves, because the only other paced message in
    // this transfer is the single literal carrying the 29-byte tail.
    EXPECT_GT(a->router->send_gate().offers() - offers_before, 1000u)
        << "COPY messages went out unpaced — nothing else bounds them";
}
