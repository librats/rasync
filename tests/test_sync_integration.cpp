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
    ig.add(".rasync-tmp");
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
    std::unique_ptr<librats::Node> node;
    std::unique_ptr<SyncService>   svc;
    Scanner       scanner;
    Manifest      current;

    explicit Endpoint(SyncMode mode = SyncMode::TwoWay, bool source = false)
        : scanner(make_ignore()) {
        root = dir.str();
        cfg.root = root;
        cfg.data_dir = dir.sub(".rasync");
        cfg.mode = mode;
        cfg.source = source;

        librats::NodeConfig ncfg;
        ncfg.listen_port = 0;
        ncfg.bind_address = "127.0.0.1";
        ncfg.protocol = kProtocolName;
        ncfg.data_dir = cfg.data_dir;
        ncfg.enable_network_monitor = false;
        node = std::make_unique<librats::Node>(ncfg);
        svc = std::make_unique<SyncService>(*node, cfg);
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
