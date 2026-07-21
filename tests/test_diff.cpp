#include <gtest/gtest.h>

#include "core/diff.h"

#include <algorithm>

using namespace rasync;

namespace {

FileMeta fm(const std::string& content, int64_t mtime = 100) {
    FileMeta m;
    m.hash = sha256(content);
    m.size = content.size();
    m.mtime = mtime;
    return m;
}

bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

ReconcileOptions two_way() { ReconcileOptions o; o.mode = SyncMode::TwoWay; return o; }

} // namespace

TEST(Diff, RemoteAddedIsPulled) {
    Manifest base, local, remote;
    remote.set("new.txt", fm("hello"));
    SyncPlan p = reconcile(base, local, remote, two_way());
    EXPECT_TRUE(has(p.pull, "new.txt"));
    EXPECT_TRUE(p.delete_local.empty());
}

TEST(Diff, LocalAddedIsPushed) {
    Manifest base, local, remote;
    local.set("mine.txt", fm("hello"));
    SyncPlan p = reconcile(base, local, remote, two_way());
    EXPECT_TRUE(has(p.push, "mine.txt"));
    EXPECT_TRUE(p.pull.empty());
}

TEST(Diff, IdenticalContentNoOp) {
    Manifest base, local, remote;
    local.set("f", fm("same"));
    remote.set("f", fm("same"));
    SyncPlan p = reconcile(base, local, remote, two_way());
    EXPECT_TRUE(p.empty());
}

TEST(Diff, RemoteDeletePropagatesToLocalDelete) {
    // File existed in base and locally (unchanged) but was deleted on remote.
    Manifest base, local, remote;
    base.set("gone.txt", fm("data"));
    local.set("gone.txt", fm("data"));
    // remote lacks it
    SyncPlan p = reconcile(base, local, remote, two_way());
    EXPECT_TRUE(has(p.delete_local, "gone.txt"));
}

TEST(Diff, LocalDeletePropagatesToRemoteDelete) {
    Manifest base, local, remote;
    base.set("gone.txt", fm("data"));
    remote.set("gone.txt", fm("data"));
    SyncPlan p = reconcile(base, local, remote, two_way());
    EXPECT_TRUE(has(p.delete_remote, "gone.txt"));
    EXPECT_TRUE(p.delete_local.empty());
}

TEST(Diff, RemoteModifiedIsPulled) {
    Manifest base, local, remote;
    base.set("f", fm("v1"));
    local.set("f", fm("v1"));       // unchanged locally
    remote.set("f", fm("v2"));      // changed remotely
    SyncPlan p = reconcile(base, local, remote, two_way());
    EXPECT_TRUE(has(p.pull, "f"));
}

TEST(Diff, ConflictNewerWins) {
    Manifest base, local, remote;
    base.set("f", fm("v0", 10));
    local.set("f", fm("localedit", 200));   // newer
    remote.set("f", fm("remoteedit", 150));
    SyncPlan p = reconcile(base, local, remote, two_way());
    EXPECT_TRUE(has(p.conflicts, "f"));
    EXPECT_TRUE(has(p.push, "f"));   // local is newer → local wins → peer pulls
    EXPECT_FALSE(has(p.pull, "f"));
}

TEST(Diff, ConflictNewerWinsRemote) {
    Manifest base, local, remote;
    base.set("f", fm("v0", 10));
    local.set("f", fm("localedit", 150));
    remote.set("f", fm("remoteedit", 200));   // remote newer
    SyncPlan p = reconcile(base, local, remote, two_way());
    EXPECT_TRUE(has(p.conflicts, "f"));
    EXPECT_TRUE(has(p.pull, "f"));
}

TEST(Diff, ConflictResolutionIsSymmetric) {
    // The peer runs reconcile with local/remote swapped and must reach the
    // complementary decision, so exactly one side transfers.
    Manifest base;
    base.set("f", fm("v0", 10));
    Manifest a, b;
    a.set("f", fm("A", 200));
    b.set("f", fm("B", 200));  // same mtime → hash tie-break, must be deterministic

    SyncPlan pa = reconcile(base, a, b, two_way());
    SyncPlan pb = reconcile(base, b, a, two_way());

    const bool a_pushes = has(pa.push, "f");
    const bool b_pushes = has(pb.push, "f");
    EXPECT_NE(a_pushes, b_pushes);              // exactly one side is the winner
    EXPECT_EQ(has(pa.pull, "f"), b_pushes);
    EXPECT_EQ(has(pb.pull, "f"), a_pushes);
}

TEST(Diff, MergedBaselineConverges) {
    Manifest base, local, remote;
    base.set("keep", fm("k"));
    local.set("keep", fm("k"));
    remote.set("keep", fm("k"));
    local.set("addedlocal", fm("L"));
    remote.set("addedremote", fm("R"));
    base.set("delboth", fm("d"));  // deleted on both sides

    Manifest merged = merged_baseline(base, local, remote, two_way());
    EXPECT_TRUE(merged.contains("keep"));
    EXPECT_TRUE(merged.contains("addedlocal"));
    EXPECT_TRUE(merged.contains("addedremote"));
    EXPECT_FALSE(merged.contains("delboth"));
}

TEST(Diff, MirrorReplicaPullsAndDeletes) {
    Manifest base, source, replica;
    source.set("a", fm("a"));
    source.set("b", fm("b"));
    replica.set("a", fm("a"));       // already present, identical
    replica.set("stale", fm("x"));   // not on source → should be deleted

    ReconcileOptions o;
    o.mode = SyncMode::Mirror;
    o.source = false;  // we are the replica
    SyncPlan p = reconcile(base, replica, source, o);
    EXPECT_TRUE(has(p.pull, "b"));
    EXPECT_FALSE(has(p.pull, "a"));
    EXPECT_TRUE(has(p.delete_local, "stale"));
}

TEST(Diff, MirrorSourceNeverChanges) {
    Manifest base, source, replica;
    source.set("a", fm("a"));
    replica.set("stale", fm("x"));

    ReconcileOptions o;
    o.mode = SyncMode::Mirror;
    o.source = true;  // we are the source of truth
    SyncPlan p = reconcile(base, source, replica, o);
    EXPECT_TRUE(p.pull.empty());
    EXPECT_TRUE(p.delete_local.empty());
    // Informational: the replica will pull "a" and delete "stale".
    EXPECT_TRUE(has(p.push, "a"));
}

TEST(Diff, MirrorNoDeleteKeepsExtras) {
    Manifest base, source, replica;
    source.set("a", fm("a"));
    replica.set("a", fm("a"));
    replica.set("extra", fm("e"));

    ReconcileOptions o;
    o.mode = SyncMode::Mirror;
    o.source = false;
    o.propagate_deletes = false;
    SyncPlan p = reconcile(base, replica, source, o);
    EXPECT_TRUE(p.delete_local.empty());
}
