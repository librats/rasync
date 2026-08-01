#include <gtest/gtest.h>

#include "core/file_stat.h"
#include "test_util.h"

using namespace rasync;

using rasync::test::Junction;
using rasync::test::make_symlink;

// Two questions the platform's own answer is needed for, because the CRT's and
// the standard library's are not stable:
//
//  - *what a link stands for*. UCRT's `stat` answers a symlink either by name —
//    reporting the zero-length reparse point — or by opening a handle and
//    reporting the target, depending on the ucrtbase the binary happens to run
//    against. That is how one build advertised a followed link to its peer as an
//    empty file on a CI runner and correctly on the machine it was written on.
//  - *whether an entry is a link at all*. MSVC calls a junction a junction and
//    not a symlink; libstdc++ on Windows calls nothing a link. A junction is a
//    directory that can point at its own ancestor, so believing either answer
//    means a walk that never ends.

TEST(FileStat, ReportsAPlainFile) {
    test::TempDir dir;
    test::write_file(dir.sub("a.txt"), "0123456789");

    const FileStat s = stat_follow(dir.sub("a.txt"));
    EXPECT_TRUE(s.exists);
    EXPECT_FALSE(s.directory);
    EXPECT_EQ(s.size, 10u);
    EXPECT_GT(s.mtime, 1000000000LL);  // a plausible Unix second, not a FILETIME tick
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

// ── stat_nofollow: is this entry a link? ─────────────────────────────────────

TEST(FileStat, NoFollowReportsAnOrdinaryFileAsNoLink) {
    test::TempDir dir;
    test::write_file(dir.sub("a.txt"), "0123456789");

    const FileStat s = stat_nofollow(dir.sub("a.txt"));
    EXPECT_TRUE(s.exists);
    EXPECT_FALSE(s.link);
    EXPECT_FALSE(s.directory);
    EXPECT_EQ(s.size, 10u);
}

TEST(FileStat, NoFollowReportsADirectoryAsNoLink) {
    test::TempDir dir;
    const FileStat s = stat_nofollow(dir.str());
    EXPECT_TRUE(s.exists);
    EXPECT_FALSE(s.link);
    EXPECT_TRUE(s.directory);
}

TEST(FileStat, NoFollowSeesTheLinkAndFollowSeesTheTarget) {
    test::TempDir dir;
    test::write_file(dir.sub("real.txt"), "the real contents");
    if (!make_symlink("real.txt", dir.sub("link.txt"), /*directory=*/false))
        GTEST_SKIP() << "symlinks unavailable on this host";

    const FileStat self = stat_nofollow(dir.sub("link.txt"));
    EXPECT_TRUE(self.exists);
    EXPECT_TRUE(self.link);
    EXPECT_FALSE(stat_nofollow(dir.sub("real.txt")).link);
    // The pair is the whole contract: one call describes the link, the other the
    // file it stands for, and only the second one carries a size.
    EXPECT_EQ(stat_follow(dir.sub("link.txt")).size, std::string("the real contents").size());
}

TEST(FileStat, ABrokenSymlinkStillExistsAsAnEntry) {
    test::TempDir dir;
    if (!make_symlink("nothing-here.txt", dir.sub("dangling.txt"), /*directory=*/false))
        GTEST_SKIP() << "symlinks unavailable on this host";

    // The entry is there and is a link — that is what makes it skippable rather
    // than invisible. Only resolving it comes back empty-handed.
    const FileStat self = stat_nofollow(dir.sub("dangling.txt"));
    EXPECT_TRUE(self.exists);
    EXPECT_TRUE(self.link);
    EXPECT_FALSE(stat_follow(dir.sub("dangling.txt")).exists);
}

// ── Junctions ────────────────────────────────────────────────────────────────
//
// Windows' other name-surrogate reparse point, and the one no API of the C++
// standard library reports as a link: MSVC gives it `file_type::junction`, and
// libstdc++ on Windows nothing at all. It is a directory that can point at its
// own ancestor, which is all it takes to make a walk endless.

TEST(FileStat, AJunctionIsALink) {
    test::TempDir dir, outside;
    test::write_file(outside.sub("a.txt"), "a");
    Junction junc(outside.path(), dir.sub("junc"));
    if (!junc.ok()) GTEST_SKIP() << "junctions unavailable on this host";

    const FileStat self = stat_nofollow(dir.sub("junc"));
    EXPECT_TRUE(self.exists);
    EXPECT_TRUE(self.link) << "a junction that reads as an ordinary directory is a walk that never ends";
    EXPECT_TRUE(self.directory);

    // Resolved, it is simply the directory it points at.
    const FileStat target = stat_follow(dir.sub("junc"));
    EXPECT_TRUE(target.exists);
    EXPECT_TRUE(target.directory);
}

// ── real_path ────────────────────────────────────────────────────────────────

TEST(FileStat, RealPathResolvesAJunctionToItsTarget) {
    test::TempDir dir, outside;
    Junction junc(outside.path(), dir.sub("junc"));
    if (!junc.ok()) GTEST_SKIP() << "junctions unavailable on this host";

    // What the cycle detector rests on: the junction and its target have to come
    // back as one string. `weakly_canonical` does not manage this everywhere.
    EXPECT_EQ(real_path(dir.sub("junc")), real_path(outside.str()));
    EXPECT_FALSE(real_path(outside.str()).empty());
}

TEST(FileStat, RealPathResolvesASymlinkToItsTarget) {
    test::TempDir dir;
    test::write_file(dir.sub("real.txt"), "x");
    if (!make_symlink("real.txt", dir.sub("link.txt"), /*directory=*/false))
        GTEST_SKIP() << "symlinks unavailable on this host";

    EXPECT_EQ(real_path(dir.sub("link.txt")), real_path(dir.sub("real.txt")));
}

TEST(FileStat, RealPathOfADirectoryIsStableAcrossSpellings) {
    test::TempDir dir;
    test::write_file(dir.sub("sub/a.txt"), "a");

    // Same directory, two spellings: the walk compares these strings, so they
    // have to agree.
    EXPECT_EQ(real_path(dir.sub("sub")), real_path(dir.sub("sub/.")));
    EXPECT_EQ(real_path(dir.sub("sub")), real_path(dir.sub("sub/../sub")));
}

TEST(FileStat, RealPathOfAMissingPathIsEmpty) {
    test::TempDir dir;
    EXPECT_TRUE(real_path(dir.sub("nothing-here")).empty());
}
