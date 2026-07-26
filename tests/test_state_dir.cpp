#include <gtest/gtest.h>

#include "core/state_dir.h"
#include "test_util.h"

#include <cstdlib>
#include <filesystem>
#include <string>

using namespace rasync;
namespace fs = std::filesystem;

namespace {

void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void unset_env(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

/// Points RASYNC_HOME at a temp directory for the duration of a test, so nothing
/// here can touch (or depend on) the real user profile.
struct ScopedStateHome {
    test::TempDir dir;
    explicit ScopedStateHome() { set_env("RASYNC_HOME", dir.str()); }
    ~ScopedStateHome() { unset_env("RASYNC_HOME"); }
};

} // namespace

TEST(StateDir, HomeOverrideWins) {
    ScopedStateHome home;
    EXPECT_EQ(user_state_root(), home.dir.str());
}

TEST(StateDir, StateLivesOutsideTheSyncedTree) {
    ScopedStateHome home;
    test::TempDir tree;

    const std::string state = state_dir_for(tree.str());
    // The whole point: syncing a directory writes nothing into it.
    EXPECT_EQ(state.find(tree.str()), std::string::npos) << "state landed inside the synced tree";
    EXPECT_NE(state.find(home.dir.str()), std::string::npos) << "state is not under the state root";
}

TEST(StateDir, NameIsStableAcrossSpellingsOfTheSamePath) {
    // A run that comes up with a different state directory reads as "never
    // synced": empty baseline, no delete propagation. So the mapping has to
    // survive the ways a user actually types a path.
    const std::string plain = state_dir_name("/home/u/data");
    EXPECT_EQ(state_dir_name("/home/u/data/"), plain);
    EXPECT_EQ(state_dir_name("/home/u/./data"), plain);
    EXPECT_EQ(state_dir_name("/home/u/x/../data"), plain);
#ifdef _WIN32
    EXPECT_EQ(state_dir_name("C:\\Users\\u\\Data"), state_dir_name("c:/users/u/data"));
#endif
}

TEST(StateDir, DifferentTreesWithTheSameLeafNameDoNotShareState) {
    EXPECT_NE(state_dir_name("/home/a/data"), state_dir_name("/home/b/data"));
}

TEST(StateDir, NameKeepsTheDirectoryRecognisable) {
    const std::string name = state_dir_name("/home/u/photos");
    EXPECT_EQ(name.rfind("photos-", 0), 0u) << name;
    EXPECT_EQ(name.size(), std::string("photos-").size() + 16) << name;
}

TEST(StateDir, MigrationMovesLegacyStateAndLeavesTheTreeClean) {
    test::TempDir tree;
    test::TempDir target;
    const std::string legacy = tree.sub(".rasync");
    test::write_file(legacy + "/baseline.man", "baseline");
    test::write_file(legacy + "/identity.key", "identity");

    EXPECT_EQ(migrate_state_dir(legacy, target.str()), 2u);
    EXPECT_EQ(test::read_file(target.sub("baseline.man")), "baseline");
    EXPECT_EQ(test::read_file(target.sub("identity.key")), "identity");
    EXPECT_FALSE(fs::exists(legacy)) << "the synced directory still holds rasync's state";
}

TEST(StateDir, MigrationNeverOverwritesTheNewerState) {
    test::TempDir tree;
    test::TempDir target;
    const std::string legacy = tree.sub(".rasync");
    test::write_file(legacy + "/baseline.man", "old");
    test::write_file(target.sub("baseline.man"), "current");

    EXPECT_EQ(migrate_state_dir(legacy, target.str()), 0u);
    EXPECT_EQ(test::read_file(target.sub("baseline.man")), "current");
    // Nothing was migrated, so nothing is thrown away either.
    EXPECT_TRUE(fs::exists(legacy + "/baseline.man"));
}

TEST(StateDir, MigrationIsANoOpWithoutALegacyDirectory) {
    test::TempDir tree;
    test::TempDir target;
    EXPECT_EQ(migrate_state_dir(tree.sub(".rasync"), target.str()), 0u);
    EXPECT_EQ(migrate_state_dir(target.str(), target.str()), 0u);  // same directory
}

TEST(StateDir, OwnerMarkerNamesTheSyncedDirectory) {
    test::TempDir state;
    record_state_owner(state.str(), "/home/u/data");
    EXPECT_EQ(test::read_file(state.sub("directory.txt")), "/home/u/data\n");
}

TEST(StateDir, NodeStateIsNotAnyFolderState) {
    // One process syncs many folders over one node, so its identity cannot live
    // in a folder's directory: the peer id would then depend on which directory
    // was named first on the command line.
    ScopedStateHome home;
    test::TempDir tree_a, tree_b;

    const std::string node = node_state_dir();
    EXPECT_FALSE(node.empty());
    EXPECT_NE(node, state_dir_for(tree_a.str()));
    EXPECT_NE(node, state_dir_for(tree_b.str()));
    EXPECT_NE(node.find(home.dir.str()), std::string::npos);
    // And it does not move when the folders do.
    EXPECT_EQ(node, node_state_dir());
}
