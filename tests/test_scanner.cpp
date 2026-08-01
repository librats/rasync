#include <gtest/gtest.h>

#include "core/scanner.h"
#include "test_util.h"

#include <filesystem>

using namespace rasync;

using rasync::test::Junction;
using rasync::test::make_symlink;

TEST(Scanner, WalksTreeAndHashes) {
    test::TempDir dir;
    test::write_file(dir.sub("a.txt"), "aaa");
    test::write_file(dir.sub("sub/b.txt"), "bbb");
    test::write_file(dir.sub("sub/deep/c.txt"), "ccc");

    Scanner sc;
    ScanStats stats;
    Manifest m = sc.scan(dir.str(), nullptr, &stats);

    EXPECT_EQ(m.size(), 3u);
    EXPECT_TRUE(m.contains("a.txt"));
    EXPECT_TRUE(m.contains("sub/b.txt"));
    EXPECT_TRUE(m.contains("sub/deep/c.txt"));
    ASSERT_NE(m.find("a.txt"), nullptr);
    EXPECT_EQ(m.find("a.txt")->hash, sha256("aaa"));
    EXPECT_EQ(stats.files_seen, 3u);
    EXPECT_EQ(stats.files_hashed, 3u);
}

TEST(Scanner, HonoursIgnore) {
    test::TempDir dir;
    test::write_file(dir.sub("keep.txt"), "keep");
    test::write_file(dir.sub("skip.tmp"), "tmp");
    test::write_file(dir.sub("node_modules/lib.js"), "js");

    IgnoreList ig;
    ig.add("*.tmp");
    ig.add("node_modules/");

    Scanner sc(ig);
    Manifest m = sc.scan(dir.str());
    EXPECT_TRUE(m.contains("keep.txt"));
    EXPECT_FALSE(m.contains("skip.tmp"));
    EXPECT_FALSE(m.contains("node_modules/lib.js"));
    EXPECT_EQ(m.size(), 1u);
}

TEST(Scanner, ReusesHashCacheForUnchangedFiles) {
    test::TempDir dir;
    test::write_file(dir.sub("a.txt"), "aaa");
    test::write_file(dir.sub("b.txt"), "bbb");

    Scanner sc;
    Manifest first = sc.scan(dir.str());

    // Re-scan with the previous manifest as cache: nothing changed, so nothing
    // is re-hashed.
    ScanStats stats;
    Manifest second = sc.scan(dir.str(), &first, &stats);
    EXPECT_EQ(second.size(), 2u);
    EXPECT_EQ(stats.files_hashed, 0u);
    EXPECT_EQ(second.fingerprint(), first.fingerprint());
}

TEST(Scanner, DetectsModification) {
    test::TempDir dir;
    std::string path = dir.sub("a.txt");
    test::write_file(path, "original");

    Scanner sc;
    Manifest first = sc.scan(dir.str());

    // Rewrite with different length so size differs (mtime resolution independent).
    test::write_file(path, "changed contents entirely");
    ScanStats stats;
    Manifest second = sc.scan(dir.str(), &first, &stats);

    EXPECT_EQ(stats.files_hashed, 1u);
    EXPECT_NE(second.find("a.txt")->hash, first.find("a.txt")->hash);
}

TEST(Scanner, EmptyDirectoryYieldsEmptyManifest) {
    test::TempDir dir;
    Scanner sc;
    Manifest m = sc.scan(dir.str());
    EXPECT_TRUE(m.empty());
}

TEST(Scanner, SkipsSymlinkedFiles) {
    test::TempDir dir;
    test::write_file(dir.sub("real.txt"), "real");
    if (!make_symlink("real.txt", dir.sub("link.txt"), /*directory=*/false))
        GTEST_SKIP() << "symlinks unavailable on this host";

    Scanner sc;
    ScanStats stats;
    Manifest m = sc.scan(dir.str(), nullptr, &stats);

    EXPECT_TRUE(m.contains("real.txt"));
    EXPECT_FALSE(m.contains("link.txt"));  // the target syncs on its own; the link never does
    EXPECT_EQ(m.size(), 1u);
    EXPECT_EQ(stats.files_seen, 1u);
    EXPECT_EQ(stats.symlinks_skipped, 1u);
}

TEST(Scanner, SymlinkOutsideTheRootIsNotFollowed) {
    test::TempDir dir, outside;
    test::write_file(outside.sub("secret.txt"), "not ours to sync");
    test::write_file(dir.sub("mine.txt"), "ours");
    if (!make_symlink(outside.path(), dir.sub("escape"), /*directory=*/true))
        GTEST_SKIP() << "symlinks unavailable on this host";

    Scanner sc;
    Manifest m = sc.scan(dir.str());

    // Following the link would have copied someone else's tree to the peer.
    EXPECT_EQ(m.size(), 1u);
    EXPECT_TRUE(m.contains("mine.txt"));
    EXPECT_FALSE(m.contains("escape/secret.txt"));
}

TEST(Scanner, SymlinkLoopDoesNotHangTheWalk) {
    test::TempDir dir;
    test::write_file(dir.sub("sub/a.txt"), "a");
    // A link back to the root: following it would walk sub/ again as loop/sub/,
    // then loop/loop/sub/, and never finish.
    if (!make_symlink(dir.path(), dir.sub("loop"), /*directory=*/true))
        GTEST_SKIP() << "symlinks unavailable on this host";

    Scanner sc;
    ScanStats stats;
    Manifest m = sc.scan(dir.str(), nullptr, &stats);

    EXPECT_EQ(m.size(), 1u);
    EXPECT_TRUE(m.contains("sub/a.txt"));
    EXPECT_EQ(stats.symlinks_skipped, 1u);
}

// ── --follow-symlinks ────────────────────────────────────────────────────────
//
// The opposite bargain: a link stands for what it points at, so the peer receives
// ordinary files and can rebuild the structure on a platform that has no links.
// What has to be shown is that content really crosses (including from outside the
// tree, which is the point), and that the walk still terminates now that the tree
// is a graph.

TEST(Scanner, FollowedFileSymlinkIsScannedAsItsTarget) {
    test::TempDir dir;
    test::write_file(dir.sub("real.txt"), "the real contents");
    if (!make_symlink("real.txt", dir.sub("link.txt"), /*directory=*/false))
        GTEST_SKIP() << "symlinks unavailable on this host";

    Scanner sc({}, /*follow_symlinks=*/true);
    ScanStats stats;
    Manifest m = sc.scan(dir.str(), nullptr, &stats);

    ASSERT_TRUE(m.contains("link.txt"));
    // Same bytes, same size as the target — not a zero-length reparse point,
    // which is what the link's own directory entry reports on Windows.
    EXPECT_EQ(m.find("link.txt")->hash, sha256("the real contents"));
    EXPECT_EQ(m.find("link.txt")->size, std::string("the real contents").size());
    EXPECT_EQ(m.find("link.txt")->mtime, m.find("real.txt")->mtime);
    EXPECT_EQ(m.size(), 2u);
    EXPECT_EQ(stats.symlinks_followed, 1u);
    EXPECT_EQ(stats.symlinks_skipped, 0u);
}

TEST(Scanner, FollowedDirectorySymlinkBringsInATreeFromOutside) {
    test::TempDir dir, outside;
    test::write_file(outside.sub("a.txt"), "outside a");
    test::write_file(outside.sub("deep/b.txt"), "outside b");
    test::write_file(dir.sub("mine.txt"), "ours");
    if (!make_symlink(outside.path(), dir.sub("linked"), /*directory=*/true))
        GTEST_SKIP() << "symlinks unavailable on this host";

    Scanner sc({}, /*follow_symlinks=*/true);
    ScanStats stats;
    Manifest m = sc.scan(dir.str(), nullptr, &stats);

    // The peer sees a plain subdirectory: this is what lets a machine with no
    // links of its own end up with the same structure.
    EXPECT_EQ(m.size(), 3u);
    EXPECT_TRUE(m.contains("mine.txt"));
    ASSERT_TRUE(m.contains("linked/a.txt"));
    EXPECT_EQ(m.find("linked/a.txt")->hash, sha256("outside a"));
    EXPECT_TRUE(m.contains("linked/deep/b.txt"));
    EXPECT_EQ(stats.symlinks_followed, 1u);
}

TEST(Scanner, FollowedSymlinkLoopStillTerminates) {
    test::TempDir dir;
    test::write_file(dir.sub("sub/a.txt"), "a");
    // The link the walk must refuse: it points at a directory it is already
    // inside, so following it yields loop/sub, loop/loop/sub, ... forever.
    if (!make_symlink(dir.path(), dir.sub("loop"), /*directory=*/true))
        GTEST_SKIP() << "symlinks unavailable on this host";

    Scanner sc({}, /*follow_symlinks=*/true);
    ScanStats stats;
    Manifest m = sc.scan(dir.str(), nullptr, &stats);

    EXPECT_EQ(m.size(), 1u);
    EXPECT_TRUE(m.contains("sub/a.txt"));
    EXPECT_EQ(stats.symlinks_skipped, 1u);
    EXPECT_EQ(stats.symlinks_followed, 0u);
}

TEST(Scanner, TwoDirectoriesLinkingToEachOtherStillTerminate) {
    // Neither link points at its own ancestor, so nothing local looks like a
    // cycle — only the chain of directories walked through reveals one.
    test::TempDir dir;
    test::write_file(dir.sub("a/file.txt"), "in a");
    test::write_file(dir.sub("b/file.txt"), "in b");
    if (!make_symlink(dir.path() / "b", dir.sub("a/to-b"), /*directory=*/true) ||
        !make_symlink(dir.path() / "a", dir.sub("b/to-a"), /*directory=*/true))
        GTEST_SKIP() << "symlinks unavailable on this host";

    Scanner sc({}, /*follow_symlinks=*/true);
    Manifest m = sc.scan(dir.str(), nullptr, nullptr);

    // One level of each link is walked; the step that would close the cycle is
    // refused. The walk finishing at all is the assertion that matters.
    EXPECT_TRUE(m.contains("a/file.txt"));
    EXPECT_TRUE(m.contains("b/file.txt"));
    EXPECT_TRUE(m.contains("a/to-b/file.txt"));
    EXPECT_TRUE(m.contains("b/to-a/file.txt"));
    EXPECT_FALSE(m.contains("a/to-b/to-a/file.txt"));
    EXPECT_EQ(m.size(), 4u);
}

TEST(Scanner, TwoLinksToOneDirectoryAreNotACycle) {
    test::TempDir dir, outside;
    test::write_file(outside.sub("shared.txt"), "shared");
    if (!make_symlink(outside.path(), dir.sub("one"), /*directory=*/true) ||
        !make_symlink(outside.path(), dir.sub("two"), /*directory=*/true))
        GTEST_SKIP() << "symlinks unavailable on this host";

    Scanner sc({}, /*follow_symlinks=*/true);
    Manifest m = sc.scan(dir.str(), nullptr, nullptr);

    // The directory really is in two places, and a peer rebuilding the structure
    // has to be told about both.
    EXPECT_TRUE(m.contains("one/shared.txt"));
    EXPECT_TRUE(m.contains("two/shared.txt"));
}

TEST(Scanner, BrokenSymlinkIsSkippedNotSyncedAsEmpty) {
    test::TempDir dir;
    test::write_file(dir.sub("keep.txt"), "keep");
    if (!make_symlink("nothing-here.txt", dir.sub("dangling.txt"), /*directory=*/false))
        GTEST_SKIP() << "symlinks unavailable on this host";

    Scanner sc({}, /*follow_symlinks=*/true);
    ScanStats stats;
    Manifest m = sc.scan(dir.str(), nullptr, &stats);

    // A dangling link has no content to send; syncing it as a zero-byte file
    // would create one on the peer that never existed anywhere.
    EXPECT_EQ(m.size(), 1u);
    EXPECT_TRUE(m.contains("keep.txt"));
    EXPECT_FALSE(m.contains("dangling.txt"));
    EXPECT_EQ(stats.symlinks_skipped, 1u);
    EXPECT_EQ(stats.symlinks_followed, 0u);
}

TEST(Scanner, IgnoreStillWinsOverFollowing) {
    test::TempDir dir, outside;
    test::write_file(outside.sub("big.bin"), "not wanted");
    test::write_file(dir.sub("keep.txt"), "keep");
    if (!make_symlink(outside.path(), dir.sub("cache"), /*directory=*/true))
        GTEST_SKIP() << "symlinks unavailable on this host";

    IgnoreList ig;
    ig.add("cache/");
    Scanner sc(ig, /*follow_symlinks=*/true);
    ScanStats stats;
    Manifest m = sc.scan(dir.str(), nullptr, &stats);

    EXPECT_EQ(m.size(), 1u);
    EXPECT_TRUE(m.contains("keep.txt"));
    EXPECT_EQ(stats.symlinks_followed, 0u);  // excluded before it is even probed
}

TEST(Scanner, IgnoredSymlinkIsNotCountedTwice) {
    test::TempDir dir;
    test::write_file(dir.sub("keep.txt"), "keep");
    if (!make_symlink("keep.txt", dir.sub("skip.tmp"), /*directory=*/false))
        GTEST_SKIP() << "symlinks unavailable on this host";

    IgnoreList ig;
    ig.add("*.tmp");
    Scanner sc(ig);
    ScanStats stats;
    Manifest m = sc.scan(dir.str(), nullptr, &stats);

    // Excluded by pattern, so it never reaches the symlink probe at all.
    EXPECT_EQ(m.size(), 1u);
    EXPECT_EQ(stats.symlinks_skipped, 0u);
}

// ── Junctions ────────────────────────────────────────────────────────────────
//
// The Windows link nothing in the standard library calls a link, and the one a
// user has without asking for it: `mklink /J` needs no privilege, and Windows
// puts several in every profile — `AppData\Local\Application Data` points at
// `AppData\Local`, its own parent. A walk that reads one as an ordinary
// directory descends `junc/junc/junc/…` until it runs out of memory, so these
// tests hang rather than fail if the classification regresses.

TEST(Scanner, JunctionIsSkippedLikeASymlink) {
    test::TempDir dir, outside;
    test::write_file(outside.sub("theirs.txt"), "not ours to sync");
    test::write_file(dir.sub("mine.txt"), "ours");
    Junction junc(outside.path(), dir.sub("junc"));
    if (!junc.ok()) GTEST_SKIP() << "junctions unavailable on this host";

    Scanner sc;
    ScanStats stats;
    Manifest m = sc.scan(dir.str(), nullptr, &stats);

    // Following it would have copied someone else's tree to the peer, which is
    // exactly what the default refuses for a symlink.
    EXPECT_EQ(m.size(), 1u);
    EXPECT_TRUE(m.contains("mine.txt"));
    EXPECT_FALSE(m.contains("junc/theirs.txt"));
    EXPECT_EQ(stats.symlinks_skipped, 1u);
}

TEST(Scanner, SelfReferentialJunctionDoesNotHangTheWalk) {
    test::TempDir dir;
    test::write_file(dir.sub("sub/a.txt"), "a");
    // The shape Windows ships in every user profile: a junction pointing at the
    // directory that contains it.
    Junction loop(dir.path(), dir.sub("loop"));
    if (!loop.ok()) GTEST_SKIP() << "junctions unavailable on this host";

    Scanner sc;
    ScanStats stats;
    Manifest m = sc.scan(dir.str(), nullptr, &stats);

    EXPECT_EQ(m.size(), 1u);
    EXPECT_TRUE(m.contains("sub/a.txt"));
    EXPECT_EQ(stats.symlinks_skipped, 1u);
}

TEST(Scanner, FollowedJunctionBringsInATreeFromOutside) {
    test::TempDir dir, outside;
    test::write_file(outside.sub("a.txt"), "outside a");
    test::write_file(outside.sub("deep/b.txt"), "outside b");
    test::write_file(dir.sub("mine.txt"), "ours");
    Junction linked(outside.path(), dir.sub("linked"));
    if (!linked.ok()) GTEST_SKIP() << "junctions unavailable on this host";

    Scanner sc({}, /*follow_symlinks=*/true);
    ScanStats stats;
    Manifest m = sc.scan(dir.str(), nullptr, &stats);

    // Followed, a junction is a plain subdirectory to the peer — the same bargain
    // `--follow-symlinks` makes everywhere else.
    EXPECT_EQ(m.size(), 3u);
    ASSERT_TRUE(m.contains("linked/a.txt"));
    EXPECT_EQ(m.find("linked/a.txt")->hash, sha256("outside a"));
    EXPECT_TRUE(m.contains("linked/deep/b.txt"));
    EXPECT_EQ(stats.symlinks_followed, 1u);
}

TEST(Scanner, FollowedJunctionLoopStillTerminates) {
    test::TempDir dir;
    test::write_file(dir.sub("sub/a.txt"), "a");
    Junction loop(dir.path(), dir.sub("loop"));
    if (!loop.ok()) GTEST_SKIP() << "junctions unavailable on this host";

    // Recognising the cycle means resolving the junction to the directory it
    // points at — which is why the chain is keyed on `real_path` and not on
    // `weakly_canonical`, whose Windows answer for a junction is the junction.
    Scanner sc({}, /*follow_symlinks=*/true);
    ScanStats stats;
    Manifest m = sc.scan(dir.str(), nullptr, &stats);

    EXPECT_EQ(m.size(), 1u);
    EXPECT_TRUE(m.contains("sub/a.txt"));
    EXPECT_EQ(stats.symlinks_skipped, 1u);
    EXPECT_EQ(stats.symlinks_followed, 0u);
}
