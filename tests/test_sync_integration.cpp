#include <gtest/gtest.h>

#include "core/scanner.h"
#include "net/sync_service.h"
#include "version.h"
#include "test_util.h"

#include "node/node.h"

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

Manifest disk_state(const std::string& root) {
    return Scanner(make_ignore()).scan(root);
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

// One node+service+scanner rooted at a fresh temp dir, driven directly (no daemon).
struct Endpoint {
    test::TempDir dir;
    std::string   root;
    SyncConfig    cfg;
    std::atomic<int>               synced_events{0};
    std::unique_ptr<librats::Node> node;
    std::unique_ptr<SyncService>   svc;
    Scanner       scanner;
    Manifest      current;

    explicit Endpoint(SyncMode mode = SyncMode::TwoWay, bool source = false,
                      size_t max_update_bytes = 0,
                      ConflictPolicy conflict = ConflictPolicy::Newer)
        : scanner(make_ignore()) {
        root = dir.str();
        cfg.root = root;
        cfg.data_dir = dir.sub(".rasync");
        cfg.mode = mode;
        cfg.source = source;
        cfg.conflict = conflict;
        if (max_update_bytes) cfg.max_update_bytes = max_update_bytes;

        librats::NodeConfig ncfg;
        ncfg.listen_port = 0;
        ncfg.bind_address = "127.0.0.1";
        ncfg.protocol = kProtocolName;
        ncfg.data_dir = cfg.data_dir;
        ncfg.enable_network_monitor = false;
        node = std::make_unique<librats::Node>(ncfg);

        // Fires from reactor/session threads once a peer's tree matches ours.
        SyncEvents ev;
        ev.synced = [this](const librats::PeerId&, uint64_t, uint64_t) {
            synced_events.fetch_add(1);
        };
        svc = std::make_unique<SyncService>(*node, cfg, ev);
        svc->attach();
    }

    ~Endpoint() {
        if (svc) svc->shutdown();
        if (node) node->stop();
    }

    void publish() {
        current = scanner.scan(root, &current, nullptr);
        svc->set_local_manifest(current);
    }

    void start() {
        svc->load_baseline();
        publish();
        ASSERT_TRUE(node->start());
    }

    uint16_t port() const { return node->listen_port(); }
};

void connect(Endpoint& from, Endpoint& to) {
    from.node->connect("127.0.0.1", to.port());
}

// A bare librats node that speaks the rasync channel by hand. An Endpoint can
// only ever behave itself; these tests need a peer that lies — about the protocol
// version it speaks, or about the paths its tree contains.
struct RawPeer {
    test::TempDir                  dir;
    std::unique_ptr<librats::Node> node;

    RawPeer() {
        librats::NodeConfig ncfg;
        ncfg.listen_port = 0;
        ncfg.bind_address = "127.0.0.1";
        ncfg.protocol = kProtocolName;
        ncfg.data_dir = dir.str();
        ncfg.enable_network_monitor = false;
        node = std::make_unique<librats::Node>(ncfg);
    }
    ~RawPeer() { if (node) node->stop(); }

    /// Handle inbound rasync messages, opcode already peeled off. Register before start().
    template <typename Fn>
    void on_message(Fn fn) {
        node->on(proto::kChannel, [fn](const librats::Peer& peer, librats::ByteView payload) {
            BinaryReader r(payload.data(), payload.size());
            auto op = static_cast<proto::Op>(r.u8());
            if (r.ok()) fn(peer.id(), op, r);
        });
    }

    void send(const librats::PeerId& to, BinaryWriter& w) {
        node->send(to, proto::kChannel, librats::ByteView(w.buffer()));
    }

    /// A Hello claiming `version`, `mode` and `conflict`; the rest is what an honest
    /// peer sends. Each of the three is something a real misconfiguration can get
    /// wrong, and only a peer we control can get it wrong on purpose.
    void say_hello(const librats::PeerId& to, uint8_t version,
                   SyncMode mode = SyncMode::TwoWay,
                   ConflictPolicy conflict = ConflictPolicy::Newer) {
        auto w = proto::message(proto::Op::Hello);
        w.u8(version);
        w.u8(static_cast<uint8_t>(mode));
        w.u8(static_cast<uint8_t>(conflict));
        w.u64(0);
        w.str16("raw-peer");
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
    test::write_file(a.dir.sub("a.txt"), "hello world");
    test::write_file(a.dir.sub("sub/b.txt"), "nested content");

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] {
        Manifest da = disk_state(a.root), db = disk_state(b.root);
        return db.size() == 2 && da.fingerprint() == db.fingerprint();
    })) << "B never converged to A";

    EXPECT_EQ(test::read_file(b.dir.sub("a.txt")), "hello world");
    EXPECT_EQ(test::read_file(b.dir.sub("sub/b.txt")), "nested content");
}

TEST(SyncIntegration, TwoWayMergesBothDirections) {
    Endpoint a, b;
    test::write_file(a.dir.sub("from_a.txt"), "content A");
    test::write_file(b.dir.sub("from_b.txt"), "content B");

    a.start();
    b.start();
    connect(a, b);

    ASSERT_TRUE(wait_until([&] {
        Manifest da = disk_state(a.root), db = disk_state(b.root);
        return da.size() == 2 && db.size() == 2 && da.fingerprint() == db.fingerprint();
    })) << "two-way merge did not converge";

    EXPECT_EQ(test::read_file(a.dir.sub("from_b.txt")), "content B");
    EXPECT_EQ(test::read_file(b.dir.sub("from_a.txt")), "content A");
}

TEST(SyncIntegration, ModificationPropagatesAsDelta) {
    Endpoint a, b;
    // A sizable file so the change is a small fraction (delta path).
    auto original = test::random_bytes(400 * 1024, 123);
    test::write_file(a.dir.sub("big.bin"), original);

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] {
        return disk_state(b.root).contains("big.bin") &&
               disk_state(a.root).fingerprint() == disk_state(b.root).fingerprint();
    })) << "initial big file did not sync";

    // Modify a small middle region on A and re-publish. Also change the length so
    // the size+mtime quick-check reliably flags it (mtime has 1-second resolution,
    // like rsync — a same-size edit within the same second would otherwise hide).
    auto modified = original;
    for (size_t i = 200000; i < 200100; ++i) modified[i] ^= 0xaa;
    auto extra = test::random_bytes(37, 999);
    modified.insert(modified.end(), extra.begin(), extra.end());
    test::write_file(a.dir.sub("big.bin"), modified);
    a.publish();

    ASSERT_TRUE(wait_until([&] {
        return disk_state(a.root).fingerprint() == disk_state(b.root).fingerprint() &&
               test::read_file(b.dir.sub("big.bin")).size() == modified.size();
    })) << "modification did not propagate";

    std::string got = test::read_file(b.dir.sub("big.bin"));
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
    test::write_file(a.dir.sub(rel), original);

    // Hashing a 14 MiB tree on every poll would dominate the test, and the final
    // path only ever gets the new content after verification + atomic rename — so
    // its size is a sound (and cheap) convergence signal.
    auto b_size = [&](size_t want) {
        std::error_code ec;
        return std::filesystem::file_size(b.dir.sub(rel), ec) == want;
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
    test::write_file(a.dir.sub(rel), modified);
    a.publish();

    ASSERT_TRUE(wait_until([&] { return b_size(modified.size()); }))
        << "large delta stalled — the sender never drained its send window";

    std::string got = test::read_file(b.dir.sub(rel));
    ASSERT_EQ(got.size(), modified.size());
    EXPECT_EQ(0, std::memcmp(got.data(), modified.data(), modified.size()));
}

TEST(SyncIntegration, DeletionPropagates) {
    Endpoint a, b;
    test::write_file(a.dir.sub("keep.txt"), "keep");
    test::write_file(a.dir.sub("remove.txt"), "remove me");

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] { return disk_state(b.root).size() == 2; }))
        << "initial files did not sync";

    // Delete on A, re-publish; the deletion should reach B.
    std::filesystem::remove(a.dir.sub("remove.txt"));
    a.publish();

    ASSERT_TRUE(wait_until([&] {
        Manifest db = disk_state(b.root);
        return !db.contains("remove.txt") && db.contains("keep.txt");
    })) << "deletion did not propagate";
}

TEST(SyncIntegration, PropagatedDeletionPrunesEmptyDirectories) {
    Endpoint a, b;
    // A transfer creates parent directories on the way in, so B ends up holding
    // nested/deep/ purely because gone.txt arrived through it.
    test::write_file(a.dir.sub("nested/deep/gone.txt"), "x");
    test::write_file(a.dir.sub("nested/keep.txt"), "y");

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] { return disk_state(b.root).size() == 2; }))
        << "initial files did not sync";
    ASSERT_TRUE(std::filesystem::exists(b.dir.sub("nested/deep")));

    std::filesystem::remove(a.dir.sub("nested/deep/gone.txt"));
    a.publish();

    ASSERT_TRUE(wait_until([&] { return !disk_state(b.root).contains("nested/deep/gone.txt"); }))
        << "deletion did not propagate";

    // Poll for the pruning rather than asserting straight away: it happens just
    // after the unlink, so the file being gone does not yet prove it has run.
    ASSERT_TRUE(wait_until([&] { return !std::filesystem::exists(b.dir.sub("nested/deep")); }))
        << "the directory the deletion emptied was not pruned";

    // Pruning stops at the first ancestor that still holds something.
    EXPECT_TRUE(std::filesystem::exists(b.dir.sub("nested/keep.txt")));
    EXPECT_TRUE(std::filesystem::exists(b.root));
}

TEST(SyncIntegration, StaleTempFilesAreClearedAtStartup) {
    Endpoint a;
    // A leftover from a killed run: the name a session would pick again, since
    // xids restart at 1. If it survived, open_write would append into it.
    std::string stale = a.dir.sub(std::string(kTempDirName) + "/dead-1.part");
    test::write_file(stale, std::string(4096, 'x'));
    ASSERT_TRUE(std::filesystem::exists(stale));

    a.svc->clean_temp_dir();

    EXPECT_FALSE(std::filesystem::exists(stale));
    EXPECT_FALSE(std::filesystem::exists(a.dir.sub(kTempDirName)));
}

TEST(SyncIntegration, TempFilesDoNotOutliveTheTransfer) {
    Endpoint a, b;
    test::write_file(a.dir.sub("payload.bin"), test::random_bytes(300 * 1024, 7));

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] {
        return disk_state(a.root).fingerprint() == disk_state(b.root).fingerprint() &&
               disk_state(b.root).contains("payload.bin");
    })) << "file did not sync";

    // A converged receiver must leave no .part behind: every temp file is either
    // renamed into place or dropped by ~Incoming.
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(b.dir.sub(kTempDirName), ec))
        ADD_FAILURE() << "leftover temp file: " << e.path().string();
}

TEST(SyncIntegration, ManifestTrafficStaysProportionalToChanges) {
    Endpoint a, b;
    constexpr int kFiles = 120;
    for (int i = 0; i < kFiles; ++i)
        test::write_file(a.dir.sub("f" + std::to_string(i) + ".txt"),
                         "payload " + std::to_string(i));

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] {
        return disk_state(b.root).size() == kFiles &&
               disk_state(a.root).fingerprint() == disk_state(b.root).fingerprint();
    })) << "files did not sync";

    // B pulled 120 files. Describing its tree once per received file — the
    // behaviour incremental updates replaced — costs ~121 messages and ~435 KB.
    // Two are enough: the opening reset, and one coalesced update once the
    // inbound queue drained (measured: 2 messages, ~7 KB).
    EXPECT_LE(b.svc->advert_messages(), 4u)
        << "B re-described its tree far more often than it changed";
    EXPECT_LT(b.svc->advert_bytes(), 32u * 1024)
        << "manifest bookkeeping grew with the tree, not with the changes";

    // A's tree never changed, so after the opening description it says nothing.
    EXPECT_LE(a.svc->advert_messages(), 2u);
}

TEST(SyncIntegration, ConvergenceIsAnnouncedAndBaselinePersisted) {
    // Convergence is now decided behind an O(1) "did anything move?" gate, and
    // `--once` plus two-way delete propagation both hang off this event and the
    // baseline it writes — so both are worth pinning down.
    Endpoint a, b;
    test::write_file(a.dir.sub("one.txt"), "1");
    test::write_file(a.dir.sub("two.txt"), "2");

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] {
        return a.synced_events.load() > 0 && b.synced_events.load() > 0;
    })) << "neither side ever announced convergence";

    EXPECT_TRUE(std::filesystem::exists(b.dir.sub(".rasync/baseline.man")))
        << "no baseline persisted — deletes could not propagate on the next run";

    // Nothing changed afterwards, so nothing re-announces it.
    const int seen = b.synced_events.load();
    std::this_thread::sleep_for(milliseconds(400));
    EXPECT_EQ(b.synced_events.load(), seen) << "convergence announced repeatedly";
}

TEST(SyncIntegration, LargeManifestUpdatesAreChunkedAndReassembled) {
    // A's updates are capped absurdly low, so even a handful of files has to go
    // out as several kUpdateMore chunks. The receiver must hold them all back and
    // apply them at once — a half-applied tree reads as "the peer deleted the
    // rest", which would propagate as deletions.
    Endpoint a(SyncMode::TwoWay, false, /*max_update_bytes=*/80);
    Endpoint b;
    for (int i = 0; i < 6; ++i)
        test::write_file(a.dir.sub("chunk" + std::to_string(i) + ".txt"), "c");
    test::write_file(b.dir.sub("only_on_b.txt"), "b");

    a.start();
    b.start();
    connect(b, a);

    ASSERT_TRUE(wait_until([&] {
        Manifest da = disk_state(a.root), db = disk_state(b.root);
        return da.size() == 7 && db.size() == 7 && da.fingerprint() == db.fingerprint();
    })) << "chunked manifest update did not converge";

    // The cap really did split the description into several messages.
    EXPECT_GE(a.svc->advert_messages(), 3u);
    // Nothing was lost to a partially applied view.
    EXPECT_TRUE(disk_state(b.root).contains("only_on_b.txt"));
    EXPECT_TRUE(disk_state(a.root).contains("only_on_b.txt"));
}

TEST(SyncIntegration, PeerWithAnIncompatibleProtocolVersionIsRefused) {
    // v1 and v2 describe a tree differently, so a mismatched pair that keeps
    // talking just logs a malformed message per advertisement and never
    // converges. The session must say Bye once and go inert instead.
    Endpoint a;
    test::write_file(a.dir.sub("a.txt"), "content");
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
    EXPECT_EQ(a.svc->advert_messages(), 0u);
    EXPECT_EQ(byes.load(), 1) << "refusal announced more than once";
}

TEST(SyncIntegration, PeerWithANonComplementaryConflictPolicyIsRefused) {
    // Two peers converge only because their plans are complements, which holds only
    // while they resolve conflicts complementarily. A mismatched pair either stalls
    // forever (both sides think they won) or swaps the two versions round after round
    // (both think they lost) — and neither says so. Catch it at the handshake.
    Endpoint a;  // default policy: newer, whose complement is newer
    test::write_file(a.dir.sub("a.txt"), "content");
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
    EXPECT_EQ(a.svc->advert_messages(), 0u);
    EXPECT_EQ(byes.load(), 1) << "refusal announced more than once";
}

TEST(SyncIntegration, ComplementaryConflictPoliciesArePairedUp) {
    // The other half of the rule: "local" is meant to pair with "remote", and that
    // pairing must sail straight through — the check has to reject a bad pair, not
    // every pair that isn't identical.
    Endpoint a(SyncMode::TwoWay, /*source=*/false, /*max_update_bytes=*/0,
               ConflictPolicy::PreferLocal);
    test::write_file(a.dir.sub("a.txt"), "content");
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
    test::write_file(src.dir.sub("doc.txt"), "authoritative");
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
            auto w = proto::message(proto::Op::ManifestUpdate);
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
    EXPECT_FALSE(std::filesystem::exists(a.dir.sub("../rasync-escaped-a.txt")));
    EXPECT_FALSE(std::filesystem::exists(a.dir.sub("../rasync-escaped-b.txt")));
}

TEST(SyncIntegration, MirrorReplicaFollowsSource) {
    Endpoint src(SyncMode::Mirror, /*source=*/true);
    Endpoint rep(SyncMode::Mirror, /*source=*/false);

    test::write_file(src.dir.sub("doc.txt"), "authoritative");
    test::write_file(rep.dir.sub("stale.txt"), "should be removed");

    src.start();
    rep.start();
    connect(rep, src);

    ASSERT_TRUE(wait_until([&] {
        Manifest dr = disk_state(rep.root);
        return dr.contains("doc.txt") && !dr.contains("stale.txt");
    })) << "replica did not mirror the source";

    EXPECT_EQ(test::read_file(rep.dir.sub("doc.txt")), "authoritative");
    // The source must be untouched.
    EXPECT_FALSE(disk_state(src.root).contains("stale.txt"));
}
