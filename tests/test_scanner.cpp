#include <gtest/gtest.h>

#include "core/scanner.h"
#include "test_util.h"

#include <filesystem>

using namespace rasync;

namespace {

// Creating a symlink needs Developer Mode or admin rights on Windows. A host
// without them cannot exercise these paths at all, so the tests skip rather than
// fail — but they must never pass by silently not creating the link, hence the
// explicit check that one now exists.
bool make_symlink(const std::filesystem::path& target,
                  const std::string& link,
                  bool directory) {
    std::error_code ec;
    if (directory) std::filesystem::create_directory_symlink(target, link, ec);
    else           std::filesystem::create_symlink(target, link, ec);
    if (ec) return false;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(link, ec));
}

} // namespace

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
