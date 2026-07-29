#include <gtest/gtest.h>

#include "core/file_stat.h"
#include "test_util.h"

using namespace rasync;

using rasync::test::make_symlink;

// What the whole file exists for: a link reports the file it points at. Read
// through the CRT's `stat` this is a coin flip on Windows — UCRT answers a
// symlink out of GetFileAttributesEx and reports a zero-length reparse point,
// MinGW opens a handle and reports the target — which is exactly how a followed
// link once reached a peer as an empty file on one toolchain and not the other.

TEST(FileStat, ReportsAPlainFile) {
    test::TempDir dir;
    test::write_file(dir.sub("a.txt"), "0123456789");

    const FileStat s = stat_follow(dir.sub("a.txt"));
    EXPECT_TRUE(s.exists);
    EXPECT_FALSE(s.directory);
    EXPECT_EQ(s.size, 10u);
    EXPECT_GT(s.mtime, 1000000000);  // a plausible Unix second, not a FILETIME tick
}

TEST(FileStat, ReportsADirectory) {
    test::TempDir dir;
    test::write_file(dir.sub("sub/a.txt"), "x");

    const FileStat s = stat_follow(dir.sub("sub"));
    EXPECT_TRUE(s.exists);
    EXPECT_TRUE(s.directory);
    EXPECT_EQ(s.size, 0u);
}

TEST(FileStat, AMissingPathDoesNotExist) {
    test::TempDir dir;
    EXPECT_FALSE(stat_follow(dir.sub("nothing-here")).exists);
}

TEST(FileStat, ASymlinkReportsItsTargetNotItself) {
    test::TempDir dir;
    test::write_file(dir.sub("real.txt"), "the real contents");
    if (!make_symlink("real.txt", dir.sub("link.txt"), /*directory=*/false))
        GTEST_SKIP() << "symlinks unavailable on this host";

    const FileStat target = stat_follow(dir.sub("real.txt"));
    const FileStat link   = stat_follow(dir.sub("link.txt"));

    EXPECT_TRUE(link.exists);
    EXPECT_FALSE(link.directory);
    EXPECT_EQ(link.size, std::string("the real contents").size());
    EXPECT_EQ(link.size, target.size);
    EXPECT_EQ(link.mtime, target.mtime);
}

TEST(FileStat, ADirectorySymlinkReportsADirectory) {
    test::TempDir dir, outside;
    test::write_file(outside.sub("a.txt"), "a");
    if (!make_symlink(outside.path(), dir.sub("linked"), /*directory=*/true))
        GTEST_SKIP() << "symlinks unavailable on this host";

    const FileStat s = stat_follow(dir.sub("linked"));
    EXPECT_TRUE(s.exists);
    EXPECT_TRUE(s.directory);
}

TEST(FileStat, ABrokenSymlinkDoesNotExist) {
    test::TempDir dir;
    if (!make_symlink("nothing-here.txt", dir.sub("dangling.txt"), /*directory=*/false))
        GTEST_SKIP() << "symlinks unavailable on this host";

    // Nothing behind the link, so nothing to sync — the scanner reads this as
    // "skip" rather than syncing a zero-byte file the peer never had.
    EXPECT_FALSE(stat_follow(dir.sub("dangling.txt")).exists);
}
