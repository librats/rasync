#include <gtest/gtest.h>

#include "core/path.h"

using namespace rasync;

TEST(Path, OrdinaryRelativePathsAreAccepted) {
    EXPECT_TRUE(is_safe_relpath("a.txt"));
    EXPECT_TRUE(is_safe_relpath("sub/dir/file.bin"));
    EXPECT_TRUE(is_safe_relpath(".hidden"));
    EXPECT_TRUE(is_safe_relpath("..leading-dots.txt"));   // not a ".." component
    EXPECT_TRUE(is_safe_relpath("dir/..name/f"));
    EXPECT_TRUE(is_safe_relpath("space in name.txt"));
}

TEST(Path, EmptyPathIsRejected) {
    EXPECT_FALSE(is_safe_relpath(""));
}

TEST(Path, AbsolutePathsAreRejected) {
    EXPECT_FALSE(is_safe_relpath("/etc/passwd"));
    EXPECT_FALSE(is_safe_relpath("\\windows\\system32"));
    EXPECT_FALSE(is_safe_relpath("C:/Windows/System32"));
    EXPECT_FALSE(is_safe_relpath("c:x"));
}

TEST(Path, TraversalComponentsAreRejected) {
    EXPECT_FALSE(is_safe_relpath(".."));
    EXPECT_FALSE(is_safe_relpath("../escape.txt"));
    EXPECT_FALSE(is_safe_relpath("../../../../etc/passwd"));
    EXPECT_FALSE(is_safe_relpath("sub/../../escape.txt"));  // escapes despite the prefix
    EXPECT_FALSE(is_safe_relpath("sub/.."));
    EXPECT_FALSE(is_safe_relpath("."));
    EXPECT_FALSE(is_safe_relpath("./a.txt"));
    EXPECT_FALSE(is_safe_relpath("a/./b"));
}

TEST(Path, BackslashesAreRejectedAnywhere) {
    // A backslash is a separator on Windows, so a path carrying one cannot be
    // reasoned about as a single component — refuse it rather than guess.
    EXPECT_FALSE(is_safe_relpath("sub\\file.txt"));
    EXPECT_FALSE(is_safe_relpath("dir/..\\escape"));
}
