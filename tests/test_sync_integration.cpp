#include <gtest/gtest.h>

#include "core/scanner.h"
#include "net/sync_service.h"
#include "version.h"
#include "test_util.h"

#include "node/node.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <thread>

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
                      size_t max_update_bytes = 0)
        : scanner(make_ignore()) {
        root = dir.str();
        cfg.root = root;
        cfg.data_dir = dir.sub(".rasync");
        cfg.mode = mode;
        cfg.source = source;
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
