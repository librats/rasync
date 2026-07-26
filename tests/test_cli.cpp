#include <gtest/gtest.h>

#include "app/cli.h"

#include <string>
#include <vector>

using namespace rasync;

namespace {

/// Parse a command line written the way a user types it. The leading argv[0] is
/// added here so no test has to remember it.
ParseResult parse(std::vector<std::string> args) {
    std::vector<std::string>  storage{"rasync"};
    storage.insert(storage.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& s : storage) argv.push_back(s.data());
    return parse_args(static_cast<int>(argv.size()), argv.data());
}

/// Assert the line parsed, and hand back the options *by value* — the callers
/// pass a temporary ParseResult, whose lifetime ends with the full expression.
Options ok(const ParseResult& r) {
    EXPECT_EQ(r.action, ParseResult::Run) << r.error;
    return r.options;
}

} // namespace

TEST(Cli, ASingleDirectoryIsStillTheWholeCommandLine) {
    const Options o = ok(parse({"./data"}));
    ASSERT_EQ(o.folders.size(), 1u);
    EXPECT_EQ(o.folders[0].directory, "./data");
    EXPECT_TRUE(o.folders[0].name.empty()) << "no --name means the leaf name, decided later";
    EXPECT_EQ(o.folders[0].mode, SyncMode::TwoWay);
}

TEST(Cli, DirectoriesAreKeptInCommandLineOrder) {
    const Options o = ok(parse({"./a", "./b", "./c"}));
    ASSERT_EQ(o.folders.size(), 3u);
    EXPECT_EQ(o.folders[0].directory, "./a");
    EXPECT_EQ(o.folders[1].directory, "./b");
    EXPECT_EQ(o.folders[2].directory, "./c");
}

TEST(Cli, MissingDirectoryIsAnError) {
    auto r = parse({"--key", "s3cr3t"});
    EXPECT_EQ(r.action, ParseResult::Error);
    EXPECT_NE(r.error.find("directory"), std::string::npos);
}

// ── the scoping rule: before the first directory is a default, after one is an
//    override for that directory ────────────────────────────────────────────

TEST(Cli, OptionsBeforeTheFirstDirectoryApplyToEveryFolder) {
    const Options o = ok(parse({"--conflict", "larger", "--no-delete", "./a", "./b"}));
    ASSERT_EQ(o.folders.size(), 2u);
    for (const auto& f : o.folders) {
        EXPECT_EQ(f.conflict, ConflictPolicy::Larger);
        EXPECT_TRUE(f.no_delete);
    }
}

TEST(Cli, OptionsAfterADirectoryApplyOnlyToIt) {
    const Options o = ok(parse({"./a", "./b", "--mirror", "--source"}));
    ASSERT_EQ(o.folders.size(), 2u);
    EXPECT_EQ(o.folders[0].mode, SyncMode::TwoWay);
    EXPECT_FALSE(o.folders[0].source);
    EXPECT_EQ(o.folders[1].mode, SyncMode::Mirror);
    EXPECT_TRUE(o.folders[1].source);
}

TEST(Cli, ALaterDirectoryDoesNotInheritAnEarlierDirectorysOverrides) {
    // The default is what came before the *first* directory, never what was
    // scoped to the previous one — otherwise a per-folder flag would leak forward.
    const Options o = ok(parse({"--no-delta", "./a", "--no-delete", "./b"}));
    ASSERT_EQ(o.folders.size(), 2u);
    EXPECT_TRUE(o.folders[0].no_delta);
    EXPECT_TRUE(o.folders[0].no_delete);
    EXPECT_TRUE(o.folders[1].no_delta) << "the shared default was lost";
    EXPECT_FALSE(o.folders[1].no_delete) << "an override leaked into the next folder";
}

TEST(Cli, IgnorePatternsAccumulateDefaultsThenPerFolder) {
    const Options o = ok(parse({"--ignore", "*.tmp", "./a", "--ignore", "*.log", "./b"}));
    ASSERT_EQ(o.folders.size(), 2u);
    EXPECT_EQ(o.folders[0].ignores, (std::vector<std::string>{"*.tmp", "*.log"}));
    EXPECT_EQ(o.folders[1].ignores, (std::vector<std::string>{"*.tmp"}));
}

TEST(Cli, ConnectionOptionsStayNodeWideWhereverTheyAppear) {
    // One connection carries every folder, so these cannot be per-folder however
    // they are written.
    const Options o = ok(parse({"./a", "--port", "9000", "./b", "--key", "s3cr3t",
                                 "--peer", "host:1234", "--interval", "7", "--once"}));
    EXPECT_EQ(o.port, 9000);
    EXPECT_EQ(o.key, "s3cr3t");
    ASSERT_EQ(o.peers.size(), 1u);
    EXPECT_EQ(o.peers[0], "host:1234");
    EXPECT_EQ(o.interval, 7);
    EXPECT_TRUE(o.once);
    EXPECT_TRUE(o.discover) << "--key turns discovery on";
    EXPECT_EQ(o.folders.size(), 2u);
}

// ── folder names ─────────────────────────────────────────────────────────────

TEST(Cli, NameAppliesToTheDirectoryItFollows) {
    const Options o = ok(parse({"~/Docs", "--name", "documents", "~/Pictures", "--name", "photos"}));
    ASSERT_EQ(o.folders.size(), 2u);
    EXPECT_EQ(o.folders[0].name, "documents");
    EXPECT_EQ(o.folders[1].name, "photos");
}

TEST(Cli, NameBeforeAnyDirectoryIsAnError) {
    // Unlike every other folder option, a name identifies one folder — as a
    // default it would put two trees on one name.
    auto r = parse({"--name", "documents", "./a", "./b"});
    EXPECT_EQ(r.action, ParseResult::Error);
    EXPECT_NE(r.error.find("--name"), std::string::npos);
}

TEST(Cli, TwoFoldersMayNotShareAName) {
    auto r = parse({"./a", "--name", "shared", "./b", "--name", "shared"});
    EXPECT_EQ(r.action, ParseResult::Error);
    EXPECT_NE(r.error.find("shared"), std::string::npos);
}

TEST(Cli, FolderNamesRejectWhatCannotBeMatchedByEye) {
    // Names are compared byte for byte between peers, so anything invisible in one
    // is a mismatch nobody can see. Catch it here, not as a sync that never starts.
    std::string why;
    EXPECT_TRUE(valid_folder_name("documents", why)) << why;
    EXPECT_TRUE(valid_folder_name("my docs 2024", why)) << why;
    EXPECT_TRUE(valid_folder_name("документы", why)) << why;   // non-ASCII is fine

    EXPECT_FALSE(valid_folder_name("", why));
    EXPECT_FALSE(valid_folder_name(" docs", why));
    EXPECT_FALSE(valid_folder_name("docs ", why));
    EXPECT_FALSE(valid_folder_name(std::string("docs\twith a tab"), why));
    EXPECT_FALSE(valid_folder_name(std::string("nul\0inside", 10), why));
    EXPECT_FALSE(valid_folder_name(std::string(256, 'x'), why));
    EXPECT_TRUE(valid_folder_name(std::string(255, 'x'), why)) << why;
}

TEST(Cli, AnInvalidNameIsRejectedAtParseTime) {
    auto r = parse({"./a", "--name", "trailing "});
    EXPECT_EQ(r.action, ParseResult::Error);
    EXPECT_NE(r.error.find("whitespace"), std::string::npos) << r.error;
}

// ── mirror-role coherence, now per folder ────────────────────────────────────

TEST(Cli, MirrorNeedsARolePerFolder) {
    EXPECT_EQ(parse({"--mirror", "--source", "./a"}).action, ParseResult::Run);
    auto r = parse({"./a", "./b", "--mirror"});   // the role is missing on b only
    EXPECT_EQ(r.action, ParseResult::Error);
    EXPECT_NE(r.error.find("--mirror"), std::string::npos);
}

TEST(Cli, SourceAndReplicaAreMutuallyExclusive) {
    EXPECT_EQ(parse({"--mirror", "--source", "--replica", "./a"}).action, ParseResult::Error);
}

TEST(Cli, MirrorRolesOnlyApplyWithMirror) {
    EXPECT_EQ(parse({"--source", "./a"}).action, ParseResult::Error);
}

TEST(Cli, OneFolderMayMirrorWhileAnotherMerges) {
    const Options o = ok(parse({"./work", "./archive", "--mirror", "--source"}));
    ASSERT_EQ(o.folders.size(), 2u);
    EXPECT_EQ(o.folders[0].mode, SyncMode::TwoWay);
    EXPECT_EQ(o.folders[1].mode, SyncMode::Mirror);
    EXPECT_TRUE(o.folders[1].source);
}

// ── the plumbing every option shares ─────────────────────────────────────────

TEST(Cli, InlineValuesWorkForFolderOptionsToo) {
    const Options o = ok(parse({"./a", "--name=documents", "--conflict=larger"}));
    ASSERT_EQ(o.folders.size(), 1u);
    EXPECT_EQ(o.folders[0].name, "documents");
    EXPECT_EQ(o.folders[0].conflict, ConflictPolicy::Larger);
}

TEST(Cli, BadValuesAreRejected) {
    EXPECT_EQ(parse({"--port", "70000", "./a"}).action, ParseResult::Error);
    EXPECT_EQ(parse({"--interval", "0", "./a"}).action, ParseResult::Error);
    EXPECT_EQ(parse({"--conflict", "whatever", "./a"}).action, ParseResult::Error);
    EXPECT_EQ(parse({"--allow", "not-a-peer-id", "./a"}).action, ParseResult::Error);
    EXPECT_EQ(parse({"--nonsense", "./a"}).action, ParseResult::Error);
    EXPECT_EQ(parse({"./a", "--name"}).action, ParseResult::Error);   // value missing
}

TEST(Cli, HelpAndVersionShortCircuit) {
    EXPECT_EQ(parse({"--help"}).action, ParseResult::Help);
    EXPECT_EQ(parse({"-h", "./a"}).action, ParseResult::Help);
    EXPECT_EQ(parse({"--version"}).action, ParseResult::Version);
}

TEST(Cli, HelpTextDocumentsTheMultiFolderForm) {
    const std::string help = help_text("rasync");
    EXPECT_NE(help.find("--name"), std::string::npos);
    EXPECT_NE(help.find("directory"), std::string::npos);
}
