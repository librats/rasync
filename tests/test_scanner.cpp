#include <gtest/gtest.h>

#include "core/scanner.h"
#include "test_util.h"

using namespace rasync;

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
