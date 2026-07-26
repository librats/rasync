#include <gtest/gtest.h>

#include "net/protocol.h"
#include "core/manifest.h"
#include "core/delta.h"

using namespace rasync;

TEST(Protocol, MessageStartsWithOpcode) {
    auto w = proto::message(proto::Op::FileStart);
    ASSERT_GE(w.size(), 1u);
    EXPECT_EQ(w.buffer()[0], static_cast<uint8_t>(proto::Op::FileStart));
}

TEST(Protocol, HelloRoundTrip) {
    auto w = proto::message(proto::Op::Hello);
    w.u8(proto::kVersion);
    w.u8(0);           // mode
    w.u8(1);           // conflict

    BinaryReader r(w.buffer());
    EXPECT_EQ(static_cast<proto::Op>(r.u8()), proto::Op::Hello);
    EXPECT_EQ(r.u8(), proto::kVersion);
    EXPECT_EQ(r.u8(), 0);
    EXPECT_EQ(r.u8(), 1);
    EXPECT_TRUE(r.ok());
}

/// The version is the first thing on the wire, so a peer of any other version is
/// recognisable before a single field behind it has to make sense.
TEST(Protocol, VersionIsTheFirstFieldOfHello) {
    auto w = proto::message(proto::Op::Hello);
    w.u8(proto::kVersion);
    BinaryReader r(w.buffer());
    r.u8();
    EXPECT_EQ(r.u8(), proto::kVersion);
}

namespace {
/// Decode one ManifestUpdate message into (flags, patch), as a receiver would.
std::pair<uint8_t, ManifestPatch> read_update(const Bytes& msg) {
    BinaryReader r(msg);
    EXPECT_EQ(static_cast<proto::Op>(r.u8()), proto::Op::ManifestUpdate);
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
    auto w = proto::message(proto::Op::ManifestUpdate);
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
    auto w2 = proto::message(proto::Op::ManifestUpdate);
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

    auto w = proto::message(proto::Op::Request);
    w.str16("file.bin");
    w.u8(1);
    sig.encode(w);

    BinaryReader r(w.buffer());
    EXPECT_EQ(static_cast<proto::Op>(r.u8()), proto::Op::Request);
    EXPECT_EQ(r.str16(), "file.bin");
    EXPECT_EQ(r.u8(), 1);
    auto back = Signature::decode(r);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->blocks.size(), sig.blocks.size());
}
