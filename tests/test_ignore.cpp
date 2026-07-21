#include <gtest/gtest.h>

#include "core/ignore.h"

using namespace rasync;

TEST(Glob, SegmentWildcards) {
    EXPECT_TRUE(glob_match("*.log", "app.log", /*anchored=*/false));
    EXPECT_TRUE(glob_match("*.log", "a/b/c.log", false));
    EXPECT_FALSE(glob_match("*.log", "a/b/c.txt", false));
    EXPECT_TRUE(glob_match("build", "src/build", false));
    EXPECT_TRUE(glob_match("te?t", "test", false));
    EXPECT_FALSE(glob_match("te?t", "teest", false));
}

TEST(Glob, AnchoredPaths) {
    EXPECT_TRUE(glob_match("src/*.cpp", "src/main.cpp", true));
    EXPECT_FALSE(glob_match("src/*.cpp", "lib/src/main.cpp", true));
    EXPECT_TRUE(glob_match("a/**/z", "a/b/c/z", true));
    EXPECT_TRUE(glob_match("a/**/z", "a/z", true));
    EXPECT_TRUE(glob_match("a/**", "a/b/c/d", true));
    EXPECT_FALSE(glob_match("a/**/z", "a/b/c/y", true));
}

TEST(Ignore, BasenameAnywhere) {
    IgnoreList ig;
    ig.add("node_modules");
    ig.add("*.tmp");
    EXPECT_TRUE(ig.matches("node_modules", true));
    EXPECT_TRUE(ig.matches("pkg/node_modules", true));
    EXPECT_TRUE(ig.matches("a/b/scratch.tmp", false));
    EXPECT_FALSE(ig.matches("a/b/keep.txt", false));
}

TEST(Ignore, DirOnlyPattern) {
    IgnoreList ig;
    ig.add("build/");
    EXPECT_TRUE(ig.matches("build", true));    // a directory named build
    EXPECT_FALSE(ig.matches("build", false));  // a *file* named build is kept
}

TEST(Ignore, AnchoredToRoot) {
    IgnoreList ig;
    ig.add("/secret.txt");
    EXPECT_TRUE(ig.matches("secret.txt", false));
    EXPECT_FALSE(ig.matches("sub/secret.txt", false));
}

TEST(Ignore, NegationReincludes) {
    IgnoreList ig;
    ig.add("*.log");
    ig.add("!keep.log");
    EXPECT_TRUE(ig.matches("debug.log", false));
    EXPECT_FALSE(ig.matches("keep.log", false));
}

TEST(Ignore, CommentsAndBlankLinesSkipped) {
    IgnoreList ig;
    ig.add_lines("# a comment\n\n*.bak\n");
    EXPECT_TRUE(ig.matches("x.bak", false));
    EXPECT_EQ(ig.matches("# a comment", false), false);
}

TEST(Ignore, LaterRuleWins) {
    IgnoreList ig;
    ig.add("!important");  // re-include first (no effect yet)
    ig.add("important");   // ...then ignore — later rule wins
    EXPECT_TRUE(ig.matches("important", false));
}
