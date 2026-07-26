#include <gtest/gtest.h>

#include "net/protocol.h"
#include "core/manifest.h"
#include "core/delta.h"

#include <set>
#include <string>
#include <vector>

using namespace rasync;

namespace {
constexpr uint32_t kTag = 0x11223344;

/// Read past a message's header, checking it names the folder and opcode we meant.
BinaryReader open(const Bytes& msg, uint32_t tag, proto::Op op) {
    BinaryReader r(msg);
    EXPECT_EQ(r.u32(), tag);
    EXPECT_EQ(static_cast<proto::Op>(r.u8()), op);
    return r;
}
} // namespace

TEST(Protocol, MessageStartsWithFolderTagThenOpcode) {
    auto w = proto::message(kTag, proto::Op::FileStart);
    ASSERT_EQ(w.size(), proto::kHeaderBytes);
    // Big-endian, so the tag reads straight off a packet dump.
    EXPECT_EQ(w.buffer()[0], 0x11);
    EXPECT_EQ(w.buffer()[1], 0x22);
    EXPECT_EQ(w.buffer()[2], 0x33);
    EXPECT_EQ(w.buffer()[3], 0x44);
    EXPECT_EQ(w.buffer()[4], static_cast<uint8_t>(proto::Op::FileStart));
}

/// The tag comes before the opcode because it is what *routing* needs: a message
/// for a folder we do not have is dropped without the opcode ever mattering.
TEST(Protocol, FolderTagIsTheFirstFieldOfEveryMessage) {
    for (auto op : {proto::Op::Hello, proto::Op::ManifestUpdate, proto::Op::FileLiteral,
                    proto::Op::Bye}) {
        auto w = proto::message(kTag, op);
        BinaryReader r(w.buffer());
        EXPECT_EQ(r.u32(), kTag);
    }
}

TEST(Protocol, FolderTagIsDeterministicAndNameDependent) {
    // Both peers derive the tag from the name alone, with nothing exchanged — so
    // one name must give the same 32 bits on every machine and every run.
    EXPECT_EQ(proto::folder_tag("docs"), proto::folder_tag("docs"));
    EXPECT_NE(proto::folder_tag("docs"), proto::folder_tag("photos"));
    // Names are matched byte for byte; nothing folds case or trims for the user.
    EXPECT_NE(proto::folder_tag("docs"), proto::folder_tag("Docs"));
    EXPECT_NE(proto::folder_tag("docs"), proto::folder_tag("docs "));
    EXPECT_NE(proto::folder_tag(""), proto::folder_tag("docs"));
}

TEST(Protocol, FolderTagIsNeverTheReservedControlValue) {
    // kControlTag means "no folder", so no name may produce it.
    for (int i = 0; i < 2000; ++i)
        EXPECT_NE(proto::folder_tag("folder-" + std::to_string(i)), proto::kControlTag);
}

TEST(Protocol, FolderTagsAreSpreadAcrossTheSpace) {
    // A weak derivation (a length, a first byte) would still be "deterministic and
    // name-dependent" while colliding constantly in practice.
    std::set<uint32_t> tags;
    for (int i = 0; i < 1000; ++i) tags.insert(proto::folder_tag("f" + std::to_string(i)));
    EXPECT_EQ(tags.size(), 1000u) << "folder tags collided among 1000 distinct names";
}

TEST(Protocol, HelloRoundTrip) {
    auto w = proto::message(kTag, proto::Op::Hello);
    w.u8(proto::kVersion);
    w.u8(0);              // mode
    w.u8(1);              // conflict
    w.str16("photos");    // folder

    BinaryReader r = open(w.buffer(), kTag, proto::Op::Hello);
    EXPECT_EQ(r.u8(), proto::kVersion);
    EXPECT_EQ(r.u8(), 0);
    EXPECT_EQ(r.u8(), 1);
    EXPECT_EQ(r.str16(), "photos");
    EXPECT_TRUE(r.ok());
}

/// The version is the first thing behind the header, so a peer of any other
/// version is recognisable before a single field behind it has to make sense.
TEST(Protocol, VersionIsTheFirstFieldOfHello) {
    auto w = proto::message(kTag, proto::Op::Hello);
    w.u8(proto::kVersion);
    BinaryReader r(w.buffer());
    r.u32();
    r.u8();
    EXPECT_EQ(r.u8(), proto::kVersion);
}

/// Hello carries the folder's name and not just its tag, because 32 bits of hash
/// are not enough to bet two directory trees on. The name is what lets a receiver
/// confirm the two sides mean the same folder before it acts on anything.
TEST(Protocol, HelloCarriesTheNameBehindTheTag) {
    const std::string name = "documents";
    auto w = proto::message(proto::folder_tag(name), proto::Op::Hello);
    w.u8(proto::kVersion);
    w.u8(0);
    w.u8(0);
    w.str16(name);

    BinaryReader r(w.buffer());
    EXPECT_EQ(r.u32(), proto::folder_tag(name));
    r.u8(); r.u8(); r.u8(); r.u8();
    EXPECT_EQ(r.str16(), name);
}

namespace {
/// Decode one ManifestUpdate message into (flags, patch), as a receiver would.
std::pair<uint8_t, ManifestPatch> read_update(const Bytes& msg) {
    BinaryReader r = open(msg, kTag, proto::Op::ManifestUpdate);
    uint8_t flags = r.u8();
    auto patch = ManifestPatch::decode(r);
    EXPECT_TRUE(patch.has_value());
    EXPECT_TRUE(r.ok());
    return {flags, patch.value_or(ManifestPatch{})};
}
} // namespace

TEST(Protocol, ManifestUpdateRoundTrip) {
    FileMeta fm; fm.size = 3; fm.mtime = 9; fm.hash = sha256("abc");
    Manifest tree;
    tree.set("a/b.txt", fm);

    // The first update of a session: a reset carrying the whole tree, because the
    // receiver has no view of us to difference against yet.
    ManifestPatch full;
    for (const auto& [path, meta] : tree.entries()) full.set.emplace_back(path, meta);
    auto w = proto::message(kTag, proto::Op::ManifestUpdate);
    w.u8(proto::kUpdateReset);
    full.encode(w);

    auto [flags, patch] = read_update(w.buffer());
    EXPECT_EQ(flags, proto::kUpdateReset);
    Manifest view;  // the receiver starts from empty on a reset
    view.apply(patch);
    EXPECT_EQ(view.fingerprint(), tree.fingerprint());

    // Every later update is a difference against that same view.
    Manifest next = tree;
    next.remove("a/b.txt");
    next.set("c.txt", fm);
    auto w2 = proto::message(kTag, proto::Op::ManifestUpdate);
    w2.u8(0);
    diff_manifests(tree, next).encode(w2);

    auto [flags2, patch2] = read_update(w2.buffer());
    EXPECT_EQ(flags2, 0);
    view.apply(patch2);
    EXPECT_EQ(view.fingerprint(), next.fingerprint());
    EXPECT_FALSE(view.contains("a/b.txt"));
}

TEST(Protocol, RequestWithSignatureRoundTrip) {
    std::vector<uint8_t> data(1000, 7);
    Signature sig = signature_of(data.data(), data.size(), 256);

    auto w = proto::message(kTag, proto::Op::Request);
    w.str16("file.bin");
    w.u8(1);
    sig.encode(w);

    BinaryReader r = open(w.buffer(), kTag, proto::Op::Request);
    EXPECT_EQ(r.str16(), "file.bin");
    EXPECT_EQ(r.u8(), 1);
    auto back = Signature::decode(r);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->blocks.size(), sig.blocks.size());
}

/// The header is fixed width, so a router can read the tag off any message
/// without knowing what the opcode behind it means.
TEST(Protocol, HeaderWidthMatchesWhatEveryMessageWrites) {
    auto w = proto::message(kTag, proto::Op::Bye);   // the one message with no body
    EXPECT_EQ(w.size(), proto::kHeaderBytes);
}
